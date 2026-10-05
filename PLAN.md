# RG DS custom firmware development plan

Build a DS-focused handheld experience on official Anbernic Linux, using SimpleOS as the integration reference and creating a distinctive DS/DSi-inspired interface. The first goal is a reliable way to put a change on the device and try it. A public installer and OTA releases follow once game launching, saves, sleep, and recovery work consistently.

Planning baseline: 5 October 2026. The device currently runs ROCKNIX; a stock Linux development SD card can be prepared. The first release focuses on DS games. The project name and GitHub repository can be chosen later.

The initial development base is now implemented: native SDL2 preview/probe, all three emulator adapters, SSH deployment/rollback, live development watchers, persistent appearance/brightness/power controls, and a DSi-inspired library with eased icon motion, touch dragging, a full-width range map, ROM banner icons, cached box art and session playtime. Emulator selection and ScreenScraper login/scraping are under Library settings. Live ScreenScraper validation needs the project's application credentials and a player's own login. Hardware controls have desktop simulation, capability detection and a recovery journal. Sound, Connectivity, System, favourites/recent games and home layout/wallpaper remain placeholders. See `README.md`, `docs/DEVICE_SETUP.md`, `docs/DEVELOPMENT.md` and `docs/SCREENSCRAPER.md`. The stock card has been inventoried and the menu started over SSH: Linux 6.1.141, Python 3.11.8, glibc 2.41, SDL 2.32.10 over Wayland, two 640×480 displays and the Anbernic joystick. Installed hashes remained intact. Physical gestures, gameplay, panel order, power/sleep and service credentials need hardware/user validation. Public installation, signed OTA and the unified in-game menu remain planned.

## What we have to build on

| Component | Verified finding | Implication |
| --- | --- | --- |
| Official Linux | SimpleOS targets the stock Linux system. Anbernic currently lists `RGDS-LINUX-V1.0-260513` on its RG DS download page. | Prepare a dedicated card and record the actual image checksum and installed version before selecting a supported baseline. |
| Local SimpleOS package | `simpleos-source/` is a 38 MB install package. Its `system/VERSION` contains `20260927`. | Keep it unchanged as a reference. |
| Launcher and hooks | `bin/simpleos`, `lib/libsimplehook.so`, and `lib/libdrafix.so` are compiled binaries. The launcher is an AArch64 Linux ELF; there are no C/C++ sources or build files here. | A complete UI redesign cannot be made by editing the supplied scripts alone. |
| Public SimpleOS repository | The recursive tree at commit `2c39c237539fa939f904de806510c6addbeb545d` contains documentation, images, `.gitignore`, and `LICENSE`, with no launcher source. Its license is MIT. | Obtain the missing launcher/hook source and build instructions if possible; otherwise write a new launcher while retaining useful integration patterns. |
| Display integration | `loop.sh` uses SDL2 under Weston/Wayland and assumes top `DSI-1`, bottom `DSI-2`. | Start with the existing compositor. Verify panel names, placement, and touch mapping on the stock card. |
| Game integration | `run.sh` contains adapters for stock DraStic, DSperate, and NNDDSS. The menu and supervisor exchange commands through `userdata/command`. | All three engines are required in the development base and first release. Verify each separately on stock Linux. |
| Installer | `install-boot.sh` changes autostart, init scripts, and splash files, and invokes `sosbootlogo` on a boot block device. | Design a smaller installer. First launcher experiments should use a temporary APPS entry. |
| Updates | The supplied updater downloads GitHub release ZIPs and stages `system.new`. Activation removes the existing `system` before moving the candidate into place. The downloader can retry with TLS verification disabled. | Reuse the progress/status idea, but design versioned activation, verified downloads, and recovery before publishing OTA. |

The missing source is the first implementation dependency, not a reason to postpone hardware discovery or design. Ask the SimpleOS author for the launcher, hook, and helper sources, a matching revision, and build instructions. If available, assess how easily rendering can be separated from device logic. If unavailable, proceed with a native launcher and a small compatibility layer. Contacting the author has not been done.

ROCKNIXDS offers useful visual and technical references. Its Pixel renderer separates UI code from EmulationStation glue, so parts may be worth studying or adapting later. Running its complete patched EmulationStation is not required for our design. Its MIT license excludes material inherited from the older upstream `dii-ess-aye` theme; use original artwork and sounds, and preserve the applicable notices for any reused components.

Sources: [SimpleOS repository](https://github.com/boorngos/SimpleOS), [SimpleOS source tree](https://api.github.com/repos/boorngos/SimpleOS/git/trees/2c39c237539fa939f904de806510c6addbeb545d?recursive=1), [SimpleOS license](https://github.com/boorngos/SimpleOS/blob/2c39c237539fa939f904de806510c6addbeb545d/LICENSE), [Anbernic downloads](https://win.anbernic.com/download/640.html), [ROCKNIXDS license](https://github.com/JorreFog/ROCKNIXDS/blob/b30ff7c3cdebbfc873741dd727b4d96a78eee9cf/LICENSE), [Pixel engine design](https://github.com/JorreFog/ROCKNIXDS/blob/b30ff7c3cdebbfc873741dd727b4d96a78eee9cf/dii-ess-aye/rnds/README.md).

## Product direction

The default home should feel like a little DS, with deliberate roles for the two screens. A proposed starting layout is:

| Screen or interaction | First design |
| --- | --- |
| Top screen | Clock, battery/Wi-Fi status, selected game's banner or artwork, title, and last-played information. |
| Bottom screen | A DSi-style horizontal cartridge row, selection frame, readable title, and a small set of touch targets for library, favourites, and settings. |
| Controls | D-pad selects, A launches, B returns, L/R move through pages or groups, Home opens the quick menu. Touch supports the same core actions. |
| Visual language | Light backgrounds, restrained colour accents, crisp borders, DS proportions, short animations, and original menu sounds. Dark mode uses the same navigation. |
| Library | Offline ROM scanning, embedded DS icons when available, local artwork with a fallback, recent games, favourites, and predictable sorting. |
| Game options | Clear global defaults and per-game overrides, with restart requirements shown where relevant. |
| In-game menu | Resume, save/load with slot information, game options, and return home; never hide whether a save succeeded. |

Test the cartridge row against a compact grid with a realistic library before committing to one. Avoid filling both screens with repeated information. Navigation and launching must work without touching the screen, and ordinary use must work without Wi-Fi.

Themes should change palettes, fonts, icons, backgrounds, sounds, and constrained layout values through a versioned data format. Keep navigation and save/update logic in code. Start with one polished light theme and a dark variant; add a theme editor or broad layout scripting only if there is a real need.

## Architecture and build approach

Keep four responsibilities separate: the launcher renders and handles navigation; a supervisor launches games and recovers from crashes; device adapters handle display/input/power/network; the installer and updater manage releases. The UI should talk to a small typed interface, with fake implementations for desktop previews.

If we need a new launcher, the initial candidate is C++17 with SDL2, CMake, and a small image/font dependency set. This matches the observed SDL2/Wayland environment and supports desktop previews. Confirm actual library versions and required features on the card before committing to the stack. A two-screen render/input probe is the decision gate.

Build Linux AArch64 artifacts using a pinned Linux toolchain and a sysroot compatible with the verified stock firmware. The Mac can run the editor and desktop preview; a Linux container/VM or Linux CI can produce device binaries. Do not assume that a modern ARM64 binary will run against the stock libc or vendor SDL/Mali libraries. Verify linkage and run the probe on hardware early.

The supervisor owns emulator lifecycle and returns to the launcher on normal exit or crash. Reuse the existing command-file protocol behind an adapter initially if useful, writing commands atomically. Keep emulator configurations and saves outside versioned application code. Treat proprietary/bundled emulator files separately: use stock DraStic already on the device, and establish each other component's build and distribution terms before including it in a public package.

Proposed repository layout once implementation starts:

```text
upstream/           pinned source or documented package references
src/launcher/       screens, input, navigation, library
src/platform/       desktop mocks and RG DS adapters
runtime/            supervisor and emulator adapters
themes/             original default themes
tools/              build, probe, deploy, log collection, packaging
packaging/          APPS entries, installer, updater, recovery
docs/               device findings, compatibility, release instructions
```

The current workspace is not a Git repository. Initialize it when creating the implementation base, record upstream versions/checksums, and keep build output, ROMs, BIOS, credentials, and personal saves out of source control. A GitHub fork of SimpleOS alone would currently give us its documentation tree; our repository must also contain the actual implementation and any permitted imported source.

## Fast testing on the device

1. Keep the working ROCKNIX card as a fallback. Back up saves, including their emulator/core versions; save states are often core/version-specific, so preserve in-game saves too.
2. Flash the official RG DS Linux image to a separate development card, complete first boot, and verify both displays, controls, stylus, sound, Wi-Fi, lid sleep, and charging on stock.
3. Enable SSH using whatever the verified stock firmware provides. If it is unavailable, bootstrap through an APPS entry and SD-card copying. Record the IP and confirm access before relying on wireless deployment. Set up SSH keys where supported.
4. Capture a read-only device inventory: kernel/OS version, architecture, libc and SDL versions, mount layout/filesystems, Weston outputs, input devices, backlights, battery and lid interfaces, network tools, and free space. Store findings without private keys or Wi-Fi passwords.
5. Add a `Try RGDS` APPS entry that starts a small display/input probe from the user partition. It should temporarily hand over the display/input session, restore stock UI on exit or failure, and avoid installing autostart or changing boot resources. Verify this handover on the actual stock launcher.
6. Replace the probe with a minimal launcher: select one DS game, launch stock DraStic, and return. Preserve stock behaviour while this slice is developed.
7. Add a deployment tool that copies a complete build to a staging directory over SSH, checks its files, switches the development build while no game is running, restarts only our launcher, and tails logs. Use `rsync` if present; otherwise use SCP/tar. Do not require an SD reflash for a UI change.

Use the same renderer on the desktop, with two stacked 640×480 preview panels and fake battery/network/library data. Real hardware remains essential for panel placement, touch, sleep, audio, and emulator behaviour.

Implemented development commands (device use still requires the stock card and inventory):

```text
make preview
make dev
make dev-device DEVICE=<IP>
make build-device
make deploy DEVICE=<IP>
make logs DEVICE=<IP>
make rollback DEVICE=<IP>
```

The desktop watcher rebuilds on source saves, restarts after a successful build, and remembers cartridge/core selection. Theme colour edits reload in the existing process after about 300 ms of stable contents. The device watcher deploys over SSH, queues the newest build while a game runs, and applies it after returning to the menu. Theme-only changes upload to a writable override without modifying hashed release contents. Invalid themes and failed compilation retain the working UI. Other asset types do not yet have a reload path.

Aim for under one minute from an incremental UI change to seeing it on the device after the first build. This remains a hardware target to measure. Code deployments currently transfer the complete private package; theme-only updates are small. Boot, power, and installer changes need actual reboot tests.

## Installation and recovery

The first public package should install over one explicitly supported stock Linux revision. Proposed flow: flash stock once, boot it, copy our ZIP contents next to `Roms/`, run `APPS → Install RGDS`, and reboot. Existing SimpleOS installs need a separate migration path; detect them and preserve their data rather than silently mixing both boot systems.

The installer must check firmware compatibility, storage space, package integrity, existing hooks, and emulator availability before applying changes. Back up every changed hook/file with its original metadata and record a journal so rerunning an interrupted install can repair it. Show progress on both screens and keep a readable log on the user partition.

Install application files on the user partition and introduce the smallest necessary boot hook. Defer early boot splash changes; the audited SimpleOS package does write boot resources despite leaving U-Boot itself alone. Branding can start in the launcher.

Recovery must work when the launcher is broken and Wi-Fi is unavailable. Add a marker that can be created from a computer, such as `rgds/userdata/boot_stock`, and have the stable boot hook honour it before running the launcher. Return to stock must restore any stock frontend execution permissions we changed. SimpleOS's existing marker is `simpleos/userdata/boot_stock`; it selects the stock menu and is not a full uninstall.

A repair/uninstall APPS entry should restore our backed-up hooks and files while retaining ROMs and user saves. A boot-button shortcut is optional only after early input detection has been verified. Test recovery from a missing binary, broken theme, launcher crash loop, and interrupted install before distributing the package.

## OTA releases and rollback

GitHub Releases can host the package and a small signed manifest once a repository exists. No separate update server is needed initially. Stable is the default channel; beta is opt-in. Check on request and optionally in the background, but install only after user confirmation and never during an active game.

Proposed manifest fields: project identifier, version/build number, channel, device model, supported stock firmware, minimum updater version, package URL, byte size, SHA-256, release notes, and data schema version. Sign the exact manifest bytes with a maintained signature implementation; embed the trusted public key in the installed updater. Do not invent cryptography.

The update sequence should be:

1. Validate the signed manifest, device/firmware compatibility, battery/charging condition, and free space for download plus extraction plus rollback.
2. Download to a temporary file over verified HTTPS. Correct device time/CA issues visibly; never silently disable certificate verification.
3. Verify the manifest signature and package hash, then validate archive paths/types and expected contents before extraction. Manual ZIP updates use the same checks and activation code.
4. Extract to a new versioned directory. Validate binaries/assets and create a completion record only after the staged files have been flushed to storage.
5. Activate a pending release through a small stable boot shim, retaining the last known working release and its state records.
6. Let a supervisor mark the release healthy only after both screens render, input initializes, and the launcher stays responsive through a probation period. A failed candidate falls back to the previous release or stock after bounded retries.
7. Provide a visible manual rollback option for regressions that boot successfully. Keep the previous release until the new one is accepted.

Proposed on-card layout:

```text
rgds/
  boot.sh
  releases/<version>/
  state/                 active, previous, pending and health records
  userdata/              settings, library data and logs
  themes/user/
```

Verify the actual filesystem before implementing activation. The audited early-mount script uses VFAT for `/mnt/mmc`; symlink-based `current` pointers cannot be assumed. Prefer small replaceable state files with checksums and redundant generations, immutable release directories, and explicit completion markers. A rename alone does not guarantee survival of arbitrary power loss on FAT. Validate recovery on a disposable development card, and keep the boot shim able to find the previous valid generation.

Updates must not overwrite ROMs, BIOS, user themes, saves, or per-game settings. Version writable data separately; back up before migrations and make changes readable by the rollback version where possible. If a core's save-state format changes, keep both versions and show the limitation before updating. Boot-hook/updater changes require their own staged migration and recovery path; ordinary UI releases should not rewrite them.

When GitHub is ready, CI should build with the pinned toolchain, run desktop smoke checks, validate themes/manifests/package structure, and produce a ZIP plus hashes. Promote a build to stable only after device testing. Keep signing credentials out of the repository and keep development deployment separate from the public update channel.

## Milestones and release criteria

| Milestone | Deliverable | Acceptance gate |
| --- | --- | --- |
| 0 Device baseline | Stock development card, inventory, recovery instructions, source/build decision. | Stock works, access is repeatable, and a failed experiment can be removed from the card. |
| 1 First playable slice | Native two-screen launcher, button/touch selection, DraStic/DSperate/NNDDSS adapters, deploy/log tools. | All three cores launch/return reliably and a UI change reaches the device without reflashing. |
| 2 Personal alpha | Library scanning, favourites/recent, first theme, basic game settings, sleep/wake integration. | Daily play preserves saves, both panels wake correctly, and offline use works. |
| 3 Installable alpha | Packaged installer, repair/uninstall, stock fallback, local package update and rollback. | A clean supported card installs successfully; induced install/update failures recover. |
| 4 OTA beta | GitHub release pipeline, signed metadata, download verification, channels, rollback UI. | Corrupt/wrong-model packages are rejected and interrupted updates keep a usable release. |
| 5 Public stable | Documentation, support bundle, compatibility list and release notes. | A fresh install and the device regression matrix pass on the exact published build. |

Measure cold boot to usable home, launch/return time, menu responsiveness, idle resource use, and suspend drain against the stock/SimpleOS baseline. Aim for smooth 60 Hz menu interaction when the display supports it, and avoid redrawing continuously when idle. Set boot and battery targets after measurement.

The device regression matrix should include two-screen cold boots, a 2D game, a demanding 3D game, stylus-heavy gameplay, audio/microphone, button mapping, save/load and normal saves, long/repeated lid sleep and wake, charging while asleep/off, empty/large libraries, missing art, ROM paths with spaces/non-ASCII characters, disabled Wi-Fi, theme errors, core crashes, low storage, manual rollback, and interrupted updates. Use in-game saves to confirm persistence across reboot; file existence alone is insufficient.

The first concrete implementation session delivers inventory tooling and a temporary two-screen probe; the inventory itself requires the stock card. All three emulator adapters belong in this base. The first playable goal is: power on, choose a cartridge, play, close the lid, resume, and return home with saves intact. UI polish and further emulator features build on that working path.

## Audited package references

These files provide evidence for the current integration, not guarantees about another firmware revision:

- `simpleos-source/README.txt`: installation, Wi-Fi/SSH defaults, games, updates, and stock marker.
- `simpleos-source/simpleos/system/loop.sh`: SDL2/Wayland setup, panel assumptions, command protocol, supervisor and power handling.
- `simpleos-source/simpleos/system/run.sh`: emulator adapters and hook/library setup.
- `simpleos-source/simpleos/install/install-boot.sh`: modified init/autostart/splash paths and boot-device logo write.
- `simpleos-source/simpleos/system/ota.py`: GitHub endpoint, release selection, download behaviour and TLS fallback.
- `simpleos-source/simpleos/system/ota.sh`: ZIP staging and external emulator payload installation.
- `simpleos-source/simpleos/main.sh` and `system/ota-main.sh`: activation that removes the old system tree.

Reference binary SHA-256 values:

```text
simpleos       f95dd22f5233342e717c3e83c91b7d986db89b6f8b268b6ac7c45da08f432dff
libsimplehook  8ef4e63a7f42d23971f2acceb7f683efcc95e151d34277c19de2c05ffec760e1
libdrafix      347bf28865c67b74f8c9e7535d33ea5099a9bb32ebb44f33a8e5c3c3113db2a8
```
