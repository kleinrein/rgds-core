package main

// A whole-card backup before anything is erased: every byte of the old card, gzip'd, in "RG DS backup" in the
// person's home folder. With it, flashing is never a one-way door: the flasher lists the backups it finds there
// beside the release, and putting one back is an ordinary flash. So a stock card (or a BananaOS or ROCKNIX one)
// comes back exactly as it was, games and all.
//
// Beside each backup, <name>.info holds its unpacked size (gzip only keeps it mod 4 GiB, and cards are bigger),
// its checksum and which card it came from.

import (
	"compress/flate"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type backupInfo struct {
	Size   int64  `json:"size"`   // unpacked: the card's size
	Sha256 string `json:"sha256"` // of the .img.gz
	Card   string `json:"card"`
	Made   string `json:"made"`
}

func backupRoot() string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = os.TempDir()
	}
	return filepath.Join(home, "RG DS backup")
}

var unsafeName = regexp.MustCompile(`[^A-Za-z0-9 ._-]+`)

// backupPath is where a backup of the card goes; nothing is written there yet.
func backupPath(card string) string {
	name := strings.TrimSpace(unsafeName.ReplaceAllString(card, ""))
	if name == "" {
		name = "Card"
	}
	base := fmt.Sprintf("%s %s", name, time.Now().Format("2006-01-02 15.04"))
	path := filepath.Join(backupRoot(), base+".img.gz")
	for n := 2; isFile(path) || isFile(path+".part"); n++ { // never over an earlier backup
		path = filepath.Join(backupRoot(), fmt.Sprintf("%s (%d).img.gz", base, n))
	}
	return path
}

func readInfo(path string) (backupInfo, bool) {
	var info backupInfo
	data, err := os.ReadFile(path + ".info")
	if err != nil || json.Unmarshal(data, &info) != nil || info.Size <= 0 {
		return info, false
	}
	return info, true
}

// listBackups finds the whole-card backups made earlier, newest first.
func listBackups() []Image {
	entries, _ := os.ReadDir(backupRoot())
	var out []Image
	for _, e := range entries {
		path := filepath.Join(backupRoot(), e.Name())
		if !strings.HasSuffix(e.Name(), ".img.gz") || !isFile(path) {
			continue
		}
		info, ok := readInfo(path)
		if !ok {
			continue
		}
		st, _ := os.Stat(path)
		out = append(out, Image{Model: "backup:" + e.Name(), Name: strings.TrimSuffix(e.Name(), ".img.gz"), Size: st.Size(),
			Unpacked: info.Size, Card: info.Card, Made: info.Made, Cached: true, path: path})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Made > out[j].Made })
	return out
}

// checkBackup is a backup's own checksum check before it goes back on a card.
func checkBackup(ctx context.Context, path string, progress func(phase string, done, total int64)) error {
	info, ok := readInfo(path)
	if !ok {
		return errors.New("this backup has lost its .info file")
	}
	got, err := hashFile(ctx, path, func(done, total int64) { progress("check", done, total) })
	if err != nil {
		return err
	}
	if got != info.Sha256 {
		return errors.New("this backup is damaged (its checksum does not match)")
	}
	return nil
}

// backupCard reads the whole card into path (.img.gz) before the flash writes anything.
func backupCard(t target, size int64, card, path string, report reporter) (err error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	part := path + ".part"
	f, err := os.Create(part)
	if err != nil {
		return fmt.Errorf("can't make the backup: %w", err)
	}
	defer func() {
		f.Close()
		if err != nil {
			os.Remove(part)
		}
	}()
	h := sha256.New()
	z, _ := gzip.NewWriterLevel(io.MultiWriter(f, h), flate.BestSpeed) // empty space packs to nothing even so
	buf := alignedBuffer(chunk)
	report("backup-card", 0, size)
	for off := int64(0); off < size; off += chunk {
		n := int(min(int64(chunk), size-off))
		if err := readBlock(t, buf, n, off, report); err != nil {
			return err
		}
		if _, err := z.Write(buf[:n]); err != nil {
			return backupWriteErr(err)
		}
		report("backup-card", off+int64(n), size)
	}
	if err := z.Close(); err != nil {
		return backupWriteErr(err)
	}
	if err := f.Sync(); err != nil {
		return backupWriteErr(err)
	}
	f.Close()
	if err := os.Rename(part, path); err != nil {
		return err
	}
	info, _ := json.MarshalIndent(backupInfo{Size: size, Sha256: hex.EncodeToString(h.Sum(nil)), Card: card,
		Made: time.Now().Format(time.RFC3339)}, "", " ")
	if err := os.WriteFile(path+".info", info, 0o644); err != nil {
		return err
	}
	giveBack(path, path+".info")
	logf("backup: %d bytes of %s to %s", size, card, path)
	return nil
}

func backupWriteErr(err error) error {
	if errors.Is(err, syscall.ENOSPC) {
		return errors.New("this computer ran out of space for the backup. Nothing was written to the card. Free some space, or flash without the backup")
	}
	return fmt.Errorf("the backup could not be saved (%v). Nothing was written to the card", err)
}

// giveBack hands files a root worker made (Linux, pkexec) to the person who asked for them.
func giveBack(paths ...string) {
	uid, err := strconv.Atoi(os.Getenv("PKEXEC_UID"))
	if err != nil || os.Geteuid() != 0 {
		return
	}
	for _, p := range append(paths, filepath.Dir(paths[0])) {
		os.Lchown(p, uid, uid)
	}
}
