//go:build !darwin || !cgo

package main

// Windows and Linux (and a Mac build without cgo) show the page in a browser window (window.go).
func nativeWindow(string, *server) bool { return false }
