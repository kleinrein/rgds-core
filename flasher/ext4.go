package main

// A read-only ext4 reader, just enough to take DraStic's DS saves off an Anbernic stock card before it is erased.
// The stock system keeps them in /mnt/vendor/deep/drastic*/backup on an ext4 partition, which macOS and Windows
// can't read, and the flasher holds the raw card at that moment anyway (flash.go). It follows the partition table
// (GPT, else MBR), the superblock and group descriptors, inodes with extent trees or the old block maps, and plain
// or hashed directories (a hashed directory's blocks are read as plain ones, which they also are). It never writes,
// skips anything it doesn't understand, and checks no checksums.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strings"
)

// sectorReader reads any range through whole aligned blocks: raw card devices take nothing else.
type sectorReader struct{ r io.ReaderAt }

func (s sectorReader) ReadAt(p []byte, off int64) (int, error) {
	start := off &^ (align - 1)
	end := roundUp(off + int64(len(p)))
	buf := alignedBuffer(int(end - start))
	n, err := s.r.ReadAt(buf, start)
	if int64(n) < off-start+int64(len(p)) {
		if err == nil {
			err = io.ErrUnexpectedEOF
		}
		return 0, err
	}
	return copy(p, buf[off-start:]), nil
}

type ext4 struct {
	r                       io.ReaderAt
	base                    int64 // the partition's offset on the card
	block                   int64
	inodesPerGroup, inodeSz uint32
	descSize                int64
	firstDataBlock          int64
	is64                    bool
}

const (
	ext4Magic   = 0xEF53
	extentMagic = 0xF30A
	flagExtents = 0x80000
	flagInline  = 0x10000000
	maxFile     = 64 << 20 // saves are small; anything bigger is not what we came for
)

func (fs *ext4) read(off, n int64) ([]byte, error) {
	buf := make([]byte, n)
	if _, err := fs.r.ReadAt(buf, fs.base+off); err != nil {
		return nil, err
	}
	return buf, nil
}

// openExt4 reads the superblock of the partition at base; nil when it isn't ext2/3/4.
func openExt4(r io.ReaderAt, base int64) (*ext4, error) {
	fs := &ext4{r: r, base: base}
	sb, err := fs.read(1024, 1024)
	if err != nil {
		return nil, err
	}
	le := binary.LittleEndian
	if le.Uint16(sb[0x38:]) != ext4Magic {
		return nil, errors.New("not ext4")
	}
	log := le.Uint32(sb[0x18:])
	if log > 6 {
		return nil, errors.New("odd block size")
	}
	fs.block = 1024 << log
	fs.firstDataBlock = int64(le.Uint32(sb[0x14:]))
	fs.inodesPerGroup = le.Uint32(sb[0x28:])
	fs.inodeSz = 128
	if le.Uint32(sb[0x4C:]) >= 1 { // dynamic revision: the inode size is in the superblock
		fs.inodeSz = uint32(le.Uint16(sb[0x58:]))
	}
	fs.descSize = 32
	if le.Uint32(sb[0x60:])&0x80 != 0 { // INCOMPAT_64BIT
		fs.is64 = true
		if d := int64(le.Uint16(sb[0xFE:])); d >= 64 {
			fs.descSize = d
		}
	}
	if fs.inodesPerGroup == 0 || fs.inodeSz < 128 {
		return nil, errors.New("odd superblock")
	}
	return fs, nil
}

// inode returns the raw inode ino (2 is the root).
func (fs *ext4) inode(ino uint32) ([]byte, error) {
	if ino == 0 {
		return nil, errors.New("inode 0")
	}
	group, index := int64((ino-1)/fs.inodesPerGroup), int64((ino-1)%fs.inodesPerGroup)
	desc, err := fs.read((fs.firstDataBlock+1)*fs.block+group*fs.descSize, fs.descSize)
	if err != nil {
		return nil, err
	}
	le := binary.LittleEndian
	table := int64(le.Uint32(desc[0x08:]))
	if fs.is64 && fs.descSize >= 64 {
		table |= int64(le.Uint32(desc[0x28:])) << 32
	}
	return fs.read(table*fs.block+index*int64(fs.inodeSz), int64(fs.inodeSz))
}

// content reads a file's or directory's bytes.
func (fs *ext4) content(in []byte) ([]byte, error) {
	le := binary.LittleEndian
	size := int64(le.Uint32(in[0x04:])) | int64(le.Uint32(in[0x6C:]))<<32
	if size < 0 || size > maxFile {
		return nil, fmt.Errorf("a %d-byte file", size)
	}
	flags := le.Uint32(in[0x20:])
	iblock := in[0x28 : 0x28+60]
	if flags&flagInline != 0 {
		return append([]byte(nil), iblock[:min(size, 60)]...), nil
	}
	out := make([]byte, size)
	put := func(logical, physical, count int64) error { // count blocks from physical go to logical; 0 is a hole
		for i := int64(0); i < count; i++ {
			at := (logical + i) * fs.block
			if at >= size {
				return nil
			}
			if physical == 0 {
				continue
			}
			b, err := fs.read((physical+i)*fs.block, fs.block)
			if err != nil {
				return err
			}
			copy(out[at:], b)
		}
		return nil
	}
	if flags&flagExtents != 0 {
		return out, fs.extents(iblock, put, 0)
	}
	// the old block map: 12 direct blocks, then one, two and three levels of indirect blocks
	per := fs.block / 4
	var walk func(block uint32, level int, logical int64) error
	walk = func(block uint32, level int, logical int64) error {
		if block == 0 || logical*fs.block >= size {
			return nil
		}
		if level == 0 {
			return put(logical, int64(block), 1)
		}
		b, err := fs.read(int64(block)*fs.block, fs.block)
		if err != nil {
			return err
		}
		span := int64(1)
		for l := 1; l < level; l++ {
			span *= per
		}
		for i := int64(0); i < per; i++ {
			if err := walk(le.Uint32(b[i*4:]), level-1, logical+i*span); err != nil {
				return err
			}
		}
		return nil
	}
	for i := 0; i < 12; i++ {
		if err := walk(le.Uint32(iblock[i*4:]), 0, int64(i)); err != nil {
			return nil, err
		}
	}
	logical := int64(12)
	for level := 1; level <= 3; level++ {
		if err := walk(le.Uint32(iblock[(11+level)*4:]), level, logical); err != nil {
			return nil, err
		}
		span := int64(1)
		for l := 0; l < level; l++ {
			span *= per
		}
		logical += span
	}
	return out, nil
}

// extents walks an extent tree node (the inode's own 60 bytes, or a block of the tree).
func (fs *ext4) extents(node []byte, put func(logical, physical, count int64) error, depth int) error {
	le := binary.LittleEndian
	if len(node) < 12 || le.Uint16(node) != extentMagic || depth > 8 {
		return errors.New("bad extent tree")
	}
	entries, level := int(le.Uint16(node[2:])), le.Uint16(node[6:])
	for i := 0; i < entries; i++ {
		e := node[12+i*12:]
		if len(e) < 12 {
			return errors.New("bad extent tree")
		}
		if level == 0 {
			length := int64(le.Uint16(e[4:]))
			physical := int64(le.Uint16(e[6:]))<<32 | int64(le.Uint32(e[8:]))
			if length > 32768 { // not yet written: reads as zeros
				length, physical = length-32768, 0
			}
			if err := put(int64(le.Uint32(e)), physical, length); err != nil {
				return err
			}
			continue
		}
		leaf := int64(le.Uint16(e[8:]))<<32 | int64(le.Uint32(e[4:]))
		b, err := fs.read(leaf*fs.block, fs.block)
		if err != nil {
			return err
		}
		if err := fs.extents(b, put, depth+1); err != nil {
			return err
		}
	}
	return nil
}

type dirEntry struct {
	name  string
	inode uint32
	dir   bool
}

func (fs *ext4) list(ino uint32) ([]dirEntry, error) {
	in, err := fs.inode(ino)
	if err != nil {
		return nil, err
	}
	if binary.LittleEndian.Uint16(in)&0xF000 != 0x4000 {
		return nil, errors.New("not a directory")
	}
	data, err := fs.content(in)
	if err != nil {
		return nil, err
	}
	var out []dirEntry
	le := binary.LittleEndian
	for at := 0; at+8 <= len(data); {
		ino, rec, n, kind := le.Uint32(data[at:]), int(le.Uint16(data[at+4:])), int(data[at+6]), data[at+7]
		if rec < 8 || at+rec > len(data) {
			// a block's entries end where the block does; carry on at the next block
			at = (at/int(fs.block) + 1) * int(fs.block)
			continue
		}
		if ino != 0 && n > 0 && at+8+n <= len(data) {
			name := string(data[at+8 : at+8+n])
			if name != "." && name != ".." {
				out = append(out, dirEntry{name, ino, kind == 2})
			}
		}
		at += rec
	}
	return out, nil
}

// lookup follows a path from the root; names compare exactly, as Linux does.
func (fs *ext4) lookup(path string) (uint32, error) {
	ino := uint32(2)
	for _, part := range strings.Split(strings.Trim(path, "/"), "/") {
		if part == "" {
			continue
		}
		entries, err := fs.list(ino)
		if err != nil {
			return 0, err
		}
		next := uint32(0)
		for _, e := range entries {
			if e.name == part {
				next = e.inode
			}
		}
		if next == 0 {
			return 0, fmt.Errorf("%s: not found", part)
		}
		ino = next
	}
	return ino, nil
}

func (fs *ext4) file(ino uint32) ([]byte, error) {
	in, err := fs.inode(ino)
	if err != nil {
		return nil, err
	}
	if binary.LittleEndian.Uint16(in)&0xF000 != 0x8000 {
		return nil, errors.New("not a file")
	}
	return fs.content(in)
}

// partitions lists the card's partitions as byte offsets: GPT entries, else the four MBR ones.
func partitions(r io.ReaderAt) []int64 {
	le := binary.LittleEndian
	head := make([]byte, 1024)
	if _, err := r.ReadAt(head, 0); err != nil {
		return nil
	}
	var out []int64
	if bytes.Equal(head[512:520], []byte("EFI PART")) {
		entries, count, size := int64(le.Uint64(head[512+72:])), int64(le.Uint32(head[512+80:])), int64(le.Uint32(head[512+84:]))
		if count > 256 || size < 128 || size > 1024 {
			return nil
		}
		table := make([]byte, count*size)
		if _, err := r.ReadAt(table, entries*512); err != nil {
			return nil
		}
		for i := int64(0); i < count; i++ {
			e := table[i*size:]
			if first := int64(le.Uint64(e[32:])); first > 0 && !bytes.Equal(e[:16], make([]byte, 16)) {
				out = append(out, first*512)
			}
		}
		return out
	}
	if head[510] == 0x55 && head[511] == 0xAA {
		for i := 0; i < 4; i++ {
			e := head[446+i*16:]
			if e[4] != 0 && e[4] != 0xEE {
				if first := int64(le.Uint32(e[8:])); first > 0 {
					out = append(out, first*512)
				}
			}
		}
	}
	return out
}

// stockDrasticSaves finds DraStic's homes on every ext4 partition of the card (the stock vendor partition holds
// deep/drastic*/backup at its root; a system partition would hold it under mnt/vendor) and returns their in-game
// saves, as bundle paths ("vendor/deep/<home>/backup/<game>.dsv"). Errors only skip what they touch.
func stockDrasticSaves(r io.ReaderAt) map[string][]byte {
	out := map[string][]byte{}
	sr := sectorReader{r}
	for _, base := range partitions(sr) {
		fs, err := openExt4(sr, base)
		if err != nil {
			continue
		}
		for _, deep := range []string{"deep", "mnt/vendor/deep"} {
			dir, err := fs.lookup(deep)
			if err != nil {
				continue
			}
			homes, err := fs.list(dir)
			if err != nil {
				continue
			}
			for _, home := range homes {
				if !home.dir || !strings.HasPrefix(home.name, "drastic") {
					continue
				}
				backup, err := fs.lookup(deep + "/" + home.name + "/backup")
				if err != nil {
					continue
				}
				files, err := fs.list(backup)
				if err != nil {
					continue
				}
				for _, f := range files {
					if f.dir || !strings.HasSuffix(strings.ToLower(f.name), ".dsv") || strings.ContainsAny(f.name, "/\\") {
						continue
					}
					data, err := fs.file(f.inode)
					if err != nil || len(data) == 0 {
						logf("stock DS save %s: %v", f.name, err)
						continue
					}
					out["vendor/deep/"+home.name+"/backup/"+f.name] = data
				}
			}
		}
	}
	return out
}
