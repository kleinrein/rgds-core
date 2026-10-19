# Startup, idle power and polish pass — 13 October 2026

Device: standard RG DS, stock Anbernic Linux (Buildroot 2024.02, kernel
6.1.141, glibc 2.41), owner's SD card. Builds `19304fcba249` → `8f8823be7e73`.

## Measured results

| Measurement | Before | After |
| --- | ---: | ---: |
| Kernel start → first launcher frame | 25.6 s | **13.0–13.2 s** |
| RGDS session start → first frame | 10.23 s | **2.70–2.79 s** |
| Launcher CPU at an idle menu | 22.4% of a core | ~1% (3% while the charging bolt animates) |
| Session guardian CPU at idle | 7.8% | 2.2% |
| `device.info` helper (Python + 6× nmcli) | every 15 s, 0.75 s CPU each | only on link change, network pages, or every 5 min |
| Shared ALSA stream at an idle menu | always RUNNING | closed after 3 s of silence |
| Realtek driver kernel log | ~1.6 lines/s | errors only |

Boot timing is `boot-startup.json` (`kernel_boot_to_first_frame_seconds`),
three consecutive reboots per build. CPU is `/proc/<pid>/stat` utime+stime
over 10–20 s at the Home screen.

## Causes and fixes

**Stock fstrim at boot (largest).** `S99fstrim` discards all free space on
every SD partition at boot, then hourly: 13 s of card I/O (8 s for the FAT
games partition alone) starting just before RGDS loads, stalling every startup
read. `runtime/system_tuning.py` clears only that script's execute bit while
RGDS owns startup (rcS skips non-executable scripts; bytes unchanged,
hash-reviewed, journalled in `/userdata/rgds-boot/tuning.json`) and runs one
idle-priority trim per week, 3 minutes after the first frame. The stock script's
own pid file records the parent shell's PID, so its loop is found by its exact
command line. Choosing Anbernic startup (`boot_control.disable`) or APPS → RGDS
Recovery restores the original mode.

**Release verification at boot and launch.** `activate.verify_startup` and
`verify_engine` re-hashed files on every boot/launch (0.5 s per DSperate launch,
1.35 s for RG DS Experimental, warm). A per-release memo in
`userdata/cache/verified/<build>.json` records `[mode, size, mtime, ctime]` and
digest per verified file, bound to the manifest's SHA-256; FAT inode numbers
change per mount and are excluded. Unchanged files are trusted; any metadata
change re-hashes. Install, activation and OTA always hash fully. A deferred
`activate.py audit` (60 s after the first frame, nice 19 + idle I/O class, at
most daily) re-hashes memoized files and drops the memo on any mismatch. The
startup inventory classifies non-startup payloads from directory-entry types
instead of one `stat` each.

**Idle CPU.** SDL2 keeps feeding silence while an audio device is merely
paused, through ALSA's 32→48 kHz resampler: the menu-sound thread used 16% of
a core and kept the RK817 DAC/I2S running. `MenuAudio` now opens at 48 kHz on
first use and closes after 3 s of silence; reopening restores the DAC level
with a direct ALSA control write (`platform/alsa_mixer.hpp`). The UI loop
sleeps in `poll(2)` on private evdev readers (`InputWake`) instead of waking
every 8 ms; held controls, drags, pending hardware writes and timed UI keep a
short cadence. While a game runs, the launcher waits on a pidfd instead of
10 ms polling. The guardian scans `/proc` with raw reads, wakes every 0.5 s
instead of 0.1 s, and `update_guard.py` blocks in `waitpid` outside OTA trials.

## Power-off needing two presses

The RK817's off source after a stock shutdown is `0x80` (SLEEP pin): the stock
kernel lets the firmware assert the PMIC SLEEP pin, after which the PMIC
ignores PWRON for 20–30 s. This matches
[TheGammaSqueeze/rg_ds_kernel 7e5f173f4d](https://github.com/TheGammaSqueeze/rg_ds_kernel/commit/7e5f173f4d),
which instead writes `SYS_CFG3 (0xf4) DEV_OFF`. Without replacing the kernel,
`system_tuning` adds an owned final busybox `::shutdown` step after
`umount -a -r` (`/usr/local/sbin/rgds-final-poweroff`). It acts only when RGDS
requested the power-off (marker in devtmpfs; never on reboot), remounts
`/proc` and `/sys` if needed, refuses while any block filesystem is still
writable, logs to the kernel log (kept by ramoops), then sets DEV_OFF.
A dry run on the device computed `w f4 19` from `SYS_CFG3 = 0x18`; the live
helper correctly refused while filesystems were writable. **Physically
accepted on build `1386025a69e5`:** after RGDS Power off the owner pressed
Power once and the device started immediately; the RK817 off source then read
`0x08` (register DEV_OFF) instead of `0x80` (SLEEP pin). One cycle; repeat on
battery and charger. Stock Anbernic power-off is unchanged (no marker).

## Lid-sleep diagnosis (in progress)

The owner sleeps the device by closing the lid. If anything other than the
user wakes it while the lid stays closed (RTC alarm, battery or charger IRQ),
the lid latch only re-arms on an open edge, so RGDS would stay fully awake
until the lid opens. Each deep sleep now appends a record to
`userdata/logs/sleep-journal.jsonl`: battery capacity/voltage/fuel-gauge
charge, suspend counters, suspended time (CLOCK_BOOTTIME − CLOCK_MONOTONIC),
the wake-capable IRQs that fired (`/proc/interrupts` deltas: power key, hall
switch, RTC alarm, charger plug, PMIC) and whether the lid was still closed.

## Other changes

- Library, Settings and in-game menu share one 44 px back button.
- Theme & display → Accent colour: the sixteen DS favourite colours, adjusted
  for 3:1 contrast against the active light/dark surfaces.
- Network → Network file sharing: Off, Open on this Wi-Fi (guest access, no
  password) or Password protected. Both advertise `_smb._tcp` as "RG DS" through
  the firmware's avahi-daemon (an RGDS-owned file in `/etc/avahi/services`,
  removed when sharing stops) so the handheld appears under Network in Finder.
  Shares veto macOS clutter (`._*`, `.DS_Store`, …) so copies from a Mac do not
  leave stray `._Game.nds` files. Device verification pending.
- Bridge: queue-mode SDL audio (DSperate, the fork) uses 512-frame periods on
  the shared 2048-frame dmix ring, and the SDL audio thread gets nice −10. The
  previous 2048-frame period equalled the whole ring, so writes waited for an
  empty ring and underran under load. Device verification pending.

## Mario Kart DS (Figure-8 race checkpoint, frame 5300)

Normal phase, 25 s, muted, harness profiles, build `8f8823be7e73`.

| Engine | Shader | Scale | Game FPS | Emu ms | Work ms p95 | >20 ms frames |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| DSperate | Off | 1 | 60.02 | 8.35 | 11.68 | 20 |
| RG DS Experimental | Off | 1 | 59.81 | 9.76 | 13.84 | 18 |
| DSperate | LCD + both colours | 1 | 60.07 | 7.89 | 11.22 | 16 |
| RG DS Experimental | LCD + both colours | 1 | 59.61 | 9.27 | 13.95 | 31 |
| RG DS Experimental | Off | 2 | 59.69 (58.90 avg) | 11.68 | 22.26 | 296 |

The fork costs ~15% more emulation time than DSperate here; 2× misses the
budget. Engine logs showed periodic ALSA underruns at 60 FPS (addressed above).
Slow first frames (16–24 s) in these runs came from the harness copying the
500 MB release to FAT immediately before launch; normal launches use the memo.

## GCC 13.3 + PGO engines

Both engines previously built with Zig/Clang 18 without PGO. Upstream's CI
builds GCC 13.3 with the PGO profile committed under `pgo/aarch64`. The Lima VM
(Ubuntu 24.04, GCC 13.3) builds the same verified, patched trees with
`-DDSPERATE_PGO=use`, zero control-flow mismatches, static libstdc++ and
glibc ≥ 2.38 (the device has 2.41). Same race checkpoint, engine binaries run
directly with SDL's dummy audio driver, 40 s each, mean emulation ms/frame:

| Binary | Run 1 | Run 2 | Worst second |
| --- | ---: | ---: | ---: |
| DSperate, Zig/Clang | 7.61 | 7.64 | 8.3 |
| DSperate, GCC + PGO | **7.03** | **7.03** | 7.4 |
| RG DS Experimental, Zig/Clang | 7.39 | — | 7.8 |
| RG DS Experimental, GCC + PGO | **7.08** | — | 7.5 |

`tools/build_engines_gcc.py` implements the build; `build_engines.py` and
`build_rgds_experimental.py` take `--compiler gcc-pgo` (the Makefile default,
`ENGINE_COMPILER`), record the compiler, PGO manifest and fingerprint, and
validators allow glibc 2.38 only for that recorded mode. The builder is
included in each corresponding-source archive. The Vulkan variant still builds
with Zig. Raw runs (no bridge) show the fork no slower than DSperate, so the
extra ~1.4 ms seen inside RGDS sessions is an integration cost still to locate.

## October 14: DSCore, lid-sleep result and enhanced 3D

**Lid sleep, measured overnight.** One suspend lasting 8 h 16 min; the only
wake was the hall switch (lid open). The CW2015 gauge fell 2675 → 2505 mAh:
**~20 mA average in deep sleep** (~0.5 %/h). No spurious wakes, so the sleep
path itself is correct; the remainder is hardware kept powered by the stock
device tree (Wi-Fi module "power remain", Bluetooth, amplifiers, DRAM).

**DSCore.** RG DS Experimental is now shown as DSCore and is the
recommended default for new installations (internal ID `rgds-experimental`
is unchanged, so saves and checkpoints carry over). Native resolution now
launches the lean GCC + PGO binary: RGDS previously started the Vulkan build
at every resolution so 2x-5x could switch live, costing ~2.6 ms/frame with GPU
3D off. Mario Kart race, harness, muted:

| DSCore native | Emu ms | Work p95 ms | Frames over budget |
| --- | ---: | ---: | ---: |
| Before (Vulkan build, Zig) | 9.74 | 13.85 | 7 |
| After (native, GCC + PGO) | **7.43** | **10.45** | 0 |

Crossing between Native and 2x-5x in the in-game menu saves the choice,
checkpoints and restarts into the other binary (`game_menu.renderer_restart`).

**Enhanced 3D without the duplicate CPU raster.** At 2x-5x the native
software raster only feeds guest-visible readers (display capture, the
display FIFO, states). `rgds-vulkan-hires.patch` now defers it while
`skippable()` holds; capture/FIFO starting late, a state write or a renderer
switch call `materialize_native()`, which dispatches exactly the deferred
bands. `DS_VERIFY_HIRES` or `RGDS_NATIVE_3D=1` keep the old behaviour. A
headless replay of the Mario Kart race showed `capture 0/0` on all 241 frames,
so the deferral applies on every frame there. The Vulkan variant also builds
with GCC + PGO (`build_rgds_vulkan.py --compiler gcc-pgo`).

| DSCore Mario Kart | Avg FPS | Work p95 ms | Frames >20 ms |
| --- | ---: | ---: | ---: |
| 2x before | 58.90 | 22.26 | 296 |
| 2x after | **59.63** | **15.47** | **14** |
| 3x Smooth after | 50.39 | 19.53 | 324 |
| 3x Sharp after | 53.97 | 20.02 | 267 |

The profile at 3x shows no "3d spans" stage (deferred) and moves the
bottleneck to the single-threaded enhanced compositor (~9.9 ms/frame) plus
GPU readback waits (~3.7 ms). Next: split the compositor over the now idle
band workers, or resolve to 640x480 on the GPU before readback.

**Accessibility.** The default light palette now meets WCAG AA for text:
muted `#5e6c78` and accent `#236eae` (≥ 4.6:1 on background, cards and the
focused row; white labels on the accent 5.4:1). DS favourite colours are
adjusted to 4.5:1. Theme & display → Accent colour opens a DSi-style 4x4
grid of named colour tiles with a live top-screen preview, Classic blue and
a High contrast mode (black/white text, 3:1+ component edges, 7:1 accents).
All new strings are translated in the 14 catalogs.

## October 14 (afternoon): calmer settings, cover stage, DSi rail

**Theme & display** moved from a hand-packed page (two choice pairs and five
36 px rows on one screen) to the shared settings list: 66 px cards, four per
screen, scrollable by touch, wheel and controller like every other list.
Rows: Colour mode and Typeface (both options shown in place: tap a side,
Left/Right, or A to swap), Accent colour (swatch + name, opens the DSi grid),
High contrast (now a first-level switch), Icon style, Screen brightness
(Left/Right adjusts directly; A opens the slider), Animations and Touch
indicator. New contrast and palette symbols; "Colour mode" is translated.

**Game top screen.** With a cover, the upper screen shows only the box art,
centred and large (up to 440×344) on a soft shadow and white card edge with a
faint glass sheen; the title already sits in the lower screen's bubble. Play
count and time sit in one small icon + number pill under it, only for played
games. A changed selection settles in over 190 ms (the first draw does not
animate). Without a cover, the cartridge's DS icon is centred at 128 px with
its title and developer.

**Sharper covers.** GameTDB's `coverS` art is 128×115, soft at that size.
Automatic artwork now tries Libretro's box art first (512 px, stored at 384)
and keeps GameTDB's exact game-code match as the fallback. "Fetch missing
artwork" also re-checks covers narrower than 200 px once per game
(`sharp_checked=1` in `info.ini`); without a sharper match the existing cover
is kept and not requested again.

**Horizontal rail (DSi menu).** Glass end buttons (bright upper band with a
crisp edge, reflected light along the bottom), a recessed groove with one
pip per item while they stay ≥ 5 px apart, a glass window over the visible
range that the pips show through, a position notch, and a glossy thumb with
engraved grip. Holding the thumb lifts and widens it with a soft accent glow
and floats "12 / 62" above it. Lower thumb rows stay plain accent, so labels
keep their contrast.
