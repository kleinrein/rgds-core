#!/bin/sh
# Builds RG DS Flasher for every platform into flasher/dist/:
#
#   RG-DS-Flasher-Windows.exe     Windows x64 (one file, no installer)
#   RG-DS-Flasher-macOS.zip       RG DS Flasher.app, universal (Apple silicon + Intel); the .app stays next to it
#   RG-DS-Flasher-Linux-x64       Linux x64
#
#   flasher/build.sh [--test]     --test runs the Go tests first
#
# Needs Go (go.mod fetches the pinned toolchain). The macOS app is universal when lipo is there (macOS); elsewhere
# it ships two zips, one per architecture. CODESIGN_IDENTITY="Developer ID Application: ..." signs the app with the
# hardened runtime for notarization; without it the app is signed ad hoc.
set -eu
cd "$(dirname "$0")"
VERSION=$(cat ../VERSION 2>/dev/null || echo dev)
OUT=dist
NAME="RG DS Flasher"

# The UI embeds the launcher's brand and font: keep the copies in sync with assets/.
cp ../assets/brand/mark.svg ../assets/brand/lockup-light.svg ../assets/brand/lockup-dark.svg ui/brand/
cp ../assets/fonts/source/Nunito.ttf ../assets/fonts/source/OFL.txt ui/fonts/

[ "${1:-}" = "--test" ] && go test ./...
rm -rf "$OUT"
mkdir -p "$OUT"

build() { # GOOS GOARCH output [extra ldflags]
	echo "  $1/$2 -> $3"
	CGO_ENABLED=0 GOOS=$1 GOARCH=$2 go build -trimpath -ldflags "-s -w -X main.version=$VERSION ${4:-}" -o "$3" .
}

build windows amd64 "$OUT/RG-DS-Flasher-Windows.exe" "-H windowsgui"
build linux amd64 "$OUT/RG-DS-Flasher-Linux-x64"
build darwin arm64 "$OUT/flasher-darwin-arm64"
build darwin amd64 "$OUT/flasher-darwin-amd64"

app() { # bundle-dir binary...
	mkdir -p "$1/Contents/MacOS" "$1/Contents/Resources"
	if [ $# -gt 2 ]; then
		lipo -create "$2" "$3" -output "$1/Contents/MacOS/$NAME"
	else
		cp "$2" "$1/Contents/MacOS/$NAME"
	fi
	cp packaging/AppIcon.icns "$1/Contents/Resources/"
	sed "s/@VERSION@/$VERSION/g" packaging/Info.plist >"$1/Contents/Info.plist"
	command -v xattr >/dev/null && xattr -cr "$1"
	if [ -n "${CODESIGN_IDENTITY:-}" ]; then
		codesign --force --options runtime --timestamp -s "$CODESIGN_IDENTITY" "$1"
	elif command -v codesign >/dev/null; then
		codesign --force -s - "$1"
	fi
}

zipapp() { # bundle-dir zip
	if command -v ditto >/dev/null; then
		ditto -c -k --norsrc --noextattr --keepParent "$1" "$2"
	else
		(cd "$(dirname "$1")" && zip -qry "$(basename "$2")" "$(basename "$1")")
	fi
}

if command -v lipo >/dev/null; then
	app "$OUT/mac/$NAME.app" "$OUT/flasher-darwin-arm64" "$OUT/flasher-darwin-amd64"
	zipapp "$OUT/mac/$NAME.app" "$OUT/RG-DS-Flasher-macOS.zip"
	mv "$OUT/mac/$NAME.app" "$OUT/" # ready to open from here
else
	for arch in arm64 amd64; do
		app "$OUT/mac-$arch/$NAME.app" "$OUT/flasher-darwin-$arch"
		zipapp "$OUT/mac-$arch/$NAME.app" "$OUT/RG-DS-Flasher-macOS-$arch.zip"
	done
fi
rm -rf "$OUT"/flasher-darwin-* "$OUT"/mac*/

(cd "$OUT" && shasum -a 256 RG-DS-Flasher-* >SHA256SUMS 2>/dev/null || sha256sum RG-DS-Flasher-* >SHA256SUMS)
ls -l "$OUT"
