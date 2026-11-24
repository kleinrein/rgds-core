package main

// Which parts of the image are worth writing. A card image is mostly empty space: 0.9.1's is 1680 MiB, of which
// 260 MiB hold anything. The rest is the second system slot (empty until the first update) and the first slot past
// the end of its squashfs. Writing those zeros changes nothing anyone ever reads, so they are skipped. That makes
// writing and checking a card about four times faster.
//
// The rules, per partition in the image's GPT, in 4 MiB chunks:
//   - the chunk that holds the start of a partition, and everything outside partitions, is always written, so no old
//     filesystem signature survives where something looks for one (os_update's slot_release mounts slot b as
//     squashfs: zeros at its start make it "no filesystem", as on a fresh card);
//   - a partition that starts with a squashfs: zero chunks wholly past its bytes_used are skipped;
//   - a partition whose first chunk is all zeros (no filesystem yet): its other zero chunks are skipped;
//   - anything else (FAT, ext4, unknown) is written in full, zero chunks included: a filesystem may rely on them.
// A chunk is only ever skipped when it really is all zeros, so the image decides, not the rules alone.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"unicode/utf16"
)

type part struct {
	name   string
	lo, hi int64 // byte range on the image
}

// Plan says, chunk by chunk, what the flash writes.
type Plan struct {
	Total   int64  // unpacked image size
	Written int64  // bytes the flash writes (and reads back)
	write   []bool // by chunk index
}

func (p *Plan) writes(index int64) bool {
	return p == nil || index >= int64(len(p.write)) || p.write[index]
}

// gptParts reads the partition table from the image's first chunk (512-byte sectors).
func gptParts(head []byte) []part {
	if len(head) < 1024 || string(head[512:520]) != "EFI PART" {
		return nil
	}
	start := int64(binary.LittleEndian.Uint64(head[512+72:]))
	count := int64(binary.LittleEndian.Uint32(head[512+80:]))
	size := int64(binary.LittleEndian.Uint32(head[512+84:]))
	if size < 128 || count > 256 {
		return nil
	}
	var parts []part
	for i := int64(0); i < count; i++ {
		at := start*512 + i*size
		if at+size > int64(len(head)) {
			break
		}
		e := head[at : at+size]
		if bytes.Equal(e[:16], make([]byte, 16)) {
			continue
		}
		lo := int64(binary.LittleEndian.Uint64(e[32:]))
		hi := int64(binary.LittleEndian.Uint64(e[40:]))
		if hi < lo {
			continue
		}
		u := make([]uint16, 36)
		for j := range u {
			u[j] = binary.LittleEndian.Uint16(e[56+2*j:])
		}
		for j, c := range u {
			if c == 0 {
				u = u[:j]
				break
			}
		}
		name := string(utf16.Decode(u))
		parts = append(parts, part{name, lo * 512, (hi + 1) * 512})
	}
	return parts
}

func allZero(b []byte) bool {
	for len(b) >= 8 {
		if binary.LittleEndian.Uint64(b) != 0 {
			return false
		}
		b = b[8:]
	}
	for _, c := range b {
		if c != 0 {
			return false
		}
	}
	return true
}

// planner decides chunk by chunk, in image order, as the image is unpacked.
type planner struct {
	parts []part
	limit map[int]int64 // per partition: skip zero chunks from here on (-1: never)
}

func (p *planner) decide(index int64, data []byte) bool {
	off := index * chunk
	if index == 0 {
		p.parts = gptParts(data)
		p.limit = map[int]int64{}
		return true
	}
	end := off + int64(len(data))
	written := false
	// a partition that starts in this chunk: what it holds decides its later chunks, and this one is written
	for i, pt := range p.parts {
		if pt.lo < off || pt.lo >= end {
			continue
		}
		first := data[pt.lo-off:]
		switch {
		case len(first) >= 48 && string(first[:4]) == "hsqs":
			p.limit[i] = pt.lo + roundUp(int64(binary.LittleEndian.Uint64(first[40:])))
		case allZero(first[:min(len(first), chunk)]):
			p.limit[i] = pt.lo
		default:
			p.limit[i] = -1
		}
		written = true
	}
	if written {
		return true
	}
	for i, pt := range p.parts {
		if end <= pt.lo || off >= pt.hi {
			continue
		}
		if off < pt.lo || end > pt.hi {
			return true // straddles a partition edge
		}
		from, ok := p.limit[i]
		if !ok || from < 0 || off < from {
			return true
		}
		return !allZero(data)
	}
	return true // outside every partition
}

// planImage unpacks the image once and returns what the flash will write.
func planImage(path string, progress func(done, total int64)) (*Plan, error) {
	total, err := unpackedSize(path)
	if err != nil {
		return nil, err
	}
	src, err := openImage(path)
	if err != nil {
		return nil, err
	}
	defer src.Close()
	plan := &Plan{Total: total}
	var pl planner
	buf := make([]byte, chunk)
	var off int64
	for index := int64(0); ; index++ {
		n, err := io.ReadFull(src, buf)
		if n > 0 {
			w := pl.decide(index, buf[:n])
			plan.write = append(plan.write, w)
			if w {
				plan.Written += int64(n)
			}
			off += int64(n)
			if progress != nil {
				progress(off, total)
			}
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			break
		}
		if err != nil {
			return nil, errors.New("can't unpack the image: " + err.Error())
		}
	}
	plan.Total = off
	return plan, nil
}
