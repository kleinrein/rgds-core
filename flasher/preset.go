package main

// Setup before the first start. The flasher asks for what the handheld's setup would ask first (language, time
// zone, the DS name, a Wi-Fi network) and puts the answers on the card as preset.ini in the same bundle the saves
// travel in (carry.go). On the first boot the handoff (os/buildroot/rootfs/usr/libexec/rgds/handoff) adds the
// network to wifi.conf, so the handheld is online from the first screen. Setup then takes the language, time zone
// and name and skips those steps (src/launcher/welcome_screen.hpp).
//
// The Wi-Fi password never travels as typed: what goes on the card is the WPA key made from it and the network
// name (PBKDF2-SHA1, 4096 rounds, as wpa_passphrase does), which is what wpa_supplicant keeps anyway.

import (
	"bytes"
	"crypto/pbkdf2"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"
)

// Preset is what the Set up step asks; empty fields are left to the handheld's own setup.
type Preset struct {
	Language string `json:"language"`
	Timezone string `json:"timezone"`
	Name     string `json:"name"`
	SSID     string `json:"ssid"`
	Password string `json:"password"`
}

var (
	languageCode = regexp.MustCompile(`^[a-z]{2}-[A-Z]{2}$`)
	zoneName     = regexp.MustCompile(`^[A-Za-z0-9_+\-]+(/[A-Za-z0-9_+\-]+)*$`)
)

const nameLimit = 10 // the DS firmware's nickname

func (p Preset) empty() bool {
	return p.Language == "" && p.Timezone == "" && strings.TrimSpace(p.Name) == "" && p.SSID == ""
}

// ini is the preset as the handoff and setup read it; it checks every field first.
func (p Preset) ini() ([]byte, error) {
	var b bytes.Buffer
	b.WriteString("# RG DS Flasher: setup answers for the first start\nformat=1\n")
	if p.Language != "" {
		if !languageCode.MatchString(p.Language) {
			return nil, errors.New("that language is not one the handheld has")
		}
		fmt.Fprintf(&b, "language=%s\n", p.Language)
	}
	if p.Timezone != "" {
		if !zoneName.MatchString(p.Timezone) || len(p.Timezone) > 64 {
			return nil, errors.New("that time zone is not one the handheld knows")
		}
		fmt.Fprintf(&b, "timezone=%s\n", p.Timezone)
	}
	if name := strings.TrimSpace(p.Name); name != "" {
		if utf8.RuneCountInString(name) > nameLimit || strings.ContainsAny(name, "\n\r=") {
			return nil, fmt.Errorf("the DS name can have %d letters at most", nameLimit)
		}
		fmt.Fprintf(&b, "name=%s\n", name)
	}
	if p.SSID != "" {
		if len(p.SSID) > 32 {
			return nil, errors.New("a Wi-Fi network name has 32 characters at most")
		}
		fmt.Fprintf(&b, "wifi_ssid=%s\n", hex.EncodeToString([]byte(p.SSID)))
		switch n := len(p.Password); {
		case n == 0:
			b.WriteString("wifi_open=1\n")
		case n == 64 && isHex(p.Password): // already a WPA key
			fmt.Fprintf(&b, "wifi_psk=%s\n", strings.ToLower(p.Password))
		case n >= 8 && n <= 63 && printableASCII(p.Password):
			key, err := pbkdf2.Key(sha1.New, p.Password, []byte(p.SSID), 4096, 32)
			if err != nil {
				return nil, err
			}
			fmt.Fprintf(&b, "wifi_psk=%s\n", hex.EncodeToString(key))
		case n >= 8 && n <= 63:
			return nil, errors.New("a Wi-Fi password can only have plain letters, digits and symbols (no accents)")
		default:
			return nil, errors.New("a Wi-Fi password has 8 to 63 characters")
		}
	}
	return b.Bytes(), nil
}

// printableASCII is what WPA takes as a passphrase (IEEE 802.11i): characters 32 to 126.
func printableASCII(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < 32 || s[i] > 126 {
			return false
		}
	}
	return true
}

func isHex(s string) bool {
	_, err := hex.DecodeString(s)
	return err == nil
}

// savePreset writes the preset for one flash, readable by this user only; "" when there is nothing to set up.
func savePreset(p Preset) (string, error) {
	if p.empty() {
		return "", nil
	}
	data, err := p.ini()
	if err != nil {
		return "", err
	}
	path := filepath.Join(cacheDir(), "preset.ini")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		return "", err
	}
	return path, nil
}

// withPreset adds preset.ini to the bundle (a new one when there is none) and returns the bundle to write.
func withPreset(bundle, preset string) (string, error) {
	data, err := os.ReadFile(preset)
	if err != nil {
		return bundle, err
	}
	files := map[string]bundled{}
	if bundle != "" {
		if files, err = readBundle(bundle); err != nil {
			return bundle, err
		}
	}
	files["preset.ini"] = bundled{data, time.Now().Truncate(time.Second)}
	out := filepath.Join(filepath.Dir(preset), "carry-preset.tar.gz")
	if err := writeBundle(out, files); err != nil {
		return bundle, err
	}
	return out, nil
}
