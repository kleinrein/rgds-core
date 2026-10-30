package main

import (
	"bufio"
	"compress/gzip"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unsafe"
)

// Disk is a card the UI may offer. Only removable, external disks are ever listed; Problem says why one of them
// still can't be written (too big for a card, too small for the image).
type Disk struct {
	ID       string   `json:"id"`   // disk4, 2 (PhysicalDrive2), sdb
	Path     string   `json:"path"` // what the user knows it as: /dev/disk4, Disk 2, /dev/sdb
	Name     string   `json:"name"`
	Size     int64    `json:"size"`
	Volumes  []string `json:"volumes"`
	Problem  string   `json:"problem,omitempty"`
	Warnings []string `json:"warnings,omitempty"`
	mounts   []string
}

// reopener is a target that can find its card again after the reader dropped it.
type reopener interface{ Reopen() error }

// target is a whole disk opened raw for reading and writing.
type target interface {
	io.WriterAt
	io.ReaderAt
	Sync() error
	// Finish closes the disk and ejects it.
	Finish() error
	Close() error
}

const (
	chunk = 4 << 20
	align = 4096 // offsets and lengths stay multiples of this: raw devices take whole sectors only
)

// finishDisks adds the checks every platform shares.
func finishDisks(disks []Disk) []Disk {
	for i := range disks {
		d := &disks[i]
		if d.Size > maxBytes {
			d.Problem = "Larger than 256 GB, so not a card"
		} else if d.Size <= 0 {
			d.Problem = "No card in this reader"
		}
		for _, m := range d.mounts {
			for _, marker := range []string{"roms", "Roms", "MUOS", "muos"} {
				if info, err := os.Stat(filepath.Join(m, marker)); err == nil && info.IsDir() {
					d.Warnings = append(d.Warnings, "It has games on it ("+filepath.Base(m)+"). Copy them off first, they will be erased too.")
					goto next
				}
			}
		}
	next:
	}
	return disks
}

func findDisk(id string) (Disk, error) {
	disks, err := listDisks()
	if err != nil {
		return Disk{}, err
	}
	for _, d := range disks {
		if d.ID == id {
			if d.Problem != "" {
				return d, errors.New(d.Problem)
			}
			return d, nil
		}
	}
	return Disk{}, errors.New("the card is gone; put it back in and pick it again")
}

// waitForDisk finds a card again after its reader dropped it: the one external disk with the same name and size.
func waitForDisk(old Disk) (Disk, error) {
	for wait := 0; wait < 40; wait++ {
		time.Sleep(500 * time.Millisecond)
		disks, err := listDisks()
		if err != nil {
			continue
		}
		var same []Disk
		for _, d := range disks {
			if d.Name == old.Name && d.Size == old.Size {
				same = append(same, d)
			}
		}
		if len(same) > 1 {
			return Disk{}, errors.New("more than one card like it is connected; keep only one in")
		}
		if len(same) == 1 {
			return same[0], nil
		}
	}
	return Disk{}, errors.New("the card did not come back; check that it sits firmly in the reader")
}

func alignedBuffer(size int) []byte {
	buf := make([]byte, size+align)
	off := int(uintptr(unsafe.Pointer(&buf[0])) & (align - 1))
	if off != 0 {
		off = align - off
	}
	return buf[off : off+size : off+size]
}

func openImage(path string) (io.ReadCloser, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	if !strings.HasSuffix(strings.ToLower(path), ".gz") {
		return f, nil
	}
	z, err := gzip.NewReader(bufio.NewReaderSize(f, 1<<20))
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("%s is not a gzip file: %w", filepath.Base(path), err)
	}
	z.Multistream(false)
	return struct {
		io.Reader
		io.Closer
	}{z, f}, nil
}

type reporter func(phase string, done, total int64)

// writeImage unpacks the image on the fly and writes every byte of it, zero blocks included. Card readers can
// drop the card for a moment (a USB reset), and the OS then reads the partition table again and mounts what it
// finds, writing to it. So the first chunk (partition tables, U-Boot) is zeroed first and the card looks blank
// until the very end:
//
//  1. zero the first chunk;
//  2. write the rest in order, reading each chunk back two chunks behind (past the reader's cache);
//  3. read the rest back once more, rewriting any chunk that no longer matches;
//  4. write the first chunk, check it, and check the start of the card again if the reader dropped it meanwhile.
//
// It returns the number of bytes written.
func writeImage(path string, t target, total int64, report reporter) (int64, error) {
	src, err := openImage(path)
	if err != nil {
		return 0, err
	}
	defer src.Close()

	type block struct {
		buf []byte
		n   int
		off int64
	}
	const lag = 2 // chunks between writing one and reading it back
	free := make(chan []byte, 5+lag)
	for i := 0; i < cap(free); i++ {
		free <- alignedBuffer(chunk)
	}
	full := make(chan block, 4)
	stop := make(chan struct{})
	readErr := make(chan error, 1)
	var sums [][sha256.Size]byte // of every chunk, by index
	var sumsMu sync.Mutex
	go func() {
		defer close(full)
		var off int64
		for {
			var buf []byte
			select {
			case buf = <-free:
			case <-stop:
				readErr <- nil
				return
			}
			n, err := io.ReadFull(src, buf)
			if n > 0 {
				sumsMu.Lock()
				sums = append(sums, sha256.Sum256(buf[:n]))
				sumsMu.Unlock()
				full <- block{buf, n, off}
				off += int64(n)
			}
			if err == io.EOF || err == io.ErrUnexpectedEOF { // gzip has checked its CRC by now
				readErr <- nil
				return
			}
			if err != nil {
				readErr <- fmt.Errorf("can't unpack the image: %w", err)
				return
			}
		}
	}()
	fail := func(err error) (int64, error) {
		close(stop)
		for b := range full {
			free <- b.buf
		}
		return 0, err
	}

	back := alignedBuffer(chunk)
	resets := 0 // times the reader dropped the card; counted by readBlock and writeBlock through report
	rep := func(phase string, done, total int64) {
		if phase == "reopen" {
			resets++
		}
		report(phase, done, total)
	}
	sumOf := func(off int64) [sha256.Size]byte {
		sumsMu.Lock()
		defer sumsMu.Unlock()
		return sums[off/chunk]
	}
	// check reads one chunk back; a mismatch is rewritten from the image (twice at most) before it counts.
	check := func(off int64, n int) error {
		for try := 0; ; try++ {
			if err := readBlock(t, back, n, off, rep); err != nil {
				return err
			}
			if sha256.Sum256(back[:n]) == sumOf(off) {
				return nil
			}
			logf("mismatch at %d (try %d)", off, try)
			if try == 2 {
				return fmt.Errorf("the card keeps reading back different from the image at %d MB. It may be fake or worn out; try another card or reader", off/1000000)
			}
			if err := rewrite(path, t, off, n, rep); err != nil {
				return err
			}
		}
	}

	if err := writeBlock(t, back, chunk, 0, rep); err != nil { // back is all zeros here
		return fail(err)
	}
	var head *block
	var pending []block // written, not yet read back
	var written int64
	report("write", 0, total)
	for b := range full {
		written += int64(b.n)
		if b.off == 0 {
			head = &b
			continue
		}
		if err := writeBlock(t, b.buf, b.n, b.off, rep); err != nil {
			return fail(err)
		}
		pending = append(pending, b)
		if len(pending) > lag {
			if err := check(pending[0].off, pending[0].n); err != nil {
				return fail(err)
			}
			free <- pending[0].buf
			pending = pending[1:]
		}
		report("write", written, max(total, written))
	}
	if err := <-readErr; err != nil {
		return 0, err
	}
	if err := t.Sync(); err != nil {
		logf("sync: %v", err)
	}
	for _, b := range pending {
		if err := check(b.off, b.n); err != nil {
			return 0, err
		}
	}
	logf("written and checked chunk by chunk: %d bytes, %d resets", written, resets)

	// The final pass, after the reader's cache has long moved on.
	report("verify", 0, written)
	for off := int64(chunk); off < written; off += chunk {
		if err := check(off, int(min(int64(chunk), written-off))); err != nil {
			return 0, err
		}
		report("verify", off+chunk, written)
	}

	// The partition table last: from here the OS may mount the card if the reader drops it.
	if head != nil {
		before := resets
		if err := writeBlock(t, head.buf, head.n, 0, rep); err != nil {
			return 0, err
		}
		if err := t.Sync(); err != nil {
			logf("sync: %v", err)
		}
		if err := check(0, head.n); err != nil {
			return 0, err
		}
		if resets > before { // the OS may have mounted RGDS-BOOT and written to it: check the start of the card again
			logf("reader reset after the partition table went on; checking the boot partition again")
			for off := int64(0); off < min(written, bootArea); off += chunk {
				if err := check(off, int(min(int64(chunk), written-off))); err != nil {
					return 0, err
				}
			}
		}
	}
	report("verify", written, written)
	logf("final pass matches, %d resets", resets)
	return written, nil
}

// bootArea covers RGDS-BOOT (134 MB from 1 MiB): what an OS writes to when it mounts the card.
const bootArea = 192 << 20

// rewrite writes one chunk again from the image, unpacking it from the start (a few seconds at worst).
func rewrite(path string, t target, off int64, n int, report reporter) error {
	src, err := openImage(path)
	if err != nil {
		return err
	}
	defer src.Close()
	if _, err := io.CopyN(io.Discard, src, off); err != nil {
		return err
	}
	buf := alignedBuffer(chunk)
	if _, err := io.ReadFull(src, buf[:n]); err != nil {
		return err
	}
	logf("rewriting %d", off)
	return writeBlock(t, buf, n, off, report)
}

// readBlock reads n bytes at off, finding the card again when its reader dropped it.
func readBlock(t target, buf []byte, n int, off int64, report reporter) error {
	var err error
	for try := 0; try < 5; try++ {
		if _, err = t.ReadAt(buf[:roundUp(int64(n))], off); err == nil || err == io.EOF {
			return nil
		}
		logf("read at %d failed: %v", off, describeErr(err))
		r, ok := t.(reopener)
		if !ok {
			break
		}
		report("reopen", 0, 0)
		if rerr := r.Reopen(); rerr != nil {
			logf("reopen: %v", rerr)
			return fmt.Errorf("the card reader lost the card (%v)", rerr)
		}
	}
	return fmt.Errorf("the card can't be read back (%v). Try another card reader", err)
}

// writeBlock retries a failed write: a reader that drops the card for a moment gets it back (Reopen), and
// writing goes on where it stopped. Every byte is still read back afterwards.
func writeBlock(t target, buf []byte, n int, off int64, report reporter) error {
	size := roundUp(int64(n))
	clear(buf[n:size]) // a short last block is padded with zeros to a whole sector
	var err error
	for try := 0; try < 5; try++ {
		if _, err = t.WriteAt(buf[:size], off); err == nil {
			return nil
		}
		logf("write at %d failed: %v", off, describeErr(err))
		if r, ok := t.(reopener); ok {
			report("reopen", 0, 0)
			if rerr := r.Reopen(); rerr != nil {
				logf("reopen: %v", rerr)
				break
			}
		} else {
			time.Sleep(time.Second)
		}
	}
	if errors.Is(err, errWriteProtected) {
		return err
	}
	return fmt.Errorf("writing the card failed at %d MB (%v). Check the card sits firmly, or try another reader", off/1000000, err)
}

var errWriteProtected = errors.New("the card is write-protected: slide the lock switch on the SD adapter up (away from LOCK)")

func roundUp(n int64) int64 { return (n + align - 1) &^ (align - 1) }

// flash is the whole job for one card; it runs in the UI process (macOS, Linux as root) or in the worker.
func flash(imagePath, diskID string, report reporter) error {
	defer keepAwake()()
	total, err := unpackedSize(imagePath)
	if err != nil {
		return err
	}
	report("prepare", 0, total)
	logf("flash %s (%d bytes unpacked) to %s", imagePath, total, diskID)
	d, err := findDisk(diskID)
	if err != nil {
		logf("disk %s: %v", diskID, err)
		return err
	}
	logf("disk %+v", d)
	if total > d.Size {
		return fmt.Errorf("%s is too small for the image", d.Name)
	}
	t, err := openTarget(d)
	if err != nil {
		logf("open: %v", describeErr(err))
		return err
	}
	if _, err := writeImage(imagePath, t, total, report); err != nil {
		logf("failed: %v", err)
		t.Close()
		return err
	}
	report("eject", 0, 0)
	if err := t.Finish(); err != nil {
		logf("eject: %v", err) // the card is written and checked; it can come out all the same
	}
	logf("done")
	return nil
}
