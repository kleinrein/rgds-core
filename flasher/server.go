package main

import (
	"context"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

//go:embed ui
var uiFiles embed.FS

// update is one progress step, from the UI process's own jobs or posted by the worker.
type update struct {
	Phase string `json:"phase"`
	Done  int64  `json:"done"`
	Total int64  `json:"total"`
	Error string `json:"error,omitempty"`
}

type Ready struct {
	Name    string `json:"name"`
	Model   string `json:"model"`
	Version string `json:"version"`
	Size    int64  `json:"size"`   // unpacked: what the card needs
	Writes  int64  `json:"writes"` // what the flash writes and reads back (plan.go)
	Signed  bool   `json:"signed"` // checked against RG DS's signature (sign.go)
	path    string
}

// State is everything the UI shows; it is pushed to the page whenever it changes.
type State struct {
	OS        string   `json:"os"`
	Version   string   `json:"version"`
	Phase     string   `json:"phase"` // idle download check ready prepare write verify eject done error
	Done      int64    `json:"done"`
	Total     int64    `json:"total"`
	Rate      float64  `json:"rate"` // bytes per second
	Error     string   `json:"error,omitempty"`
	ErrorStep string   `json:"errorStep,omitempty"` // download or flash
	Release   *Release `json:"release"`
	Loading   bool     `json:"loading"`
	RelError  string   `json:"releaseError,omitempty"`
	Image     *Ready   `json:"image"`
	Disk      *Disk    `json:"disk"`
	Carried   *Carried `json:"carried,omitempty"`
	CardCopy  string   `json:"cardCopy,omitempty"` // the whole-card backup this flash made (backup.go)
	Busy      bool     `json:"busy"`
	Log       string   `json:"log"`
}

type server struct {
	mu       sync.Mutex
	st       State
	changed  chan struct{}
	local    string
	cancel   context.CancelFunc
	job      string // the worker job that may report
	base     string // http://127.0.0.1:port/t/token/
	clients  int
	lastSeen time.Time
	rateAt   time.Time
	rateDone int64
	finish   func(error)     // how the worker's job ends (run)
	flashed  map[string]bool // images that went onto a card this run: downloaded ones go when the flasher closes
}

func randomToken() string {
	b := make([]byte, 16)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func serve(local string, withWindow bool) error {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	token := randomToken()
	host := ln.Addr().String()
	s := &server{
		st:       State{OS: runtime.GOOS, Version: version, Phase: "idle", Log: logPath()},
		changed:  make(chan struct{}, 1),
		local:    local,
		base:     "http://" + host + "/t/" + token + "/",
		lastSeen: time.Now(),
	}
	ui, _ := fs.Sub(uiFiles, "ui")
	static := http.StripPrefix("/t/"+token, http.FileServer(http.FS(ui)))
	mux := http.NewServeMux()
	mux.HandleFunc("/t/"+token+"/", func(w http.ResponseWriter, r *http.Request) {
		if r.Host != host {
			http.Error(w, "wrong host", 403)
			return
		}
		path := strings.TrimPrefix(r.URL.Path, "/t/"+token)
		if !strings.HasPrefix(path, "/api/") {
			w.Header().Set("Cache-Control", "no-store")
			static.ServeHTTP(w, r)
			return
		}
		if r.Method == "POST" && r.Header.Get("Content-Type") != "application/json" {
			http.Error(w, "json only", 415)
			return
		}
		s.api(w, r, strings.TrimPrefix(path, "/api/"))
	})
	go s.loadRelease()
	go s.watchLifetime()
	errc := make(chan error, 1)
	go func() { errc <- http.Serve(ln, mux) }()
	if !withWindow {
		fmt.Println(s.base)
		go s.exitOnQuit(nil)
		return <-errc
	}
	if nativeWindow(s.base, s) { // the Mac's own window; it runs until the program ends
		return nil
	}
	browser := openWindow(s.base)
	if browser != nil {
		go func() { // a browser that started but never showed the page (a snap without access to its profile)
			time.Sleep(15 * time.Second)
			if !s.everConnected() {
				logf("no window after 15 s; opening the default browser")
				openDefault(s.base)
			}
		}()
	}
	go s.exitOnQuit(browser)
	return <-errc
}

// exitOnQuit ends the program on Close or a closed window, the browser window with it.
func (s *server) exitOnQuit(browser *window) {
	<-quit
	s.waitForPages(2 * time.Second)
	if browser != nil {
		browser.close()
	}
	s.shutdown()
	os.Exit(0)
}

// shutdown tidies up as the program ends: a release image that went onto a card this run is removed, so the flasher
// leaves nothing big behind (the next card downloads it again). Not while a card is being written.
func (s *server) shutdown() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.st.Busy {
		return
	}
	for path := range s.flashed {
		if filepath.Dir(path) != cacheDir() {
			continue // a --image file or a backup: the person's own
		}
		for _, p := range []string{path, path + ".sha256", path + ".sig"} {
			os.Remove(p)
		}
		logf("removed the downloaded image %s", filepath.Base(path))
	}
	s.flashed = nil
}

// dropPreset removes the setup answers of a flash that is over or never started: they hold the Wi-Fi key.
func dropPreset(path string) {
	if path != "" {
		os.Remove(path)
		os.Remove(filepath.Join(filepath.Dir(path), "carry-preset.tar.gz"))
	}
}

var (
	quit     = make(chan struct{})
	quitOnce sync.Once
)

// stop ends the program, whichever of Close or the closed window asks first.
func stop() { quitOnce.Do(func() { close(quit) }) }

// waitForPages gives the page a moment to close its own window, so the browser ends normally.
func (s *server) waitForPages(limit time.Duration) {
	for end := time.Now().Add(limit); time.Now().Before(end); time.Sleep(50 * time.Millisecond) {
		s.mu.Lock()
		n := s.clients
		s.mu.Unlock()
		if n == 0 {
			return
		}
	}
}

// watchLifetime ends the program once its window is closed and no card is being written.
func (s *server) watchLifetime() {
	for range time.Tick(time.Second) {
		s.mu.Lock()
		idle := s.clients == 0 && !s.st.Busy
		gone := time.Since(s.lastSeen)
		s.mu.Unlock()
		if idle && gone > 6*time.Second && (s.everConnected() || gone > 3*time.Minute) {
			stop()
			return
		}
	}
}

var connected bool // a page has been open at least once

func (s *server) everConnected() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return connected
}

func (s *server) notify() {
	select {
	case s.changed <- struct{}{}:
	default:
	}
}

// broadcast fans one change signal out to every open page.
var (
	subsMu sync.Mutex
	subs   = map[chan struct{}]bool{}
)

func (s *server) fanOut() {
	for range s.changed {
		subsMu.Lock()
		for c := range subs {
			select {
			case c <- struct{}{}:
			default:
			}
		}
		subsMu.Unlock()
	}
}

var fanOnce sync.Once

func (s *server) events(w http.ResponseWriter, r *http.Request) {
	fanOnce.Do(func() { go s.fanOut() })
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "no streaming", 500)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-store")
	c := make(chan struct{}, 1)
	subsMu.Lock()
	subs[c] = true
	subsMu.Unlock()
	s.mu.Lock()
	s.clients++
	connected = true
	s.mu.Unlock()
	defer func() {
		subsMu.Lock()
		delete(subs, c)
		subsMu.Unlock()
		s.mu.Lock()
		s.clients--
		s.lastSeen = time.Now()
		s.mu.Unlock()
	}()
	ping := time.NewTicker(15 * time.Second)
	defer ping.Stop()
	for {
		s.mu.Lock()
		body, _ := json.Marshal(s.st)
		s.mu.Unlock()
		if _, err := fmt.Fprintf(w, "data: %s\n\n", body); err != nil {
			return
		}
		fl.Flush()
		select {
		case <-r.Context().Done():
			return
		case <-c:
			time.Sleep(80 * time.Millisecond) // coalesce bursts of progress
		case <-ping.C:
		}
	}
}

func (s *server) loadRelease() {
	s.mu.Lock()
	s.st.Loading, s.st.RelError = true, ""
	s.mu.Unlock()
	s.notify()
	var rel *Release
	var err error
	if s.local != "" {
		rel = localRelease(s.local)
	} else {
		rel, err = fetchRelease(context.Background())
	}
	if backups := listBackups(); rel != nil {
		rel.Backups = backups
	} else if len(backups) > 0 { // offline with nothing saved: a backup can still go back on a card
		rel, err = &Release{Offline: true, Backups: backups}, nil
	}
	s.mu.Lock()
	s.st.Loading = false
	if err != nil {
		logf("release: %v", err)
		s.st.RelError = err.Error()
	} else {
		logf("release %s, %d images, offline %v", rel.Version, len(rel.Images), rel.Offline)
		s.st.Release = rel
	}
	s.mu.Unlock()
	s.notify()
}

// progress records one step and keeps a smoothed rate for the time-left estimate.
func (s *server) progress(phase string, done, total int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	// not phases: how many DS saves the flash read off a stock card, and where their backup went (carry.go)
	if phase == "dssaves" || strings.HasPrefix(phase, "dsbackup:") {
		if s.st.Carried == nil { // a card nobody chose to keep anything from, rescued anyway
			s.st.Carried = &Carried{OnCard: true}
		}
		if phase == "dssaves" {
			s.st.Carried.DSSaves = int(done)
		} else if s.st.Carried.Folder == "" {
			s.st.Carried.Folder = strings.TrimPrefix(phase, "dsbackup:")
		}
		return
	}
	now := time.Now()
	if phase != s.st.Phase {
		logf("phase %s (%d/%d)", phase, done, total)
	}
	if phase != s.st.Phase || done < s.rateDone {
		s.st.Rate, s.rateAt, s.rateDone = 0, now, done
	} else if dt := now.Sub(s.rateAt).Seconds(); dt >= 0.5 {
		inst := float64(done-s.rateDone) / dt
		if s.st.Rate == 0 {
			s.st.Rate = inst
		} else {
			s.st.Rate = 0.7*s.st.Rate + 0.3*inst
		}
		s.rateAt, s.rateDone = now, done
	}
	s.st.Phase, s.st.Done, s.st.Total = phase, done, total
	s.notify()
}

func (s *server) fail(step string, err error) {
	logf("%s failed: %v", step, err)
	s.mu.Lock()
	s.st.Phase, s.st.Busy, s.st.Error, s.st.ErrorStep = "error", false, friendly(err), step
	s.job = ""
	s.mu.Unlock()
	s.notify()
}

func friendly(err error) string {
	if errors.Is(err, context.Canceled) {
		return "Stopped."
	}
	msg := err.Error()
	if msg == "" {
		return "Something went wrong."
	}
	return strings.ToUpper(msg[:1]) + msg[1:]
}

func (s *server) api(w http.ResponseWriter, r *http.Request, name string) {
	var req struct {
		Model   string `json:"model"`
		Disk    string `json:"disk"`
		Confirm string `json:"confirm"`
		Keep    bool   `json:"keep"`   // bring the old system's saves along (carry.go)
		Games   bool   `json:"games"`  // and copy its games to this computer
		Backup  bool   `json:"backup"` // copy the whole card to this computer first (backup.go)
		Preset  Preset `json:"preset"` // setup answers for the first start (preset.go)
		update
	}
	if r.Method == "POST" {
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&req); err != nil {
			http.Error(w, "bad json", 400)
			return
		}
	}
	reply := func(err error) {
		if err != nil {
			http.Error(w, friendly(err), 409)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte("{}"))
	}
	switch {
	case name == "events":
		s.events(w, r)
	case name == "disks":
		disks, err := listDisks()
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		s.mu.Lock()
		need := int64(0)
		if s.st.Image != nil {
			need = s.st.Image.Size
		}
		s.mu.Unlock()
		for i := range disks {
			if d := &disks[i]; d.Problem == "" && need > d.Size {
				d.Problem = fmt.Sprintf("Too small, the image needs %.1f GB", float64(need)/1e9)
			} else if d.Problem == "" && d.Size < 7e9 {
				d.Warnings = append(d.Warnings, "8 GB or more is best: the rest of the card holds your games.")
			}
		}
		w.Header().Set("Content-Type", "application/json")
		if disks == nil {
			disks = []Disk{}
		}
		json.NewEncoder(w).Encode(disks)
	case r.Method != "POST":
		http.Error(w, "post only", 405)
	case name == "release":
		s.mu.Lock()
		busy := s.st.Loading
		s.mu.Unlock()
		if !busy {
			go s.loadRelease()
		}
		reply(nil)
	case name == "prepare":
		reply(s.prepare(req.Model))
	case name == "cancel":
		s.mu.Lock()
		if s.cancel != nil {
			s.cancel()
		}
		s.mu.Unlock()
		reply(nil)
	case name == "flash":
		reply(s.startFlash(req.Disk, req.Confirm, req.Keep, req.Games, req.Backup, req.Preset))
	case name == "backup":
		s.mu.Lock()
		folder := s.st.CardCopy
		if s.st.Carried != nil && s.st.Carried.Folder != "" {
			folder = s.st.Carried.Folder
		}
		s.mu.Unlock()
		if folder != "" {
			showFile(folder)
		}
		reply(nil)
	case name == "log":
		showFile(logPath())
		reply(nil)
	case name == "quit":
		s.mu.Lock()
		busy := s.st.Busy
		s.mu.Unlock()
		if busy {
			reply(errors.New("a card is being written"))
			return
		}
		reply(nil)
		stop()
	case name == "again":
		s.mu.Lock()
		if !s.st.Busy {
			s.st.Carried, s.st.CardCopy = nil, ""
		}
		if !s.st.Busy && s.st.Image != nil {
			s.st.Phase, s.st.Error, s.st.Disk = "ready", "", nil
		} else if !s.st.Busy {
			s.st.Phase, s.st.Error = "idle", ""
		}
		s.mu.Unlock()
		s.notify()
		reply(nil)
	case strings.HasPrefix(name, "worker/"):
		s.workerUpdate(strings.TrimPrefix(name, "worker/"), req.update)
		reply(nil)
	default:
		http.NotFound(w, r)
	}
}

func (s *server) prepare(modelName string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.st.Busy {
		return errors.New("busy")
	}
	if s.st.Release == nil {
		return errors.New("no release loaded")
	}
	var img *Image
	for _, list := range [][]Image{s.st.Release.Images, s.st.Release.Backups} {
		for i := range list {
			if list[i].Model == modelName {
				img = &list[i]
			}
		}
	}
	if img == nil {
		return fmt.Errorf("no %s image in this release", modelName)
	}
	ctx, cancel := context.WithCancel(context.Background())
	s.cancel = cancel
	s.st.Busy, s.st.Error, s.st.Image, s.st.Phase, s.st.Done, s.st.Total = true, "", nil, "check", 0, 0
	version := s.st.Release.Version
	go func(img Image) {
		defer keepAwake()()
		path, signed, err := fetchImage(ctx, img, s.progress)
		var plan *Plan
		if err == nil {
			plan, err = planFor(path, s.progress)
		}
		if err != nil {
			s.fail("download", err)
			return
		}
		s.mu.Lock()
		img.Cached, img.path = true, path
		for i := range s.st.Release.Images {
			if s.st.Release.Images[i].Name == img.Name {
				s.st.Release.Images[i] = img
			}
		}
		writes := plan.Written
		if plan.write == nil {
			writes = plan.Total
		}
		if strings.HasPrefix(img.Model, "backup:") {
			version = ""
		}
		s.st.Image = &Ready{Name: img.Name, Model: img.Model, Version: version, Size: plan.Total, Writes: writes, Signed: signed, path: path}
		s.st.Phase, s.st.Busy, s.cancel = "ready", false, nil
		s.mu.Unlock()
		s.notify()
	}(*img)
	s.notify()
	return nil
}

func (s *server) startFlash(id, confirm string, keep, games, backup bool, preset Preset) error {
	if id == "" || confirm != id {
		return errors.New("not confirmed; nothing was written")
	}
	s.mu.Lock()
	restoring := s.st.Image != nil && strings.HasPrefix(s.st.Image.Model, "backup:")
	s.mu.Unlock()
	if !restoring { // --ssh-key travels with the setup answers; a backup goes back byte for byte, without it
		preset.SSHKey = devSSHKey
	}
	presetPath, err := savePreset(preset) // a bad answer stops here, before anything happens to the card
	if err != nil {
		return err
	}
	s.mu.Lock()
	if s.st.Busy || s.st.Image == nil {
		s.mu.Unlock()
		return errors.New("the image is not ready")
	}
	job := Job{Image: s.st.Image.path, Disk: id, Preset: presetPath}
	s.st.Busy, s.st.Error, s.st.Phase, s.st.Done, s.st.Total = true, "", "prepare", 0, 0
	s.mu.Unlock()
	s.notify()
	d, err := findDisk(id)
	if err != nil {
		dropPreset(presetPath)
		s.idle()
		return err
	}
	if backup {
		job.Backup = backupPath(d.Name)
	}
	s.mu.Lock()
	s.st.Disk, s.st.Carried, s.st.CardCopy = &d, nil, job.Backup
	s.mu.Unlock()
	if keep && d.Carry != nil {
		// The old card's saves (and games) to this computer first, while it is still mounted; then the flash
		// takes the bundle along. A backup that fails stops here: nothing has been written to the card yet.
		go func() {
			folder, bundle, err := keepCarry(d.Carry, games, s.progress)
			if err != nil {
				dropPreset(presetPath)
				s.fail("flash", err)
				return
			}
			copied := 0
			if games {
				copied = d.Carry.Games
			}
			s.mu.Lock()
			s.st.Carried = &Carried{Folder: folder, OnCard: true, Games: copied, GamesLeft: d.Carry.Games - copied}
			s.mu.Unlock()
			job.Carry = bundle
			if err := s.run(job); err != nil {
				dropPreset(presetPath)
				s.fail("flash", err)
			}
		}()
		return nil
	}
	if d.Carry != nil { // its saves were offered and left unticked: the flash brings none, DS saves included
		job.Carry = "-"
	}
	if err := s.run(job); err != nil { // the worker never started (Windows' permission prompt said No)
		dropPreset(presetPath)
		s.idle()
		return err
	}
	return nil
}

// run writes the card: here when this process may, otherwise in the elevated worker.
func (s *server) run(j Job) error {
	finish := func(err error) {
		dropPreset(j.Preset) // only as long as the flash needs it
		if err != nil {
			s.mu.Lock()
			if j.Backup != "" && !isFile(j.Backup) {
				s.st.CardCopy = ""
			}
			s.mu.Unlock()
			s.fail("flash", err)
			return
		}
		s.mu.Lock()
		s.st.Phase, s.st.Busy, s.job = "done", false, ""
		if s.flashed == nil {
			s.flashed = map[string]bool{}
		}
		s.flashed[j.Image] = true
		if j.Backup != "" && s.st.Release != nil { // the new backup can go back from the first step
			s.st.Release.Backups = listBackups()
		}
		s.mu.Unlock()
		s.notify()
	}
	if inProcess(j.Disk) {
		go func() { finish(flash(j, s.progress)) }()
		return nil
	}
	job := randomToken()
	s.mu.Lock()
	s.job, s.finish = job, finish
	s.mu.Unlock()
	// This blocks while Windows shows its admin prompt, so the lock is not held here.
	args := []string{"--worker", "--report", s.base + "api/worker/" + job, "--disk", j.Disk, "--image", j.Image, "--log", logPath()}
	for flag, value := range map[string]string{"--carry": j.Carry, "--backup": j.Backup, "--preset": j.Preset} {
		if value != "" {
			args = append(args, flag, value)
		}
	}
	wait, err := startWorker(args)
	if err != nil {
		return err
	}
	go func() {
		err := wait()
		s.mu.Lock()
		running := s.job == job
		s.mu.Unlock()
		if running { // the worker ended without its last word
			if err == nil {
				err = errors.New("the writer stopped")
			}
			finish(err)
		}
	}()
	return nil
}

// idle goes back to the card list after a flash that never started.
func (s *server) idle() {
	s.mu.Lock()
	s.st.Busy, s.st.Phase, s.st.Disk, s.job = false, "ready", nil, ""
	s.mu.Unlock()
	s.notify()
}

func (s *server) workerUpdate(job string, u update) {
	s.mu.Lock()
	mine := job != "" && job == s.job
	s.mu.Unlock()
	if !mine {
		return
	}
	switch u.Phase {
	case "error", "done":
		s.mu.Lock()
		finish := s.finish
		s.job, s.finish = "", nil
		s.mu.Unlock()
		var err error
		if u.Phase == "error" {
			err = errors.New(u.Error)
		}
		if finish != nil {
			finish(err)
		}
	default:
		s.progress(u.Phase, u.Done, u.Total)
	}
}
