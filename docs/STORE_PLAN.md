# Store plan

Status: plan only (2026-11-26). Nothing here is built.

The store is our own catalog of apps, games and ports for **RG DS OS** (the Buildroot image). Every entry in it is a
package **we** build, test and maintain: our recipe, our archive, our signature, our install folder, our launch path
through the launcher's runner. It exists only on RG DS OS; nothing about it runs on any other firmware.

What the store is for:

1. Make the handheld worth having before anyone owns a single game: someone who just flashed the card finds useful
   things to do on day one.
2. Show off what only this hardware has: two screens, touch on the bottom one, the mic, rumble, the lid, Wi-Fi to
   other RG DS handhelds, USB-C to a PC.
3. Prove the app format. If our own store apps are plain app.ini packages, outside developers see that it works.

Not in the store: **Media, Music, the Sound toy and PictoChat stay built into the image** (owner, 2026-11-26).

**The first blocker**: an app on RG DS OS gets only the top panel today; the bottom screen shows our info page
(`docs/APPS.md`). A calculator keypad, a drawing pad or a flipbook cannot be built that way. Most of the launch list
(§4) needs a **two-screen app mode** first (§2.6), so it is phase 1.

## 0. The rule: no PortMaster

PortMaster stays its own thing with its own GUI (pugwash, `docs/PORTMASTER_PLAN.md` Stage A). The store has no
PortMaster content and no PortMaster plumbing:

- no harbourmaster calls, no PortMaster catalog or Multiverse reads, nothing read from or written to
  `/data/portmaster`;
- no PortMaster runtimes (its squashfs `python_3.11`, `love_11.5`, `mono`, …), no `gptokeyb`, no
  `runtime/portmaster_compat.py` (the focal debs and the 32-bit libmali layer). Store packages are therefore
  `arch=aarch64` and `gl=gles` only; the store's checker refuses `armhf` and `gl4es`;
- store games never appear on the **Ports** shelf, which stays PortMaster's (plus hand-copied app.ini apps, as
  `docs/APPS.md` says today).

When one of our entries is a game PortMaster also has (Doom engines, OpenTyrian, …), ours is a separate package built
from upstream source by our recipe, not a harbourmaster install and not a copy of a PortMaster zip.

`docs/PORTMASTER_PLAN.md` §10 Stage B ("our own store" as a native page over harbourmaster) is a different thing and
is not part of this plan. With the owner's 2026-11-26 rule it should be marked dropped there: PortMaster keeps
pugwash.

One small piece of shared code needs a move so the rule holds in the files too: the DRM lease shim the runner
preloads lives at `runtime/portmaster_files/kms/$LIB/librgds-drm-lease.so` (`port_runner.lease_shims`). It is ours
(`tools/build_compat_stubs.py`), only its folder name says PortMaster. Phase 0 moves it to `runtime/kms/` and keeps
PortMaster pointing at the new place.

## 1. What already exists and what we reuse

| Piece | Where | Use in the store |
|---|---|---|
| app.ini apps | `runtime/apps.py`, `docs/APPS.md` | The package's runtime manifest and the safe zip install (no `..`, no absolute paths, no symlinks, whole-folder replace with a power-loss journal, data folder kept) |
| The runner | `runtime/port_runner.py` | Every store package starts like an app today: one cgroup2 leaf (Home freezes it, Quit kills the tree), the top panel as a DRM lease, the bottom info screen, Home menu, game switcher, playtime, Recents |
| Homebrew shelves | `runtime/homebrew_store.py`, `homebrew_screen.hpp`, `tools/build_homebrew_feed.py` | The pattern: our own feed, cached with a bundled fallback, downloads pinned to immutable URLs with size and hash, `TRUSTED` URL prefixes, status fields in `status.ini`, the top card (cover left, title, chips, description, glitter bar). Folded in later (§11 phase 5) |
| Signed OTA channel | `docs/OTA.md`, `tools/release.py`, `tools/publish_update.py`, `runtime/update_crypto.py`, `runtime/ota.py` | Ed25519 signing (`publish_update.sign`), verify through OpenSSL EVP (`update_crypto.verify`), the signed envelope (`key_id`, `signed`, `signature`), sequence + expiry, resumable downloads, the 6-hour idle check and its gameplay gate |
| Owner files upload | `runtime/port_files.py` | The QR / web upload page for packages that need files the owner bought (Doom IWADs and similar). Pointed at the package's data folder instead of a PortMaster port folder |
| Transfer queue | `runtime/transfers.py` (network plan phase 1) | One queue for downloads, so store installs show beside other transfers and survive leaving the page |
| Storage view | `runtime/storage_usage.py` | A new `apps` category for the store folder |
| Profiles | `profile_store.hpp`, `profile_visibility.hpp`, `runtime/age_rating.py` | Age rules for store games, a per-profile Apps permission, Locked store for restricted profiles |
| Toasts, key hints | `toast.hpp`, `key_hints_pills.hpp` | Every result and every button tip |

## 2. Package format

**Decision: keep app.ini as the package's runtime manifest (schema 1, a few new optional keys) and put the store's
metadata in the signed catalog, generated from a `package.ini` in the recipe folder.** No second manifest format on
the handheld.

Why: app.ini already starts a program with everything the store needs (§1), its unknown keys never stop an older
launcher, and `apps.py check` already tells a packager what is wrong. What the store needs beyond running (size,
hash, screenshots, licence, board list) describes the *download*, not the installed folder, so it belongs in the
catalog the handheld verifies, not in a file inside the archive that the archive itself would have to vouch for.

### 2.1 New app.ini keys (all optional, schema stays 1)

| Key | Values | Meaning |
|---|---|---|
| `kind` | `app`, `game`, `port`, `content` | Where it is listed (§5). `port` = an engine for a known game (Doom, Quake). `content` = data for something already on the handheld, no program (§2.7). Default `app` |
| `store` | the store id | Set by the build tool. Marks the folder as store-owned; hand-copied apps never have it |
| `screens` | `top` (default), `both` | `both`: the app gets both panels and the bottom screen's touch (§2.6) |
| `rating` | `all`, `7`, `12`, `16`, `18` | Our age rating, set at review. Kid profiles see only what passes it (§8). Hand-copied apps without one are adults-only |
| `owner_files` | a path inside `$RGDS_APP_DATA`, e.g. `files/iwads` | The folder the owner's own files go to; the launcher shows "Needs your files" until `check_files` passes |
| `check_files` | file names, `\|` separated (any one is enough) | What must be in `owner_files` for the package to start, e.g. `doom.wad\|doom2.wad\|freedoom1.wad` |
| `network` | `0`, `1` | Needs Wi-Fi to be useful (radio, podcasts). Hidden for profiles without Wireless |

`min_core` stays the minimum version key: on RG DS OS the OS and RG DS Core share one version.

### 2.2 The catalog entry (in the signed index, made by the build tool)

```json
{
  "id": "crispy-doom",
  "kind": "port",
  "title": "Crispy Doom",
  "summary": "Doom with sharper pixels. Comes with Freedoom; add your own WADs.",
  "description": "…",
  "author": "Fabian Greffrath and contributors",
  "upstream": "https://github.com/fabiangreffrath/crispy-doom",
  "version": "7.0",
  "revision": 2,
  "min_core": "0.9.3",
  "abi": 1,
  "boards": ["rgds", "rgds-plus"],
  "network": false,
  "owner_files": "optional",
  "rating": "12",
  "licence": "GPL-2.0-or-later",
  "licence_assets": "BSD-3-Clause (Freedoom)",
  "source": "https://github.com/kleinrein/rgds-store/releases/download/crispy-doom-7.0-r2/crispy-doom-7.0-r2-source.tar.xz",
  "archive": {"url": "https://github.com/kleinrein/rgds-store/releases/download/crispy-doom-7.0-r2/crispy-doom.zip",
              "size": 14123456, "unpacked": 31000000, "sha256": "…"},
  "icon": {"url": "…/icon.png", "size": 9123, "sha256": "…"},
  "screenshots": [{"url": "…/shot-1.png", "size": 210000, "sha256": "…"}],
  "added": "2026-12-01",
  "updated": "2026-12-14",
  "notes": "Revision 2: the pad's Select opens the automap."
}
```

- `version` is upstream's; `revision` is ours and goes up with every rebuild. An update is a higher
  `(version, revision)` pair for the same id; a lower one is never offered.
- `boards`: where it is listed. One aarch64 build serves both; a package that does not fit the Plus's 1024x768 or
  the RG DS's 640x480 is left off that board, never labelled as untested there.
- `rating`: our own age rating (`all`, `7`, `12`, `16`, `18`), set at review, read by the existing age rule (§8).
- `source`: for every GPL/LGPL package the exact source we built, our patches included, as a release asset next to
  the binary. That is the licence's written-offer requirement met in the simplest way.
- `owner_files`: `none`, `optional` (runs without, e.g. Freedoom included) or `required` (does not start without).

### 2.3 Archive layout

A zip, deflated, one folder named after the id:

```
crispy-doom/
    app.ini              kind=port, store=crispy-doom, exec=run.sh, owner_files=files/iwads, …
    run.sh
    bin/crispy-doom
    lib/                 bundled shared libraries (real files, no symlinks: /data is exFAT)
    share/freedoom1.wad
    icon.png
    LICENSES/            every licence text that applies, and NOTICE with the upstream URLs
```

Why zip: `apps.py` already unpacks zips safely, Python's `zipfile` is in the image, and per-file reads let the
checker inspect a package without unpacking it. exFAT has no permissions or symlinks anyway, which is the main thing
tar would give us. xz-compressed tar would be ~15 % smaller; not worth a second unpacker.

### 2.4 Where it installs

**Decision: `/data/store/<id>/`, not `/data/apps/<id>/`.** `apps.py` learns a second root.

Why: `/data/apps` is the owner's folder: they copy things into it over file sharing, and today those land on the
Ports shelf. Keeping our packages out of it means a hand-copied app and a store package can never share an id, the
store can delete a whole folder on Remove knowing it put every file there, and the APPS share never shows store
folders an owner might edit by hand. The `store=` key in app.ini is a second check: the store only ever replaces or
removes a folder whose app.ini names it.

| Path | What | On update | On Remove |
|---|---|---|---|
| `/data/store/<id>/` | The package | Replaced whole (apps.py's journal) | Deleted |
| `/data/store/.downloads/` | `<id>-<version>-r<rev>.zip.part` while downloading | — | Partial download deleted |
| `/data/rgds/userdata/apps/<id>/` | `$RGDS_APP_DATA`: saves, config, owner files | **Kept** | **Kept** |
| `/data/rgds/state/store/installed.json` | What the store installed: id, version, revision, index sequence, sha256 of the archive, time | Updated | Entry removed |
| `/data/rgds/cache/store/` | The cached index, icons and screenshots as BMP | — | — |

`$RGDS_APP_DATA` follows whatever apps.py does for profiles today; the store adds nothing there.

### 2.5 What a package may rely on in the image

The image promises a list of libraries and programs, written at image build to `/usr/share/rgds/store-abi.txt`
with a number (`abi=1`). A package names the ABI number it was built for; anything not on the list is bundled in
`lib/` or `bin/`. The build tool checks every ELF's `NEEDED` entries against the list plus `lib/`, so a package that
would fail to start never gets published.

ABI 1, from what `rgds_os_defconfig` has today:

| Provided by the image | Bundle it |
|---|---|
| glibc, libstdc++, zlib, libpng, libjpeg, freetype, fontconfig + DejaVu | Anything else: SDL2_net, SDL2_gfx, libvorbis/ogg/opus unless confirmed, libcurl (the image has none), Lua, physfs, … |
| SDL2 (kmsdrm, GLES), SDL2_ttf, SDL2_image, SDL2_mixer | SDL 1.2 (port the game to SDL2 or use sdl12-compat bundled) |
| libdrm, Mesa Panfrost (EGL, GLES 2/3.x), ALSA | Desktop GL (no gl4es in the store), Vulkan packages wait until the patched PanVK is in the image |
| python3 (ssl, sqlite, zlib, xz, bz2, expat, decimal; `.pyc` only stdlib) | pygame, LÖVE, any pip package |
| the `mpv` program, openssl, bash, rsync | libmpv and ffmpeg's libraries: present as mpv's dependencies but not promised until we add them to the list on purpose |

Runtimes several packages want (LÖVE 11, pygame) are added **to the image**, as our own Buildroot packages, when the
second package needs one, and then join the ABI list as a new number. Until then a package bundles its own. This
keeps packages small without ever reaching for PortMaster's runtime squashfs files.

### 2.6 Two-screen apps (`screens=both`)

Today the runner leases the top panel to the program and the bottom one to the menu helper, which draws the info
screen (`drm_lease.PanelLeases`). With `screens=both` the program gets both leases (`RGDS_PANEL_TOP`,
`RGDS_PANEL_BOTTOM` name them; SDL opens two windows on the two KMS displays) and the bottom touch device, and the
helper stays mapped but idle until Home. Home still works the same way: freeze the cgroup, hand the bottom lease back
to the helper for the menu, return it on resume. The port Touch screen mode (`project-port-files-touch-2026-10-26`)
already passes touch to a port, so part of the plumbing exists. On the Plus both panels are 1024x768-class; the
variables give the real sizes. A host test plus one tiny two-screen sample app prove it before any real app is built.

### 2.7 Content packages (`kind=content`)

Content is ours or freely licensed data for something already on the handheld: no program, no runner. The package
has `app.ini` with `kind=content` and `target=`, and the store copies its `files/` there:

| `target` | Lands in | Examples |
|---|---|---|
| `themes`, `wallpapers`, `bezels`, `sounds`, `shaders` | the launcher's user folders for each | theme packs, startup sounds, bezel and shader packs |
| `books` | the Media app's books folder | Project Gutenberg books, Pepper&Carrot comics |
| `roms/<system>` | the system's games folder, with cover and info.ini as the homebrew shelves do | ScummVM freeware adventures, open homebrew ROMs, 240p Test Suite |

A content package records every file it placed in `installed.json`, and Remove deletes exactly those files, never a
file it did not put there and never a save next to it. ScummVM games keep their saves where ScummVM keeps them.

### 2.8 The app kit

Our apps should look like the launcher. The companion already builds launcher-looking screens from shared headers
(`project-companion-ui-2026-10-25`). Phase 2 packs those (cards, rows, key-hint badges, toasts, fonts, theme
colours, the two-screen setup) into `store/kit/`, a small C++/SDL2 static library every store app links. Later the
same kit is offered to outside developers with a sample app.

Input: store packages use SDL's game controller API (the runner sets `SDL_GAMECONTROLLERCONFIG`). A game that only
reads a keyboard gets a patch in its recipe; there is no `gptokeyb`. Panels: packages read `RGDS_PANEL_WIDTH` /
`RGDS_PANEL_HEIGHT` and draw for 640x480 or 1024x768.

## 3. Catalog and publishing

### 3.1 Where things live

- **Recipes** in this repository, `store/<id>/`, so they build with the same Buildroot sysroot and toolchain as the
  image (the Lima VM) and get reviewed in the same pull requests:

  ```
  store/crispy-doom/
      package.ini        id, kind, title, summary, description, author, upstream, version, revision, licence,
                         licence_assets, rating, boards, network, owner_files, min_core
      build.sh           fetches the pinned upstream tag (sha256 checked), applies patches/, builds against the
                         sysroot, fills the package folder
      patches/
      app/               app.ini, run.sh, icon.png and anything else copied as is
      screenshots/       PNGs from `make shot` (tools/device_shot.py) on the handheld
      LICENSES/
  ```

- **Built packages, sources and the index** in a separate public repository, proposed `kleinrein/rgds-store`, as
  GitHub release assets. One release per package version (`crispy-doom-7.0-r2`), immutable once published. The
  index is `index.json` on that repository's `main` branch, served through `raw.githubusercontent.com`.

Why a separate repository: the OTA channel depends on `releases/latest` of `rgds-core` pointing at the newest OS
release; store releases there would fight over "latest". It also keeps the OS repository's release page readable,
and a takedown or abuse report about one package can never reach the repository the OTA channel lives in.

### 3.2 Signing

**Decision: a separate Ed25519 store key**, made by `tools/store.py init` at `~/.config/rgds/store-key.pem`, pinned
in `packaging/store-trust.json` and carried in the image at `app/os/store-trust.json`, the same way
`update-trust.json` is.

Why not the OTA key: the OTA key can replace the whole system; the store key only vouches for packages that run in a
cgroup with the owner's permissions. The store will publish far more often, and might one day be signed by a CI job;
neither should put the OS key in reach. Like the OTA trust file, the store trust file lives on the read-only image,
holds more than one key during a rotation, and can only change through an OS update. A `state/store-trust.json`
overrides it for testing, as with OTA.

The index is the same envelope the OTA feed uses: `{key_id, signed (base64 of the exact JSON bytes), signature}`.
The signed JSON holds `schema`, `target` (`rgds-os-rk3566`), `sequence` (publish time; never lower than the cached
one), `issued`, `expires` (30 days), `abi` list the store knows, and `entries`. Every download (archive, icon,
screenshot) is checked against the size and sha256 the signed index gives; no package needs a signature of its own.
Downloads only from the HTTPS prefixes the trust file lists (`https://github.com/kleinrein/rgds-store/releases/`,
`https://objects.githubusercontent.com/` for the redirect), never HTTP, never a URL from anywhere else.

### 3.3 `tools/store.py`

| Command | Does |
|---|---|
| `init` | Creates the store key and pins it in `packaging/store-trust.json` |
| `new ID` | A recipe folder from a template |
| `build ID` | Runs `build.sh` in the build VM, makes the zip, then checks it (below) |
| `check [ID…]` | `apps.py check` plus the store's rules: `kind`/`store=` set, aarch64 ELFs only, `gl` not `gl4es`, no symlinks, no file over 4 GiB, every `NEEDED` resolvable through ABI + `lib/`, LICENSES present, licence on the allowed list, size under the limit, screenshots present. CI runs it on every recipe |
| `try ID HOST` | Installs the built zip on a handheld into `/data/store/<id>` through the device's own install action, with the test userdata copy (never the owner's saves; `feedback-never-touch-owner-saves`), and launches it |
| `publish ID…` | Uploads archive, source and pictures as a release in `rgds-store`, then regenerates `index.json` from every `package.ini` plus the asset hashes, signs it, and pushes it. Prints the commands first; `--upload` runs them, like `own_os_release.py` |
| `withdraw ID [--revoke] --reason TEXT` | Marks an entry (below) and republishes the index |

The owner commits recipes and runs `publish`; nothing is published from a session.

### 3.4 Updates and removals

- **Update**: a new `(version, revision)` in the index. The handheld offers it when it is higher than
  `installed.json`'s and its `min_core`/`abi`/`boards` fit this device.
- **Withdrawn** (licence doubt, upstream asked, superseded): gone from browsing; installed copies keep working and
  keep their entry under Installed with "No longer in the store". Remove still works.
- **Revoked** (a security problem, kept for real emergencies): the launcher will not start it and offers Remove. Its
  data folder is still kept.
- A package that needs a newer OS than the handheld has is listed with "Needs RG DS OS 0.9.4" and the button opens
  Updates.

## 4. What we list first

Only things whose code **and** data we may redistribute, checked per package at review against upstream's licence
files at the pinned tag, not a summary of them. Nothing built on other companies' characters or brands, as on the
homebrew shelves. No commercial ROMs, BIOS files or Nintendo assets, the same reasoning that made PictoChat our own.

### 4.1 The launch list (about 12, plus content packs)

| # | Entry | Kind | Screens | What it is | Notes |
|---|---|---|---|---|---|
| 1 | **Calculator** | app (ours) | both | Keypad on the bottom, history on top; scientific and unit-conversion pages | The two-screen mode's first real user |
| 2 | **Clock** | app (ours) | both | Alarm, timer, stopwatch (laps on top), world clock from the 419 timezone rows | Alarms only while awake at first: the opt-in RTC wake froze once (`project-console-comforts-2026-10-28`) |
| 3 | **Notes** | app (ours) | both | Handwriting or typing, page list on top; send a note through PictoChat | |
| 4 | **Hardware test** | app (ours) | both | Buttons, sticks, touch on both screens, rumble, mic level, speakers, dead pixels, battery | Helps testers and support; many bugs only show on the device |
| 5 | **Controller mode** | app (ours) | both | A HID function on the USB gadget: the handheld as a PC gamepad, the bottom screen as touchpad or macro pad | Builds on the USB-C network gadget (`project-usb-gadget-2026-11-01`); needs the HID gadget in the kernel config, and the gadget must not block sleep (`project-usb-gadget-blocks-sleep-2026-11-26`) |
| 6 | **Flipbook** | app (ours) | both | Draw on the bottom, watch it play on top, onion skin, save a GIF to the Album | |
| 7 | **Puzzle pack** | game (ours) | both | Solitaire, sudoku, picture logic, minesweeper | |
| 8 | **Chess** | game (ours + Stockfish) | both | Board on the bottom, moves and clock on top; two handhelds over PictoChat's room networking later | Stockfish GPL-3.0: source published with the package |
| 9 | **Radio** | app (ours) | top | Internet radio from radio-browser.info's open API | Played by the music player's engine (`rgds-music`) or `mpv`; needs TLS from apps, check in phase 1 |
| 10 | **Free books** | content | — | Project Gutenberg books, Pepper&Carrot (CC-BY) for the Media app's reader | Gutenberg's own trademark text stripped from copies, as its licence asks |
| 11 | **Moonlight** | app | top | PC/Steam streaming from Sunshine | GPL-3.0; check decode speed (software ffmpeg vs the RK3566's MPP) |
| 12 | **ScummVM freeware adventures** | content | — | Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the Temptress, Drascula, Dreamweb | Released for redistribution by their owners (check each one's exact terms); ScummVM resume already works |
| — | **Content packs** | content | — | Themes, wallpapers, bezels, startup sounds, shader packs | Ours to give away; from day one |

### 4.2 Next, once the launch list works

| Entry | Kind | Licence (confirm in review) | Notes |
|---|---|---|---|
| Pixel art editor | app (ours) | ours | Its PNGs as covers or theme art |
| Music maker (step sequencer) | app (ours, or LittleGPTracker) | ours / LittleGPTracker's own | Pads on the bottom, pattern on top |
| Weather | app (ours) | Open-Meteo data CC-BY-4.0 | No API key; attribution on screen |
| SSH terminal | app (ours) | ours | Terminal on top, keyboard on the bottom |
| Kiwix (offline Wikipedia) | app | GPL-3.0; ZIM files CC-BY-SA | Big downloads; ZIMs as content |
| chiaki-ng (PlayStation Remote Play) | app | AGPL-3.0 | |
| Two-handheld board games | game (ours) | ours | PictoChat's rooms |
| Open homebrew ROMs: Tobu Tobu Girl, µCity (GB); 240p Test Suite per system | content | check each | Give the library features (covers, Game info, the companion) to people with no games |
| Freedoom with a Doom engine | port | GPL-2.0 / BSD-3-Clause | No owner files needed |
| SuperTux, Brogue CE, Abuse, Hurrican, Blobby Volley 2, NetHack, OpenTTD + OpenGFX/OpenSFX/OpenMSX | game / port | each its own (check Abuse and Hurrican data) | Five or six of these, each built by our recipe and tried on the handheld; OpenTTD needs touch-as-mouse work |
| Free DS homebrew through DSCore (Colors! and others) | content | freeware | Only with each author's written yes |

Left out at launch: ports that need the owner's game files (the `owner_files` keys stay in the format for later);
PICO-8 carts (mostly CC-BY-NC-SA: each author's permission needed); LÖVE games under non-commercial licences
(Mari0); anything heavy enough to cause heat or 3 GB RAM trouble (Endless Sky, Shattered Pixel Dungeon's Java);
emulators and libretro cores (systems ship in the image so every system gets the same game menu and library
features, `feedback-system-consistency`); data that is only "freeware" with no written terms (Cave Story's original
data needs a legal check first); fan games on other companies' characters (Sonic Robo Blast 2 and the like).

### 4.3 Things we must not host: YouTube clients and similar

**Recommendation: leave them out entirely: no package and no link-out entry on the handheld.**

PocketTube (MIT, pygame + yt-dlp) and clients like it break YouTube's terms by design and are the kind of software
that gets DMCA notices (youtube-dl's 2020 takedown on GitHub). Hosting one in `rgds-store` risks a takedown of the
store repository and a strike on the account the OTA channel lives on. A link-out entry is not much safer: we would
still be the ones listing it, signing it into our index and answering for it. It also gains little: since
2026-11-23 the image carries `mpv`, so PocketTube's own PortMaster zip runs through PortMaster for owners who want
it, which is exactly the split the owner asked for. The same rule covers ROM or BIOS downloaders, unofficial clients
for paid services and anything that exists to get around a service's rules.

## 5. Where installed things appear

| Kind | Appears | Never |
|---|---|---|
| `game`, `port` | A new **RG DS Games** shelf in Games (system id `native`, tag `RGDS`; name to confirm, §12), with covers from `icon.png`/the catalog, Recents, Favourites, playtime, Journal, the bottom info screen, like any game | On the Ports shelf |
| `content` | Where its `target` is (§2.7): a ScummVM adventure in ScummVM's shelf, a book in Media, a theme on the Theme page | In the Apps grid |
| `app` | An **Apps** tile in Home's app dock row, shown once any store app is installed. It opens an Apps page: a grid of installed apps with **Store** as its first tile | As separate dock tiles (below) |

Why one Apps tile and not one tile per app: the dock row shares 600 px between PictoChat, Media, Album and the Sound
toy (`home_apps()`); five tiles is about the most that keeps icon and name readable, and the owner wants Home kept to
everyday things. One tile scales to any number of apps. Pinning a single favourite app in place of the Apps tile is
a later option (§12).

The store itself is reached from: the Apps page's first tile, a **Store** row on the Settings root (beside Files,
the place for power tools, `project-home-apps-dock-2026-11-24`), the last row of the RG DS Games shelf ("Get more
games"), and Settings → Updates when updates are waiting. No Home tile of its own.

Packages with `owner_files=required` and no files yet show a "Needs your files" badge; A opens the upload page
(`port_files.py` aimed at `$RGDS_APP_DATA/<owner_files>`) instead of starting it.

**Remove never touches saves or user data.** It deletes `/data/store/<id>/`, its row in `installed.json`, and its
cached cover. `$RGDS_APP_DATA` (saves, config, owner files), playtime history and Journal entries stay; installing
again picks them up. The confirm sheet says so in one line: "Your saves and files stay on the card."

## 6. Screens

Launcher look throughout: the DSi card style, top screen for the picture and detail, bottom screen for the list,
`key_hints` badges on every action, toasts for results, every control reachable by touch.

**Browse** (bottom): rows with icon, title, one-line summary and a value on the right (size, "Installed", "Update",
or a percent while downloading, as `homebrew_row_value`). L/R tabs as in a library: **Games**, **Apps**, **Ports**,
**Extras** (content), **Installed**, and **Updates** when there are any (with a count). Y opens a small sheet: search with our keyboard,
sort (Newest, A–Z, Size). Top: the focused entry's detail (below), so moving the list is browsing details.

**Detail** (top, the homebrew card grown up): screenshot carousel on the left (Left/Right or a swipe with
`kinetics.hpp`, dots under it), on the right title, author, chips (kind, licence, size, "Wi-Fi", "Needs your
files", "RG DS OS 0.9.4+"), the summary, then the action area: the description, or the glitter bar with MB, speed
and time left while it downloads (`update_progress.hpp`'s numbers), or "Installed 7.0 r2". Bottom keys: A Install /
Open, X Remove (installed only), Y Update (when offered), B Back. A tap on the detail card's button does the same as A.

**Install / update / remove**: one download at a time through the transfer queue; others show "Waiting". Leaving the
store keeps downloads going; a toast says when one is installed ("Crispy Doom is in Games › RG DS Games", with
Open). Remove asks once in a confirm sheet (the data line above). Updating a package that is running is refused with
"Quit Crispy Doom first". Errors keep the reason in place of the description and a Retry key, like homebrew and OTA.

**Storage**: the store header shows free space on the card. Install is refused up front when the archive plus its
unpacked size does not fit, with "Needs 31 MB more; Storage" opening the existing Storage view, which gains an
**Apps & games** category for `/data/store`. Installed tab sorts by size on request.

**Update badges**: store updates count into the badge on the RG DS mark and into Settings → Updates, which gets an
"Apps & games" section under the system update: one row per update and **Update all**. The Home "System update"
banner stays for the OS only, so a store update never nags on Home.

**Status fields** (same pattern as homebrew): `st_tab`, `st_count`, `st_<n>_{id,title,kind,summary,size,state,icon}`,
`st_get`, `st_received`, `st_size`, `st_updates`; actions `store-browse {tab}`, `store-detail {id}`,
`store-install {id}`, `store-remove {id}`, `store-update {id|all}`, `store-check`.

## 7. Safety

- **Signatures**: §3.2. A bad signature, an older or same sequence with different content, an expired index, a
  hash or size mismatch, or a URL outside the trusted prefixes stops the action with a plain message. Nothing
  unverified is ever unpacked.
- **Unpacking**: `apps.py`'s rules (no `..`, absolute paths or symlinks; unpacked-size limit; the folder name must
  match the id) plus: the store only replaces or removes a folder whose app.ini has `store=<id>`.
- **Running**: the runner's cgroup leaf and the top-panel lease, as every app. The store adds no privileges. Packages
  run as the same user apps run as today; a sandbox (a private mount namespace with `/data/rgds` read-only except
  `$RGDS_APP_DATA`) is worth doing in the runner for all apps later, but is not a store blocker.
- **Kid profiles**: the Store (Apps page tile, Settings row) is Locked for restricted profiles, like Files; installing,
  updating and removing happen only in an unrestricted profile. Store games go through the existing age rule with
  the `rating` (`game_visible`), so a 7+ profile sees `all` and `7` games. Store apps also carry a `rating` and
  need a per-profile **Apps** permission (off by default, beside Ports and Homebrew in the profile's rules): a kid
  sees only apps we approved for their age; `network=1` apps also need the profile's Wireless permission. A Little kid profile (`simple=1`) never sees the Apps tile.
- **Offline**: the cached signed index (`cache/store/index.json`) lists everything with its cached pictures; with no
  cache, a copy of the index built into the image at release time lists the catalog as it was then. Without Wi-Fi
  the Install key says "Connect to Wi-Fi to install" and opens Wi-Fi settings. Installed packages never need the
  network to start; nothing at launch phones home. An expired index still lists and removes; it does not install.
- **Background checks**: one conditional request for `index.json` piggybacks on the OTA check every six hours, with
  its gates (no check during gameplay, none during an update trial). Store updates install only when asked, unless
  the owner sets the auto level (§12).

## 8. Kid profile and age details

`profile_visibility.hpp` gains `allow_apps` next to `allow_ports` / `allow_homebrew`, stored as `apps=1` in the
profile's file. `game_visible` treats the `native` system by rating like any game shelf (not by permission like
Ports), because we rate every store game ourselves. Store apps go by `allow_apps` **and** their `rating`. The
`rating` key is in the format from the first package, so the catalogue never fills up with unrated entries. Content
installed into a system's folder (ScummVM adventures, homebrew ROMs) gets its rating in its `info.ini`, so the
existing rule sees it like any game.

## 9. Licence policy for entries

Allowed: OSI-approved or FSF-free licences for code; CC0, CC-BY, CC-BY-SA, OFL, or an explicit written
redistribution grant for data. Not allowed: "non-commercial" data (CC-BY-NC) unless the owner decides otherwise
per entry; "freeware" with no terms; anything with brands we do not own. Each recipe keeps the licence files in
`LICENSES/` and the review checks them against upstream at the pinned tag. GPL/LGPL/AGPL packages publish their
exact source with patches (§2.2 `source`).

## 10. What this plan does not do

- No PortMaster anything (§0). No changes to the Ports shelf or to how hand-copied apps work.
- No payments, accounts, ratings or comments. No third-party submissions until the owner decides (§12).
- No commercial ROMs, BIOS or Nintendo assets ever. Free ROMs come only as reviewed content packages (§2.7); the
  existing homebrew feed stays as it is until phase 6.
- Media, Music, the Sound toy and PictoChat are not moved into the store.

## 11. Phases

**Phase 0 — decisions and trust (small, but the long pole).** The owner answers §12. `tools/store.py init`,
`packaging/store-trust.json`, and the image carrying it in `app/os/` plus `store-abi.txt`. Because a device can only
trust a key its image carries, this has to ride in the next OS release even if nothing uses it yet. Move the lease
shim out of `portmaster_files/` (§0).

**Phase 1 — apps that work on the handheld, on both screens (smallest useful step).** No app from an app.ini folder
has run on the handheld yet (`docs/APPS.md`, "Not done yet"), so that comes first: a tiny sample app started from
`/data/apps` on both boards. Then `screens=both` in the runner (§2.6) with the same sample drawing on both panels
and reading bottom touch, Home and Quit still working. Then a **Remove** entry for apps (the format has none). TLS
from an app's Python checked (Radio and Weather need it). Done when the sample installs, runs on both screens,
survives Home and quits, and is removed with its data kept, on an RG DS and a Plus, with a copy of userdata.

**Phase 2 — one store package end to end, no store screens.** `tools/store.py new/build/check/try/publish`, the
`rgds-store` repository, a signed `index.json`. `runtime/store.py` (index fetch and verify, install, remove,
`installed.json`, content targets) with `device_services.py` actions and host tests (real signatures, tampering,
expiry, wrong hash, interrupted unpack). `apps.py` lists `/data/store`; the RG DS Games shelf. The app kit
(§2.8). First packages: **Calculator** (ours, two screens, the kit) and one content package (a ScummVM freeware
adventure). Installed through the device action, updated to r2 and removed with saves kept, on the handheld.

**Phase 3 — the store screens.** Browse, detail with screenshots, install/remove with progress through the transfer
queue, toasts, key hints, touch; the Settings row; the Apps tile and page; kid rules (`allow_apps`, `rating`, Locked
store).

**Phase 4 — updates and storage.** The background check, badges on the RG DS mark, the Apps & games section on
Updates with Update all, the Storage category, withdrawn and revoked handling, the offline index in the image.

**Phase 5 — the launch list.** The rest of §4.1 (Clock, Notes, Hardware test, Controller mode, Flipbook, Puzzle
pack, Chess, Radio, Free books, Moonlight, the other ScummVM adventures) and the content packs. Then §4.2 as each is
ready; the first image runtime (LÖVE or pygame) when two packages want it; owner files only if the owner wants them.

**Phase 6 — fold in homebrew.** The homebrew feed's entries become content packages in the same signed index (built
by `build_homebrew_feed.py`), shown under Extras; the Games "Homebrew" entry opens there. The homebrew shelves then
gain the signature they lack today (they rely on TLS, the URL prefix and per-file hashes).

## 12. Questions for the owner

1. **Shelf name** for store games and ports: "RG DS Games" (recommended), or another name? It sits in Games beside
   the systems.
2. **Repository**: a separate public `kleinrein/rgds-store` for packages and the index (recommended), or release
   assets on `rgds-core`?
3. **Key**: a separate store signing key (recommended), or reuse the OTA key?
4. **Apps on Home**: one Apps tile that opens a grid (recommended), or let the owner pin one or two apps directly into
   the dock?
5. **Store entry points**: Settings root row + Apps page + "Get more games" on the shelf (recommended), or also a
   Home tile?
6. **Updates**: badge only, installed when asked (recommended), or follow `auto_update=2` and install store updates
   in the background too?
7. **Launch list** (§4.1): these twelve plus content packs, or swap some? Controller mode needs a kernel config
   change (USB HID gadget) and an image release.
8. **Data after Remove**: kept always, as now. Should the Storage view later offer "Delete data of removed apps" as
   its own explicit action?
9. **Outside contributions**: only packages we maintain (recommended for now), or accept recipe pull requests from
   others with our review?
10. **Ratings**: we rate every game ourselves (`all`/7/12/16/18) — fine, or use PEGI where one exists?
11. **YouTube clients**: leave out completely, no link-out (recommended) — agreed?
12. **Keyboard-only games**: patch them for the pad or leave them out (no `gptokeyb` in the store) — agreed?
13. **PortMaster plan**: mark `PORTMASTER_PLAN.md` §10 Stage B as dropped, since PortMaster keeps its own GUI?
14. **One store, not two**: an earlier idea was one store page with two backends (our catalogue and harbourmaster).
    With the no-PortMaster rule this plan keeps them apart: PortMaster in pugwash, ours in the Store. Confirm?
15. **Plan file name**: `docs/STORE_PLAN.md` (this file), or rename to `APP_STORE_PLAN.md`?
