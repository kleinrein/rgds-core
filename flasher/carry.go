package main

// Coming from BananaOS. Its card has the same shape as RG DS OS's (boot, two root slots, exFAT data), so flashing
// erases it: our larger root slots land on the start of its data partition. Before that, while the card is still
// mounted and readable without admin rights, the flasher keeps what can't be downloaded again:
//
//   - a backup folder on this computer, "RG DS backup/BananaOS <date>": saves/, states/, drastic/backup/, bios/,
//     banana/playstats.tsv, favorites.txt, systems.conf, and manifest.json (the games it had, by name and size);
//     with games ticked also roms/, under RG DS OS's folder names (megadrive is md), ready to copy back;
//   - a bundle on the new card: the same files minus the games, as a gzip'd tar written past the image near the
//     card's end, with a one-line header at the start of the last MiB ("RGDS-CARRY 1 <offset> <length> <sha256>").
//     The first boot copies it in (os/buildroot/rootfs/usr/libexec/rgds/handoff), and setup's Bring over step
//     puts the saves and play time where RG DS OS keeps them (runtime/migrate.py).
//
// The bundle goes on before the image, so a fake card that wraps its end onto its start is caught by the image's
// own read-back, and it is read back again after the image, so the image can't have spoiled it either.

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Carry is what an old system's card holds that can come along, as the card list shows it.
type Carry struct {
	Source    string `json:"source"` // what the person knows it as: BananaOS, the stock system
	Kind      string `json:"kind"`   // bananaos or stock (the manifest's source, migrate.py's layout)
	Saves     int    `json:"saves"`
	States    int    `json:"states"`
	Played    int    `json:"played"` // games with play time
	Hours     int64  `json:"hours"`
	Games     int    `json:"games"`
	GameBytes int64  `json:"gameBytes"`
	root      string
}

// Carried is what the done screen tells: where the backup is, and whether the card has the bundle.
type Carried struct {
	Folder    string `json:"folder"`
	OnCard    bool   `json:"onCard"`
	Games     int    `json:"games"`
	GamesLeft int    `json:"gamesLeft"` // games that are neither in the backup nor on the new card
	DSSaves   int    `json:"dsSaves"`   // the stock system's DS saves, read off its ext4 partitions at flash time
}

const (
	mib         = 1 << 20
	carryLimit  = 512 * mib // states make bundles big; past this they stay in the backup only
	carryMagic  = "RGDS-CARRY 1"
	carryMargin = 512 * mib // room after the image for the data partition's own start
)

// What each old system keeps that the game files are not, relative to its data or games partition. The stock
// system's DS saves are not here: DraStic keeps them on an ext4 partition, read at flash time (ext4.go).
var carryParts = map[string][]string{
	"bananaos": {"saves", "states", "drastic/backup", "bios", "banana/playstats.tsv", "banana/favorites.txt", "banana/systems.conf"},
	"stock":    {"saves_RA", "states_RA", "save"},
}

// Each old system's games folder and the names its system folders have where RG DS OS's differ (a missing name is
// the same in lower case; "" stays behind). Anbernic's stock names: FC, SFC, MD, PS... (migrate.py STOCK_FOLDERS).
var gameFolders = map[string]string{"bananaos": "roms", "stock": "Roms"}
var ourFolders = map[string]map[string]string{
	"bananaos": {"megadrive": "md", "genesis": "md", "pico-8": "pico8", "ports": ""},
	"stock": {"fc": "nes", "sfc": "snes", "sega32x": "md", "mdcd": "segacd", "gg": "sms", "ps": "psx", "dreamcast": "dc",
		"a2600": "atari", "a5200": "atari5200", "a7800": "atari7800", "ws": "wonderswan", "pico": "pico8",
		"ports": "", "apps": "", "mame": "", "fbneo": "", "neogeo": "", "cps1": "", "cps2": "", "cps3": "", "openbor": ""},
}

var notGame = map[string]bool{".srm": true, ".sav": true, ".dsv": true, ".state": true, ".auto": true, ".bak": true,
	".xml": true, ".png": true, ".jpg": true, ".txt": true, ".cfg": true}

func isDir(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

// The card list is asked for every 1.5 s while it shows; counting a card's games walks all of it, so each mount's
// answer is kept for a while (a card swapped in another reader comes back under a new mount point anyway).
var carrySeen = struct {
	sync.Mutex
	at  map[string]time.Time
	got map[string]*Carry
}{at: map[string]time.Time{}, got: map[string]*Carry{}}

// detectCarry finds a BananaOS data partition among a card's mount points.
func detectCarry(mounts []string) *Carry {
	key := strings.Join(mounts, "\x00")
	carrySeen.Lock()
	if at, ok := carrySeen.at[key]; ok && time.Since(at) < 20*time.Second {
		c := carrySeen.got[key]
		carrySeen.Unlock()
		return c
	}
	carrySeen.Unlock()
	c := findCarry(mounts)
	carrySeen.Lock()
	carrySeen.at[key], carrySeen.got[key] = time.Now(), c
	carrySeen.Unlock()
	return c
}

func findCarry(mounts []string) *Carry {
	for _, m := range mounts {
		// Anbernic's stock games partition (/mnt/mmc): Roms/ with RetroArch's saves_RA beside it, or a DS folder.
		// FAT and exFAT are case-blind, so "Roms" also finds BananaOS's roms: banana/ tells that one apart.
		if isDir(filepath.Join(m, "Roms")) && !isDir(filepath.Join(m, "banana")) &&
			(isDir(filepath.Join(m, "saves_RA")) || isDir(filepath.Join(m, "states_RA")) || isDir(filepath.Join(m, "save")) ||
				isDir(filepath.Join(m, "Roms", "NDS"))) {
			c := &Carry{Source: "the stock system", Kind: "stock", root: m}
			for _, part := range []string{"saves_RA", "save"} {
				walk(filepath.Join(m, part), func(path string, _ fs.FileInfo) {
					switch strings.ToLower(filepath.Ext(path)) {
					case ".srm", ".sav", ".dsv":
						c.Saves++
					}
				})
			}
			walk(filepath.Join(m, "states_RA"), func(string, fs.FileInfo) { c.States++ })
			for _, g := range carryGames(m, "stock") {
				c.Games++
				c.GameBytes += g.size
			}
			return c
		}
		if !isDir(filepath.Join(m, "banana")) || !(isDir(filepath.Join(m, "saves")) || isDir(filepath.Join(m, "roms"))) {
			continue
		}
		c := &Carry{Source: "BananaOS", Kind: "bananaos", root: m}
		walk(filepath.Join(m, "saves"), func(string, fs.FileInfo) { c.Saves++ })
		walk(filepath.Join(m, "drastic/backup"), func(path string, _ fs.FileInfo) {
			if strings.HasSuffix(strings.ToLower(path), ".dsv") {
				c.Saves++
			}
		})
		walk(filepath.Join(m, "states"), func(string, fs.FileInfo) { c.States++ })
		for _, g := range carryGames(m, "bananaos") {
			c.Games++
			c.GameBytes += g.size
		}
		if text, err := os.ReadFile(filepath.Join(m, "banana/playstats.tsv")); err == nil {
			var seconds int64
			for _, line := range strings.Split(string(text), "\n") {
				cells := strings.Split(line, "\t")
				var s int64
				if len(cells) >= 4 {
					if _, err := fmt.Sscan(cells[2], &s); err == nil && s > 0 {
						c.Played++
						seconds += s
					}
				}
			}
			c.Hours = (seconds + 1800) / 3600
		}
		return c
	}
	return nil
}

// walk calls fn for every file under root, skipping hidden ones (macOS leaves ._ files on exFAT).
func walk(root string, fn func(path string, info fs.FileInfo)) {
	filepath.WalkDir(root, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if strings.HasPrefix(e.Name(), ".") && path != root {
			if e.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if e.Type().IsRegular() {
			if info, err := e.Info(); err == nil {
				fn(path, info)
			}
		}
		return nil
	})
}

type game struct {
	folder, rel string // Banana's folder, the path inside it (forward slashes)
	size        int64
}

func carryGames(root, kind string) []game {
	var games []game
	folders, _ := os.ReadDir(filepath.Join(root, gameFolders[kind]))
	for _, f := range folders {
		if !f.IsDir() || strings.HasPrefix(f.Name(), ".") || ourName(kind, f.Name()) == "" {
			continue
		}
		base := filepath.Join(root, gameFolders[kind], f.Name())
		walk(base, func(path string, info fs.FileInfo) {
			if notGame[strings.ToLower(filepath.Ext(path))] || strings.Contains(filepath.Base(path), ".state") {
				return
			}
			rel, _ := filepath.Rel(base, path)
			games = append(games, game{f.Name(), filepath.ToSlash(rel), info.Size()})
		})
	}
	return games
}

func ourName(kind, folder string) string {
	if to, ok := ourFolders[kind][strings.ToLower(folder)]; ok {
		return to
	}
	return strings.ToLower(folder)
}

// backupFolder is where this computer keeps the copy: next to the user's documents, where people look.
func backupFolder(source string) string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = os.TempDir()
	}
	return filepath.Join(home, "RG DS backup", strings.TrimPrefix(source, "the ")+" "+time.Now().Format("2006-01-02 1504"))
}

// keepCarry copies the old card's saves (and with games its games) to the backup folder and makes the bundle
// for the new card. It runs in the UI process, before the card is unmounted for writing.
func keepCarry(c *Carry, withGames bool, report reporter) (folder, bundle string, err error) {
	folder = backupFolder(c.Source)
	if err := os.MkdirAll(folder, 0o755); err != nil {
		return "", "", err
	}
	logf("carry: %s from %s to %s (games %v)", c.Source, c.root, folder, withGames)
	type item struct {
		from, to string
		size     int64
	}
	var items []item
	for _, part := range carryParts[c.Kind] {
		src := filepath.Join(c.root, filepath.FromSlash(part))
		walk(src, func(path string, info fs.FileInfo) {
			rel, _ := filepath.Rel(c.root, path)
			items = append(items, item{path, filepath.Join(folder, rel), info.Size()})
		})
	}
	games := carryGames(c.root, c.Kind)
	if withGames {
		for _, g := range games {
			items = append(items, item{filepath.Join(c.root, gameFolders[c.Kind], g.folder, filepath.FromSlash(g.rel)),
				filepath.Join(folder, "roms", ourName(c.Kind, g.folder), filepath.FromSlash(g.rel)), g.size})
		}
	}
	var total, done int64
	for _, it := range items {
		total += it.size
	}
	report("backup", 0, total)
	for _, it := range items {
		if err := copyFile(it.from, it.to); err != nil {
			return folder, "", fmt.Errorf("copying %s to this computer failed (%v). Is there room on it?", filepath.Base(it.from), err)
		}
		done += it.size
		report("backup", done, total)
	}
	// backup: where the flash adds what it reads at flash time (the stock system's DS saves), on this computer too
	manifest := map[string]any{"format": 1, "source": c.Kind, "backup": folder, "made": time.Now().UTC().Format(time.RFC3339), "roms": [][]any{}}
	list := [][]any{}
	for _, g := range games {
		list = append(list, []any{g.folder + "/" + g.rel, g.size})
	}
	manifest["roms"] = list
	text, _ := json.MarshalIndent(manifest, "", " ")
	if err := os.WriteFile(filepath.Join(folder, "manifest.json"), text, 0o644); err != nil {
		return folder, "", err
	}
	bundle = filepath.Join(cacheDir(), "carry.tar.gz")
	size, err := packCarry(folder, bundle, true)
	if err == nil && size > carryLimit {
		logf("carry: %d bytes with states; leaving the states in the backup", size)
		size, err = packCarry(folder, bundle, false)
	}
	if err == nil && size > carryLimit {
		err = fmt.Errorf("the saves are too big to put on the card (%d MB); they are in %s", size/mib, folder)
	}
	if err != nil {
		os.Remove(bundle)
		return folder, "", err
	}
	logf("carry: bundle %d bytes", size)
	return folder, bundle, nil
}

func copyFile(from, to string) error {
	if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
		return err
	}
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(to + ".part")
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		os.Remove(to + ".part")
		return err
	}
	if err := out.Close(); err != nil {
		os.Remove(to + ".part")
		return err
	}
	if info, err := in.Stat(); err == nil {
		os.Chtimes(to+".part", info.ModTime(), info.ModTime()) // which save is newest is decided by time
	}
	return os.Rename(to+".part", to)
}

// packCarry tars the backup folder (games left out, states only with states) into bundle; returns its size.
func packCarry(folder, bundle string, states bool) (int64, error) {
	f, err := os.Create(bundle)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	z := gzip.NewWriter(f)
	w := tar.NewWriter(z)
	err = filepath.WalkDir(folder, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(folder, path)
		rel = filepath.ToSlash(rel)
		if rel == "roms" || (!states && (rel == "states" || rel == "states_RA")) {
			return filepath.SkipDir
		}
		if !e.Type().IsRegular() {
			return nil
		}
		info, err := e.Info()
		if err != nil {
			return err
		}
		h := &tar.Header{Name: rel, Mode: 0o644, Size: info.Size(), ModTime: info.ModTime(), Typeflag: tar.TypeReg, Format: tar.FormatPAX}
		if err := w.WriteHeader(h); err != nil {
			return err
		}
		in, err := os.Open(path)
		if err != nil {
			return err
		}
		_, err = io.Copy(w, in)
		in.Close()
		return err
	})
	if err == nil {
		err = w.Close()
	}
	if err == nil {
		err = z.Close()
	}
	if err != nil {
		return 0, err
	}
	info, err := f.Stat()
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}

// carryPlace is where the bundle goes on a card of size bytes: the header at the start of the last MiB, the bundle
// right before it (on a MiB). The first boot puts the GPT's backup in the last 33 sectors, past both.
func carryPlace(size, length int64) (blob, header int64) {
	header = (size/mib - 1) * mib
	blob = header - (length+mib-1)/mib*mib
	return blob, header
}

// writeCarry puts the bundle on the card past the image (total bytes) and its header near the card's end.
func writeCarry(t target, size, total int64, bundle string, report reporter) error {
	data, err := os.ReadFile(bundle)
	if err != nil {
		return err
	}
	blob, header := carryPlace(size, int64(len(data)))
	if blob < total+carryMargin {
		return errors.New("the card is too small to take the saves along as well")
	}
	sum := sha256.Sum256(data)
	logf("carry: %d bytes at %d, header at %d", len(data), blob, header)
	buf := alignedBuffer(chunk)
	for off := 0; off < len(data); off += chunk {
		n := copy(buf, data[off:])
		if err := writeBlock(t, buf, n, blob+int64(off), report); err != nil {
			return err
		}
		report("carry", int64(off+n), int64(len(data)))
	}
	head := alignedBuffer(align)
	clear(head)
	copy(head, fmt.Sprintf("%s %d %d %s\n", carryMagic, blob, len(data), hex.EncodeToString(sum[:])))
	if err := writeBlock(t, head, align, header, report); err != nil {
		return err
	}
	return checkCarry(t, size, bundle, report)
}

// checkCarry reads the header and the bundle back from the card and compares them with the file.
func checkCarry(t target, size int64, bundle string, report reporter) error {
	data, err := os.ReadFile(bundle)
	if err != nil {
		return err
	}
	blob, header := carryPlace(size, int64(len(data)))
	sum := sha256.Sum256(data)
	want := fmt.Sprintf("%s %d %d %s\n", carryMagic, blob, len(data), hex.EncodeToString(sum[:]))
	head := alignedBuffer(align)
	if err := readBlock(t, head, align, header, report); err != nil {
		return err
	}
	if !bytes.HasPrefix(head, []byte(want)) {
		return errors.New("the saves did not read back from the card; this card may not be as big as it says")
	}
	buf := alignedBuffer(chunk)
	h := sha256.New()
	for off := 0; off < len(data); off += chunk {
		n := min(chunk, len(data)-off)
		if err := readBlock(t, buf, n, blob+int64(off), report); err != nil {
			return err
		}
		h.Write(buf[:n])
	}
	if !bytes.Equal(h.Sum(nil), sum[:]) {
		return errors.New("the saves did not read back from the card; this card may not be as big as it says")
	}
	return nil
}

// addStockSaves reads the stock system's DS saves off the card's ext4 partitions (DraStic's backup/, ext4.go) once
// the card is open and before anything is written, puts them in the backup folder on this computer and in a new
// bundle beside the old one. Any other bundle is returned as it is.
func addStockSaves(r io.ReaderAt, bundle string, report reporter) (string, error) {
	files, err := readBundle(bundle)
	if err != nil {
		return bundle, err
	}
	var manifest struct {
		Source string `json:"source"`
		Backup string `json:"backup"`
	}
	json.Unmarshal(files["manifest.json"].data, &manifest)
	if manifest.Source != "stock" {
		return bundle, nil
	}
	saves := stockDrasticSaves(r)
	logf("carry: %d DS saves on the stock card's ext4 partitions", len(saves))
	report("dssaves", int64(len(saves)), int64(len(saves)))
	if len(saves) == 0 {
		return bundle, nil
	}
	now := time.Now()
	for name, data := range saves {
		files[name] = bundled{data, now}
		if manifest.Backup != "" {
			to := filepath.Join(manifest.Backup, filepath.FromSlash(name))
			if err := os.MkdirAll(filepath.Dir(to), 0o755); err == nil {
				err = os.WriteFile(to, data, 0o644)
			}
			if err != nil {
				logf("carry: backup of %s: %v", name, err)
			}
		}
	}
	out := filepath.Join(os.TempDir(), "rgds-carry-ds.tar.gz")
	if err := writeBundle(out, files); err != nil {
		return bundle, err
	}
	return out, nil
}

type bundled struct {
	data []byte
	at   time.Time
}

func readBundle(path string) (map[string]bundled, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		return nil, err
	}
	r := tar.NewReader(z)
	files := map[string]bundled{}
	for {
		h, err := r.Next()
		if err == io.EOF {
			return files, nil
		}
		if err != nil {
			return nil, err
		}
		if h.Typeflag != tar.TypeReg {
			continue
		}
		data, err := io.ReadAll(r)
		if err != nil {
			return nil, err
		}
		files[h.Name] = bundled{data, h.ModTime}
	}
}

func writeBundle(path string, files map[string]bundled) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	z := gzip.NewWriter(f)
	w := tar.NewWriter(z)
	for name, b := range files {
		h := &tar.Header{Name: name, Mode: 0o644, Size: int64(len(b.data)), ModTime: b.at, Typeflag: tar.TypeReg, Format: tar.FormatPAX}
		if err = w.WriteHeader(h); err == nil {
			_, err = w.Write(b.data)
		}
		if err != nil {
			break
		}
	}
	if err == nil {
		err = w.Close()
	}
	if err == nil {
		err = z.Close()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// rescueStockSaves is for a card with no bundle: when its ext4 partitions hold DraStic's saves, they become one
// (a stock manifest with no games), with a copy in the backup folder; "" when there are none.
func rescueStockSaves(r io.ReaderAt, report reporter) string {
	saves := stockDrasticSaves(r)
	if len(saves) == 0 {
		return ""
	}
	logf("carry: %d DS saves on a card nobody chose to keep anything from; they come along", len(saves))
	folder := backupFolder("stock system")
	manifest, _ := json.MarshalIndent(map[string]any{"format": 1, "source": "stock", "backup": folder,
		"made": time.Now().UTC().Format(time.RFC3339), "roms": [][]any{}}, "", " ")
	now := time.Now()
	files := map[string]bundled{"manifest.json": {manifest, now}}
	for name, data := range saves {
		files[name] = bundled{data, now}
		to := filepath.Join(folder, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(to), 0o755); err == nil {
			err = os.WriteFile(to, data, 0o644)
			if err != nil {
				logf("carry: backup of %s: %v", name, err)
			}
		}
	}
	os.WriteFile(filepath.Join(folder, "manifest.json"), manifest, 0o644)
	out := filepath.Join(os.TempDir(), "rgds-carry-ds.tar.gz")
	if err := writeBundle(out, files); err != nil {
		logf("carry: %v", err)
		return ""
	}
	report("dssaves", int64(len(saves)), int64(len(saves)))
	report("dsbackup:"+folder, 0, 0)
	return out
}
