package main

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// Image is one card image the UI offers: a release asset, a cached copy of one, or the file given with --image.
type Image struct {
	Model    string `json:"model"` // "rgds", "plus", "file" or "backup:<file name>" (backup.go)
	Name     string `json:"name"`
	Size     int64  `json:"size"` // download size
	Cached   bool   `json:"cached"`
	Unpacked int64  `json:"unpacked,omitempty"` // a backup's card size
	Card     string `json:"card,omitempty"`     // the card a backup came from
	Made     string `json:"made,omitempty"`
	Signed   bool   `json:"signed"` // the release has a signed note for it (sign.go)
	url      string
	sumURL   string
	sigURL   string // the signed note (sign.go); "" for releases from before images were signed
	path     string // set for a cached or local image
}

type Release struct {
	Version string  `json:"version"`
	Images  []Image `json:"images"`
	Offline bool    `json:"offline"` // GitHub could not be reached; only cached images are offered
	Backups []Image `json:"backups"` // whole-card backups this computer has (backup.go)
}

var assetName = regexp.MustCompile(`^RG-DS-OS-(Plus-)?(\d[0-9A-Za-z.\-]*)\.img\.gz$`)

func cacheDir() string {
	base, err := os.UserCacheDir()
	if err != nil {
		base = os.TempDir()
	}
	dir := filepath.Join(base, "RG DS Flasher")
	os.MkdirAll(dir, 0o755)
	return dir
}

func model(plus string) string {
	if plus != "" {
		return "plus"
	}
	return "rgds"
}

var client = &http.Client{Timeout: 0}

func fetchRelease(ctx context.Context) (*Release, error) {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", "https://api.github.com/repos/"+repo+"/releases/latest", nil)
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "rgds-flasher/"+version)
	resp, err := client.Do(req)
	if err != nil {
		return cachedRelease(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		// The API allows 60 calls an hour per address, which a shared connection runs out of; the release page
		// redirect has no such limit and the asset names follow from the tag.
		if rel, err := releaseFromRedirect(ctx); err == nil {
			return rel, nil
		}
		return cachedRelease(fmt.Errorf("GitHub answered %s", resp.Status))
	}
	var body struct {
		Tag    string `json:"tag_name"`
		Assets []struct {
			Name string `json:"name"`
			Size int64  `json:"size"`
			URL  string `json:"browser_download_url"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return cachedRelease(err)
	}
	sums, sigs := map[string]string{}, map[string]string{}
	for _, a := range body.Assets {
		if strings.HasSuffix(a.Name, ".img.gz.sha256") {
			sums[strings.TrimSuffix(a.Name, ".sha256")] = a.URL
		}
		if strings.HasSuffix(a.Name, ".img.gz.sig") {
			sigs[strings.TrimSuffix(a.Name, ".sig")] = a.URL
		}
	}
	rel := &Release{Version: strings.TrimPrefix(body.Tag, "v")}
	for _, a := range body.Assets {
		m := assetName.FindStringSubmatch(a.Name)
		if m == nil || sums[a.Name] == "" {
			continue
		}
		img := Image{Model: model(m[1]), Name: a.Name, Size: a.Size, url: a.URL, sumURL: sums[a.Name], sigURL: sigs[a.Name], Signed: sigs[a.Name] != ""}
		path := filepath.Join(cacheDir(), a.Name)
		if info, err := os.Stat(path); err == nil && info.Size() == a.Size && isFile(path+".sha256") {
			img.Cached, img.path = true, path
		}
		rel.Images = append(rel.Images, img)
	}
	sortImages(rel.Images)
	if len(rel.Images) == 0 {
		return nil, fmt.Errorf("the latest release (%s) has no card images", body.Tag)
	}
	return rel, nil
}

func releaseFromRedirect(ctx context.Context) (*Release, error) {
	noFollow := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	req, _ := http.NewRequestWithContext(ctx, "HEAD", "https://github.com/"+repo+"/releases/latest", nil)
	req.Header.Set("User-Agent", "rgds-flasher/"+version)
	resp, err := noFollow.Do(req)
	if err != nil {
		return nil, err
	}
	resp.Body.Close()
	loc := resp.Header.Get("Location")
	i := strings.LastIndex(loc, "/tag/")
	if i < 0 {
		return nil, errors.New("no latest release")
	}
	tag := loc[i+5:]
	ver := strings.TrimPrefix(tag, "v")
	rel := &Release{Version: ver}
	for _, plus := range []string{"", "Plus-"} {
		name := "RG-DS-OS-" + plus + ver + ".img.gz"
		url := "https://github.com/" + repo + "/releases/download/" + tag + "/" + name
		img := Image{Model: model(plus), Name: name, url: url, sumURL: url + ".sha256", sigURL: url + ".sig"}
		path := filepath.Join(cacheDir(), name)
		if isFile(path) && isFile(path+".sha256") {
			info, _ := os.Stat(path)
			img.Cached, img.path, img.Size = true, path, info.Size()
		}
		rel.Images = append(rel.Images, img)
	}
	return rel, nil
}

// cachedRelease offers the images downloaded earlier when GitHub cannot be reached.
func cachedRelease(cause error) (*Release, error) {
	entries, _ := os.ReadDir(cacheDir())
	rel := &Release{Offline: true}
	for _, e := range entries {
		m := assetName.FindStringSubmatch(e.Name())
		path := filepath.Join(cacheDir(), e.Name())
		info, err := e.Info()
		if m == nil || err != nil || !isFile(path+".sha256") {
			continue
		}
		if rel.Version != "" && rel.Version != m[2] {
			continue // the cache keeps one version; anything else is left over from an interrupted clean-up
		}
		rel.Version = m[2]
		rel.Images = append(rel.Images, Image{Model: model(m[1]), Name: e.Name(), Size: info.Size(), Cached: true, path: path})
	}
	if len(rel.Images) == 0 {
		return nil, fmt.Errorf("can't reach GitHub (%v)", cause)
	}
	sortImages(rel.Images)
	return rel, nil
}

func localRelease(path string) *Release {
	info, _ := os.Stat(path)
	return &Release{Version: "", Images: []Image{{Model: "file", Name: filepath.Base(path), Size: info.Size(), Cached: true, path: path}}}
}

func sortImages(images []Image) {
	if len(images) == 2 && images[0].Model == "plus" {
		images[0], images[1] = images[1], images[0]
	}
}

// fetchImage makes img available in the cache and checks it against its signed note (or, for a release from before
// signing, its .sha256); progress reports bytes. signed says the image is RG DS's own, by its signature.
func fetchImage(ctx context.Context, img Image, progress func(phase string, done, total int64)) (path string, signed bool, err error) {
	if img.Model == "file" {
		signed, err := checkLocal(img.path, progress)
		return img.path, signed, err
	}
	if strings.HasPrefix(img.Model, "backup:") {
		return img.path, false, checkBackup(ctx, img.path, progress)
	}
	dir := cacheDir()
	final := filepath.Join(dir, img.Name)
	want, signed := "", false
	if note, err := imageNote(ctx, img, final); err != nil {
		return "", false, err
	} else if note != nil {
		want, signed = note.Sha256, true
		if img.Size == 0 {
			img.Size = note.Size
		}
	} else if img.sumURL != "" {
		text, err := getSmall(ctx, img.sumURL)
		if err != nil {
			return "", false, fmt.Errorf("can't download the checksum: %w", err)
		}
		want = parseSum(text)
		if want == "" {
			return "", false, errors.New("the release's .sha256 file is not a checksum")
		}
	} else {
		text, _ := os.ReadFile(final + ".sha256") // offline: the checksum saved with the download
		want = parseSum(string(text))
	}
	logf("image %s: sha256 %s, signed %v", img.Name, want, signed)
	if info, err := os.Stat(final); err == nil && (img.Size == 0 || info.Size() == img.Size) {
		got, err := hashFile(ctx, final, func(done, total int64) { progress("check", done, total) })
		if err != nil {
			return "", false, err
		}
		if got == want {
			return final, signed, nil
		}
		os.Remove(final)
		if img.url == "" {
			return "", false, errors.New("the saved image is damaged and GitHub can't be reached to download it again")
		}
	}
	part := final + ".part"
	for try := 0; try < 5; try++ { // a dropped connection resumes where it stopped
		if err = download(ctx, img, part, progress); err == nil || ctx.Err() != nil {
			break
		}
		select {
		case <-ctx.Done():
		case <-time.After(time.Duration(2*(try+1)) * time.Second):
		}
	}
	if err != nil {
		return "", false, err
	}
	got, err := hashFile(ctx, part, func(done, total int64) { progress("check", done, total) })
	if err != nil {
		return "", false, err
	}
	if got != want {
		os.Remove(part)
		return "", false, errors.New("the download was damaged (checksum mismatch); try again")
	}
	if err := os.Rename(part, final); err != nil {
		return "", false, err
	}
	os.WriteFile(final+".sha256", []byte(want+"  "+img.Name+"\n"), 0o644)
	if !signed {
		os.Remove(final + ".sig") // a note left from an earlier copy says nothing about this one
	}
	pruneCache(img.Name)
	return final, signed, nil
}

// imageNote fetches and checks the image's signed note, and keeps it beside the download (for offline use and the
// done screen). nil, nil: the release has no note (one from before signing) and none was kept.
func imageNote(ctx context.Context, img Image, final string) (*cardNote, error) {
	var envelope []byte
	if img.sigURL != "" {
		text, err := getSmall(ctx, img.sigURL)
		switch {
		case err == nil:
			envelope = []byte(text)
		case err.Error() == "404 Not Found" && strings.Contains(img.sigURL, "/releases/download/"):
			// guessed from the tag (API rate limit): a release from before signing has none
		default:
			return nil, fmt.Errorf("can't download the image's signature: %w", err)
		}
	} else if img.url == "" { // offline: the note saved with the download, if it had one
		envelope, _ = os.ReadFile(final + ".sig")
	}
	if envelope == nil {
		return nil, nil
	}
	note, err := verifyNote(envelope, img.Name)
	if err != nil {
		return nil, err
	}
	os.WriteFile(final+".sig", envelope, 0o644)
	return note, nil
}

// download resumes into part when an earlier download stopped.
func download(ctx context.Context, img Image, part string, progress func(phase string, done, total int64)) error {
	f, err := os.OpenFile(part, os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	offset, _ := f.Seek(0, io.SeekEnd)
	if img.Size > 0 && offset >= img.Size {
		offset = 0
		f.Truncate(0)
	}
	req, _ := http.NewRequestWithContext(ctx, "GET", img.url, nil)
	req.Header.Set("User-Agent", "rgds-flasher/"+version)
	if offset > 0 {
		req.Header.Set("Range", fmt.Sprintf("bytes=%d-", offset))
	}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("download failed: %w", err)
	}
	defer resp.Body.Close()
	switch {
	case resp.StatusCode == 206 && offset > 0:
	case resp.StatusCode == 200:
		offset = 0
		f.Truncate(0)
	case resp.StatusCode == 416 && offset > 0: // already complete; the checksum decides
		return nil
	default:
		return fmt.Errorf("download failed: %s", resp.Status)
	}
	size := img.Size
	if size == 0 && resp.ContentLength > 0 {
		size = offset + resp.ContentLength
	}
	if _, err := f.Seek(offset, io.SeekStart); err != nil {
		return err
	}
	buf := make([]byte, 1<<20)
	done := offset
	progress("download", done, size)
	for {
		n, err := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				return werr
			}
			done += int64(n)
			progress("download", done, size)
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return fmt.Errorf("download stopped: %w", err)
		}
	}
	if size > 0 && done != size {
		return fmt.Errorf("the download stopped at %d of %d MB; try again to resume", done/1000000, size/1000000)
	}
	return f.Sync()
}

// pruneCache removes images of other versions, so the cache holds one release.
func pruneCache(keep string) {
	m := assetName.FindStringSubmatch(keep)
	entries, _ := os.ReadDir(cacheDir())
	for _, e := range entries {
		name := strings.TrimSuffix(strings.TrimSuffix(strings.TrimSuffix(e.Name(), ".sha256"), ".part"), ".sig")
		if o := assetName.FindStringSubmatch(name); o != nil && o[2] != m[2] {
			os.Remove(filepath.Join(cacheDir(), e.Name()))
		}
	}
}

// checkLocal verifies a --image file against a .sig (signed note) or .sha256 next to it, when there is one.
func checkLocal(path string, progress func(phase string, done, total int64)) (bool, error) {
	want, signed := "", false
	if envelope, err := os.ReadFile(path + ".sig"); err == nil {
		note, err := verifyNote(envelope, filepath.Base(path))
		if err != nil {
			return false, err
		}
		want, signed = note.Sha256, true
	} else if text, err := os.ReadFile(path + ".sha256"); err == nil {
		want = parseSum(string(text))
	} else {
		return false, nil
	}
	got, err := hashFile(context.Background(), path, func(done, total int64) { progress("check", done, total) })
	if err != nil {
		return false, err
	}
	if got != want {
		return false, fmt.Errorf("%s does not match its checksum", filepath.Base(path))
	}
	return signed, nil
}

func getSmall(ctx context.Context, url string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", url, nil)
	req.Header.Set("User-Agent", "rgds-flasher/"+version)
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", errors.New(resp.Status)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 4096))
	return string(body), err
}

func parseSum(text string) string {
	fields := strings.Fields(text)
	if len(fields) == 0 || len(fields[0]) != 64 {
		return ""
	}
	if _, err := hex.DecodeString(fields[0]); err != nil {
		return ""
	}
	return strings.ToLower(fields[0])
}

func hashFile(ctx context.Context, path string, progress func(done, total int64)) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	info, _ := f.Stat()
	h := sha256.New()
	buf := make([]byte, 4<<20)
	var done int64
	for {
		if ctx.Err() != nil {
			return "", ctx.Err()
		}
		n, err := f.Read(buf)
		h.Write(buf[:n])
		done += int64(n)
		progress(done, info.Size())
		if err == io.EOF {
			return hex.EncodeToString(h.Sum(nil)), nil
		}
		if err != nil {
			return "", err
		}
	}
}

// unpackedSize is the size of the raw image: the gzip trailer's ISIZE (the card images are far below 4 GiB), or
// the file size for an .img.
func unpackedSize(path string) (int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	info, _ := f.Stat()
	if b, ok := readInfo(path); ok { // a whole-card backup: gzip's own size field wraps at 4 GiB
		return b.Size, nil
	}
	if !strings.HasSuffix(strings.ToLower(path), ".gz") {
		return info.Size(), nil
	}
	var tail [4]byte
	if _, err := f.ReadAt(tail[:], info.Size()-4); err != nil {
		return 0, err
	}
	return int64(binary.LittleEndian.Uint32(tail[:])), nil
}
