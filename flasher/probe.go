package main

// Fake cards. Cheap microSD cards often claim far more space than they have: the controller says 128 GB, the chip
// holds 8, and anything written past the real size lands on top of earlier data (or vanishes). A flash looks
// fine at first and the card breaks once games fill it. That's the "black screen after a week" kind of report.
//
// Before anything is written, the flasher puts a small marker on a few hundred places spread over the whole card,
// then reads them all back. On a real card every marker comes back as written. On a fake one the markers past the
// real size overwrite the ones below it, or read back as zeros, and how they fail tells about how much the card
// really holds. What was there before is put back if the card fails, so a card that turns out to be fake loses
// nothing. A card reader that caches recent writes could answer that first check from its cache, so the markers
// past the image are read once more after it: hundreds of MB later, no cache still holds them. Those markers stay
// on the card in space nothing has used yet (the data partition's mkfs only writes its own start).

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"sort"
)

const probeSize = align // one sector-aligned block per place

// probeSpots picks the places for a card of size bytes: an even spread, plus both sides of every power of two
// (fakes usually wrap at their real size, which is one), and the very last block.
func probeSpots(size int64) []int64 {
	end := size &^ (probeSize - 1)
	seen := map[int64]bool{}
	var spots []int64
	add := func(off int64) {
		off &^= probeSize - 1
		if off < chunk || off+probeSize > end || seen[off] { // the first chunk is the partition table's
			return
		}
		seen[off] = true
		spots = append(spots, off)
	}
	const spread = 192
	for i := int64(1); i <= spread; i++ {
		add(size / spread * i)
		add(size/spread*i - size/spread/2)
	}
	for p := int64(64 << 20); p < size; p <<= 1 {
		add(p - probeSize)
		add(p)
		add(p + p/2)
	}
	add(end - probeSize)
	sort.Slice(spots, func(i, j int) bool { return spots[i] < spots[j] })
	return spots
}

// probeBlock is what goes at off for this run: a header with the run's nonce and the offset (so a block that turns
// up somewhere else says where it was written), then bytes that follow from both.
func probeBlock(buf []byte, nonce []byte, off int64) {
	var seed [40]byte
	copy(seed[:], "RGDS-PROBE")
	binary.LittleEndian.PutUint64(seed[10:], uint64(off))
	copy(seed[18:], nonce)
	copy(buf, seed[:34])
	for i := 64; i < len(buf); i += sha256.Size {
		binary.LittleEndian.PutUint32(seed[36:], uint32(i))
		sum := sha256.Sum256(seed[:])
		copy(buf[i:], sum[:])
	}
}

// probeOrigin is where a block read back was written in this run, or -1 when it is no marker of this run.
func probeOrigin(buf []byte, nonce []byte) int64 {
	if string(buf[:10]) != "RGDS-PROBE" || string(buf[18:34]) != string(nonce) {
		return -1
	}
	return int64(binary.LittleEndian.Uint64(buf[10:]))
}

// errFakeCard is a card that holds less than it says.
type errFakeCard struct {
	claims, holds int64
}

func (e errFakeCard) Error() string {
	if e.holds <= 0 {
		return fmt.Sprintf("this card is fake or broken: it says %s, but what is written to it does not stay. Use another card", sizeText(e.claims))
	}
	return fmt.Sprintf("this card is fake: it says %s, but only about %s really works. It would break once games fill it. Use another card", sizeText(e.claims), sizeText(e.holds))
}

func sizeText(n int64) string {
	if n >= 1e9 {
		return fmt.Sprintf("%.0f GB", float64(n)/1e9)
	}
	return fmt.Sprintf("%d MB", n/1e6)
}

// probe is one run's markers, for the second look after the image.
type probe struct {
	spots []int64
	nonce []byte
	size  int64
}

// probeCard checks that the card holds what it says; a card that fails gets its old contents back.
func probeCard(t target, size int64, report reporter) (*probe, error) {
	spots := probeSpots(size)
	nonce := make([]byte, 16)
	rand.Read(nonce)
	old := make([][]byte, len(spots))
	buf := alignedBuffer(probeSize)
	steps := int64(3 * len(spots))
	step := int64(0)
	tick := func() { step++; report("probe", step, steps) }
	report("probe", 0, steps)
	for i, off := range spots {
		if err := readBlock(t, buf, probeSize, off, report); err != nil {
			return nil, err
		}
		old[i] = append([]byte(nil), buf...)
		tick()
	}
	restore := func() {
		for i := len(spots) - 1; i >= 0; i-- { // the reverse order puts back what an overlapped place had first
			copy(buf, old[i])
			if err := writeBlock(t, buf, probeSize, spots[i], report); err != nil {
				logf("probe: putting back %d: %v", spots[i], err)
			}
		}
		t.Sync()
	}
	for _, off := range spots {
		probeBlock(buf, nonce, off)
		if err := writeBlock(t, buf, probeSize, off, report); err != nil {
			restore()
			return nil, err
		}
		tick()
	}
	if err := t.Sync(); err != nil {
		logf("probe sync: %v", err)
	}
	p := &probe{spots, nonce, size}
	if err := p.check(t, nil, tick, report); err != nil {
		restore()
		return nil, err
	}
	logf("probe: %d places across %d bytes all good", len(spots), size)
	return p, nil
}

// check reads the markers back (those keep says to); an errFakeCard says what the card really holds.
func (p *probe) check(t target, keep func(off int64) bool, tick func(), report reporter) error {
	buf, want := alignedBuffer(probeSize), make([]byte, probeSize)
	// a card that wraps shows a marker from higher up (the distance is its real size); one that drops writes
	// shows something else from its real size on
	holds, bad, looked := p.size, 0, 0
	for _, off := range p.spots {
		if keep != nil && !keep(off) {
			continue
		}
		looked++
		if err := readBlock(t, buf, probeSize, off, report); err != nil {
			return err
		}
		probeBlock(want, p.nonce, off)
		if string(buf) != string(want) {
			bad++
			if from := probeOrigin(buf, p.nonce); from > off {
				holds = min(holds, from-off)
			} else {
				holds = min(holds, off)
			}
		}
		if tick != nil {
			tick()
		}
	}
	if bad > 0 {
		logf("probe: %d of %d places bad; holds about %d of %d bytes", bad, looked, holds, p.size)
		return errFakeCard{claims: p.size, holds: holds}
	}
	return nil
}

// recheck is the second look, after the image: the markers that nothing has written over since.
func (p *probe) recheck(t target, from int64, carry [2]int64, report reporter) error {
	err := p.check(t, func(off int64) bool {
		return off >= from && (off+probeSize <= carry[0] || off >= carry[1])
	}, nil, report)
	if err == nil {
		logf("probe: markers past the image still good")
	}
	return err
}
