# RG DS Experimental 1

An experimental GPL-3.0-or-later fork of the pinned DSperate core. The original
DSperate entry remains the default. This fork has its own engine ID,
`rgds-experimental`, and its own battery saves, configuration and checkpoints.
Choose **Game options → Emulator → RG DS Experimental**.

The first build targets Cortex-A55 with ARM64 JIT, NEON and LTO. It removes
network transport and achievement integration from the binary. It retains
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

Build from this checkout with `make build-rgds-experimental`. The normal
package includes the binary, license, notice, complete modified upstream source
and build recipe. The pinned pristine checkout is never edited.

## Rebuilding the corresponding-source archive

`DSperate/` contains the complete modified source, including the stylus helper.
`rgds/` contains the original build tools and patch files. A direct CMake build
of `DSperate/` does not require git or patch application. Use Zig 0.13 targeting
`aarch64-linux-gnu.2.17`, CMake and Ninja, plus an ARM64 SDL2 link SDK. Generate
compiler wrappers with `rgds/tools/build_engines.py::toolchain`, and configure
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
