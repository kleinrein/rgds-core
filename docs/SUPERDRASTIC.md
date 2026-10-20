# Optional SuperDrastic backend — shared menu tested, 2026-10-12

SuperDrastic is now bundled as an **experimental DraStic backend**. DSperate
remains the default emulator and Standard remains the default DraStic backend.
Existing choices are preserved. The [device review](archive/DEVICE_REVIEW_2026-10-11.md) records native startup, three menu cycles, verified checkpoint exit and successful checkpoint loading in both directions with Standard. A subsequent original-ROM clock test proves fast-forward acceleration and normal-speed restoration. Physical touch/controls/audio, suspend and retail-game fast-forward remain acceptance items.

## Choose it

- For one game: Game options → Emulator → DraStic, then **DraStic backend →
  SuperDrastic (experimental)**. Other games retain their emulator/default.
- For all DraStic games: Settings → Library → **DraStic backend**. This setting
  does not change the default emulator. A game's backend can inherit or override it.
- Choose **Standard** to return to the existing RGDS DraStic integration. Backend
  changes take effect on the next game launch; Standard's in-game settings also
  allow selecting the next backend without changing its current renderer.

Existing core IDs remain unchanged; RG DS Experimental is a separate fifth entry. SuperDrastic still needs the existing,
verified ARM64 DraStic r2.5.2.2 binary; it is not an independent emulator or OS.

## Behavior and current limits

The optional path uses SuperDrastic's direct DRM output, pacing, touch, audio
pump, with the **shared RGDS game menu**. It does not stack the RGDS display
bridge or stock `libdrastouch` with that renderer. The menu helper writes a
shared RGBA frame which the native presenter converts for both DRM panels.
The emulation thread waits while the menu is open; the presenter remains active.
Acknowledgements require the actual scan buffers to contain the menu image.
The RGDS supervisor excludes menu/suspend time from playtime.

The existing Home shortcut settings and Fast-forward button/style are reused.
With default controls, Anbernic alone opens the RGDS menu and Start + Anbernic
requests exit. Menu navigation stays fixed when gameplay is remapped.
Gameplay mapping is prepared at launch; unlike the Standard RGDS bridge, remaps
made outside the game require a relaunch on this backend. Fast-forward sends a
native button event held for 160 milliseconds of monotonic time; release no
longer waits for 40 display presentations, which can become sparse during
fast-forward. The mapped physical button still needs a latency and release
check. Normal speed is the launch default.

Volume, brightness, battery, saves, controls and navigation use the shared RGDS
menu and hardware services. Physical volume keys use the RGDS volume watcher.
Debounced lid sleep retains the stock suspend path. Physical touch, audible
playback and lid-sleep acceptance still require hands-on testing.

This initial profile uses **native output with no shader**, native resolution by
default, and existing per-game DraStic threading/resolution/frameskip preferences.
RGDS's standard shader choice is retained but disabled in this backend's game
options. The pinned upstream shader assets are included with their source for
subsequent integration; the RGDS shader picker is not passed to a different shader
ABI. The shared menu follows the RGDS theme. RetroAchievements is explicitly disabled in the RGDS session; no credentials
are adopted from another frontend. The upstream updater is not used.
Native resolution is the default. Per-game options include 2× standard,
2× Gengis and 3× Gengis. Gengis at 3× rasterizes 768×576 and downsamples to the
512×384 DraStic interface before panel presentation. It is CPU/NEON rendering,
not Vulkan. Upstream 3× Gengis lacks shadow-polygon rendering, so it remains an
explicit experimental choice. There is no 5× option. RGDS retains power policy.

On 2026-10-12 the original 128-triangle test ran at 59.96 emulated FPS at 3×.
Three shared-menu cycles each advanced **zero** emulated frames while paused;
open took 443–458 ms and resume 130–135 ms. Save, overwrite, undo, load,
undo-load, export and save-on-exit all acknowledged successfully, with exit 0.
Both menu panels were captured and visually checked. This synthetic scene is
not a demanding retail-game benchmark. The 1,536-triangle probe measured about
60 FPS at 2× and 37.8 FPS at 3× with the earlier native-menu build.

## Saves

Standard and SuperDrastic use the **same DraStic executable and writable save
home** (`emulators/drastic/backup` and `savestates`). They share RGDS's
`library/<game-key>/auto-drastic.dss` checkpoint. This is same-engine reuse, not
conversion or sharing with DSperate or melonDS.

Automatic loading uses a disposable copy inside the native savestates directory,
so upstream's consume-on-load operation cannot delete the canonical checkpoint.
A newer `.dsv` in-game save prevents an older automatic state from being loaded
by either DraStic backend. Auto-load and Auto-save remain independent settings.
A confirmed new exit checkpoint is copied and checked before replacing the
canonical file. The previous version is retained under
`library/<game-key>/checkpoint-backups/drastic-<hash>.dss`.
Checkpoint contents are flushed before replacement, followed by a best-effort
directory flush using the same filesystem policy as release/ownership records.
Power-loss recovery on filesystems that cannot flush directories remains untested.

SuperDrastic uses SIGKILL after a verified exit receipt. RGDS accepts that
as success only with the native integration's exit receipt; a crash is not a
successful save. Save failure preserves the previous canonical checkpoint and
reports an error while leaving the game/menu running for recovery. Keep using the game's
normal Save command. The device log confirms a SuperDrastic checkpoint loaded in Standard and the resulting Standard checkpoint loaded in SuperDrastic. Auto-save off preserved the canonical checkpoint SHA-256. This automated test does not replace visually confirming expected in-game progress or a game-native battery save.
A requested automatic save followed by an unexpected zero-status exit also
reports failure when the checkpoint handoff was not completed; the process exit
status alone cannot claim that request saved progress.

## Display ownership and failure handling

The runner releases the RGDS SDL display, allocates an unused Linux VT, leaves
it in text mode and unblanks it. Weston must release DRM on that switch. Native
readiness has a deadline; a busy display or passthrough verdict aborts the launch
and restores the previous VT after reaping the process. Every runner exit path
attempts restoration, including failed process creation and menu initialization.
There is no automatic fallback launch, compositor kill, or shell-config execution.

Stock Weston released and reacquired DRM in the device smoke tests.
The target is standard RG DS on Anbernic Linux; this is not Plus or independent-OS
acceptance. Local tests can establish the ownership/control protocol, not prove
that the physical display driver obeys it.

## Build and evidence

Upstream: [JorreFog/SuperDrastic](https://github.com/JorreFog/SuperDrastic/tree/930c4c5bb3e904557cd8cc42a01a4e27834f5ba1),
version `0.5.0-beta.1`, commit `930c4c5bb3e904557cd8cc42a01a4e27834f5ba1`.
`tools/build_superdrastic.py` checks the commit, tree and every tracked source
byte, applies `tools/engine_patches/superdrastic/rgds-integration.patch`, and builds
an ARM64 shared library. It compiles pinned generated assembly without executing
upstream installers, update scripts or build scripts.

```sh
make build-superdrastic
python3 -B tools/build_superdrastic.py --check
make package
```

ARM64 Linux needs GCC/G++, Git, CMake, `libdrm-dev`, `libasound2-dev` and `libudev-dev`. On this Mac the tool uses the
existing local Lima ARM64 instance (`rgds-image`, a historical name for the
local toolchain). No new OS image or handheld connection is involved. The
optional library targets glibc ≤2.38; the recorded stock OS has glibc 2.41.
The RGDS launcher/standard bridge retain their own ABI targets.

The stock SDL 2.32.10 lacks the dummy-video driver SuperDrastic requires. A separately built, pinned private SDL runtime provides dummy video, software rendering, Linux input/joysticks and ALSA. Launch verifies its hash and enumerates required video/audio drivers before changing profiles or acquiring the display. Its library search path is confined to this backend. The package includes its zlib license, full source archive and build options.

`build/superdrastic/build.json` records source, integration, compiler, ABI and
payload hashes. The package includes its notices, corresponding upstream source,
patch and RGDS integration/build source. Runtime verification checks this backend
only when selected; source archives are checked at installation rather than
rehashing them before every game.

Host tests cover defaults/inheritance, real SDL settings interactions, hook
isolation, native profile generation, command scheduling, readiness deadlines,
checkpoint preservation/promotion, native hardware requests, suspend failure and
VT restoration. The local Linux probe also loads the actual ARM64 library with
immediate symbol resolution and exercises its config parser, signal handling and
successful no-save/failed-save receipts. These are not game benchmarks.

### Actual emulation-speed check — 2026-10-11

On the standard RG DS running Anbernic Linux, candidate
`0.0.1-dev-dd7887c95c6a` ran RGDS's original homebrew VBlank probe. The test reads
the probe's monotonically increasing DS RAM counter and its inverse, then divides
the counter delta by elapsed monotonic wall time. It does not use display swaps,
the native FPS overlay, or a target refresh-rate label.

| Phase | DS VBlanks | Elapsed seconds | Emulated frames/second |
| --- | ---: | ---: | ---: |
| Normal | 433 | 7.2251 | 59.93 |
| Fast-forward | 5,677 | 7.2294 | 785.27 |
| Normal restored | 433 | 7.2207 | 59.97 |

Fast-forward reached **13.10× the measured normal rate on this lightweight
synthetic scene**. This establishes actual acceleration and restoration; it does
not predict retail-game speed or compare against other distributions. Commands
used the native control protocol, so physical mapped hold/toggle handling still
requires testing. Exit reported `state=saved` and released the session lock.

`tools/probe_emulated_clock.py` requires the owned benchmark process hierarchy
and original probe ROM. Its explicit DraStic option checks the exact regular
shared-RAM file against the process's descriptor device/inode, scans one 4 MiB
main-RAM mapping, then samples only the validated 32-byte clock. It excludes
graphics/device mappings and makes no process-memory writes. It checks process
start identity throughout and rejects backward or invalid counters.

Raw samples and exact arithmetic are retained in
`build/same-scene-20261011/super-live-candidate.jsonl` and
`super-live-candidate-summary.json`. The maximum sample read took 3.08 ms;
startup and toggle transition periods are outside the measured intervals.

Logs: `userdata/logs/drastic.log`, `superdrastic.log` (plus three previous native
logs), and per-run `userdata/session/superdrastic-*` receipts. Preserve them for subsequent
device comparisons. See [NEXT_DEVICE_TEST.md](archive/NEXT_DEVICE_TEST.md)
for the physical acceptance sequence.
