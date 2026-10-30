package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var (
	kernel32                 = syscall.NewLazyDLL("kernel32.dll")
	procFindFirstVolume      = kernel32.NewProc("FindFirstVolumeW")
	procFindNextVolume       = kernel32.NewProc("FindNextVolumeW")
	procFindVolumeClose      = kernel32.NewProc("FindVolumeClose")
	procGetVolumePathNames   = kernel32.NewProc("GetVolumePathNamesForVolumeNameW")
	procGetVolumeInformation = kernel32.NewProc("GetVolumeInformationW")
	procSetErrorMode         = kernel32.NewProc("SetErrorMode")
	procFlushFileBuffers     = kernel32.NewProc("FlushFileBuffers")
	shell32                  = syscall.NewLazyDLL("shell32.dll")
	procShellExecuteEx       = shell32.NewProc("ShellExecuteExW")
	procIsUserAnAdmin        = shell32.NewProc("IsUserAnAdmin")
)

const (
	ioctlStorageQueryProperty   = 0x002D1400
	ioctlStorageEjectMedia      = 0x002D4808
	ioctlDiskGetDriveGeometryEx = 0x000700A0
	ioctlDiskIsWritable         = 0x00070024
	ioctlVolumeGetDiskExtents   = 0x00560000
	fsctlLockVolume             = 0x00090018
	fsctlUnlockVolume           = 0x0009001C
	fsctlDismountVolume         = 0x00090020

	busUSB = 7
	busSD  = 12
	busMMC = 13

	fileFlagNoBuffering  = 0x20000000
	fileFlagWriteThrough = 0x80000000
	shareRW              = syscall.FILE_SHARE_READ | syscall.FILE_SHARE_WRITE
)

func init() {
	// No "There is no disk in the drive" dialogs while empty card readers are probed.
	const semFailCriticalErrors, semNoOpenFileErrorBox = 0x0001, 0x8000
	procSetErrorMode.Call(semFailCriticalErrors | semNoOpenFileErrorBox)
}

type volume struct {
	path    string // \\?\Volume{...} without the trailing backslash
	letters []string
	label   string
	disks   []uint32
}

func ioctl(h syscall.Handle, code uint32, in []byte, out []byte) (uint32, error) {
	var n uint32
	var inp, outp *byte
	if len(in) > 0 {
		inp = &in[0]
	}
	if len(out) > 0 {
		outp = &out[0]
	}
	err := syscall.DeviceIoControl(h, code, inp, uint32(len(in)), outp, uint32(len(out)), &n, nil)
	return n, err
}

func open(path string, access uint32, flags uint32) (syscall.Handle, error) {
	p, _ := syscall.UTF16PtrFromString(path)
	return syscall.CreateFile(p, access, shareRW, nil, syscall.OPEN_EXISTING, flags, 0)
}

func volumes() []volume {
	buf := make([]uint16, 512)
	h, _, _ := procFindFirstVolume.Call(uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if syscall.Handle(h) == syscall.InvalidHandle {
		return nil
	}
	defer procFindVolumeClose.Call(h)
	var out []volume
	for {
		name := syscall.UTF16ToString(buf)
		v := volume{path: strings.TrimSuffix(name, `\`)}
		root, _ := syscall.UTF16PtrFromString(name)
		paths := make([]uint16, 1024)
		var n uint32
		if r, _, _ := procGetVolumePathNames.Call(uintptr(unsafe.Pointer(root)), uintptr(unsafe.Pointer(&paths[0])), uintptr(len(paths)), uintptr(unsafe.Pointer(&n))); r != 0 {
			v.letters = multiString(paths[:n])
		}
		label := make([]uint16, 261)
		if r, _, _ := procGetVolumeInformation.Call(uintptr(unsafe.Pointer(root)), uintptr(unsafe.Pointer(&label[0])), uintptr(len(label)), 0, 0, 0, 0, 0); r != 0 {
			v.label = syscall.UTF16ToString(label)
		}
		if h, err := open(v.path, 0, 0); err == nil {
			ext := make([]byte, 8+24*32)
			if _, err := ioctl(h, ioctlVolumeGetDiskExtents, nil, ext); err == nil {
				count := binary.LittleEndian.Uint32(ext)
				for i := uint32(0); i < count && 8+24*(i+1) <= uint32(len(ext)); i++ {
					v.disks = append(v.disks, binary.LittleEndian.Uint32(ext[8+24*i:]))
				}
			}
			syscall.CloseHandle(h)
		}
		out = append(out, v)
		if r, _, _ := procFindNextVolume.Call(h, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf))); r == 0 {
			return out
		}
	}
}

func multiString(s []uint16) []string {
	var out []string
	start := 0
	for i, c := range s {
		if c == 0 {
			if i > start {
				out = append(out, syscall.UTF16ToString(s[start:i]))
			}
			start = i + 1
		}
	}
	return out
}

func cString(b []byte, off uint32) string {
	if off == 0 || int(off) >= len(b) {
		return ""
	}
	end := int(off)
	for end < len(b) && b[end] != 0 {
		end++
	}
	return strings.TrimSpace(string(b[off:end]))
}

func listDisks() ([]Disk, error) {
	vols := volumes()
	system := map[uint32]bool{}
	sysDrive := strings.ToUpper(os.Getenv("SystemDrive")) + `\`
	if sysDrive == `\` {
		sysDrive = `C:\`
	}
	for _, v := range vols {
		for _, l := range v.letters {
			if strings.EqualFold(l, sysDrive) {
				for _, d := range v.disks {
					system[d] = true
				}
			}
		}
	}
	var disks []Disk
	for i := uint32(0); i < 64; i++ {
		if system[i] {
			continue
		}
		h, err := open(fmt.Sprintf(`\\.\PhysicalDrive%d`, i), 0, 0)
		if err != nil {
			continue
		}
		desc := make([]byte, 1024)
		query := make([]byte, 12) // STORAGE_PROPERTY_QUERY: StorageDeviceProperty, PropertyStandardQuery
		_, qerr := ioctl(h, ioctlStorageQueryProperty, query, desc)
		geo := make([]byte, 256)
		_, gerr := ioctl(h, ioctlDiskGetDriveGeometryEx, nil, geo)
		_, werr := ioctl(h, ioctlDiskIsWritable, nil, nil)
		syscall.CloseHandle(h)
		if qerr != nil {
			continue
		}
		removable := desc[10] != 0
		bus := binary.LittleEndian.Uint32(desc[28:])
		if !(bus == busSD || bus == busMMC || (bus == busUSB && removable)) {
			continue // internal disks, USB hard drives and virtual disks are never offered
		}
		name := strings.TrimSpace(strings.Trim(cString(desc, binary.LittleEndian.Uint32(desc[12:])), "-") + " " +
			cString(desc, binary.LittleEndian.Uint32(desc[16:])))
		if name == "" {
			name = "SD card"
		}
		d := Disk{ID: strconv.Itoa(int(i)), Path: fmt.Sprintf("Disk %d", i), Name: name}
		if gerr == nil {
			d.Size = int64(binary.LittleEndian.Uint64(geo[24:]))
		}
		if werr == syscall.Errno(19) { // ERROR_WRITE_PROTECT
			d.Problem = "Write-protected: slide the lock switch on the SD adapter up"
		}
		for _, v := range vols {
			if contains(v.disks, i) {
				if v.label != "" {
					d.Volumes = append(d.Volumes, v.label)
				}
				for _, l := range v.letters {
					d.mounts = append(d.mounts, l)
					if v.label == "" {
						d.Volumes = append(d.Volumes, strings.TrimSuffix(l, `\`))
					}
				}
			}
		}
		disks = append(disks, d)
	}
	return finishDisks(disks), nil
}

func contains(list []uint32, n uint32) bool {
	for _, v := range list {
		if v == n {
			return true
		}
	}
	return false
}

// winTarget is \\.\PhysicalDriveN, unbuffered and written through, with every volume on it locked and dismounted
// for as long as it is open: Windows can't mount, scan or offer to format any of it until the card is ejected.
type winTarget struct {
	h      syscall.Handle
	locked []syscall.Handle
	disk   Disk
}

func openTarget(d Disk) (target, error) {
	n, err := strconv.Atoi(d.ID)
	if err != nil {
		return nil, err
	}
	t := &winTarget{h: syscall.InvalidHandle, disk: d}
	for _, v := range volumes() {
		if !contains(v.disks, uint32(n)) {
			continue
		}
		h, err := open(v.path, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0)
		if err != nil {
			t.Close()
			return nil, fmt.Errorf("can't take over %s: %w", strings.Join(append(v.letters, v.label), " "), err)
		}
		t.locked = append(t.locked, h)
		locked := false
		for try := 0; try < 20 && !locked; try++ { // Explorer lets go of a fresh card after a moment
			if _, err := ioctl(h, fsctlLockVolume, nil, nil); err == nil {
				locked = true
			} else {
				time.Sleep(250 * time.Millisecond)
			}
		}
		if _, err := ioctl(h, fsctlDismountVolume, nil, nil); err != nil {
			t.Close()
			return nil, fmt.Errorf("can't dismount %s; close any windows showing the card: %w", strings.Join(v.letters, " "), err)
		}
	}
	t.h, err = open(fmt.Sprintf(`\\.\PhysicalDrive%d`, n), syscall.GENERIC_READ|syscall.GENERIC_WRITE, fileFlagNoBuffering|fileFlagWriteThrough)
	if err != nil {
		t.Close()
		return nil, fmt.Errorf("can't open %s for writing: %w", d.Path, err)
	}
	return t, nil
}

// Reopen takes the card over again after its reader dropped it; Windows may give it another disk number.
func (t *winTarget) Reopen() error {
	t.Close()
	d, err := waitForDisk(t.disk)
	if err != nil {
		return err
	}
	nt, err := openTarget(d)
	if err != nil {
		return err
	}
	*t = *nt.(*winTarget)
	return nil
}

func (t *winTarget) WriteAt(p []byte, off int64) (int, error) {
	var done uint32
	o := syscall.Overlapped{Offset: uint32(off), OffsetHigh: uint32(off >> 32)}
	err := syscall.WriteFile(t.h, p, &done, &o)
	if err == syscall.Errno(19) {
		return 0, errWriteProtected
	}
	if err == nil && int(done) != len(p) {
		err = errors.New("short write")
	}
	return int(done), err
}

func (t *winTarget) ReadAt(p []byte, off int64) (int, error) {
	var done uint32
	o := syscall.Overlapped{Offset: uint32(off), OffsetHigh: uint32(off >> 32)}
	err := syscall.ReadFile(t.h, p, &done, &o)
	if err == nil && int(done) != len(p) {
		err = errors.New("short read")
	}
	return int(done), err
}

func (t *winTarget) Sync() error {
	if r, _, err := procFlushFileBuffers.Call(uintptr(t.h)); r == 0 {
		return err
	}
	return nil
}

// Finish ejects the card while its volumes are still locked, so Explorer never gets to show the new partitions
// (and never asks to format the ones it can't read) before the card is in the handheld.
func (t *winTarget) Finish() error {
	ioctl(t.h, ioctlStorageEjectMedia, nil, nil) // built-in readers often can't; the card is flushed either way
	return t.Close()
}

func (t *winTarget) Close() error {
	if t.h != syscall.InvalidHandle {
		syscall.CloseHandle(t.h)
		t.h = syscall.InvalidHandle
	}
	for _, h := range t.locked {
		ioctl(h, fsctlUnlockVolume, nil, nil)
		syscall.CloseHandle(h)
	}
	t.locked = nil
	return nil
}

func isAdmin() bool {
	r, _, _ := procIsUserAnAdmin.Call()
	return r != 0
}

func inProcess(string) bool { return isAdmin() }

type shellExecuteInfo struct {
	size       uint32
	mask       uint32
	hwnd       uintptr
	verb       *uint16
	file       *uint16
	parameters *uint16
	directory  *uint16
	show       int32
	instApp    uintptr
	idList     uintptr
	class      *uint16
	keyClass   uintptr
	hotKey     uint32
	icon       uintptr
	process    syscall.Handle
}

// startWorker starts this program again as administrator (the UAC prompt) with the worker arguments.
func startWorker(args []string) (func() error, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	quoted := make([]string, len(args))
	for i, a := range args {
		quoted[i] = syscall.EscapeArg(a)
	}
	const seeMaskNoCloseProcess, seeMaskNoAsync, swHide = 0x40, 0x100, 0
	info := shellExecuteInfo{mask: seeMaskNoCloseProcess | seeMaskNoAsync, show: swHide}
	info.size = uint32(unsafe.Sizeof(info))
	info.verb, _ = syscall.UTF16PtrFromString("runas")
	info.file, _ = syscall.UTF16PtrFromString(exe)
	info.parameters, _ = syscall.UTF16PtrFromString(strings.Join(quoted, " "))
	if r, _, err := procShellExecuteEx.Call(uintptr(unsafe.Pointer(&info))); r == 0 {
		if errno, ok := err.(syscall.Errno); ok && errno == 1223 { // ERROR_CANCELLED
			return nil, errors.New("Windows needs your OK to write the card: choose Yes when it asks")
		}
		return nil, fmt.Errorf("can't start the writer: %w", err)
	}
	return func() error {
		defer syscall.CloseHandle(info.process)
		syscall.WaitForSingleObject(info.process, syscall.INFINITE)
		var code uint32
		syscall.GetExitCodeProcess(info.process, &code)
		if code != 0 {
			return fmt.Errorf("the writer stopped (exit %d)", code)
		}
		return nil
	}, nil
}

// keepAwake keeps Windows from sleeping until the returned func is called. The setting belongs to a thread, so
// one goroutine locked to its thread holds it.
func keepAwake() func() {
	const esContinuous, esSystemRequired = 0x80000000, 0x00000001
	set := kernel32.NewProc("SetThreadExecutionState")
	stop := make(chan struct{})
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		set.Call(esContinuous | esSystemRequired)
		<-stop
		set.Call(esContinuous)
	}()
	return func() { close(stop) }
}
