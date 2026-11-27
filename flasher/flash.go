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
	Carry    *Carry   `json:"carry,omitempty"` // an old system whose saves can come along (carry.go)
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
		// BananaOS: its saves can come along, and the dialog says what happens to the games
		if d.Carry = detectCarry(d.mounts); d.Carry != nil {
			continue
		}
		for _, v := range d.Volumes {
			if strings.EqualFold(v, "ROCKNIX") {
				d.Warnings = append(d.Warnings, "It has ROCKNIX. Its games and saves are on a part this computer can't read: copy them off on the handheld first (ROCKNIX's network share), or keep this card and use a new one. RG DS OS can bring them over from the old card in an RG DS Plus's second slot or a USB card reader.")
				goto next
			}
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

// writeImage unpacks the image on the fly and writes what plan says (plan.go: everything but the empty space no one
// reads; nil writes every byte, zero blocks included). Card readers can
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
func writeImage(path string, t target, plan *Plan, report reporter) (int64, error) {
	total := plan.Total // plan is never nil: &Plan{Total: n} writes every byte
	if plan.write != nil {
		total = plan.Written
	}
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
	type span struct {
		off int64
		n   int
	}
	var spans []span // the chunks written, in order: what the checks read back
	const lag = 2    // chunks between writing one and reading it back
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
		if !plan.writes(b.off / chunk) {
			free <- b.buf
			continue
		}
		written += int64(b.n)
		spans = append(spans, span{b.off, b.n})
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
	var checked int64
	for _, sp := range spans {
		if sp.off != 0 {
			if err := check(sp.off, sp.n); err != nil {
				return 0, err
			}
		}
		checked += int64(sp.n)
		report("verify", checked, written)
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
			for _, sp := range spans {
				if sp.off >= bootArea {
					break
				}
				if err := check(sp.off, sp.n); err != nil {
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

// Job is one card to write; the UI process hands it to flash, or to the worker as flags.
type Job struct {
	Image  string // .img or .img.gz
	Disk   string // Disk.ID
	Carry  string // the bundle for the card's end (carry.go, preset.go); "-": the person declined their saves
	Backup string // where the whole old card goes first (backup.go); "" for none
	Preset string // setup answers for the first start (preset.go); "" for none
	Format bool   // format the card before the flash (format.go)
}

// planFor is what flash writes from an image: the plan (plan.go) for an RG DS OS image, every byte of anything
// big (a whole-card backup goes back exactly as it was, and planning one would mean unpacking it twice).
func planFor(path string, report reporter) (*Plan, error) {
	total, err := unpackedSize(path)
	if err != nil {
		return nil, err
	}
	if _, backup := readInfo(path); backup || total > 8<<30 {
		return &Plan{Total: total}, nil // a backup goes back byte for byte, empty space included
	}
	return planImage(path, func(done, total int64) { report("plan", done, total) })
}

// flash is the whole job for one card; it runs in the UI process (macOS, Linux as root) or in the worker:
//
//  1. plan what to write, find the card, and open it;
//  2. read what must survive off it: the stock system's DS saves, and with Backup the whole card;
//  3. check it holds what it says (probe.go): a fake card stops here, before anything of the old card is lost;
//  4. put the bundle near its end, then the image, then check the bundle again; eject.
func flash(job Job, report reporter) error {
	defer keepAwake()()
	report("prepare", 0, 0)
	plan, err := planFor(job.Image, report)
	if err != nil {
		return err
	}
	report("prepare", 0, plan.Total)
	logf("flash %s (%d bytes unpacked, %d to write) to %s", job.Image, plan.Total, plan.Written, job.Disk)
	d, err := findDisk(job.Disk)
	if err != nil {
		logf("disk %s: %v", job.Disk, err)
		return err
	}
	logf("disk %+v", d)
	if plan.Total > d.Size {
		return fmt.Errorf("%s is too small for the image", d.Name)
	}
	t, err := openTarget(d)
	if err != nil {
		logf("open: %v", describeErr(err))
		return err
	}
	fail := func(what string, err error) error {
		logf("%s: %v", what, err)
		t.Close()
		return err
	}
	// the stock system's DS saves: read off the old card's ext4 partitions now, before anything is written. A card
	// nobody chose to keep anything from still gets them looked for: a Plus's slot 1 system card holds DraStic's
	// saves while its games (and so what the card list recognises) are on the slot 2 card.
	carry := job.Carry
	if carry == "-" { // the person unticked Bring my saves along: nothing comes
		carry = ""
	} else if carry == "" {
		carry = rescueStockSaves(t, report)
	} else {
		withDS, err := addStockSaves(t, carry, report)
		if err != nil {
			logf("carry: stock DS saves: %v", err) // the rest still goes; the backup folder has what was kept
		}
		carry = withDS
	}
	if job.Preset != "" {
		withSetup, err := withPreset(carry, job.Preset)
		if err != nil {
			return fail("preset", err)
		}
		carry = withSetup
	}
	if job.Backup != "" {
		if err := backupCard(t, d.Size, d.Name, job.Backup, report); err != nil {
			return fail("backup", err)
		}
	}
	pr, err := probeCard(t, d.Size, report)
	if err != nil {
		return fail("probe", err)
	}
	if job.Format { // after the probe: a fake card stops before anything is erased
		if err := formatFirst(t, d.Size, report); err != nil {
			return fail("format", err)
		}
	}
	// the saves kept from an old card first, past the image; checked again once the image is on (carry.go)
	if carry != "" {
		if err := writeCarry(t, d.Size, plan.Total, carry, report); err != nil {
			return fail("carry", err)
		}
	}
	if _, err := writeImage(job.Image, t, plan, report); err != nil {
		return fail("failed", err)
	}
	var carryArea [2]int64 // what the bundle and its header cover: the markers there are gone
	if job.Format {        // the format zeroed the card's last MiB
		carryArea = [2]int64{d.Size&^(sector-1) - (1 << 20), d.Size}
	}
	if carry != "" {
		if err := checkCarry(t, d.Size, carry, report); err != nil {
			return fail("carry after the image", err)
		}
		if info, err := os.Stat(carry); err == nil {
			blob, _ := carryPlace(d.Size, info.Size())
			carryArea = [2]int64{blob, d.Size} // the bundle sits below the last MiB: it covers the format's too
		}
	}
	if err := pr.recheck(t, roundUp(plan.Total), carryArea, report); err != nil {
		return fail("probe after the image", err)
	}
	report("eject", 0, 0)
	if err := t.Finish(); err != nil {
		logf("eject: %v", err) // the card is written and checked; it can come out all the same
	}
	logf("done")
	return nil
}
