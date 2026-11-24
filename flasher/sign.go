package main

// Signed card images. Each release image comes with <image>.sig: an Ed25519-signed note (the same envelope and the
// same pinned key as the handheld's own updates, packaging/update-trust.json, copied here as trust.json by
// build.sh) that names the image, its size and its sha256. A .sha256 file next to an image only proves the download
// matches whatever sits beside it on the server. The signature proves the image is ours: someone who could swap
// both on GitHub still can't make a note the flasher accepts.

import (
	"crypto/ed25519"
	_ "embed"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
)

//go:embed trust.json
var trustJSON []byte

// cardNote is what tools/own_os_release.py signs for each card image.
type cardNote struct {
	Schema   int    `json:"schema"`
	Kind     string `json:"kind"` // "rgds-card-image"
	Name     string `json:"name"`
	Version  string `json:"version"`
	Size     int64  `json:"size"`     // of the .img.gz
	Unpacked int64  `json:"unpacked"` // of the image
	Sha256   string `json:"sha256"`   // of the .img.gz
}

func trustedKeys() map[string]ed25519.PublicKey {
	var trust struct {
		Keys map[string]string `json:"keys"`
	}
	json.Unmarshal(trustJSON, &trust)
	keys := map[string]ed25519.PublicKey{}
	for id, h := range trust.Keys {
		if k, err := hex.DecodeString(h); err == nil && len(k) == ed25519.PublicKeySize {
			keys[id] = k
		}
	}
	return keys
}

// verifyNote checks a .sig envelope for the image called name and returns what it signs.
func verifyNote(envelope []byte, name string) (*cardNote, error) {
	var env struct {
		KeyID     string `json:"key_id"`
		Signed    string `json:"signed"`
		Signature string `json:"signature"`
	}
	if err := json.Unmarshal(envelope, &env); err != nil {
		return nil, errors.New("the image's signature file is not readable")
	}
	key, ok := trustedKeys()[env.KeyID]
	if !ok {
		return nil, fmt.Errorf("the image is signed with a key this flasher does not know (%q); get the latest RG DS Flasher", env.KeyID)
	}
	signed, err1 := base64.StdEncoding.DecodeString(env.Signed)
	sig, err2 := base64.StdEncoding.DecodeString(env.Signature)
	if err1 != nil || err2 != nil || !ed25519.Verify(key, signed, sig) {
		return nil, errors.New("the image's signature is not valid: this image may not be from RG DS. Nothing was written")
	}
	var note cardNote
	if err := json.Unmarshal(signed, &note); err != nil || note.Schema != 1 || note.Kind != "rgds-card-image" {
		return nil, errors.New("the image's signature is for something else")
	}
	if note.Name != name || parseSum(note.Sha256) == "" {
		return nil, fmt.Errorf("the signature is for %s, not %s", note.Name, name)
	}
	return &note, nil
}
