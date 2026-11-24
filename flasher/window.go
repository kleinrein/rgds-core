package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// window is the browser we started. exited closes when that process ends.
type window struct {
	proc    *os.Process
	exited  chan struct{}
	profile string
}

// The browser window's profile lives only as long as the flasher: a new folder each run, removed at the end, so
// Chrome's caches never pile up (they reached 190 MB in one profile kept across runs). These switches keep Chrome
// from fetching what a one-page app window never uses: component updates, Safe Browsing lists, on-device models,
// translation, sync, and the keyring prompt on Linux.
var browserArgs = []string{"--window-size=560,800", "--no-first-run", "--no-default-browser-check",
	"--disable-extensions", "--hide-crash-restore-bubble", "--disable-session-crashed-bubble", "--disable-background-mode",
	"--disable-component-update", "--disable-background-networking", "--disable-sync", "--no-pings",
	"--disable-domain-reliability", "--disable-client-side-phishing-detection", "--safebrowsing-disable-auto-update",
	"--disable-breakpad", "--password-store=basic", "--use-mock-keychain", "--no-service-autorun",
	"--disable-features=Translate,OptimizationHints,OptimizationGuideModelDownloading,OptimizationHintsFetching," +
		"OptimizationTargetPrediction,MediaRouter,AutofillServerCommunication,CertificateTransparencyComponentUpdater"}

// clearProfiles removes the browser profiles earlier runs left: the one shared profile of older versions (its
// browser, kept alive by macOS, is ended first), and any a crash left. A profile a browser still has open belongs
// to another flasher running right now, and stays.
func clearProfiles() {
	matches, _ := filepath.Glob(filepath.Join(cacheDir(), "window*"))
	for _, dir := range matches {
		if filepath.Base(dir) == "window" {
			endLeftover(dir)
		} else if profileInUse(dir) {
			continue
		}
		if err := os.RemoveAll(dir); err == nil {
			logf("removed the old browser profile %s", filepath.Base(dir))
		}
	}
}

// profileInUse says whether a running browser has the profile open: Chromium's SingletonLock link names its pid
// on macOS and Linux; on Windows its lockfile can't be removed while the browser runs.
func profileInUse(dir string) bool {
	if runtime.GOOS == "windows" {
		err := os.Remove(filepath.Join(dir, "lockfile"))
		return err != nil && !os.IsNotExist(err)
	}
	link, err := os.Readlink(filepath.Join(dir, "SingletonLock"))
	if err != nil {
		return false
	}
	pid, err := strconv.Atoi(link[strings.LastIndex(link, "-")+1:])
	if err != nil || pid <= 1 {
		return false
	}
	p, err := os.FindProcess(pid)
	return err == nil && p.Signal(syscall.Signal(0)) == nil
}

// openWindow shows the UI in a Chromium-based browser's app mode (a plain window with no tabs or address bar), in
// a profile of its own so the process is ours to close. Without one it opens the default browser.
func openWindow(url string) *window {
	clearProfiles()
	profile, err := os.MkdirTemp(cacheDir(), "window-")
	if err != nil {
		profile = filepath.Join(cacheDir(), "window-"+randomToken()[:8])
	}
	args := append([]string{"--app=" + url, "--user-data-dir=" + profile}, browserArgs...)
	for _, browser := range browsers() {
		if _, err := os.Stat(browser); err != nil {
			if browser, err = exec.LookPath(browser); err != nil {
				continue
			}
		}
		cmd := exec.Command(browser, args...)
		if err := cmd.Start(); err == nil {
			w := &window{cmd.Process, make(chan struct{}), profile}
			go func() { cmd.Wait(); close(w.exited) }()
			return w
		}
	}
	os.Remove(profile)
	openDefault(url)
	return nil
}

func openDefault(url string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", url).Start()
	case "windows":
		exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	default:
		exec.Command("xdg-open", url).Start()
	}
}

// close lets the browser end the way it would for a user. The page has closed its own window by now; on macOS
// the browser keeps running without windows, so it gets SIGTERM, which it treats as a normal quit. A kill is the
// last resort only: the browser takes it for a crash and offers to restore the page next time.
func (w *window) close() {
	wait := func(d time.Duration) bool {
		select {
		case <-w.exited:
			return true
		case <-time.After(d):
			return false
		}
	}
	if wait(1500 * time.Millisecond) {
		return
	}
	if runtime.GOOS != "windows" { // Windows has no gentle signal; the browser ends with its last window
		w.proc.Signal(syscall.SIGTERM)
	}
	if !wait(4 * time.Second) {
		w.proc.Kill()
		wait(2 * time.Second)
	}
	for try := 0; try < 10; try++ { // Windows keeps the files locked a moment after the browser ends
		if os.RemoveAll(w.profile) == nil {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// endLeftover ends a browser still running on our profile from an earlier run (macOS keeps it alive without
// windows). Otherwise the new window would be handed to it, the process we start would exit at once, and the
// flasher could never close the browser it shows. Chromium's SingletonLock link names "<host>-<pid>"; the pid
// counts only if that process really runs with our profile. Windows has no such link, and its browser ends with
// its last window.
func endLeftover(profile string) {
	if runtime.GOOS == "windows" {
		return
	}
	link, err := os.Readlink(filepath.Join(profile, "SingletonLock"))
	if err != nil {
		return
	}
	pid, err := strconv.Atoi(link[strings.LastIndex(link, "-")+1:])
	if err != nil || pid <= 1 {
		return
	}
	out, err := exec.Command("ps", "-o", "command=", "-p", strconv.Itoa(pid)).Output()
	if err != nil || !strings.Contains(string(out), "--user-data-dir="+profile) {
		return
	}
	logf("ending the browser left from an earlier run (pid %d)", pid)
	p, err := os.FindProcess(pid)
	if err != nil {
		return
	}
	p.Signal(syscall.SIGTERM)
	for i := 0; i < 40; i++ {
		time.Sleep(100 * time.Millisecond)
		if p.Signal(syscall.Signal(0)) != nil {
			return
		}
	}
	p.Kill()
	time.Sleep(300 * time.Millisecond)
}

func browsers() []string {
	switch runtime.GOOS {
	case "darwin":
		home, _ := os.UserHomeDir()
		var out []string
		for _, app := range []string{"Google Chrome", "Microsoft Edge", "Chromium", "Brave Browser"} {
			out = append(out, "/Applications/"+app+".app/Contents/MacOS/"+app, home+"/Applications/"+app+".app/Contents/MacOS/"+app)
		}
		return out
	case "windows":
		var out []string
		for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles", "LocalAppData"} {
			if dir := os.Getenv(env); dir != "" {
				out = append(out, dir+`\Microsoft\Edge\Application\msedge.exe`, dir+`\Google\Chrome\Application\chrome.exe`)
			}
		}
		return out
	default:
		if os.Geteuid() == 0 {
			return nil // Chromium refuses to run as root; sudo users get the default browser
		}
		return []string{"google-chrome", "chromium", "chromium-browser", "microsoft-edge"}
	}
}

// showFile shows a file in Finder or Explorer (selected), or opens its folder on Linux.
func showFile(path string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", "-R", path).Start()
	case "windows":
		exec.Command("explorer", "/select,", path).Start()
	default:
		exec.Command("xdg-open", filepath.Dir(path)).Start()
	}
}
