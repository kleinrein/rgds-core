# DSCore (RG DS Experimental 1)

An experimental GPL-3.0-or-later fork of the pinned DSperate core. The original
DSperate entry remains the default. This fork has its own engine ID,
`rgds-experimental`, and its own battery saves, configuration and checkpoints.
Choose **Game options → Emulator → DSCore**.

The first build targets Cortex-A55 with ARM64 JIT, NEON and LTO. It keeps
local wireless and RetroAchievements (Casual only). RG DS OS has no libcurl, so
the runtime passes `--cheevos-helper` and achievements' requests go through
`runtime/achievements.py serve`; unlock notices are drawn by the RG DS bridge
(`rgds_toast`) on the lower panel, and `--cheevos-list` writes the game's list
for the Game info page. It retains
normal emulated timing and full rendering during normal play; no device overclock.
These build choices are experiments, not measured speedup claims.

The fork fixes SDL stylus ownership (secondary fingers cannot move or release
a stroke), rejects mouse interference during a finger stroke, and clamps
normalized edge coordinates to the last panel pixel. Upstream portability and
completed-emulation-frame telemetry patches are retained. It does not yet
establish that Phantom Hourglass drawing passes on the physical touchscreen.

Fast-forward reuses its existing presentation cadence to skip hidden 2D/3D
raster work. CPU, geometry and audio emulation still run. Capture and display
FIFO frames are always drawn, including capture first enabled after the raster
skip decision. That late-capture case has a captured-VRAM regression test in
`tests/gpu_test.cpp`. Normal play restores the configured rendering policy.
`DS_FF_RASTER=0` provides an A/B measurement switch; `emu.ff_skip=0` disables
this optimization along with presentation skipping.

The fork now distinguishes ARM7 HALT from console STOP. During emulated lid
sleep, CPU/video/DMA timestamps stay frozen while the RTC and silent frontend
audio continue. This fixes the scanline DMA phase corruption seen after opening
the lid in New Super Mario Bros. The fork removes native gameplay OSD drawing
(FF/FPS, state labels and screenshot flash). RGDS owns the game counter,
fast-forward badge, volume HUD and in-game controls.
The legacy pause/settings painter and its source files are removed too. Pause
requests go directly to the RGDS supervisor. Loading, save galleries and error
presentation belong to RGDS; firmware and game graphics remain guest content.

**In-game menu → Graphics → 3D resolution** selects Native or experimental
2×–5× Vulkan rendering for this engine. The page also contains 3D edge smoothing,
screen filters, frameskip, active renderer information and Save & restart.
Resolution, smoothing and frameskip apply live at a safe frame boundary; a failed
change keeps the previous renderer. The pre-launch Game options resolution picker
is available too and sets the next launch. DS controls include a ten-unit DS nickname,
defaulting to RG DS Core; console identity changes apply on a fresh game boot. Native
remains the default and fastest option. The enhanced binary uses a private
PanVK/kbase driver and Vulkan loader; it does not replace system graphics
libraries. A 5× 3D target is 1280×960, composed to the handheld's 640×480 panel
with native-grid 2D art. Smooth mode averages four coverage-weighted 3D
samples per panel pixel; at 5× this is an exact 2×2 box resolve. Transparent
texels do not darken polygon edges. Sharp mode keeps point sampling. The independent native software raster still supplies
all guest-visible capture pixels and saved state. High-resolution visuals use
an experimental hardware rasterizer and are not a claim of perfect accuracy
or 60 FPS in every game. See `docs/RGDS_VULKAN.md` and the dated benchmark report.
The compositor caches its horizontal sample positions until the 3D scale,
edge-smoothing mode or nine-bit scroll offset changes. It preserves the DS
transparent half of the scrolling plane and does not change guest rendering.

The fork's source is `dscore/` ([FORK.md](../../../dscore/FORK.md)); edit it directly.
**Graphics → 2D scaling** (Off, Crisp, HD, HD soft) resamples the 2D layers of the enhanced screen
at 2×–5×; see `docs/RGDS_VULKAN.md`. It is display only and applies live.

Build from this checkout with `make build-rgds-experimental`. The build fingerprints
`dscore/`, compiles a private copy, and records the fingerprint, so editing the source
makes an earlier build stale until it is rebuilt. The normal package includes the
binary, license, notice, complete fork source and build recipe. No upstream checkout
or patch step is involved; upstream is only the pin recorded in FORK.md.

## Online play (Nintendo WFC)

**DS controls → Online play** (Off, Wiimmfi, WiiLink WFC) puts a game on the
fan-run successors of Nintendo Wi-Fi Connection. The launcher writes
`online=1|2` to the game's `ds-input.ini`; `runtime/emulators.py` turns that
into `[net] mode = internet` and `dns = wiimmfi|wiilink` in DSCore's
`config.ini`. Online play wins over Local wireless (the engine runs one or
the other). A Kid profile gets neither unless the owner allowed wireless.

In the engine, the emulated access point (`io/wifi.cpp`, melonDS's WifiAP)
hands the game's Ethernet frames to `net/slirp_driver.cpp`, a user-mode
TCP/IP stack: the game's connections are ordinary sockets of the engine
process, so it needs no privileges and works on any link the handheld has.
DHCP gives the game 10.0.2.15 and DNS 10.0.2.3; queries to 10.0.2.3 are
rewritten to the chosen service (`SlirpDriver::parse_dns`: Wiimmfi
178.62.43.212, WiiLink WFC 5.161.56.11, or `--dns host|ADDRESS`). Both
services answer every `nintendowifi.net` name with their own servers and
accept the DS's SSL, so no game patch is needed.

The generated firmware's connection slot 1 names the access point
(`bios::kAccessPointSsid`, `RGDS-AP`), and the access point beacons the same
name: a WFC game scans passively and joins only a network its settings list.
On the first sign-in the game writes its WFC user ID (the root of every friend
code) into that slot, so the generated firmware keeps its three Wi-Fi slots in
`firmware-wifi.ovr` beside the player's `config.ini` (the same sidecar format a
real dump's `.ovr` uses; the user settings still come from `[user]`). Deleting
that file gives the console a new WFC identity and friend codes.

Verified on the Mac with the headless frontend and Mario Kart DS (2026-11-19):
first-time WFC notices, sign-in to Wiimmfi, the Regional lobby's "Searching
for opponents", and the user ID written and kept. Not yet verified on the
handheld or in a race against real players.

## Rebuilding the corresponding-source archive

`DSperate/` contains the complete fork source (the `dscore/` tree). `rgds/` contains
the build tools and `dscore/FORK.md`. A direct CMake build of `DSperate/` does not
require git or patch application. Use Zig 0.13 targeting
`aarch64-linux-gnu.2.17`, CMake and Ninja, plus an ARM64 SDL2 link SDK (`rgds/tools/engine_toolchain.py --fetch`
builds one). Generate compiler wrappers with `rgds/tools/engine_toolchain.py::toolchain`, and configure
CMake with the options in `rgds/tools/build_rgds_experimental.py::OPTIONS`,
`SDL2_DIR=<sdk>/lib/cmake/SDL2`, `CMAKE_FIND_ROOT_PATH=<sdk>` and
`CMAKE_PREFIX_PATH=<sdk>`. Build target `dsperate`; install the result as
`rgds-experimental`. Do not install the dummy SDL link SDK on the handheld.

## Physical acceptance still required

Use a legally supplied Phantom Hourglass ROM and a battery save at its map
annotation screen. Record ROM hash, build hash, model, power profile and shader.
Draw slow and fast diagonals, circles, small letters, intersecting strokes and
lines along all four edges. Verify the pencil follows the contact, lifts cleanly
and never joins separate strokes. Repeat after pause/resume and lid sleep.
Check microphone/lid puzzles, battery saves, manual/automatic checkpoints and
exit/relaunch. Record a video for visible lag; synthetic input tests cannot
measure contact-to-display latency or the touch controller's sampling limits.

Benchmark baseline DSperate and this fork from equivalent in-game saves with
native resolution, shader Off, no frameskip, audio on and the same power profile.
Use completed emulated FPS and frame-time percentiles, plus a sustained thermal
run. Do not treat title-screen FPS or host test speed as Zelda acceptance.
