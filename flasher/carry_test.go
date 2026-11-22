package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func put(t *testing.T, path, text string) {
	t.Helper()
	os.MkdirAll(filepath.Dir(path), 0o755)
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

// bananaCard is a BananaOS data partition as its docs describe it.
func bananaCard(t *testing.T) string {
	root := filepath.Join(t.TempDir(), "BANANA")
	put(t, filepath.Join(root, "roms/nds/Mario Kart DS.nds"), strings.Repeat("N", 100))
	put(t, filepath.Join(root, "roms/megadrive/Sonic.md"), "MD")
	put(t, filepath.Join(root, "roms/ports/PortMaster.sh"), "#!/bin/sh")
	put(t, filepath.Join(root, "roms/nds/._Mario Kart DS.nds"), "apple double")
	put(t, filepath.Join(root, "saves/nds/Mario Kart DS.sav"), "save")
	put(t, filepath.Join(root, "saves/Sonic.srm"), "srm")
	put(t, filepath.Join(root, "drastic/backup/Mario Kart DS.dsv"), "dsv")
	put(t, filepath.Join(root, "states/Sonic.state1"), "state")
	put(t, filepath.Join(root, "bios/bios7.bin"), "7")
	put(t, filepath.Join(root, "banana/playstats.tsv"), "nds/Mario Kart DS.nds\t3\t7200\t1700000000\n")
	put(t, filepath.Join(root, "banana/settings.conf"), "not carried")
	return root
}

func TestDetectCarryCountsBananaOS(t *testing.T) {
	root := bananaCard(t)
	if detectCarry([]string{t.TempDir()}) != nil {
		t.Fatal("an empty card is not BananaOS")
	}
	c := detectCarry([]string{root})
	if c == nil || c.Source != "BananaOS" {
		t.Fatalf("not found: %+v", c)
	}
	if c.Saves != 3 || c.States != 1 || c.Games != 2 || c.Played != 1 || c.Hours != 2 || c.GameBytes != 102 {
		t.Fatalf("counts %+v", c)
	}
}

func TestKeepCarryBacksUpAndBundlesWithoutGames(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("LOCALAPPDATA", t.TempDir())
	t.Setenv("USERPROFILE", os.Getenv("HOME"))
	c := detectCarry([]string{bananaCard(t)})
	folder, bundle, err := keepCarry(c, true, func(string, int64, int64) {})
	if err != nil {
		t.Fatal(err)
	}
	// the computer's copy has the games under RG DS OS's folder names; ports stay behind
	if _, err := os.Stat(filepath.Join(folder, "roms/md/Sonic.md")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(folder, "roms/ports")); err == nil {
		t.Fatal("ports were copied")
	}
	names := map[string]string{}
	f, _ := os.Open(bundle)
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	r := tar.NewReader(z)
	for {
		h, err := r.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(r)
		names[h.Name] = string(body)
	}
	for _, want := range []string{"saves/Sonic.srm", "drastic/backup/Mario Kart DS.dsv", "states/Sonic.state1", "bios/bios7.bin", "banana/playstats.tsv", "manifest.json"} {
		if _, ok := names[want]; !ok {
			t.Errorf("%s missing from the bundle (%v)", want, names)
		}
	}
	for name := range names {
		if strings.HasPrefix(name, "roms/") || name == "banana/settings.conf" {
			t.Errorf("%s should not be in the bundle", name)
		}
	}
	var manifest struct {
		Format int     `json:"format"`
		Source string  `json:"source"`
		Roms   [][]any `json:"roms"`
	}
	json.Unmarshal([]byte(names["manifest.json"]), &manifest)
	if manifest.Format != 1 || manifest.Source != "bananaos" || len(manifest.Roms) != 2 || manifest.Roms[1][0] != "nds/Mario Kart DS.nds" {
		t.Fatalf("manifest %+v", manifest)
	}
}

func TestCarryGoesPastTheImageAndReadsBack(t *testing.T) {
	const size = 2 << 30
	path := filepath.Join(t.TempDir(), "card")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	f.Truncate(size)
	bundle := filepath.Join(t.TempDir(), "carry.tar.gz")
	put(t, bundle, strings.Repeat("bundle!", 1000))
	if err := writeCarry(fileTarget{f}, size, size-100*mib, bundle, func(string, int64, int64) {}); err == nil {
		t.Fatal("a card with no room past the image took the saves")
	}
	if err := writeCarry(fileTarget{f}, size, 64*mib, bundle, func(string, int64, int64) {}); err != nil {
		t.Fatal(err)
	}
	blob, header := carryPlace(size, 7000)
	if header != size-mib || blob != size-2*mib {
		t.Fatalf("placed at %d, header %d", blob, header)
	}
	line := make([]byte, 120)
	f.ReadAt(line, header)
	if !strings.HasPrefix(string(line), fmt.Sprintf("RGDS-CARRY 1 %d 7000 ", blob)) {
		t.Fatalf("header %q", line)
	}
	// the image written over the bundle's place (a card that wraps) is caught
	f.WriteAt([]byte("spoilt"), blob+10)
	if err := checkCarry(fileTarget{f}, size, bundle, func(string, int64, int64) {}); err == nil {
		t.Fatal("a spoilt bundle passed")
	}
}

// stockCard is Anbernic's stock games partition (FAT, /mnt/mmc) as the computer sees it.
func stockCard(t *testing.T) string {
	root := filepath.Join(t.TempDir(), "ROMS")
	put(t, filepath.Join(root, "Roms/NDS/Mario Kart DS.nds"), strings.Repeat("N", 100))
	put(t, filepath.Join(root, "Roms/SFC/Zelda.sfc"), "Z")
	put(t, filepath.Join(root, "Roms/PORTS/Doom.sh"), "#!/bin/sh")
	put(t, filepath.Join(root, "saves_RA/Roms/SFC/Zelda.srm"), "srm")
	put(t, filepath.Join(root, "states_RA/Roms/SFC/Zelda.state1"), "state")
	return root
}

func TestStockCardCarriesRetroArchSavesAndDSSavesFromExt4(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("LOCALAPPDATA", t.TempDir())
	t.Setenv("USERPROFILE", os.Getenv("HOME"))
	c := detectCarry([]string{stockCard(t)})
	if c == nil || c.Kind != "stock" || c.Saves != 1 || c.States != 1 || c.Games != 2 {
		t.Fatalf("stock card seen as %+v", c)
	}
	folder, bundle, err := keepCarry(c, true, func(string, int64, int64) {})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(folder, "roms/snes/Zelda.sfc")); err != nil {
		t.Fatal("the games' copy keeps stock folder names:", err)
	}
	// at flash time: the old card's vendor partition (ext4) gives up DraStic's saves
	disk := card(fixture(t, "ext4-modern.img.gz"), true)
	var ds int64
	withDS, err := addStockSaves(bytes.NewReader(disk), bundle, func(phase string, done, _ int64) {
		if phase == "dssaves" {
			ds = done
		}
	})
	if err != nil || withDS == bundle || ds != 302 {
		t.Fatalf("%v, %s, %d DS saves", err, withDS, ds)
	}
	files, err := readBundle(withDS)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"manifest.json", "saves_RA/Roms/SFC/Zelda.srm", "states_RA/Roms/SFC/Zelda.state1",
		"vendor/deep/drastic_aarch64/backup/Mario Kart DS.dsv"} {
		if _, ok := files[want]; !ok {
			t.Errorf("%s missing from the bundle", want)
		}
	}
	if _, err := os.Stat(filepath.Join(folder, "vendor/deep/drastic_aarch64/backup/Pokemon Platinum.dsv")); err != nil {
		t.Error("the DS saves are not in the backup on this computer:", err)
	}
	// a BananaOS bundle is left as it is
	b := detectCarry([]string{bananaCard(t)})
	_, banana, err := keepCarry(b, false, func(string, int64, int64) {})
	if err != nil {
		t.Fatal(err)
	}
	if same, _ := addStockSaves(bytes.NewReader(disk), banana, func(string, int64, int64) {}); same != banana {
		t.Error("a BananaOS bundle was changed")
	}
}

func TestASystemCardNobodyKeptFromStillGivesUpItsDSSaves(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv("USERPROFILE", os.Getenv("HOME"))
	disk := card(fixture(t, "ext4-old.img.gz"), false)
	var folder string
	bundle := rescueStockSaves(bytes.NewReader(disk), func(phase string, _, _ int64) {
		if strings.HasPrefix(phase, "dsbackup:") {
			folder = strings.TrimPrefix(phase, "dsbackup:")
		}
	})
	if bundle == "" || folder == "" {
		t.Fatal("no bundle")
	}
	files, err := readBundle(bundle)
	if err != nil || len(files) != 303 || !strings.Contains(string(files["manifest.json"].data), `"source": "stock"`) {
		t.Fatalf("%v, %d files", err, len(files))
	}
	if _, err := os.Stat(filepath.Join(folder, "vendor/deep/drastic_aarch64/backup/Mario Kart DS.dsv")); err != nil {
		t.Fatal(err)
	}
	if rescueStockSaves(bytes.NewReader(make([]byte, 4<<20)), func(string, int64, int64) {}) != "" {
		t.Fatal("a blank card made a bundle")
	}
}
