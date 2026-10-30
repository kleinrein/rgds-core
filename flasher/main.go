// RG DS Flasher: downloads the latest RG DS OS card image and writes it to a microSD card, byte for byte.
//
// The binary serves its own UI on 127.0.0.1 and opens it in an app window (Chrome or Edge in app mode, or the
// default browser). Writing needs admin rights. macOS gets the card from Apple's authopen after one password
// prompt; Windows (UAC) and Linux (pkexec) start this binary again elevated with --worker, which reports back to
// the UI over the same local server.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
)

var version = "dev"

const (
	repo     = "kleinrein/rgds-core"
	maxBytes = 256 * 1000 * 1000 * 1000 // like tools/own_os_card.py: no card is bigger, anything bigger is a drive
)

func main() {
	worker := flag.Bool("worker", false, "internal: the elevated writer started by the UI")
	report := flag.String("report", "", "internal: where the worker reports progress")
	disk := flag.String("disk", "", "internal: the disk the worker writes")
	image := flag.String("image", "", "flash this .img or .img.gz instead of the latest release")
	logTo := flag.String("log", "", "internal: the UI's log file, so the worker writes to it too")
	list := flag.Bool("list", false, "print the cards the flasher would offer, then exit")
	noWindow := flag.Bool("no-window", false, "print the address instead of opening a window")
	showVersion := flag.Bool("version", false, "print the version")
	flag.Parse()

	if *worker {
		openLog("worker", *logTo)
	} else if !*showVersion && !*list {
		openLog("ui", "")
	}
	switch {
	case *showVersion:
		fmt.Println(version)
	case *worker:
		os.Exit(runWorker(*report, *disk, *image))
	case *list:
		disks, err := listDisks()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		if disks == nil {
			disks = []Disk{}
		}
		out, _ := json.MarshalIndent(disks, "", "  ")
		fmt.Println(string(out))
	default:
		local := ""
		if *image != "" {
			path, err := filepath.Abs(*image)
			if err != nil || !isFile(path) {
				fmt.Fprintf(os.Stderr, "%s: no such image\n", *image)
				os.Exit(1)
			}
			local = path
		}
		if err := serve(local, !*noWindow); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
}

func isFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}
