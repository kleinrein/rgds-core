package main

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"unicode/utf16"
)

// wrapTarget is a fake card: it says size but holds real bytes, and anything past them lands on top of the start.
type wrapTarget struct {
	fileTarget
	real int64
}

func (t wrapTarget) WriteAt(p []byte, off int64) (int, error) { return t.File.WriteAt(p, off%t.real) }
func (t wrapTarget) ReadAt(p []byte, off int64) (int, error)  { return t.File.ReadAt(p, off%t.real) }

// dropTarget is the other kind of fake: past the real size, writes vanish and reads give zeros.
type dropTarget struct {
	fileTarget
	real int64
}

func (t dropTarget) WriteAt(p []byte, off int64) (int, error) {
	if off >= t.real {
		return len(p), nil
	}
	return t.File.WriteAt(p, off)
}
func (t dropTarget) ReadAt(p []byte, off int64) (int, error) {
	if off >= t.real {
		clear(p)
		return len(p), nil
	}
	return t.File.ReadAt(p, off)
}

func sparseDisk(t *testing.T, size int64, fill byte) *os.File {
	path := filepath.Join(t.TempDir(), "card")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	f.Truncate(size)
	// some old contents, so putting them back can be checked
	f.WriteAt(bytes.Repeat([]byte{fill}, 64<<10), 100<<20)
	t.Cleanup(func() { f.Close() })
	return f
}

func TestProbeRealCard(t *testing.T) {
	f := sparseDisk(t, 2<<30, 0)
	if _, err := probeCard(fileTarget{f}, 2<<30, func(string, int64, int64) {}); err != nil {
		t.Fatalf("a real card failed: %v", err)
	}
}

func TestProbeWrappingFake(t *testing.T) {
	const claims, holds = 8 << 30, 1 << 30
	f := sparseDisk(t, holds, 0x77)
	before := make([]byte, 64<<10)
	f.ReadAt(before, 100<<20)
	_, err := probeCard(wrapTarget{fileTarget{f}, holds}, claims, func(string, int64, int64) {})
	var fake errFakeCard
	if !errors.As(err, &fake) {
		t.Fatalf("a fake card passed: %v", err)
	}
	if fake.holds != holds {
		t.Fatalf("estimated %d, really %d", fake.holds, holds)
	}
	after := make([]byte, 64<<10)
	f.ReadAt(after, 100<<20)
	if !bytes.Equal(before, after) {
		t.Fatal("the old contents were not put back")
	}
	if !strings.Contains(err.Error(), "says 9 GB") || !strings.Contains(err.Error(), "1 GB") {
		t.Fatalf("message %q", err)
	}
}

func TestProbeDroppingFake(t *testing.T) {
	const claims, holds = 4 << 30, 3 << 29
	f := sparseDisk(t, holds, 0)
	_, err := probeCard(dropTarget{fileTarget{f}, holds}, claims, func(string, int64, int64) {})
	var fake errFakeCard
	if !errors.As(err, &fake) {
		t.Fatalf("a fake card passed: %v", err)
	}
	if fake.holds < holds-claims/100 || fake.holds > holds+claims/100 {
		t.Fatalf("estimated %d, really %d", fake.holds, holds)
	}
}

// cachedWrapTarget is a fake card behind a reader that answers reads from its last few MB of writes.
type cachedWrapTarget struct {
	wrapTarget
	cache map[int64][]byte
	order *[]int64
}

func (t cachedWrapTarget) WriteAt(p []byte, off int64) (int, error) {
	t.cache[off] = append([]byte(nil), p...)
	*t.order = append(*t.order, off)
	for len(*t.order) > 2048 { // about 8 MB of 4 KiB writes, fewer of the big ones
		delete(t.cache, (*t.order)[0])
		*t.order = (*t.order)[1:]
	}
	return t.wrapTarget.WriteAt(p, off)
}

func (t cachedWrapTarget) ReadAt(p []byte, off int64) (int, error) {
	if c, ok := t.cache[off]; ok && len(c) == len(p) {
		return copy(p, c), nil
	}
	return t.wrapTarget.ReadAt(p, off)
}

func TestProbeSeesThroughReaderCache(t *testing.T) {
	const claims, holds = 8 << 30, 1 << 30
	f := sparseDisk(t, holds, 0)
	var order []int64
	card := cachedWrapTarget{wrapTarget{fileTarget{f}, holds}, map[int64][]byte{}, &order}
	pr, err := probeCard(card, claims, func(string, int64, int64) {})
	if err != nil {
		t.Fatalf("the cache was expected to hide the fake at first: %v", err)
	}
	// the image: enough writes that the cache moves on
	buf := alignedBuffer(chunk)
	for off := int64(0); off < 64*chunk; off += chunk {
		card.WriteAt(buf, off)
	}
	order = order[:0]
	for k := range card.cache {
		delete(card.cache, k)
	}
	var fake errFakeCard
	if err := pr.recheck(card, 64*chunk, [2]int64{}, func(string, int64, int64) {}); !errors.As(err, &fake) {
		t.Fatalf("the second look passed a fake card: %v", err)
	}
}

// gptImage makes a card image like RG DS OS's: GPT, a FAT-ish boot part, a squashfs slot with room after it, and
// an empty slot. Returns the gz path, the raw bytes and the partition starts.
func gptImage(t *testing.T) (string, []byte) {
	const mb = 1 << 20
	raw := make([]byte, 64*mb)
	rand.Read(raw[1*mb : 2*mb]) // U-Boot, outside every partition
	// p1 boot 4-16 MiB: a filesystem that is not squashfs, mostly empty: written in full
	copy(raw[4*mb:], "\xeb\x3c\x90mkfs.fat")
	// p2 root-a 16-40 MiB: squashfs of 5 MiB, zeros after
	copy(raw[16*mb:], "hsqs")
	binary.LittleEndian.PutUint64(raw[16*mb+40:], 5*mb)
	rand.Read(raw[16*mb+64 : 21*mb])
	// p3 root-b 40-64 MiB: empty
	hdr := raw[512:1024]
	copy(hdr, "EFI PART")
	binary.LittleEndian.PutUint64(hdr[72:], 2)
	binary.LittleEndian.PutUint32(hdr[80:], 128)
	binary.LittleEndian.PutUint32(hdr[84:], 128)
	for i, p := range []struct {
		name   string
		lo, hi int
	}{{"boot", 4, 16}, {"rgds-root-a", 16, 40}, {"rgds-root-b", 40, 64}} {
		e := raw[1024+i*128 : 1024+(i+1)*128]
		e[0] = 1
		binary.LittleEndian.PutUint64(e[32:], uint64(p.lo*mb/512))
		binary.LittleEndian.PutUint64(e[40:], uint64(p.hi*mb/512-1))
		for j, c := range utf16.Encode([]rune(p.name)) {
			binary.LittleEndian.PutUint16(e[56+2*j:], c)
		}
	}
	var gz bytes.Buffer
	z := gzip.NewWriter(&gz)
	z.Write(raw)
	z.Close()
	path := filepath.Join(t.TempDir(), "RG-DS-OS-9.9.9.img.gz")
	os.WriteFile(path, gz.Bytes(), 0o644)
	return path, raw
}

func TestPlanSkipsOnlyEmptySpace(t *testing.T) {
	path, raw := gptImage(t)
	plan, err := planImage(path, nil)
	if err != nil {
		t.Fatal(err)
	}
	var skipped []int
	for i, w := range plan.write {
		if !w {
			skipped = append(skipped, i)
		}
	}
	// chunks of 4 MiB: 0 head, 1-3 boot (written), 4-9 root-a (4 = start, 5 = squashfs tail, 6-9 empty),
	// 10-15 root-b (10 = start, written as zeros)
	want := []int{6, 7, 8, 9, 11, 12, 13, 14, 15}
	if len(skipped) != len(want) {
		t.Fatalf("skipped %v, want %v", skipped, want)
	}
	for i := range want {
		if skipped[i] != want[i] {
			t.Fatalf("skipped %v, want %v", skipped, want)
		}
	}
	if plan.Total != int64(len(raw)) || plan.Written != int64(len(raw)-len(want)*chunk) {
		t.Fatalf("total %d written %d", plan.Total, plan.Written)
	}

	// onto a card full of old data: everything the image has is there, the slot starts are zeros
	disk := filepath.Join(t.TempDir(), "disk")
	os.WriteFile(disk, bytes.Repeat([]byte{0xee}, len(raw)), 0o644)
	f, _ := os.OpenFile(disk, os.O_RDWR, 0)
	defer f.Close()
	n, err := writeImage(path, fileTarget{f}, plan, func(string, int64, int64) {})
	if err != nil || n != plan.Written {
		t.Fatalf("wrote %d (%v)", n, err)
	}
	got, _ := os.ReadFile(disk)
	for i := 0; i < len(raw)/chunk; i++ {
		part := got[i*chunk : (i+1)*chunk]
		if plan.write[i] && !bytes.Equal(part, raw[i*chunk:(i+1)*chunk]) {
			t.Fatalf("chunk %d differs", i)
		}
		if !plan.write[i] && part[0] != 0xee {
			t.Fatalf("chunk %d was written though skipped", i)
		}
	}
}

func TestPlanWritesNonZeroInEmptySlot(t *testing.T) {
	path, raw := gptImage(t)
	raw[50<<20] = 1 // something in the "empty" slot after all
	var gz bytes.Buffer
	z := gzip.NewWriter(&gz)
	z.Write(raw)
	z.Close()
	os.WriteFile(path, gz.Bytes(), 0o644)
	plan, _ := planImage(path, nil)
	if !plan.write[(50<<20)/chunk] {
		t.Fatal("a chunk with data was skipped")
	}
}

func withTrust(t *testing.T) ed25519.PrivateKey {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	old := trustJSON
	trustJSON, _ = json.Marshal(map[string]any{"keys": map[string]string{"test-key": hex.EncodeToString(pub)}})
	t.Cleanup(func() { trustJSON = old })
	return priv
}

func envelope(priv ed25519.PrivateKey, note cardNote) []byte {
	signed, _ := json.Marshal(note)
	out, _ := json.Marshal(map[string]string{"key_id": "test-key", "signed": base64.StdEncoding.EncodeToString(signed),
		"signature": base64.StdEncoding.EncodeToString(ed25519.Sign(priv, signed))})
	return out
}

func TestSignedNote(t *testing.T) {
	priv := withTrust(t)
	note := cardNote{Schema: 1, Kind: "rgds-card-image", Name: "RG-DS-OS-9.9.9.img.gz", Version: "9.9.9", Size: 10,
		Sha256: strings.Repeat("ab", 32)}
	env := envelope(priv, note)
	got, err := verifyNote(env, note.Name)
	if err != nil || got.Sha256 != note.Sha256 {
		t.Fatalf("a good note failed: %v", err)
	}
	if _, err := verifyNote(env, "RG-DS-OS-Plus-9.9.9.img.gz"); err == nil {
		t.Fatal("a note for another image passed")
	}
	var e map[string]string
	json.Unmarshal(env, &e)
	signed, _ := base64.StdEncoding.DecodeString(e["signed"])
	e["signed"] = base64.StdEncoding.EncodeToString(bytes.Replace(signed, []byte("abab"), []byte("abac"), 1))
	tampered, _ := json.Marshal(e)
	if _, err := verifyNote(tampered, note.Name); err == nil {
		t.Fatal("a changed note passed")
	}
	_, other, _ := ed25519.GenerateKey(rand.Reader)
	if _, err := verifyNote(envelope(other, note), note.Name); err == nil {
		t.Fatal("a note signed by someone else passed")
	}
}

func TestLocalImageSignature(t *testing.T) {
	priv := withTrust(t)
	path, _ := gptImage(t)
	data, _ := os.ReadFile(path)
	sum := sha256.Sum256(data)
	note := cardNote{Schema: 1, Kind: "rgds-card-image", Name: filepath.Base(path), Size: int64(len(data)), Sha256: hex.EncodeToString(sum[:])}
	os.WriteFile(path+".sig", envelope(priv, note), 0o644)
	signed, err := checkLocal(path, func(string, int64, int64) {})
	if err != nil || !signed {
		t.Fatalf("signed %v, %v", signed, err)
	}
	data[100] ^= 1
	os.WriteFile(path, data, 0o644)
	if _, err := checkLocal(path, func(string, int64, int64) {}); err == nil {
		t.Fatal("a changed image passed its signature")
	}
}

func TestPresetIni(t *testing.T) {
	ini, err := Preset{Language: "nb-NO", Timezone: "Europe/Oslo", Name: "Andreas", SSID: "IEEE", Password: "password"}.ini()
	if err != nil {
		t.Fatal(err)
	}
	text := string(ini)
	// IEEE 802.11i's test vector, as wpa_passphrase prints it
	for _, want := range []string{"language=nb-NO\n", "timezone=Europe/Oslo\n", "name=Andreas\n", "wifi_ssid=49454545\n",
		"wifi_psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n"} {
		if !strings.Contains(text, want) {
			t.Fatalf("%q lacks %q", text, want)
		}
	}
	if strings.Contains(text, "password") {
		t.Fatal("the password itself is in the preset")
	}
	for _, bad := range []Preset{{Language: "nb"}, {Timezone: "../etc"}, {Name: "far too long a name"}, {Name: "a\nb"},
		{SSID: "x", Password: "short"}} {
		if _, err := bad.ini(); err == nil {
			t.Fatalf("%+v passed", bad)
		}
	}
	if _, err := (Preset{SSID: "x", Password: "pässwörd123"}).ini(); err == nil {
		t.Fatal("a password WPA can't take passed")
	}
	open, _ := Preset{SSID: "Café"}.ini()
	if !strings.Contains(string(open), "wifi_open=1") || !strings.Contains(string(open), "wifi_ssid=436166c3a9") {
		t.Fatalf("open network: %q", open)
	}
}

func TestPresetJoinsBundle(t *testing.T) {
	cache := t.TempDir()
	preset := filepath.Join(cache, "preset.ini")
	os.WriteFile(preset, []byte("format=1\nname=X\n"), 0o600)
	out, err := withPreset("", preset)
	if err != nil {
		t.Fatal(err)
	}
	files, err := readBundle(out)
	if err != nil || string(files["preset.ini"].data) != "format=1\nname=X\n" || len(files) != 1 {
		t.Fatalf("%v %v", files, err)
	}
}

// A backup of an RG DS OS card goes back exactly: its empty slot is not skipped, so nothing of the card it goes
// onto stays behind.
func TestBackupOfImageGoesBackExactly(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	_, raw := gptImage(t)
	old := filepath.Join(t.TempDir(), "old")
	os.WriteFile(old, raw, 0o644)
	f, _ := os.OpenFile(old, os.O_RDWR, 0)
	defer f.Close()
	path := backupPath("Old")
	if err := backupCard(fileTarget{f}, int64(len(raw)), "Old", path, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	plan, _ := planFor(path, func(string, int64, int64) {})
	if plan.write != nil {
		t.Fatal("a backup was planned")
	}
	disk := filepath.Join(t.TempDir(), "new")
	os.WriteFile(disk, bytes.Repeat([]byte{0xee}, len(raw)), 0o644)
	g, _ := os.OpenFile(disk, os.O_RDWR, 0)
	defer g.Close()
	if _, err := writeImage(path, fileTarget{g}, plan, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(disk); !bytes.Equal(got, raw) {
		t.Fatal("the card is not the old one again")
	}
}

func TestBackupRoundTrip(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv("USERPROFILE", os.Getenv("HOME"))
	const size = 5*chunk + 4096
	f := sparseDisk(t, size, 0x42)
	path := backupPath("Generic: SD/MMC?")
	if strings.ContainsAny(filepath.Base(path), ":/?") {
		t.Fatalf("unsafe name %q", path)
	}
	if err := backupCard(fileTarget{f}, size, "Generic SD/MMC", path, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	list := listBackups()
	if len(list) != 1 || list[0].Unpacked != size || list[0].Card != "Generic SD/MMC" {
		t.Fatalf("backups %+v", list)
	}
	if n, _ := unpackedSize(path); n != size {
		t.Fatalf("unpackedSize %d", n)
	}
	if err := checkBackup(context.Background(), path, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	if again := backupPath("Generic: SD/MMC?"); again == path || !strings.Contains(again, "(2)") {
		t.Fatalf("a second backup in the same minute goes to %q", again)
	}
	// it goes back byte for byte
	disk := filepath.Join(t.TempDir(), "new")
	os.WriteFile(disk, bytes.Repeat([]byte{9}, size), 0o644)
	g, _ := os.OpenFile(disk, os.O_RDWR, 0)
	defer g.Close()
	plan, err := planFor(path, func(string, int64, int64) {})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := writeImage(path, fileTarget{g}, plan, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	a, b := make([]byte, size), make([]byte, size)
	f.ReadAt(a, 0)
	g.ReadAt(b, 0)
	if !bytes.Equal(a, b) {
		t.Fatal("the backup did not go back as it was")
	}
}
