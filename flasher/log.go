package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"time"
)

// The flash log: what happened on someone's computer, for when a card doesn't come out right. It lives next to
// the downloaded image and the UI points to it after a failure.
var (
	logMu   sync.Mutex
	logFile *os.File
	logRole = "ui"
)

func logPath() string { return filepath.Join(cacheDir(), "flasher.log") }

func openLog(role, path string) {
	logRole = role
	if path == "" {
		path = logPath()
	}
	// Only the UI rotates: a worker running as root (pkexec) would leave a new log the user can't write to.
	if info, err := os.Stat(path); err == nil && info.Size() > 2<<20 && role == "ui" {
		os.Rename(path, path+".old")
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err == nil {
		logFile = f
	}
	logf("RG DS Flasher %s on %s/%s, %v", version, runtime.GOOS, runtime.GOARCH, os.Args[1:])
}

func logf(format string, args ...any) {
	line := fmt.Sprintf("%s [%s] %s\n", time.Now().Format("2006-01-02 15:04:05.000"), logRole, fmt.Sprintf(format, args...))
	logMu.Lock()
	defer logMu.Unlock()
	if logFile != nil {
		logFile.WriteString(line)
	}
	os.Stderr.WriteString(line)
}

// describeErr adds the raw error number, which says more than the message on some systems.
func describeErr(err error) string {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return fmt.Sprintf("%v (errno %d)", err, uintptr(errno))
	}
	return fmt.Sprint(err)
}
