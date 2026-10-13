# RG DS product and release plan

RGDS is a DS-focused launcher and system integration layer for the Anbernic RG DS running its vendor Linux system. It is not a new operating system: the kernel, vendor drivers and emulator engines remain external. SimpleOS was used as a reference during development; its runtime hooks and scripts are not required by the current implementation.

**Current direction, 2026-10-11:** polish RGDS on Anbernic Linux, add a source-built melonDS frontend, and measure gameplay/menu/boot performance on the standard RG DS. The Armbian prototype and its flash workflow are retired; preserve the working stock boot path.

**Status as of 2026-10-07:** the native two-screen launcher, library, settings, three engine adapters, shared in-game menu, audio/touch/shader bridges, APPS entries, recovery path, private package builder and SSH development workflow are implemented. Offline validation and a representative hardware session are documented separately. The most recent integration and optional direct-startup changes have not passed device regression. The source has no configured GitHub remote and is not ready for public release.

The current hardware evidence is limited: a test session used the official Linux system and one representative game, but checkpoint restoration, shader appearance/performance, audio at selected levels, the latest touch and return-home changes, stock handoff, and direct startup still need device checks. The original `/mnt/mmc` card was mounted read-only on 2026-10-07; further testing should use the verified writable installation until that card is repaired or replaced. See [startup design](../BOOT.md), [the regression notes](DEVICE_TEST_2026-10-07.md), [the next device checklist](../NEXT_DEVICE_TEST.md), and [engine boundaries](../PERFORMANCE.md).

Public distribution remains a later milestone. Before it, establish the repository license and release process, resolve source/license/provenance for every emulator payload and shader, publish supported-firmware limits, provision legitimate ScreenScraper application credentials, and complete hardware recovery and compatibility checks. See [third-party provenance](../../THIRD_PARTY.md).

## Reference audit: stock Linux and SimpleOS

| Component | Finding from the original audit | Design decision |
| --- | --- | --- |
| Official Linux | SimpleOS targeted stock Linux. At audit time, Anbernic listed `RGDS-LINUX-V1.0-260513` on its RG DS download page. | The current development target is stock Linux. Record exact firmware/image details for every supported release. |
| Local SimpleOS package | The ignored `simpleos-source/` package was about 38 MB and `system/VERSION` contained `20260927`. | Keep it as a historical, local reference; it is not used as the RGDS runtime or engine bundle. |
| Launcher and hooks | The supplied launcher/hooks were compiled AArch64 binaries without their C/C++ source in that package. | RGDS uses its own native launcher and integration layer. |
| Public SimpleOS repository | The audited recursive tree at commit `2c39c237539fa939f904de806510c6addbeb545d` contained documentation, images, `.gitignore`, and `LICENSE`, with no launcher source. | Useful integration patterns informed development; the missing source is not a current dependency. |
| Display integration | `loop.sh` used SDL2 under Weston/Wayland and assumed top `DSI-1`, bottom `DSI-2`. | RGDS uses the stock compositor; verify display names and touch mapping on each supported firmware. |
| Game integration | `run.sh` contained adapters for stock DraStic, DSperate, and NNDDSS. | RGDS has separate adapters for all three; each needs its own hardware acceptance. |
| Installer | `install-boot.sh` changed autostart, init scripts, splash files, and a boot block device. | RGDS uses APPS and an owned stock-supervisor hook; it does not replace boot partitions or splash resources. |
| Updates | The supplied updater staged `system.new`, removed the old system during activation, and could retry with TLS verification disabled. | RGDS public signed OTA is not implemented; it remains a separate release milestone with safe activation/recovery requirements. |

The missing SimpleOS launcher source is no longer an implementation dependency: RGDS has its own native launcher and integration layer. The audit below remains useful as historical context for the stock environment and the design decisions made during bring-up.

ROCKNIXDS offers useful visual and technical references. Its Pixel renderer separates UI code from EmulationStation glue, so parts may be worth studying or adapting later. Running its complete patched EmulationStation is not required for our design. Its MIT license excludes material inherited from the older upstream `dii-ess-aye` theme; use original artwork and sounds, and preserve the applicable notices for any reused components.

Sources: [SimpleOS repository](https://github.com/boorngos/SimpleOS), [SimpleOS source tree](https://api.github.com/repos/boorngos/SimpleOS/git/trees/2c39c237539fa939f904de806510c6addbeb545d?recursive=1), [SimpleOS license](https://github.com/boorngos/SimpleOS/blob/2c39c237539fa939f904de806510c6addbeb545d/LICENSE), [Anbernic downloads](https://win.anbernic.com/download/640.html), [ROCKNIXDS license](https://github.com/JorreFog/ROCKNIXDS/blob/b30ff7c3cdebbfc873741dd727b4d96a78eee9cf/LICENSE), [Pixel engine design](https://github.com/JorreFog/ROCKNIXDS/blob/b30ff7c3cdebbfc873741dd727b4d96a78eee9cf/dii-ess-aye/rnds/README.md).

## Product direction

The implemented home and library use a DS/DSi-inspired cartridge carousel with deliberate roles for the two screens. The continuing product goals are:

| Screen or interaction | Current direction |
| --- | --- |
| Top screen | Status bar with time, battery and Wi-Fi; selected game's artwork and session playtime. |
| Bottom screen | DSi-inspired carousel for Settings, Games, Favourites and Recents, then a cartridge carousel for the selected view. |
| Controls | D-pad navigates, A selects/launches, B returns, X opens cartridge options, Y changes library view, and Menu/Anbernic opens Settings or the shared in-game menu. Touch supports library actions. |
| Visual language | Light/dark palettes, DS proportions, original vector artwork and short transitions. DS rounded and Pixel classic are available font styles. |
| Library | Offline ROM scanning, embedded DS banner icons, cached box art, playtime, favourites and recents. ScreenScraper is optional. |
| Game options | Global emulator/shader defaults with per-cartridge overrides and checkpoint preferences. |
| In-game menu | Shared across all three adapters: resume, checkpoint save/load, sound, brightness, shaders, emulator settings and return home. Save completion is acknowledged; load currently reports dispatch and needs gameplay validation. |

The carousel is implemented. Continue checking readability, navigation and empty/large-library behavior on both the preview and physical panels. Navigation and ordinary use should remain available without Wi-Fi or touch.

Theme scope is currently narrower than a full theme system: theme files select palette values, while font and animation choices are preferences. A theme editor, layout scripting and custom sounds are not release requirements unless later user feedback justifies them.

## Architecture and build approach

Keep four responsibilities separate: the launcher renders and handles navigation; a supervisor launches games and recovers from crashes; device adapters handle display/input/power/network; the installer and updater manage releases. The UI should talk to a small typed interface, with fake implementations for desktop previews.

The launcher is implemented in C++17 with SDL2 and a small native rendering/font stack. The same renderer supports desktop preview and device builds. Python runtime tools handle session supervision, device integration, packaging and development deployment. Recheck ABI and system capabilities against every firmware revision before claiming it supported.

Build Linux AArch64 artifacts with the current pinned Zig 0.13 toolchain and a sysroot compatible with the verified stock firmware. The Mac can run the editor and desktop preview. Do not assume that an ARM64 binary will run against a different stock libc or vendor SDL/Mali library; verify linkage and the probe on every supported firmware revision.

The supervisor owns emulator lifecycle and return-to-stock cleanup. It uses the RGDS bridge for the shared menu, audio and input integration. Keep emulator configurations and saves outside versioned application code. Treat bundled emulator files separately: DraStic is sourced from stock, and each other component needs resolved source and distribution terms before a public package can include it.

Current source layout:

```text
src/launcher/       screens, input, navigation, library and theme rendering
src/platform/       dynamically loaded SDL and device-facing native helpers
runtime/            session supervision, engine adapters and device services
themes/             default palettes
tools/              build, probe, deploy, packaging and diagnostics
packaging/          APPS entries and bootstrap/recovery scripts
assets/              fonts, shaders and emulator-profile metadata
docs/                setup, development, test evidence and release plan
```

This is a local Git repository with no configured GitHub remote. Keep build output, ROMs, BIOS, credentials and personal saves out of source control. A public release needs an explicit project license and a reviewed third-party inventory; the private engine bundle is not a source-controlled project dependency.

## Device development workflow

The inventory/probe entries, package extraction, SSH deployment, rollback and live development watcher are implemented. For a new device or firmware, first follow [DEVICE_SETUP.md](../DEVICE_SETUP.md), inspect the inventory, and validate the stock fallback before trying direct startup. Keep a known-good stock card and back up ordinary in-game saves; save states can depend on the emulator build.

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

The current package is a private development package, installed from APPS or tested through the optional stock-supervisor startup hook. It is not an installable public firmware release. A public package should initially support a narrowly identified stock Linux revision, document manual installation and recovery, and detect existing boot managers before enabling direct startup. Existing SimpleOS installs need an explicit migration path; do not mix boot systems silently.

Before public distribution, the installer must check firmware compatibility, storage space, package integrity, existing hooks, and required emulator availability before applying changes. Back up every changed hook/file with its original metadata and record a journal so rerunning an interrupted install can repair it. Show progress on both screens and keep a readable log on the user partition.

Keep application data on user-writable storage and limit direct-startup changes to the owned stock-supervisor hook. Defer early boot splash and bootloader changes; the audited SimpleOS package writes boot resources despite leaving U-Boot itself alone. Branding can stay in the launcher.

The current recovery path can disable RGDS startup from its APPS entry, and the owned hook falls back to stock on startup failure. Before public release, verify this on supported firmware with a missing release and interrupted install. Return to stock must continue to restore any stock frontend execution permissions changed by RGDS. SimpleOS's marker is `simpleos/userdata/boot_stock`; it selects the stock menu and is not a full uninstall.

The Recovery APPS entry is not a complete public uninstaller. A public uninstaller should restore owned hooks/files while retaining ROMs and user saves. A boot-button shortcut is optional only after early input detection has been verified. Test recovery from a missing binary, broken theme, launcher crash loop, and interrupted install before distributing the package.

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

| Milestone | Deliverable | Acceptance gate | Status |
| --- | --- | --- |
| 0 Device baseline | Stock development card, inventory, recovery instructions, source/build decision. | Stock works, access is repeatable, and a failed experiment can be removed from the card. | Baseline captured; original game card now needs repair/replacement. |
| 1 First playable slice | Native two-screen launcher, button/touch selection, three engine adapters, deploy/log tools. | All three cores launch/return reliably and a UI change reaches the device without reflashing. | Implemented; representative gameplay tested, full acceptance pending. |
| 2 Personal alpha | Library, favourites/recents, themes, game settings, sleep/wake integration. | Daily play preserves saves, both panels wake correctly, and offline use works. | Most UI/runtime features implemented; hardware regression incomplete. |
| 3 Installable alpha | Public package, repair/uninstall, stock fallback, local update and rollback. | A clean supported card installs successfully; induced install/update failures recover. | Private package/recovery tooling exists; public install acceptance pending. |
| 4 OTA beta | Release pipeline, signed metadata, download verification, channels, rollback UI. | Corrupt/wrong-model packages are rejected and interrupted updates keep a usable release. | Not started. |
| 5 Public stable | Documentation, support bundle, compatibility list and release notes. | Fresh install and device regression matrix pass on the exact published build. | Not started. |

Measure cold boot to usable home, launch/return time, menu responsiveness, idle resource use, and suspend drain against the stock/SimpleOS baseline. Aim for smooth 60 Hz menu interaction when the display supports it, and avoid redrawing continuously when idle. Set boot and battery targets after measurement.

The device regression matrix should include two-screen cold boots, a 2D game, a demanding 3D game, stylus-heavy gameplay, audio/microphone, button mapping, save/load and normal saves, long/repeated lid sleep and wake, charging while asleep/off, empty/large libraries, missing art, ROM paths with spaces/non-ASCII characters, disabled Wi-Fi, theme errors, core crashes, low storage, manual rollback, and interrupted updates. Use in-game saves to confirm persistence across reboot; file existence alone is insufficient.

The next acceptance goal is to verify the current build on writable hardware: boot to home, launch and return from each engine, confirm checkpoint restoration and ordinary saves, test sleep/wake and stock handoff, and validate recovery after a failed startup. The first-day probe and SSH workflow are already implemented; hardware verification is the remaining gate for those paths.

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
