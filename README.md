# RGDS development base

A DS-focused launcher experiment for the Anbernic RG DS on official Linux. This first development build has a native two-screen UI, light/dark theme files, an input probe, and adapters for DraStic, DSperate, and NNDDSS. The supplied SimpleOS package is the integration reference.

This is a temporary stock-APPS test package. It does not install a boot hook, replace stock Linux, or implement public OTA. The device display mapping, emulator behaviour, and sleep integration still need hardware validation.

## Desktop development

Requirements: Python 3, a C++17 compiler, SDL2 headers/library, and Zig 0.13 for the initial device cross build. The current Mac already has these dependencies. SDL2 loads at runtime so the device can use its vendor library.

```sh
make dev
make preview
make test
make package
```

`make dev` watches source saves, rebuilds, and restarts the preview with your cartridge/core selection restored. Theme edits reload in place automatically; compiler/theme errors retain the working preview. Use `make dev THEME=themes/default/dark.ini` to tune the dark palette. See [the live development guide](docs/DEVELOPMENT.md) for the full workflow.

Preview controls: left/right select, Enter/A or Start launches the selection (a preview message on desktop), Tab/C opens Library settings, S or the gear opens Settings, B/Escape goes back (or exits from the library), F1 switches the input probe, and R reloads the theme file manually. In Settings, up/down moves focus, A/Enter opens a section, and left/right adjusts a value. Touch/mouse works on the bottom panel. On the device, Select opens Settings; the default mapping can be changed with `RGDS_BUTTON_SETTINGS` after the input probe. You can pass `--rom-dir /path/to/games` repeatedly to scan real filenames. Desktop preview never launches the emulators or changes host hardware.

The library uses a DSi-inspired icon strip, a short eased slide and selection lift, light settings transitions, and a full-width map of the visible range. Drag the strip to browse; tap a neighbouring icon to select it, or tap the settled selected icon / Start to play. Drag the range map to seek through a large library. Native DS banner icons and titles work offline, including ZIP games via a background cache. The top screen shows cached box art and accumulated session playtime.

Emulator choice is now in Settings → Library, with DraStic, DSperate and NNDDSS retained. Settings → Library → Artwork & ScreenScraper has account login, a touch/controller keyboard, fetching missing artwork, progress, stopping a scrape and signing out. See [ScreenScraper setup](docs/SCREENSCRAPER.md) for the one-time project configuration. Live API login/download validation requires legitimate project credentials and a user's own account.

## Settings and appearance

Run `make settings` to open Settings directly. Theme & display has working Light/Dark and DS rounded/Pixel classic choices. Both screens update immediately and save automatically. DS rounded uses bundled Nunito atlases with mixed-case, proportional, antialiased text; Pixel classic retains the original uppercase bitmap font. Basic Latin and Latin-1 text are supported in the rounded option; other scripts use a fallback glyph.

Theme & display also opens Screen brightness: a touch slider and Left/Right adjust both backlights from 5% to 100% in 5% steps. Power has Stock, Battery saver, Balanced, and Performance profiles, battery/charging information, menu idle sleep (Never, 1, 2, 5, or 10 minutes), menu lid sleep, and Sleep now. Stock is the default; automatic sleep and lid sleep start disabled. The desktop preview saves these preferences and simulates hardware actions with a sample battery.

On stock Linux, `runtime/system_controls.py` detects writable backlights, CPU policies and GPU/DMC devfreq controls. Profiles use only advertised frequencies and governors: Battery saver caps at the middle available frequency, Balanced at the second highest, and Performance holds the highest. Unsupported controls show Unavailable. Settings persist in `userdata/system.ini` separately from appearance preferences. Hardware changes are journalled before writing; the session guardian restores original backlights, clocks and lid wake settings before handing control back to stock, including after a terminated launcher. Probe sessions do not apply saved settings.

Sleep now and automatic menu sleep use the kernel's advertised `mem` suspend state and retain stock's selected suspend variant. Brightness is restored on wake. Menu lid sleep uses the RG DS hall sensor (`0` closed, `1` open) with debounce and enables its wake source. Sleep controls apply in the launcher; games retain stock sleep handling. Brightness and power profiles carry into games and are reapplied when returning. Suspend/resume, panel order, hall polarity, audio and stock-service interactions require hardware validation. Sound, Connectivity, System, favourites/recent games, and home layout/wallpaper remain labelled Later.

Appearance and default-emulator preferences live in `build/dev/preferences.ini` for desktop preview and `rgds/userdata/preferences.ini` on the card. `system.ini` is stored beside them. Artwork, banner cache and playtime live under `userdata/library/`. They survive source rebuilds and release changes. Favourites, full Unicode, a custom in-game menu and unified in-game sleep handling remain future milestones.

## Test on stock Linux

Follow [the card setup guide](docs/DEVICE_SETUP.md). `make package` writes the ZIP path and `dist/latest.txt`. Copy the complete ZIP contents next to `Roms/` on a stock Linux development card, then run the Inventory and Probe entries before Try RGDS.

For SSH development, use an explicit device address:

```sh
make probe DEVICE=root@192.168.1.42
make deploy DEVICE=root@192.168.1.42
make run DEVICE=root@192.168.1.42
make logs DEVICE=root@192.168.1.42
make rollback DEVICE=root@192.168.1.42
make dev-device DEVICE=root@192.168.1.42
```

The IP above is an example. Deploy retains the previous release, stops our menu session, and refuses to activate while an emulator is running. Run starts the newly selected build. Rollback switches to the previous development build; run it again afterwards. This development mechanism uses hashes and SSH transport, not signed OTA metadata or a power-loss-tested public updater.

`make dev-device` builds/deploys on source saves and sends theme-only edits to a writable live override. Updates queue while you play, then apply after returning to the menu. It requires SSH keys and a verified host key; [DEVELOPMENT.md](docs/DEVELOPMENT.md) explains setup, logs, and rollback.

## Emulator coverage

| Engine | This package | First adapter behaviour |
| --- | --- | --- |
| DraStic | Uses the existing stock binary and vendor touch library. | Keeps stock settings, saves, and emulator menu. No SimpleOS joystick-hook remapping. |
| DSperate | Includes the binary, profile and GPL notice from the local package. | Uses its native pause/menu and a separate config/home folder under `rgds/userdata`. |
| NNDDSS | Includes the local binary, shim libraries, artwork and shaders. | Uses its native menu and a separate writable root; takes required BIOS and fonts from stock at first launch. |

All engines retain their own save-state formats. No automatic conversion or save migration is performed. ZIP games must contain exactly one `.nds`/`.dsi` file and no unsafe archive paths. Check [third-party provenance](THIRD_PARTY.md) before distributing a public release.

The launcher's core selector currently applies to the next launch. Per-game choices and their persistence are not implemented yet. Use each core's own menu to quit a game normally; the launcher then reopens. Our unified quick menu follows after launch/return and save behaviour have been verified on the device.

## Project files

- `src/launcher/`: UI, library scanning, navigation, theme validation.
- `src/platform/`: vendor SDL2 runtime loading.
- `runtime/`: temporary session ownership/recovery, emulator adapters, inventory, development activation.
- `packaging/`: stock APPS entries and bootstrap.
- `tools/`: host/device builds, private ZIP packaging, SSH development commands.
- `tests/`: archive, emulator, version activation, process ownership, and transport validation.
- [PLAN.md](PLAN.md): full product direction and roadmap.

`simpleos-source/`, `build/`, `dist/`, inventories, and personal data are excluded from Git. No upstream package files are modified by the build tools.
