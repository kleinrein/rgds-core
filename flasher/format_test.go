package main

import (
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestFormatLayouts(t *testing.T) {
	for _, c := range []struct {
		size    int64
		fs      string
		cluster int64
	}{
		{100 << 20, "FAT32", 1 << 10},
		{1999110144, "FAT32", 4 << 10}, // a 2 GB card
		{15931539456, "FAT32", 8 << 10},
		{31914983424, "FAT32", 16 << 10},
		{62537072640, "exFAT", 128 << 10},
		{255 * 1000 * 1000 * 1000, "exFAT", 128 << 10},
	} {
		l, err := layoutFor(c.size)
		if err != nil {
			t.Fatalf("%d: %v", c.size, err)
		}
		if l.FS != c.fs || l.Cluster != c.cluster {
			t.Errorf("%d: %s %d, want %s %d", c.size, l.FS, l.Cluster, c.fs, c.cluster)
		}
		regions, err := buildFormat(l, c.size)
		if err != nil {
			t.Fatalf("%d: %v", c.size, err)
		}
		end := int64(0)
		for _, r := range regions {
			if r.off < end || r.off%sector != 0 || len(r.data)%sector != 0 {
				t.Fatalf("%d: region at %d (%d bytes) overlaps or is unaligned", c.size, r.off, len(r.data))
			}
			end = r.off + int64(len(r.data))
		}
		if end > c.size {
			t.Fatalf("%d: writes past the card", c.size)
		}
		if l.start*sector%fmtAlign != 0 {
			t.Errorf("%d: partition not on 4 MiB", c.size)
		}
		mbr := regions[0].data
		if binary.LittleEndian.Uint32(mbr[454:]) != uint32(l.start) || mbr[510] != 0x55 {
			t.Errorf("%d: bad MBR", c.size)
		}
	}
	if _, err := layoutFor(32 << 20); err == nil {
		t.Error("a 32 MB card was formatted")
	}
}

// The up-case table must unpack to a mapping of all 65536 characters, with a for A and ß left as it is.
func TestUpcaseTable(t *testing.T) {
	b := upcaseTable()
	m := map[int]int{}
	idx, skip := 0, false
	for i := 0; i < len(b); i += 2 {
		v := int(binary.LittleEndian.Uint16(b[i:]))
		switch {
		case skip:
			idx += v
			skip = false
		case v == idx:
			idx++
		case v == 0xFFFF:
			skip = true
		default:
			m[idx] = v
			idx++
		}
	}
	if idx != 0x10000 {
		t.Fatalf("table covers %#x characters", idx)
	}
	if m['a'] != 'A' || m['z'] != 'Z' || m[0x00E9] != 0x00C9 {
		t.Errorf("a=%#x z=%#x é=%#x", m['a'], m['z'], m[0x00E9])
	}
	if _, ok := m['A']; ok {
		t.Error("A maps to something")
	}
	if len(b) > 8000 {
		t.Errorf("table is %d bytes, not compressed", len(b))
	}
}

// RGDS_FSCK=1 on a Mac: formats sparse images of a few sizes and checks them with macOS's own fsck_msdos and
// fsck_exfat.
func TestFormatFsck(t *testing.T) {
	if os.Getenv("RGDS_FSCK") == "" || runtime.GOOS != "darwin" {
		t.Skip("RGDS_FSCK=1 on macOS")
	}
	for _, size := range []int64{200 << 20, 1999110144, 31914983424, 62537072640} {
		l, err := layoutFor(size)
		if err != nil {
			t.Fatal(err)
		}
		regions, err := buildFormat(l, size)
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(t.TempDir(), "card.img")
		f, err := os.Create(path)
		if err != nil {
			t.Fatal(err)
		}
		f.Truncate(size)
		for _, r := range regions {
			if _, err := f.WriteAt(r.data, r.off); err != nil {
				t.Fatal(err)
			}
		}
		f.Close()
		out, err := exec.Command("hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", path).CombinedOutput()
		if err != nil {
			t.Fatalf("attach: %v %s", err, out)
		}
		dev := strings.Fields(string(out))[0]
		part := dev + "s1"
		tool := "fsck_msdos"
		if l.FS == "exFAT" {
			tool = "fsck_exfat"
		}
		res, ferr := exec.Command(tool, "-n", strings.Replace(part, "/dev/disk", "/dev/rdisk", 1)).CombinedOutput()
		info, _ := exec.Command("diskutil", "info", part).CombinedOutput()
		exec.Command("hdiutil", "detach", dev).Run()
		t.Logf("%d (%s): %s", size, l.FS, res)
		if ferr != nil {
			t.Errorf("%s on %d bytes: %v\n%s", tool, size, ferr, res)
		}
		if !strings.Contains(string(info), formatLabel) {
			t.Errorf("%d: diskutil doesn't see the label:\n%s", size, info)
		}
	}
}

// RGDS_FSCK=1 on a Mac: macOS mounts the formatted card, takes a file, and the file system still checks clean.
func TestFormatMounts(t *testing.T) {
	if os.Getenv("RGDS_FSCK") == "" || runtime.GOOS != "darwin" {
		t.Skip("RGDS_FSCK=1 on macOS")
	}
	for _, size := range []int64{1999110144, 62537072640} {
		l, _ := layoutFor(size)
		regions, _ := buildFormat(l, size)
		path := filepath.Join(t.TempDir(), "card.img")
		f, _ := os.Create(path)
		f.Truncate(size)
		for _, r := range regions {
			f.WriteAt(r.data, r.off)
		}
		f.Close()
		mnt := t.TempDir()
		out, err := exec.Command("hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-mountpoint", mnt, path).CombinedOutput()
		if err != nil {
			t.Fatalf("attach: %v %s", err, out)
		}
		dev := strings.Fields(string(out))[0]
		werr := os.WriteFile(filepath.Join(mnt, "hello.txt"), []byte("hello"), 0o644)
		df, _ := exec.Command("df", "-k", mnt).CombinedOutput()
		exec.Command("diskutil", "unmount", mnt).Run()
		tool := map[string]string{"FAT32": "fsck_msdos", "exFAT": "fsck_exfat"}[l.FS]
		res, ferr := exec.Command(tool, "-n", strings.Replace(dev, "/dev/disk", "/dev/rdisk", 1)+"s1").CombinedOutput()
		exec.Command("hdiutil", "detach", dev).Run()
		t.Logf("%s: %s", l.FS, df)
		if werr != nil || ferr != nil {
			t.Errorf("%s: write %v, %s %v\n%s", l.FS, werr, tool, ferr, res)
		}
	}
}
