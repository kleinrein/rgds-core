package main

// Format first: an option on the card step. Before the flash, the card is formatted as the SD Association's
// formatter would: an MBR with one partition, FAT32 up to 32 GiB (SDHC) and exFAT above (SDXC), with the partition
// and the data area on 4 MiB boundaries. That clears whatever the card had: old partition tables, a backup GPT at
// its end, the saves header of an earlier flash. Both file systems are written here in Go, so every OS gets the
// same card. Only the structures are written (a few MB).
//
// It runs after the fake-card check (probe.go), so a fake card stops before anything is erased.

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"unicode"
	"unicode/utf16"
)

const (
	sector      = 512
	fmtAlign    = 4 << 20 // partition start and data area: SD erase-block boundaries
	fat32Limit  = 32 << 30
	minFormat   = 64 << 20
	formatLabel = "SD CARD"
)

// Layout is what a format puts on a card of a given size; the UI shows it before anything happens.
type Layout struct {
	FS      string `json:"fs"`      // "FAT32" or "exFAT"
	Size    int64  `json:"size"`    // the partition, in bytes
	Cluster int64  `json:"cluster"` // allocation unit, in bytes
	Label   string `json:"label"`
	start   int64  // partition start, in sectors
	sectors int64  // partition length, in sectors
}

// layoutFor picks the file system and cluster size for a card of size bytes (the SD Association's choice).
func layoutFor(size int64) (Layout, error) {
	if size < minFormat {
		return Layout{}, fmt.Errorf("too small to format (%s)", sizeText(size))
	}
	start := int64(fmtAlign / sector)
	// the partition ends on a 4 MiB boundary too, short of the card's last MiB: old GPT copies and the saves header
	// live there (carry.go), and they are zeroed
	end := ((size - (1 << 20)) &^ (fmtAlign - 1)) / sector
	l := Layout{start: start, sectors: end - start, Label: formatLabel}
	l.Size = l.sectors * sector
	if size <= fat32Limit {
		l.FS = "FAT32"
		switch {
		case l.Size <= 8<<30:
			l.Cluster = 4 << 10
		case l.Size <= 16<<30:
			l.Cluster = 8 << 10
		default:
			l.Cluster = 16 << 10
		}
		for l.Cluster > sector && l.Size/l.Cluster < 65525+1024 { // FAT32 needs at least 65525 clusters
			l.Cluster /= 2
		}
		if l.Size/l.Cluster < 65525+1024 {
			return Layout{}, fmt.Errorf("too small to format (%s)", sizeText(size))
		}
	} else {
		l.FS = "exFAT"
		l.Cluster = 128 << 10
	}
	return l, nil
}

// region is one stretch of the card a format writes.
type region struct {
	off  int64
	data []byte
}

// buildFormat is every byte a format writes, the partition table (at 0) first. Regions don't overlap and are
// sector multiples.
func buildFormat(l Layout, diskSize int64) ([]region, error) {
	var serial [4]byte
	rand.Read(serial[:])
	var fs []region
	var err error
	if l.FS == "FAT32" {
		fs, err = buildFAT32(l, binary.LittleEndian.Uint32(serial[:]))
	} else {
		fs, err = buildExFAT(l, binary.LittleEndian.Uint32(serial[:]))
	}
	if err != nil {
		return nil, err
	}
	mbr := make([]byte, fmtAlign) // the whole first 4 MiB: old partition tables and U-Boot go
	rand.Read(mbr[440:444])
	p := mbr[446:462]
	p[0] = 0x00
	copy(p[1:4], []byte{0xFE, 0xFF, 0xFF}) // CHS: past 8 GB, LBA only
	p[4] = 0x0C                            // FAT32 (LBA)
	if l.FS == "exFAT" {
		p[4] = 0x07
	}
	copy(p[5:8], []byte{0xFE, 0xFF, 0xFF})
	binary.LittleEndian.PutUint32(p[8:], uint32(l.start))
	binary.LittleEndian.PutUint32(p[12:], uint32(l.sectors))
	mbr[510], mbr[511] = 0x55, 0xAA
	tail := diskSize&^(sector-1) - (1 << 20) // the last MiB: a backup GPT, the saves header
	if tail < l.start*sector+l.Size {
		return nil, errors.New("the layout runs past the card")
	}
	out := []region{{0, mbr}}
	out = append(out, fs...)
	out = append(out, region{tail, make([]byte, diskSize&^(sector-1)-tail)})
	return out, nil
}

func put16(b []byte, off int, v uint16) { binary.LittleEndian.PutUint16(b[off:], v) }
func put32(b []byte, off int, v uint32) { binary.LittleEndian.PutUint32(b[off:], v) }
func put64(b []byte, off int, v uint64) { binary.LittleEndian.PutUint64(b[off:], v) }

// label11 is a FAT short name: upper case ASCII, padded with spaces.
func label11(s string) []byte {
	b := []byte("           ")
	n := 0
	for _, r := range s {
		if n == 11 {
			break
		}
		if r > 0x7E || r < 0x20 {
			r = '_'
		}
		b[n] = byte(unicode.ToUpper(r))
		n++
	}
	return b
}

// buildFAT32 is Microsoft's FAT32 (fatgen103): reserved sectors with the boot sector, FSInfo and their copies at 6
// and 7, two FATs, and the root directory in cluster 2 with the volume label. Reserved sectors pad the data
// area onto a 4 MiB boundary.
func buildFAT32(l Layout, serial uint32) ([]region, error) {
	spc := l.Cluster / sector
	total := l.sectors
	if total > 0xFFFFFFFF {
		return nil, errors.New("too big for FAT32")
	}
	const fats = 2
	// fatgen103's FAT size, with the fewest reserved sectors: a bigger reserved area only makes the FAT roomier
	reserved := int64(32)
	fatSz := (total - reserved + (256*spc+fats)/2 - 1) / ((256*spc + fats) / 2)
	alignSec := int64(fmtAlign / sector)
	data := reserved + fats*fatSz
	if rem := (l.start + data) % alignSec; rem != 0 {
		reserved += alignSec - rem
	}
	data = reserved + fats*fatSz
	clusters := (total - data) / spc
	if clusters < 65525 || clusters+2 > fatSz*sector/4 {
		return nil, fmt.Errorf("FAT32 layout doesn't fit: %d clusters, FAT of %d sectors", clusters, fatSz)
	}

	boot := make([]byte, sector)
	copy(boot, []byte{0xEB, 0x58, 0x90})
	copy(boot[3:], "MSWIN4.1") // what old devices look for
	put16(boot, 11, sector)
	boot[13] = byte(spc)
	put16(boot, 14, uint16(reserved))
	boot[16] = fats
	boot[21] = 0xF8
	put16(boot, 24, 63)
	put16(boot, 26, 255)
	put32(boot, 28, uint32(l.start))
	put32(boot, 32, uint32(total))
	put32(boot, 36, uint32(fatSz))
	put32(boot, 44, 2) // root directory cluster
	put16(boot, 48, 1) // FSInfo
	put16(boot, 50, 6) // backup boot sector
	boot[64] = 0x80
	boot[66] = 0x29
	put32(boot, 67, serial)
	copy(boot[71:82], label11(l.Label))
	copy(boot[82:90], "FAT32   ")
	boot[510], boot[511] = 0x55, 0xAA

	info := make([]byte, sector)
	put32(info, 0, 0x41615252)
	put32(info, 484, 0x61417272)
	put32(info, 488, uint32(clusters-1)) // all free but the root directory
	put32(info, 492, 3)
	put32(info, 508, 0xAA550000)

	res := make([]byte, reserved*sector)
	copy(res[0:], boot)
	copy(res[sector:], info)
	copy(res[6*sector:], boot)
	copy(res[7*sector:], info)

	fat := make([]byte, fatSz*sector)
	put32(fat, 0, 0x0FFFFFF8)
	put32(fat, 4, 0x0FFFFFFF) // clean, no errors
	put32(fat, 8, 0x0FFFFFFF) // the root directory: one cluster

	root := make([]byte, l.Cluster)
	copy(root, label11(l.Label))
	root[11] = 0x08 // volume label

	base := l.start * sector
	out := []region{{base, res}}
	for i := int64(0); i < fats; i++ {
		out = append(out, region{base + (reserved+i*fatSz)*sector, fat})
	}
	out = append(out, region{base + data*sector, root})
	return out, nil
}

// exfatSum is the exFAT boot and up-case checksum (a rotate right, then add).
func exfatSum(sum uint32, b []byte, skip func(int) bool) uint32 {
	for i, c := range b {
		if skip != nil && skip(i) {
			continue
		}
		sum = (sum>>1 | sum<<31) + uint32(c)
	}
	return sum
}

// upcaseTable is exFAT's up-case table for the whole BMP, compressed: runs of characters that map to themselves
// are 0xFFFF followed by the run's length.
func upcaseTable() []byte {
	var out []uint16
	for c := 0; c <= 0xFFFF; {
		up := c
		if c < 0xD800 || c > 0xDFFF {
			if u := unicode.ToUpper(rune(c)); u <= 0xFFFF && u != 0xFFFF {
				up = int(u)
			}
		}
		if up != c {
			out = append(out, uint16(up))
			c++
			continue
		}
		run := 1
		for c+run <= 0xFFFF {
			n := c + run
			if n >= 0xD800 && n <= 0xDFFF {
				run++
				continue
			}
			if u := unicode.ToUpper(rune(n)); u != rune(n) && u <= 0xFFFF && u != 0xFFFF {
				break
			}
			run++
		}
		if run == 1 {
			out = append(out, uint16(c))
		} else {
			out = append(out, 0xFFFF, uint16(run))
		}
		c += run
	}
	b := make([]byte, 2*len(out))
	for i, v := range out {
		put16(b, 2*i, v)
	}
	return b
}

// buildExFAT is Microsoft's exFAT: the main and backup boot regions (12 sectors each, the last a checksum), one
// FAT, and in the cluster heap the allocation bitmap, the up-case table and the root directory, in that order.
func buildExFAT(l Layout, serial uint32) ([]region, error) {
	spc := l.Cluster / sector
	shift := 0
	for 1<<shift < spc {
		shift++
	}
	alignSec := int64(fmtAlign / sector)
	fatOffset := int64(2048) // 1 MiB into the partition
	fatLen := ((l.sectors/spc+2)*4 + sector - 1) / sector
	heap := (fatOffset + fatLen + alignSec - 1) / alignSec * alignSec
	clusters := (l.sectors - heap) / spc
	if clusters <= 0 || clusters > 0xFFFFFFF5 {
		return nil, errors.New("exFAT layout doesn't fit")
	}
	up := upcaseTable()
	bitmapLen := (clusters + 7) / 8
	nclus := func(n int64) int64 { return (n + l.Cluster - 1) / l.Cluster }
	bitmapClus, upClus := nclus(bitmapLen), nclus(int64(len(up)))
	bitmapAt, upAt := int64(2), 2+bitmapClus
	rootAt := upAt + upClus
	used := bitmapClus + upClus + 1

	bootRegion := func() []byte {
		r := make([]byte, 12*sector)
		b := r[:sector]
		copy(b, []byte{0xEB, 0x76, 0x90})
		copy(b[3:], "EXFAT   ")
		put64(b, 64, uint64(l.start))
		put64(b, 72, uint64(l.sectors))
		put32(b, 80, uint32(fatOffset))
		put32(b, 84, uint32(fatLen))
		put32(b, 88, uint32(heap))
		put32(b, 92, uint32(clusters))
		put32(b, 96, uint32(rootAt))
		put32(b, 100, serial)
		put16(b, 104, 0x0100)
		b[108] = 9
		b[109] = byte(shift)
		b[110] = 1
		b[111] = 0x80
		b[112] = byte(used * 100 / clusters)
		for i := 120; i < 510; i++ {
			b[i] = 0xF4
		}
		b[510], b[511] = 0x55, 0xAA
		for s := 1; s <= 8; s++ { // extended boot sectors
			put32(r, s*sector+508, 0xAA550000)
		}
		sum := exfatSum(0, r[:11*sector], func(i int) bool { return i == 106 || i == 107 || i == 112 })
		for i := 11 * sector; i < 12*sector; i += 4 {
			put32(r, i, sum)
		}
		return r
	}()

	fat := make([]byte, fatLen*sector)
	put32(fat, 0, 0xFFFFFFF8)
	put32(fat, 4, 0xFFFFFFFF)
	chain := func(first, n int64) {
		for c := first; c < first+n; c++ {
			next := uint32(c + 1)
			if c == first+n-1 {
				next = 0xFFFFFFFF
			}
			put32(fat, int(4*c), next)
		}
	}
	chain(bitmapAt, bitmapClus)
	chain(upAt, upClus)
	chain(rootAt, 1)

	bitmap := make([]byte, bitmapClus*l.Cluster)
	for c := int64(0); c < used; c++ {
		bitmap[c/8] |= 1 << (c % 8)
	}
	upData := make([]byte, upClus*l.Cluster)
	copy(upData, up)

	root := make([]byte, l.Cluster)
	name := utf16.Encode([]rune(l.Label))
	root[0] = 0x83
	root[1] = byte(len(name))
	for i, c := range name {
		put16(root, 2+2*i, c)
	}
	e := root[32:64]
	e[0] = 0x81
	put32(e, 20, uint32(bitmapAt))
	put64(e, 24, uint64(bitmapLen))
	e = root[64:96]
	e[0] = 0x82
	put32(e, 4, exfatSum(0, up, nil))
	put32(e, 20, uint32(upAt))
	put64(e, 24, uint64(len(up)))

	base := l.start * sector
	at := func(c int64) int64 { return base + (heap+(c-2)*spc)*sector }
	head := make([]byte, fatOffset*sector) // both boot regions, and the rest up to the FAT zeroed
	copy(head, bootRegion)
	copy(head[12*sector:], bootRegion)
	return []region{
		{base, head},
		{base + fatOffset*sector, fat},
		{at(bitmapAt), bitmap},
		{at(upAt), upData},
		{at(rootAt), root},
	}, nil
}

// formatFirst formats the card before a flash: it writes the file system and reads it all back, the partition
// table last. The card's old partitions, a backup GPT at its end and anything that recognised it go; the flash then
// writes RG DS OS over the start as usual.
func formatFirst(t target, size int64, report reporter) error {
	l, err := layoutFor(size)
	if err != nil {
		return err
	}
	regions, err := buildFormat(l, size)
	if err != nil {
		return err
	}
	logf("format: %s, %d bytes from sector %d, %d KiB clusters", l.FS, l.Size, l.start, l.Cluster>>10)
	var total int64
	for _, r := range regions {
		total += int64(len(r.data))
	}
	buf, back := alignedBuffer(chunk), alignedBuffer(chunk)
	var done int64
	each := func(r region, fn func(off int64, b []byte) error) error {
		for p := 0; p < len(r.data); p += chunk {
			if err := fn(r.off+int64(p), r.data[p:min(len(r.data), p+chunk)]); err != nil {
				return err
			}
		}
		return nil
	}
	write := func(off int64, b []byte) error {
		copy(buf, b)
		if err := writeBlock(t, buf, len(b), off, report); err != nil {
			return err
		}
		done += int64(len(b))
		report("format", done, 2*total)
		return nil
	}
	check := func(off int64, b []byte) error {
		if err := readBlock(t, back, len(b), off, report); err != nil {
			return err
		}
		if string(back[:len(b)]) != string(b) {
			return fmt.Errorf("the card reads back different at %d MB. It may be fake or worn out; try another card or reader", off/1000000)
		}
		done += int64(len(b))
		report("format", done, 2*total)
		return nil
	}
	clear(buf) // the start zeroed first: a reader that drops the card finds nothing to mount
	if err := writeBlock(t, buf, chunk, 0, report); err != nil {
		return err
	}
	for _, r := range regions[1:] {
		if err := each(r, write); err != nil {
			return err
		}
	}
	t.Sync()
	for _, r := range regions[1:] {
		if err := each(r, check); err != nil {
			return err
		}
	}
	if err := each(regions[0], write); err != nil {
		return err
	}
	t.Sync()
	if err := each(regions[0], check); err != nil {
		return err
	}
	logf("format: written and checked")
	return nil
}
