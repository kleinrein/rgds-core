//go:build darwin && cgo

package main

// The Mac window: a native window with a WKWebView, the same page the browser window shows, in this process. No
// browser is started, so there is no browser profile in the cache (Chrome's was 190 MB) and nothing to close
// separately: the window ends with the program.

/*
#cgo CFLAGS: -x objective-c -fobjc-arc -mmacosx-version-min=11.0
#cgo LDFLAGS: -framework Cocoa -framework WebKit -mmacosx-version-min=11.0
#include <stdlib.h>
void flasherRun(const char *url);
void flasherTerminate(void);
*/
import "C"

import (
	"os"
	"runtime"
	"unsafe"
)

func init() { runtime.LockOSThread() } // Cocoa wants its run loop on the main thread, where main starts

var native *server

// nativeWindow shows url and runs the window until the program ends; it never returns.
func nativeWindow(url string, s *server) bool {
	native = s
	clearProfiles()                               // the browser profile older versions kept (190 MB)
	if os.Getenv("RGDS_FLASHER_BROWSER") == "1" { // for testing the browser window on a Mac
		return false
	}
	go func() {
		<-quit
		s.shutdown()
		C.flasherTerminate()
	}()
	curl := C.CString(url)
	defer C.free(unsafe.Pointer(curl))
	C.flasherRun(curl) // the run loop; flasherTerminate ends the process
	return true
}

//export flasherBusy
func flasherBusy() C.int {
	if native == nil {
		return 0
	}
	native.mu.Lock()
	defer native.mu.Unlock()
	if native.st.Busy {
		return 1
	}
	return 0
}

//export flasherWillQuit
func flasherWillQuit() {
	if native != nil {
		native.shutdown()
	}
}
