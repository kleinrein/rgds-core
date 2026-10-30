package main

import (
	"bytes"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

func diskutil(verb string, args ...string) (map[string]any, error) {
	out, err := exec.Command("diskutil", append([]string{verb, "-plist"}, args...)...).Output()
	if err != nil {
		return nil, fmt.Errorf("diskutil %s: %w", verb, err)
	}
	v, err := parsePlist(out)
	if err != nil {
		return nil, err
	}
	m, _ := v.(map[string]any)
	return m, nil
}

func listDisks() ([]Disk, error) {
	// RGDS_FLASHER_DISK_IMAGES=1 offers attached disk images too, for testing the flasher without a card.
	images := os.Getenv("RGDS_FLASHER_DISK_IMAGES") == "1"
	scope := []string{"external", "physical"}
	if images {
		scope = scope[:1]
	}
	listing, err := diskutil("list", scope...)
	if err != nil {
		return nil, err
	}
	var disks []Disk
	for _, w := range array(listing["WholeDisks"]) {
		id, _ := w.(string)
		info, err := diskutil("info", id)
		if err != nil {
			continue
		}
		removable := boolKey(info, "RemovableMedia") || boolKey(info, "Removable") || boolKey(info, "Ejectable")
		bus := str(info, "BusProtocol")
		if boolKey(info, "Internal") || !removable || !boolKey(info, "WholeDisk") || (bus == "Disk Image" && !images) || bus == "Virtual Interface" {
			continue
		}
		name := strings.TrimSpace(strings.TrimSuffix(str(info, "MediaName"), " Media"))
		if name == "" {
			name = "SD card"
		}
		d := Disk{ID: id, Path: "/dev/" + id, Name: name, Size: num(info, "TotalSize")}
		if w, ok := info["WritableMedia"].(bool); ok && !w {
			d.Problem = "Write-protected: slide the lock switch on the SD adapter up"
		}
		if parts, err := diskutil("list", id); err == nil {
			for _, whole := range array(parts["AllDisksAndPartitions"]) {
				for _, p := range array(whole.(map[string]any)["Partitions"]) {
					pm, _ := p.(map[string]any)
					if v := str(pm, "VolumeName"); v != "" {
						d.Volumes = append(d.Volumes, v)
					}
					if m := str(pm, "MountPoint"); m != "" {
						d.mounts = append(d.mounts, m)
					}
				}
			}
		}
		disks = append(disks, d)
	}
	return finishDisks(disks), nil
}

type darwinTarget struct {
	*os.File
	id   string
	name string
	size int64
	stop chan struct{}
	mu   sync.Mutex // guards id, which Reopen changes while the watcher reads it
	auth *authorization
}

func (t *darwinTarget) disk() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.id
}

func openTarget(d Disk) (target, error) {
	out, err := exec.Command("diskutil", "unmountDisk", "/dev/"+d.ID).CombinedOutput()
	logf("unmountDisk %s: %s", d.ID, strings.TrimSpace(string(out)))
	if err != nil {
		return nil, fmt.Errorf("can't unmount the card. Close anything that uses it and try again (%s)", strings.TrimSpace(string(out)))
	}
	t := &darwinTarget{id: d.ID, name: d.Name, size: d.Size, stop: make(chan struct{})}
	if f, err := os.OpenFile("/dev/r"+d.ID, os.O_RDWR, 0); err == nil {
		t.File = f // a disk the user may write, such as an attached test image
	} else {
		if t.auth, err = authorize(); err != nil {
			return nil, err
		}
		if t.File, err = t.auth.open("/dev/r" + d.ID); err != nil {
			t.auth.release()
			return nil, err
		}
	}
	go t.keepUnmounted()
	return t, nil
}

// ReadAt reads in small pieces: some USB card readers reset on large reads (Linux limits USB storage to 120 KiB
// requests for the same reason), while they take large writes fine.
func (t *darwinTarget) ReadAt(p []byte, off int64) (int, error) {
	const piece = 256 << 10
	done := 0
	for done < len(p) {
		n, err := t.File.ReadAt(p[done:min(len(p), done+piece)], off+int64(done))
		done += n
		if err != nil {
			return done, err
		}
	}
	return done, nil
}

// keepUnmounted unmounts whatever macOS mounts from the card while it is written: once it sees the new partition
// table it mounts RGDS-BOOT and starts writing its own files there, which would spoil the check.
func (t *darwinTarget) keepUnmounted() {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-t.stop:
			return
		case <-tick.C:
		}
		out, _ := exec.Command("/sbin/mount").Output()
		id := t.disk()
		prefix := "/dev/" + id + "s"
		for _, line := range strings.Split(string(out), "\n") {
			if strings.HasPrefix(line, prefix) {
				res, _ := exec.Command("diskutil", "unmountDisk", "force", "/dev/"+id).CombinedOutput()
				logf("macOS mounted %s; unmountDisk: %s", strings.Fields(line)[0], strings.TrimSpace(string(res)))
				break
			}
		}
	}
}

func (t *darwinTarget) Close() error {
	select {
	case <-t.stop:
	default:
		close(t.stop)
	}
	t.auth.release()
	return t.File.Close()
}

// Sync does nothing: writes to /dev/rdisk are not buffered by the kernel, and the cache flush command
// (F_FULLFSYNC, DKIOCSYNCHRONIZE) makes some USB card readers reset, which takes the card away mid-check.
// The eject flushes the reader's cache.
func (t *darwinTarget) Sync() error { return nil }

// Reopen finds the card again after the reader dropped it (macOS makes a new disk for it, maybe under a new
// name) and opens it again. Only one external disk of the same name and size counts as the card.
func (t *darwinTarget) Reopen() error {
	t.File.Close() // the watcher keeps going; t.id moves to the card's new name below
	logf("reopen: waiting for %q (%d bytes)", t.name, t.size)
	d, err := waitForDisk(Disk{Name: t.name, Size: t.size})
	if err != nil {
		return err
	}
	out, _ := exec.Command("diskutil", "unmountDisk", "/dev/"+d.ID).CombinedOutput()
	logf("reopen: found %s; unmountDisk: %s", d.ID, strings.TrimSpace(string(out)))
	var f *os.File
	if t.auth != nil {
		f, err = t.auth.open("/dev/r" + d.ID)
	} else {
		f, err = os.OpenFile("/dev/r"+d.ID, os.O_RDWR, 0)
	}
	if err != nil {
		return err
	}
	t.mu.Lock()
	t.File, t.id = f, d.ID
	t.mu.Unlock()
	return nil
}

func (t *darwinTarget) Finish() error {
	t.Close()
	if out, err := exec.Command("diskutil", "eject", "/dev/"+t.disk()).CombinedOutput(); err != nil {
		return errors.New(strings.TrimSpace(string(out)))
	}
	return nil
}

// authorization is one admin password for the whole flash. Raw access to a removable disk has to go through
// Apple's authopen: macOS refuses it even to root otherwise (the removable-volumes privacy protection). Each
// authopen would ask again, and a card reader that drops the card means opening it again, perhaps under a new
// diskN. So `security authorize` asks once for every /dev/rdiskN, holds the rights while its stdout is open,
// and every authopen gets them through -extauth.
type authorization struct {
	cmd  *exec.Cmd
	pipe io.ReadCloser
	form []byte // an AuthorizationExternalForm
}

func authorize() (*authorization, error) {
	args := []string{"-q", "authorize", "-uew"}
	for i := 1; i <= 40; i++ {
		args = append(args, fmt.Sprintf("sys.openfile.readwrite./dev/rdisk%d", i))
	}
	cmd := exec.Command("security", args...)
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	form := make([]byte, 32)
	if _, err := io.ReadFull(pipe, form); err != nil {
		cmd.Wait()
		logf("security authorize: %v %s", err, strings.TrimSpace(stderr.String()))
		return nil, errors.New("the flasher needs your administrator password to write the card")
	}
	logf("authorized")
	return &authorization{cmd, pipe, form}, nil
}

func (a *authorization) release() {
	if a == nil || a.pipe == nil {
		return
	}
	a.pipe.Close()
	a.cmd.Process.Kill()
	a.cmd.Wait()
	a.pipe = nil
}

// open has authopen open path read-write and hand the descriptor back over a socket.
func (a *authorization) open(path string) (*os.File, error) {
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, err
	}
	child := os.NewFile(uintptr(fds[0]), "authopen")
	defer syscall.Close(fds[1])
	var stderr bytes.Buffer
	cmd := exec.Command("/usr/libexec/authopen", "-stdoutpipe", "-extauth", "-o", strconv.Itoa(syscall.O_RDWR), path)
	cmd.Stdin = bytes.NewReader(a.form)
	cmd.Stdout = child
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		child.Close()
		return nil, err
	}
	child.Close()
	buf := make([]byte, 64)
	oob := make([]byte, syscall.CmsgSpace(4))
	_, oobn, _, _, err := syscall.Recvmsg(fds[1], buf, oob, 0)
	werr := cmd.Wait()
	logf("authopen %s: exit %v, %d control bytes, %s", path, werr, oobn, strings.TrimSpace(stderr.String()))
	if err != nil || oobn == 0 {
		return nil, errors.New("macOS did not allow the flasher to open the card")
	}
	msgs, err := syscall.ParseSocketControlMessage(oob[:oobn])
	if err != nil || len(msgs) == 0 {
		return nil, errors.New("authopen sent no file")
	}
	got, err := syscall.ParseUnixRights(&msgs[0])
	if err != nil || len(got) == 0 {
		return nil, errors.New("authopen sent no file")
	}
	return os.NewFile(uintptr(got[0]), path), nil
}

// The writer runs in the UI process: authopen does the privileged part.
func inProcess(string) bool { return true }

func startWorker([]string) (func() error, error) { return nil, errors.New("not used on macOS") }

func boolKey(m map[string]any, key string) bool { v, _ := m[key].(bool); return v }
func str(m map[string]any, key string) string   { v, _ := m[key].(string); return v }
func num(m map[string]any, key string) int64    { v, _ := m[key].(int64); return v }
func array(v any) []any                         { a, _ := v.([]any); return a }

// parsePlist reads the XML property lists diskutil prints.
func parsePlist(data []byte) (any, error) {
	dec := xml.NewDecoder(bytes.NewReader(data))
	for {
		tok, err := dec.Token()
		if err != nil {
			return nil, err
		}
		if s, ok := tok.(xml.StartElement); ok && s.Name.Local == "plist" {
			return plistNext(dec)
		}
	}
}

func plistNext(dec *xml.Decoder) (any, error) {
	for {
		tok, err := dec.Token()
		if err != nil {
			return nil, err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			return plistValue(dec, t)
		case xml.EndElement:
			return nil, nil // end of an array or dict
		}
	}
}

func plistValue(dec *xml.Decoder, start xml.StartElement) (any, error) {
	switch start.Name.Local {
	case "dict":
		m := map[string]any{}
		for {
			k, err := plistNext(dec)
			if err != nil || k == nil {
				return m, err
			}
			key, _ := k.(string)
			v, err := plistNext(dec)
			if err != nil {
				return nil, err
			}
			m[key] = v
		}
	case "array":
		var a []any
		for {
			v, err := plistNext(dec)
			if err != nil || v == nil {
				return a, err
			}
			a = append(a, v)
		}
	case "true", "false":
		dec.Skip()
		return start.Name.Local == "true", nil
	}
	var text string
	if err := dec.DecodeElement(&text, &start); err != nil {
		return nil, err
	}
	switch start.Name.Local {
	case "integer":
		n, err := strconv.ParseInt(strings.TrimSpace(text), 10, 64)
		return n, err
	case "real":
		f, err := strconv.ParseFloat(strings.TrimSpace(text), 64)
		return f, err
	}
	return text, nil // string, key, data, date
}

// keepAwake stops the Mac from sleeping while a card is written or downloaded; the returned func ends it.
func keepAwake() func() {
	cmd := exec.Command("caffeinate", "-ims", "-w", strconv.Itoa(os.Getpid()))
	if cmd.Start() != nil {
		return func() {}
	}
	return func() { cmd.Process.Kill(); cmd.Wait() }
}
