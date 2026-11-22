package main

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"io"
	"os"
	"testing"
)

// The fixtures were made with the real mkfs.ext4 -d (e2fsprogs 1.47) from a tree like a stock vendor partition:
// deep/drastic_aarch64/backup with two patterned saves and 300 small ones (a directory of many blocks, hashed),
// and deep/drastic_aarch64/savestates. ext4-modern: 4 KiB blocks, extents, 64bit, flex_bg, metadata_csum.
// ext4-old: 1 KiB blocks, no extents (block maps with indirect blocks), 32-bit descriptors.
func fixture(t *testing.T, name string) []byte {
	f, err := os.Open("testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	b, err := io.ReadAll(z)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// card puts a filesystem at 1 MiB on a disk with a GPT (or an MBR) naming it, like a stock card's partition.
func card(fs []byte, gpt bool) []byte {
	disk := make([]byte, 1<<20+len(fs))
	copy(disk[1<<20:], fs)
	le := binary.LittleEndian
	if gpt {
		copy(disk[512:], "EFI PART")
		le.PutUint64(disk[512+72:], 2)   // entries at LBA 2
		le.PutUint32(disk[512+80:], 128) // 128 entries
		le.PutUint32(disk[512+84:], 128) // of 128 bytes
		e := disk[1024:]
		copy(e, []byte("type-guid-not-0!"))
		le.PutUint64(e[32:], 2048)
	} else {
		disk[446+4] = 0x83
		le.PutUint32(disk[446+8:], 2048)
		disk[510], disk[511] = 0x55, 0xAA
	}
	return disk
}

func pattern(n int, f func(i int) byte) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = f(i)
	}
	return b
}

func TestStockDrasticSavesFromExt4(t *testing.T) {
	kart := pattern(532480, func(i int) byte { return byte((i*7 + i/4096) % 251) })
	pokemon := pattern(8192, func(i int) byte { return byte(i * 13 % 241) })
	for _, c := range []struct {
		name string
		gpt  bool
	}{{"ext4-modern.img.gz", true}, {"ext4-old.img.gz", false}} {
		saves := stockDrasticSaves(bytes.NewReader(card(fixture(t, c.name), c.gpt)))
		if len(saves) != 302 {
			t.Errorf("%s: %d saves", c.name, len(saves))
		}
		if !bytes.Equal(saves["vendor/deep/drastic_aarch64/backup/Mario Kart DS.dsv"], kart) {
			t.Errorf("%s: Mario Kart's save differs", c.name)
		}
		if !bytes.Equal(saves["vendor/deep/drastic_aarch64/backup/Pokemon Platinum.dsv"], pokemon) {
			t.Errorf("%s: Pokemon's save differs", c.name)
		}
		if string(saves["vendor/deep/drastic_aarch64/backup/Filler game number 300 with a long name.dsv"]) != "300\n" {
			t.Errorf("%s: the last filler save is missing", c.name)
		}
	}
}

func TestExt4ReaderLeavesOtherDisksAlone(t *testing.T) {
	if got := stockDrasticSaves(bytes.NewReader(make([]byte, 4<<20))); len(got) != 0 {
		t.Fatalf("a blank disk gave %d saves", len(got))
	}
	// a FAT-like partition at 1 MiB (no ext4 magic) is skipped
	if got := stockDrasticSaves(bytes.NewReader(card(make([]byte, 2<<20), true))); len(got) != 0 {
		t.Fatalf("a non-ext4 partition gave %d saves", len(got))
	}
}
