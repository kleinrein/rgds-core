package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

func readSys(path string) string {
	b, _ := os.ReadFile(path)
	return strings.TrimSpace(string(b))
}

// mountsOf maps each disk (sdb, mmcblk0) to its mounted partitions' mount points, swap included.
func mountsOf() map[string][]string {
	out := map[string][]string{}
	add := func(dev, point string) {
		if !strings.HasPrefix(dev, "/dev/") {
			return
		}
		dev, _ = filepath.EvalSymlinks(dev)
		name := filepath.Base(dev)
		if parent := filepath.Base(filepath.Dir(sysPath(name))); parent != "block" && parent != "." {
			name = parent // a partition: its parent directory in /sys is the disk
		}
		out[name] = append(out[name], point)
	}
	for _, file := range []string{"/proc/mounts", "/proc/swaps"} {
		f, err := os.Open(file)
		if err != nil {
			continue
		}
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			fields := strings.Fields(sc.Text())
			if len(fields) >= 2 {
				point := fields[1]
				if file == "/proc/swaps" {
					point = "[swap]"
				}
				add(fields[0], strings.ReplaceAll(point, `\040`, " "))
			}
		}
		f.Close()
	}
	return out
}

func sysPath(name string) string {
	p, err := filepath.EvalSymlinks("/sys/class/block/" + name)
	if err != nil {
		return ""
	}
	return p
}

func listDisks() ([]Disk, error) {
	entries, err := os.ReadDir("/sys/block")
	if err != nil {
		return nil, err
	}
	mounts := mountsOf()
	var disks []Disk
	for _, e := range entries {
		name := e.Name()
		if !strings.HasPrefix(name, "sd") && !strings.HasPrefix(name, "mmcblk") {
			continue
		}
		dir := "/sys/block/" + name
		real := sysPath(name)
		sd := strings.HasPrefix(name, "mmcblk") && readSys(dir+"/device/type") == "SD"
		if readSys(dir+"/removable") != "1" && !strings.Contains(real, "/usb") && !sd {
			continue
		}
		// Cards are mounted under /media, /run/media or /mnt. Anything mounted elsewhere (/, swap, /home, a live
		// USB's /run/live/medium or /cdrom) belongs to the running system, so the whole disk is left out.
		system := false
		for _, m := range mounts[name] {
			if !strings.HasPrefix(m, "/media/") && !strings.HasPrefix(m, "/run/media/") && !strings.HasPrefix(m, "/mnt/") {
				system = true
			}
		}
		if system {
			continue
		}
		sectors, _ := strconv.ParseInt(readSys(dir+"/size"), 10, 64)
		label := strings.TrimSpace(readSys(dir+"/device/vendor") + " " + readSys(dir+"/device/model"))
		if label == "" {
			label = "SD card"
		}
		d := Disk{ID: name, Path: "/dev/" + name, Name: label, Size: sectors * 512, mounts: mounts[name]}
		if readSys(dir+"/ro") == "1" {
			d.Problem = "Write-protected: slide the lock switch on the SD adapter up"
		}
		if labels, err := os.ReadDir("/dev/disk/by-label"); err == nil {
			for _, l := range labels {
				target, _ := filepath.EvalSymlinks("/dev/disk/by-label/" + l.Name())
				if strings.HasPrefix(filepath.Base(target), name) {
					d.Volumes = append(d.Volumes, strings.ReplaceAll(l.Name(), `\x20`, " "))
				}
			}
		}
		disks = append(disks, d)
	}
	return finishDisks(disks), nil
}

type linuxTarget struct {
	*os.File
	dev  string
	disk Disk
}

func openTarget(d Disk) (target, error) {
	for _, m := range mountsOf()[d.ID] {
		if out, err := exec.Command("umount", m).CombinedOutput(); err != nil {
			return nil, fmt.Errorf("can't unmount %s: %s", m, strings.TrimSpace(string(out)))
		}
	}
	// O_EXCL refuses a disk that is still mounted; O_DIRECT makes the read-back come from the card, not the cache.
	f, err := os.OpenFile(d.Path, os.O_RDWR|syscall.O_EXCL|syscall.O_DIRECT, 0)
	if err != nil {
		return nil, err
	}
	return &linuxTarget{f, d.Path, d}, nil
}

// Reopen opens the card again after the reader dropped it; it may come back under another name.
func (t *linuxTarget) Reopen() error {
	t.File.Close()
	d, err := waitForDisk(t.disk)
	if err != nil {
		return err
	}
	nt, err := openTarget(d)
	if err != nil {
		return err
	}
	*t = *nt.(*linuxTarget)
	return nil
}

// keepAwake holds a systemd sleep inhibitor while a card is written. It is tied to cat's stdin, so it ends when the
// returned func closes the pipe or when the flasher dies; without systemd it does nothing.
func keepAwake() func() {
	cmd := exec.Command("systemd-inhibit", "--what=sleep:idle", "--who=RG DS Flasher", "--why=Writing a card", "--mode=block", "cat")
	in, err := cmd.StdinPipe()
	if err != nil || cmd.Start() != nil {
		return func() {}
	}
	return func() { in.Close(); cmd.Wait() }
}

func (t *linuxTarget) Finish() error {
	t.File.Close()
	exec.Command("eject", t.dev).Run() // optional: most readers stay usable without it
	return nil
}

func inProcess(string) bool { return os.Geteuid() == 0 }

// startWorker runs the writer as root through polkit; without pkexec the flasher has to be started with sudo.
func startWorker(args []string) (func() error, error) {
	pkexec, err := exec.LookPath("pkexec")
	if err != nil {
		return nil, errors.New("writing the card needs root: start the flasher with sudo")
	}
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	cmd := exec.Command(pkexec, append([]string{exe}, args...)...)
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return func() error {
		err := cmd.Wait()
		if exit, ok := err.(*exec.ExitError); ok && (exit.ExitCode() == 126 || exit.ExitCode() == 127) {
			return errors.New("the flasher needs your password to write the card")
		}
		return err
	}, nil
}
