# RG DS Core

A Nintendo DS-focused launcher for the **Anbernic RG DS and RG DS Plus**, built in C++ and Python for the stock Linux system. Browse games across both screens, choose an emulator per cartridge, and manage checkpoints through a shared in-game menu.

**Status: private development preview.** The standard RG DS is tested; RG DS Plus has experimental compatibility support with simulated hardware tests and still needs physical testing. Other firmware variants are unvalidated. This is an application installed on the stock Linux card. A public download channel, project license and redistribution clearance for all bundled components are still pending. See [third-party provenance](THIRD_PARTY.md).

[Install](#install) · [Features](#features) · [Performance](#performance) · [Build](#build-and-develop) · [Limitations](#known-limitations) · [Documentation](docs/README.md)

## AI-assisted development

AI tools have been used during development of RG DS Core, including code, documentation and visual assets. AI-generated work can contain mistakes, and passing automated tests does not establish hardware compatibility, accurate emulation or safe recovery in every situation. Treat this preview as experimental: keep backups of your saves and a working stock card. Validation claims below refer to specific recorded checks; third-party emulator code and assets retain their own authorship and licenses.

## Features

- **Two-screen library:** cartridge, cover and list views; touch and controller navigation; search, A–Z jump, favourites, recents and played/missing-artwork filters.
- **Game history:** per-game play counts and time, collection summaries and a Statistics page.
- **Shared game menu:** pause/resume, nine checkpoint slots per game and engine, previews where supported, overwrite/load undo, slot import/export and save-data backups.
- **Per-game choices:** emulator, shaders, controls and hold/toggle fast-forward. A visual shader studio compares the supplied and original effects.
- **Device controls:** Wi-Fi, brightness, volume, power profiles, timezone, battery status, idle/lid sleep and stock-launcher recovery.
- **DS controls:** real microphone, simulated blowing, emulated lid and firmware language in DSperate and melonDS. Sound settings also provide a standalone physical microphone level test.
- **Menu sounds:** an independent optional sound palette for browsing, choices, favourites, saves, loads, lid close/wake and a magical game launch. Includes effects volume and previews under Settings → Sound → Menu sounds; works with animations off. See [menu sound notes](docs/MENU_SOUNDS.md).
- **Personalization:** optional gentle spring transitions with small staggered card movements, reactive icons and directional back travel (off by default; Settings → Theme & display → Animations → View transitions), light/dark themes, rounded/pixel fonts, icon styles and eleven UI languages: English, Dansk, Deutsch, Español, Français, Italiano, Nederlands, Norsk bokmål, Norsk nynorsk, Português (Brasil) and Svenska.
- **Startup and updates:** direct startup, optional one-shot resume after clean shutdown, an SD-card installer and signed application-update/rollback support. OTA has no public feed yet.

### Emulator support

| Engine | Integration | Main boundary |
| --- | --- | --- |
| **DSperate** | Default; source-built ARM64 JIT | Compatibility and speed vary by game. |
| **DraStic** | Uses the compatible stock Linux binary | Requires the existing stock installation; emulated-frame telemetry is unavailable in the standard path. |
| **NNDDSS** | Alternative engine with independent writable data | Supplied binary provenance is unresolved for public redistribution. |
| **RG DS Experimental** | Optional source fork of DSperate; Cortex-A55 build and stylus fixes | Speedup and Phantom Hourglass drawing acceptance are unverified. See [experimental fork](src/experimental/rgds/README.md). |
| **melonDS** | Experimental source-built ARM64 JIT with live Threaded 3D | Demanding scenes can fall below full speed. |

Optional **SuperDrastic** provides a separate native display/menu backend for DraStic and remains experimental. See its [integration guide](docs/SUPERDRASTIC.md).

**Switching engines does not transfer progress.** Each engine keeps its own battery saves and checkpoints; Standard DraStic and SuperDrastic share the DraStic save home. States also depend on ROM bytes and engine versions. Use the game's own Save function and keep backups. See [save management](docs/SAVES.md).

Artwork matching works offline; downloading covers needs Wi-Fi. Automatic, GameTDB and Libretro work without account credentials. [Artwork setup](docs/SCREENSCRAPER.md) also covers optional ScreenScraper access. Games, downloaded covers and Nintendo BIOS dumps are not included.

## Performance

There are on-device measurements, but coverage is limited. These recorded **11 October 2026** results use New Super Mario Bros., World 1-1, with Mario stationary near the entrance: native resolution, shader Off, no frameskip, muted audio and the performance power profile on standard RG DS stock Linux.

| Engine / configuration | Normal emulated FPS | Session start → first game presentation |
| --- | ---: | ---: |
| DSperate, two verified checkpoint runs | 59.98 / 59.95 | 4.375 / 4.195 s |
| NNDDSS, default scheduling | 59.58 | 4.544 s |
| melonDS, two Threaded 3D variants | 44.84 / 46.40 | 4.930 / 4.716 s |
| Standard DraStic | Unavailable; display FPS is a separate metric | 3.602 s |

FPS values are means of eight one-second samples after warmup, from per-core checkpoints of the same stationary scene. DSperate's two runs used the same checkpoint; NNDDSS had one default run, and melonDS had one run per variant. These are historical results from different development builds, **not a controlled cross-core ranking or a benchmark of the current checkout**. Muted audio does not certify sound quality, and first presentation excludes physical input/display latency.

The evidence supports approximately full-speed DSperate and NNDDSS emulation in this scene and a melonDS bottleneck. It does not establish full-library performance, battery life or an advantage over SimpleOS/ROCKNIXDS. No competing OS was benchmarked. A later ten-minute Cooking Mama startup-screen soak recorded 60.00 **display** FPS and a 17.1 ms p95 presentation interval; it is not a gameplay benchmark.

[Performance methods and reproduction](docs/PERFORMANCE.md) explain frame counters, host microbenchmarks, fast-forward results and remaining work. The [original gameplay report](docs/archive/GAMEPLAY_REVIEW_2026-10-11.md) records build/ROM/checkpoint identities, clocks, temperatures and excluded runs. Raw logs remain in ignored `build/` folders locally, so they are not reproducible evidence shipped with a fresh clone. Broader claims need repeated fixed-scene gameplay, thermal/battery tests and physical input/audio measurements.

## Install

You need a **compatible official RG DS Linux card**, a computer/card reader, free space for a side-by-side release and a privately built `RG-DS-Core-Installer-<build>.zip`. There is no public installer download yet. Starting from Android or a blank card requires preparing the compatible stock Linux card first; follow [device setup](docs/DEVICE_SETUP.md).

**RG DS Plus:** use the Plus's own stock Linux card and the same application installer. The launcher adapts to its 1024×768 panels and discovers the lower touchscreen/controller without assuming original-model event numbering. Start with **APPS → RGDS Inventory**, **RGDS Probe**, then DSperate. Follow the [Plus testing guide](docs/RG_DS_PLUS.md); original-model OS images and boot-logo patches are not Plus installers, and OTA retains its validated-firmware check.

1. Exit any game, shut down normally and put the games card in your computer.
2. Extract the installer ZIP into the card root. Merge `Roms/` and copy `RGDS-Install/` alongside it, without an extra enclosing directory.
3. Put the card back and open **stock APPS → Install RG DS Core**.
4. The installer validates and installs the release, then opens RG DS Core. New installations start RG DS Core on future boots; upgrades retain your startup preference.
5. Copy `.nds` files or ZIPs containing a DS game into the stock DS folder (`Roms/NDS` on the tested card). Reopen the launcher or choose **Settings → Library → Add your games → Scan for games**.

Before installation:

```text
Roms/APPS/Install RG DS Core.sh
RGDS-Install/
  install_package.py
  package.zip
  README.txt
```

The installer creates `rgds/`, keeps userdata separate and consumes its payload ZIP after success. Repeat with a complete newer installer for a manual update. Errors are recorded in `RGDS-Install/install.log`. End users do not need SSH. A fresh-card installation through the physical APPS menu still needs acceptance before a public release. See [SD installation](docs/SD_FLASH.md).

### Controls

| Control | Action |
| --- | --- |
| D-pad / touch | Navigate and select |
| A / Start | Open a category or launch the selected game |
| B | Back; leaving the library returns to RG DS Core home |
| X / Options | Cartridge options |
| Y / View | Collections, search and filters |
| L/R in the library | Move one game; hold to accelerate, like the D-pad and on-screen arrows |
| L2/R2 in the library | Open the A–Z letter picker |
| Anbernic button | Search and filters from any launcher screen; game switcher during gameplay |
| Home button in a game | Shared in-game menu |
| Start + Anbernic in a game | Save-and-exit flow |

Tap the search bar to search within Games, Favourites or Recents. The keyboard supports touch or D-pad + A; X deletes, Y changes letters/numbers, Start searches and B cancels. SuperDrastic uses its own [backend controls](docs/SUPERDRASTIC.md).

Open Game switcher beside Resume in the in-game menu, or press Anbernic. It shows the current game and recent games. Selecting another game saves a checkpoint and waits for the current engine to exit before starting the next game with auto-load. Failed saves keep the current game open. Back/B or X opens the in-game menu; the separate Home button opens that menu directly. This switches through saved checkpoints rather than keeping several emulators running.

In the host preview, Home opens search and filters. `[` / `]` step through games and repeat while held and `Q` / `E` open the letter picker.

## Updates and recovery

**Settings → System → Updates & recovery** supports checking, release notes, download/pause/retry and installation. **No public update channel is configured.** Once provisioned, the updater verifies Ed25519-signed metadata and package hashes, stages a separate release and installs after confirmation. Automatic downloading does not restart a running game.

A supervisor outside the updated release checks the candidate's first usable frame and heartbeat. Failed trial startup restores the previous release. Saves/settings live in `rgds/userdata/`. This updates the application; it does not flash the OS.

- **Use previous build** selects the previous verified application release.
- **Stock APPS → RGDS Recovery** disables direct startup while retaining saves.
- **Settings → System → Startup → Boot into** selects RG DS Core or Anbernic.
- Card-side diagnostics: `rgds/userdata/launch.log`, `rgds/userdata/update/status.ini` and `RGDS-Install/install.log`.

See [OTA trust and release setup](docs/OTA.md) and [release acceptance](docs/RELEASE_PLAN.md). Power-loss behavior on real removable media and the public release path remain unaccepted.

## Build and develop

The launcher is C++17/SDL2; a GLES bridge integrates display, shaders and the shared menu. Python manages engines, saves, artwork, device services and updates. Releases live under `rgds/releases/`; writable data lives under `rgds/userdata/`.

```text
src/launcher/       Native library, settings, input and localization
src/platform/       Device controls and emulator integration
src/experimental/   Experimental melonDS frontend
runtime/            Session guardian, adapters and services
assets/             Fonts, branding, catalogs, shaders and previews
packaging/          Installer, stock APPS entries and update supervisor
tools/              Build, package, deploy, benchmark and release tools
tests/              Unit, SDL interaction and integration checks
docs/               Current guides; dated investigations in archive/
vendor/engines/     Ignored, separately provisioned private payloads
```

Host preview requires Python 3.9+, a C++17 compiler and SDL2 development files. Device builds use **Zig 0.13**; engine builds also need CMake and Ninja. Asset generation needs Pillow; signing needs OpenSSL 3. Follow [development setup](docs/DEVELOPMENT.md) for engine provisioning and toolchain details. A source checkout alone does not contain the private emulator bundle required for packaging.

```sh
make dev                 # Rebuild/restart the host preview on edits
make preview             # Launcher preview
make settings            # Settings preview
make game-menu           # Shared game-menu preview
make build-device        # Cross-compile launcher and bridge
make build-engines       # Source-built DSperate
make build-melonds       # Source-built experimental melonDS
make build-rgds-experimental # Experimental DSperate fork
make build-superdrastic  # Optional DraStic backend
make package             # Private development application ZIP
make installer           # Drop-on-card installer
make test                # Host and integration checks
```

`dist/latest.txt` identifies the latest application ZIP. `tools/package_installer.py` wraps an existing package. Device development uses `make deploy DEVICE=root@<device-IP>` and `make run DEVICE=root@<device-IP>` after SSH setup; the installer is the normal user route.

## Known limitations

- **Release readiness:** no public feed, complete OS image or resolved project/payload licensing. Standard RG DS is tested; Plus compatibility is implemented but awaits physical acceptance.
- **Hardware acceptance:** physical touch/screen order, audible resume, repeated sleep/wake and power cycles need broader testing. Standard DraStic launch-time screen swapping has a local configuration correction but awaits physical confirmation.
- **Menu latency:** the October 11 DSperate run required a two-second menu-close fallback. Later short checks do not establish graceful teardown across all engines and games.
- **Save portability:** no cross-engine conversion, cloud sync or whole-backup restore UI. Portable slots require matching ROM bytes and a compatible engine.
- **Scope:** no RetroAchievements UI, cheat browser or non-DS library. Shared microphone/blow/lid controls cover DSperate and melonDS; other engines retain native controls.
- **Recovery and text:** startup rollback cannot repair filesystem/OS damage or incompatible save migrations. Fonts cover the bundled Latin languages rather than full Unicode.

Use the [documentation index](docs/README.md) for current guides and the remaining device checklist. Dated investigations and superseded plans are preserved in [the archive](docs/archive/README.md); they describe the state at the time, not current guarantees.
