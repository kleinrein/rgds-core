package main

import (
	"bytes"
	"compress/gzip"
	"errors"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
)

type fileTarget struct{ *os.File }

func (fileTarget) Finish() error { return nil }

// corruptTarget flips one byte on the way back, like a fake card.
type corruptTarget struct{ fileTarget }

func (t corruptTarget) ReadAt(p []byte, off int64) (int, error) {
	n, err := t.File.ReadAt(p, off)
	if off == 2*chunk && n > 0 {
		p[7] ^= 1
	}
	return n, err
}

func testImage(t *testing.T, size int) (string, []byte) {
	raw := make([]byte, size)
	rand.New(rand.NewSource(1)).Read(raw[:size/3]) // the rest stays zero: zero blocks must be written too
	raw[size-1] = 0x5a
	var gz bytes.Buffer
	z := gzip.NewWriter(&gz)
	z.Write(raw)
	z.Close()
	path := filepath.Join(t.TempDir(), "test.img.gz")
	os.WriteFile(path, gz.Bytes(), 0o644)
	return path, raw
}

func TestWriteImage(t *testing.T) {
	size := 3*chunk + 12345 // a short last block, padded to a whole sector
	path, raw := testImage(t, size)
	if got, _ := unpackedSize(path); got != int64(size) {
		t.Fatalf("unpackedSize %d, want %d", got, size)
	}
	disk := filepath.Join(t.TempDir(), "disk")
	junk := bytes.Repeat([]byte{0xff}, 5*chunk) // old card contents under the zero blocks
	os.WriteFile(disk, junk, 0o644)
	f, _ := os.OpenFile(disk, os.O_RDWR, 0)
	defer f.Close()
	var phases []string
	n, err := writeImage(path, fileTarget{f}, int64(size), func(phase string, done, total int64) {
		if len(phases) == 0 || phases[len(phases)-1] != phase {
			phases = append(phases, phase)
		}
	})
	if err != nil {
		t.Fatal(err)
	}
	if n != int64(size) {
		t.Fatalf("wrote %d, want %d", n, size)
	}
	got, _ := os.ReadFile(disk)
	if !bytes.Equal(got[:size], raw) {
		t.Fatal("the disk does not hold the image")
	}
	if pad := got[size:roundUp(int64(size))]; !bytes.Equal(pad, make([]byte, len(pad))) {
		t.Fatal("the last sector is not padded with zeros")
	}
	if want := []string{"write", "verify"}; len(phases) != 2 || phases[0] != want[0] || phases[1] != want[1] {
		t.Fatalf("phases %v", phases)
	}
}

func TestVerifyCatchesBadCard(t *testing.T) {
	path, _ := testImage(t, 4*chunk)
	disk := filepath.Join(t.TempDir(), "disk")
	f, _ := os.Create(disk)
	defer f.Close()
	_, err := writeImage(path, corruptTarget{fileTarget{f}}, 4*chunk, func(string, int64, int64) {})
	if err == nil {
		t.Fatal("a card that reads back different passed the check")
	}
}

func TestDamagedImage(t *testing.T) {
	path, _ := testImage(t, 2*chunk)
	data, _ := os.ReadFile(path)
	data[len(data)-6] ^= 0xff // the CRC
	os.WriteFile(path, data, 0o644)
	f, _ := os.Create(filepath.Join(t.TempDir(), "disk"))
	defer f.Close()
	if _, err := writeImage(path, fileTarget{f}, 2*chunk, func(string, int64, int64) {}); err == nil {
		t.Fatal("a damaged gzip passed")
	}
}

func TestParseSum(t *testing.T) {
	sum := "961cc82adeb62c46b417ec49d545cd2dd5db59e06f4db9d29c83ffeda38a7625"
	if got := parseSum(sum + "  RG-DS-OS-0.8.0.img.gz\n"); got != sum {
		t.Fatal(got)
	}
	if parseSum("<html>Not Found</html>") != "" {
		t.Fatal("accepted html")
	}
}

func TestAssetNames(t *testing.T) {
	for name, want := range map[string]string{"RG-DS-OS-0.8.0.img.gz": "rgds", "RG-DS-OS-Plus-0.8.0.img.gz": "plus"} {
		m := assetName.FindStringSubmatch(name)
		if m == nil || model(m[1]) != want || m[2] != "0.8.0" {
			t.Fatalf("%s: %v", name, m)
		}
	}
	if assetName.MatchString("RG-DS-OS-0.8.0-0dac2b9bab68-os4f6d2207.zip") {
		t.Fatal("matched the update zip")
	}
}

// droppingTarget fails its first read, like a card reader that resets after the write, until it is reopened.
type droppingTarget struct {
	fileTarget
	dropped, reopened *bool
}

func (t droppingTarget) ReadAt(p []byte, off int64) (int, error) {
	if !*t.dropped && off > 0 {
		*t.dropped = true
		return 0, errors.New("device not configured")
	}
	return t.File.ReadAt(p, off)
}

func (t droppingTarget) Reopen() error { *t.reopened = true; return nil }

func TestVerifyAfterReaderReset(t *testing.T) {
	path, _ := testImage(t, 3*chunk)
	f, _ := os.Create(filepath.Join(t.TempDir(), "disk"))
	defer f.Close()
	var dropped, reopened bool
	var phases []string
	_, err := writeImage(path, droppingTarget{fileTarget{f}, &dropped, &reopened}, 3*chunk, func(phase string, done, total int64) {
		if len(phases) == 0 || phases[len(phases)-1] != phase {
			phases = append(phases, phase)
		}
	})
	if err != nil || !reopened {
		t.Fatalf("err %v, reopened %v", err, reopened)
	}
	if p := fmt.Sprint(phases); p != "[write reopen verify]" && p != "[write reopen write verify]" {
		t.Fatalf("phases %v", phases)
	}
}

// headWatch records whether the partition table area was still blank whenever anything else was written.
type headWatch struct {
	fileTarget
	early *bool
}

func (t headWatch) WriteAt(p []byte, off int64) (int, error) {
	if off > 0 {
		head := make([]byte, 512)
		t.File.ReadAt(head, 0)
		if !bytes.Equal(head, make([]byte, 512)) {
			*t.early = true
		}
	}
	return t.File.WriteAt(p, off)
}

func TestPartitionTableLast(t *testing.T) {
	path, raw := testImage(t, 5*chunk)
	disk := filepath.Join(t.TempDir(), "disk")
	os.WriteFile(disk, bytes.Repeat([]byte{0xee}, 6*chunk), 0o644) // an old partition table to wipe first
	f, _ := os.OpenFile(disk, os.O_RDWR, 0)
	defer f.Close()
	var early bool
	if _, err := writeImage(path, headWatch{fileTarget{f}, &early}, 5*chunk, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	if early {
		t.Fatal("the card showed a partition table while the rest was still being written")
	}
	if got, _ := os.ReadFile(disk); !bytes.Equal(got[:len(raw)], raw) {
		t.Fatal("the disk does not hold the image")
	}
}

// mountingTarget drops the card right after the partition table went on, and "the OS" mounts it and writes.
type mountingTarget struct {
	fileTarget
	reads *int
}

func (t mountingTarget) ReadAt(p []byte, off int64) (int, error) {
	*t.reads++
	if *t.reads == 9 { // the read right after the partition table went on
		t.File.WriteAt([]byte("fseventsd"), chunk+100)
		return 0, errors.New("device not configured")
	}
	return t.File.ReadAt(p, off)
}

func (mountingTarget) Reopen() error { return nil }

func TestRepairsWhatTheOSWrote(t *testing.T) {
	path, raw := testImage(t, 5*chunk)
	disk := filepath.Join(t.TempDir(), "disk")
	f, _ := os.Create(disk)
	defer f.Close()
	var reads int
	if _, err := writeImage(path, mountingTarget{fileTarget{f}, &reads}, 5*chunk, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(disk); !bytes.Equal(got[:len(raw)], raw) {
		t.Fatal("the changed chunk was not rewritten")
	}
}

func TestInlineCheckCatchesBadChunk(t *testing.T) {
	path, _ := testImage(t, 6*chunk)
	f, _ := os.Create(filepath.Join(t.TempDir(), "disk"))
	defer f.Close()
	var phases []string
	_, err := writeImage(path, corruptTarget{fileTarget{f}}, 6*chunk, func(phase string, done, total int64) {
		if len(phases) == 0 || phases[len(phases)-1] != phase {
			phases = append(phases, phase)
		}
	})
	if err == nil || fmt.Sprint(phases) != "[write]" {
		t.Fatalf("err %v, phases %v: a bad chunk must stop the write", err, phases)
	}
}
