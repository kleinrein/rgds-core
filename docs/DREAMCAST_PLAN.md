# Dreamcast on RG DS OS

Research of 19 November 2026, then wired in the same day. Speed has **not been measured on the handheld yet**: the
estimates below come from Flycast's own source (commit 3a5267a, 13 Nov 2026), what we measured for PSP and N64 on the
same chip, and reports from RK3566 handhelds, which have the same Mali-G52 and slightly slower A55s.

## Status

| Part | State |
|---|---|
| Flycast core package (`os/buildroot/package/libretro-flycast`, in the defconfig) | Built by Buildroot in the VM, patch applied, `retro_rgds_vmu` exported, links only libc/libstdc++ |
| Runtime: system `dc`, folders, core, no CPU pin, artwork, ScreenScraper | Done, host tests pass |
| Launcher: Dreamcast shelf, `dc`/`dreamcast` folders, playtime colour, second-screen picker | Done, self-test passes |
| Player: Flycast options, shared-texture frames, sticks kept, VMU on the second screen | Done, host-built, play-tests pass |
| OS: `/data/roms/dc` made at boot, the session passes it to the launcher | Done, not booted |
| Device spike | **2026-11-19**: Crazy Taxi drives at **55-60 fps** on the RG DS (was ~30) after Flycast patches 0002-0003; see "Spike results" |
| Crazy Taxi, 2026-11-24 | Patches 0006 (wait loop) and 0007 (frame drop by game lag): the game holds 60 in the scripted drive, 53-58 frames drawn where the GPU is the limit; on the RG DS until reboot, not in an image |
| Crazy Taxi, 2026-11-25 | The picture was held at 57 a second by the player's sound steering, not the GPU (about 60% busy): the Dreamcast now gets frames by the clock alone; menus back at 60. Player change, needs an image |
| Crazy Taxi, 2026-11-25, main thread | Each frame was copied twice on its way to the panel. Patch 0008 plus the player's framebuffer texture remove both: the core's work per frame in the drive 12.6 -> 10.3 ms, late passes 11 -> 3 of 300, loop 17.5 -> 16.9 ms. Core + player + bridge together: on the RG DS in image `0.9.2-2d1a6fa0f6e1-osf1d60aec` (slot b) since 2026-11-25, not yet hand-played or measured from a normal launch |

## Short answer

- **Emulator: Flycast as a libretro core in RG DS Play.** It is the only maintained, fast open-source option with an
  ARM64 JIT and a GLES 3 renderer. Redream is closed. Deecy (Zig) and nullDC-rust are experimental and target x86.
  Hollycast is a Flycast fork with extra features, not extra speed. Running it in RG DS Play gives Dreamcast the same
  in-game menu, states, frames and shaders as every other system.
- **60 fps is realistic for most of the library**, with what PSP and N64 taught us: threads not pinned, frames shared
  as a texture instead of `glReadPixels`, threaded rendering, native 640×480. The heaviest 3D games (Shenmue, Le Mans
  24 Hours, some Sonic Adventure 2 stages, Ecco) will likely need frameskip or an SH4 underclock.
- **The RG DS panel is exactly Dreamcast resolution.** The Dreamcast draws 640×480 and so does the RG DS top panel: a
  1:1 picture with nothing to upscale. The Plus (1024×768) scales the same 640×480 frame by 1.6×.
- **Memory fits on both models**, the Plus's 1 GB included, without texture upscaling or texture packs. Expected
  150-250 MB resident (estimate; the spike measures it).

## The hardware budget

| | RG DS | RG DS Plus |
|---|---|---|
| SoC | RK3568, 4× Cortex-A55 up to 1.992 GHz | same |
| GPU | Mali-G52 2EE, Panfrost (Mesa 25.0.7 + our PGO build), GLES 3.1 | same |
| RAM | 3 GB | **1 GB** |
| Top panel | 640×480 | 1024×768 |
| Dreamcast guest | SH4 200 MHz, 16 MB RAM, 8 MB VRAM, 2 MB sound RAM, PowerVR2 at 640×480 | |

The A55 is an in-order core, so the SH4 JIT thread is the most likely limit, as the PSP emulation thread and the DS
engine have been. Panfrost on the G52 is GLES 3.1 conformant and nothing more; Flycast's GLES 3 renderer needs 3.0.
Flycast's per-pixel transparency sorting (OIT) needs more than we have and would be too slow anyway.

## Why the speed estimate is hopeful

1. **Few render passes.** Flycast draws a frame as opaque, punch-through, then transparent polygons into one target.
   OutRun 2006 on PSP spent its GL thread on ~24 passes a frame at ~0.25 ms fixed cost each. A typical Dreamcast frame
   has one main pass, plus render-to-texture passes only in games that use them.
2. **No upscaling to pay for.** Native 640×480 is full quality on the RG DS, about the PSP's pixel load at the size
   where the PSP's GPU was not the limit.
3. **Threaded rendering is built in.** The SH4 JIT runs on its own thread and `retro_run` only renders, so CPU and GL
   work overlap, like N64's ThreadedRenderer (13 → 8 ms a frame there).

RK3566 handhelds (RG353 family, 1.8 GHz) are reported as running most Dreamcast games well at 640×480 under Flycast,
with frameskip in the most demanding games and the 2 GB models steadier than the 1 GB ones. Our RK3568 runs about 10%
faster and our Mesa is tuned further than stock ROCKNIX's.

## Expected speed by game type (expectation, not measured)

| Type | Examples | Expectation |
|---|---|---|
| 2D and fighting | Marvel vs. Capcom 2, Street Fighter III 3rd Strike, Capcom vs. SNK 2, Guilty Gear X | 60 fps |
| Light 3D | Soulcalibur, Dead or Alive 2, Power Stone 1-2, Crazy Taxi, Jet Set Radio, ChuChu Rocket! | 60 fps, a few dips |
| Mid 3D | Sonic Adventure 1-2, Rez, Skies of Arcadia, Resident Evil Code: Veronica, Metropolis Street Racer | Mostly 60, frameskip in heavy scenes |
| Heavy 3D | Shenmue 1-2, Le Mans 24 Hours, Ecco the Dolphin, F355 Challenge | Below 60 without frameskip or underclock |

## What is wired

### Core (`os/buildroot/package/libretro-flycast`)

- Flycast pinned at `3a5267a`, CMake with `LIBRETRO=ON USE_GLES=ON USE_VULKAN=OFF USE_OPENMP=OFF`, `-mcpu=cortex-a55`,
  bundled libraries static. No OpenMP: its texture threads would take the cores the SH4 and render threads need.
- Patch `0001-vmu-screen-hook.patch`: Flycast keeps each VMU's decoded 48×32 picture; `retro_rgds_vmu(index, dots)`
  copies it out and returns when the game last drew it.
- Speed patches `0002`-`0008` (draw ranges, whole-buffer uploads, unchanged poly state, depth from the vertex shader,
  the wait loop, frame drop by game lag, drawing into the frontend's framebuffer): each is described under "Spike results" with its numbers and its off switch.
- The checkout with every submodule is 865 MB with history; Buildroot's git download would clone all of it. The VM's
  `~/own-os/dl/libretro-flycast/` holds a hand-made tarball of a shallow checkout (172 MB, no `.git`), as was done for
  PPSSPP. A clean machine downloads the full checkout. On 2026-11-25 that tarball was gone from the VM, and a
  `libretro-flycast-dirclean` sent Buildroot cloning everything onto a disk with 1.8 GB free (it filled). The tarball
  is now made on the Mac (`.cache/flycast-src/`: `git fetch --depth 1` of the pinned commit, `submodule update
  --init --recursive --depth 1`, tar without `.git`, named `libretro-flycast-<commit>-git4.tar.gz`) and the VM's
  download folder holds a link to it. Check the tarball is there before a dirclean.
- The core is 33 MB before the image's strip step. The image's rootfs (235 of 768 MiB) and the 512 MiB OTA cap have
  room.
- **The next image build needs `--reconfigure`**: the defconfig now enables the package, and `check_cores` stops a
  build whose `output/rgds-os/.config` lacks it.

### Settings the player forces (`play/src/core/libretro_core.cpp`)

| Option | Upstream default | Ours | Why |
|---|---|---|---|
| `reicast_internal_resolution` | 640x480 | 640x480 | 1:1 on the RG DS; anything higher only costs fragment time |
| `reicast_threaded_rendering` | enabled | enabled | CPU and GL overlap; the main speed lever |
| `reicast_auto_skip_frame` | disabled | some | drops a frame only when the SH4 falls behind |
| `reicast_alpha_sorting` | per-triangle | per-strip | per-pixel (OIT) needs more than GLES 3.1; per-strip halves the translucent draws (Crazy Taxi ~52 -> ~57 game fps) |
| `reicast_anisotropic_filtering` | 4 | off | made no difference on PSP (G52) |
| `reicast_emulate_framebuffer` | disabled | disabled | slower, and N64's framebuffer copies crashed Panfrost |
| `reicast_texupscale` | 1 (off) | 1 (off) | xBRZ costs CPU and memory |
| `reicast_gdrom_fast_loading` | disabled | enabled | shorter loads; small accuracy cost |
| `reicast_digital_triggers` | disabled | enabled | the handheld's L2/R2 are buttons |
| `reicast_per_content_vmus` | disabled | VMU A1 | each game's saves in its own VMU file in the save folder, not the BIOS folder |
| `reicast_vmuN_screen_display` | (per VMU) | disabled | the VMU goes to the second screen, not over the game |

**The keys are `reicast_...`** (Flycast's libretro `CORE_OPTION_NAME`, kept for old configs). Until 2026-11-23 the
player sent `flycast_...` keys, so none of the settings above reached the core: it ran on Flycast's own defaults (no
frameskip, analogue triggers, shared VMUs), and every option test in "Spike results" before that date (frameskip,
SH4 clock, threading, DSP, sorting, resolution) measured the same unchanged core.

To try others without a rebuild: `RGDS_PLAY_CORE_OPTIONS="reicast_alpha_sorting=per-strip (fast, least accurate);reicast_enable_dsp=disabled"`.
Left at upstream for the spike to judge: `reicast_enable_dsp` (on), `reicast_delay_frame_swapping` (on),
`reicast_mipmapping` (on), `reicast_sh4clock` (200).

### Player and runtime

- **No CPU pin** for `dc` (`emulators.py`): PPSSPP's threads lost half their speed inheriting the launcher's pin.
- **Frames shared as a texture** (`gpu_direct` for `dc`, as PSP and N64): no `glReadPixels`, which cost PSP 17-20 ms.
- **Sticks stay sticks** (`system_has_stick`): the Dreamcast pad has both a stick and a D-pad.
- **Buttons by position**, as PSP: the handheld's bottom button is the Dreamcast's A (confirm), the right one B.
- **Pacing by sound**, the libretro default; PSP needed `self_paced`, Flycast may not. The spike checks this first.
- **Games**: `.gdi` and `.cdi` anywhere; `.chd`, `.cue`, `.m3u` (and `.zip`) in a `dc` or `Dreamcast` folder, since
  the PlayStation reads those too. On RG DS OS the folder is `/data/roms/dc` (the DC share in file sharing).
- **BIOS**: `dc_boot.bin` and `dc_flash.bin` in `/data/rgds/bios/dc`. Without them Flycast boots with its HLE BIOS,
  which runs many games but less reliably. We ship no BIOS, as with every other system.
- Library artwork from Libretro's "Sega - Dreamcast" catalogue and ScreenScraper system 23.

### Second screen: the VMU

The controller's VMU has a 48×32 one-colour screen that games use for health, maps and mini-games. The lower panel
now shows it large (`Vmu` in `play/src/companion/keypad.cpp`): a white VMU shell, the LCD at 8 pixels a dot, dark
dots on the green glass with unlit dots as faint ghosts, as on the real screen. It appears once the game has drawn on
the VMU; until then the second screen keeps the info screen. In the game menu, Second screen offers "VMU screen"
first after Companion (translated in all 22 languages).

Not built: a second VMU, or the VMU's own buttons and mini-games (those need VMU emulation of its own, which Flycast
does not do in libretro).

## Memory

Where Flycast's memory goes, from its source:

| Item | Size | Notes |
|---|---|---|
| Guest RAM, VRAM, sound RAM | ~26 MB touched | Backed by `/dev/shm`; reserved at the Naomi maximum (32+16+8 MB), tmpfs only allocates touched pages |
| Address space reservation | 512 MB virtual | `PROT_NONE`, costs no RAM; RSS and VSZ will look very different |
| JIT code cache | 11 MB | 10 MB + 1 MB temporary |
| Decoded textures | tens of MB | 8 MB of guest textures become RGBA8 GL textures, in system RAM on this unified-memory chip |
| Core library | 33 MB unstripped | smaller after the image's strip |
| Save state | ~25-30 MB each | estimate; our player holds a state in memory while it writes |

Estimate: 150-250 MB resident while playing. PPSSPP sits at 143-203 MB.

On the **Plus (1 GB)**:

- `/dev/shm` must be a tmpfs; rcS mounts it. Without it Flycast puts guest RAM in a file on the card.
- No texture upscaling, no custom texture packs, no texture preload (all off in our options).
- Prefer `.chd` over `.gdi`/`.cdi`; none of Flycast's readers load a whole disc into RAM.
- Measure the launcher's own memory while a game runs, and the peak during a save state, before deciding on zram.

## Risks

- **Panfrost bugs.** N64 found a framebuffer-copy crash and PSP a Mesa macro overrun. Render-to-texture games are the
  most likely place for new ones.
- **Mixed speed inside one game.** As OutRun showed, compare drawn frames over a fixed 90 s run, never short windows.
- **Build VM disk** is at 95% after the build (2 GB free).
- **No games on hand.** None are on the Mac or the handheld.

## Spike

Needs: the handheld online (USB `10.55.0.1` or Wi-Fi), a few games across the four classes above in
`/data/roms/dc`, and ideally `dc_boot.bin` + `dc_flash.bin` in `/data/rgds/bios/dc`. Nobody else may hold the panels,
and the owner must not be playing at the time.

Staging from the Mac (core from the VM, player from `tools/build_play.py device`, this checkout's runtime):

```sh
D=root@10.55.0.1
ssh $D mkdir -p /data/rgds/dc-spike/runtime
scp -O build/dc-spike/flycast_libretro.so build/play/device/rgds-play os/device-bench/dc_spike.py os/device-bench/session_hold.sh $D:/data/rgds/dc-spike/
tar -C runtime -cf - . | ssh $D tar -C /data/rgds/dc-spike/runtime -xf -
ssh $D 'mkdir -p /data/rgds/dc-spike/assets && cp -r /usr/share/rgds/releases/$(cat /usr/share/rgds/active)/assets/shaders /data/rgds/dc-spike/assets/'
```

On the handheld (a userdata of its own under the spike folder, never the owner's):

```sh
cd /data/rgds/dc-spike && sh session_hold.sh on
python3 dc_spike.py "/data/roms/dc/Game.chd" 90 FRESH=1
sh session_hold.sh off
```

Per game, 90 s in Performance mode: drawn frames and frames over budget (`RGDS_PLAY_PROFILE` lines), the game's own
clock in a shot, peak resident memory (printed at the end), GPU time from fdinfo as for PSP. Then one option at a time:
threaded rendering, alpha sorting, DSP, auto frameskip, delay frame swapping, SH4 clock, `RGDS_PLAY_DIRECT`, and
`self_paced` if sound or pacing is off. Then the launcher path (library, in-game menu, states, frames, shaders, the
VMU screen), a `dc` bezel check, and the Plus memory figures (`mem=1G` on a trial boot line).

## Spike results

2026-11-19, RG DS (3 GB), image 0.9.1-0de9d393e37c-os70534dc8, Flycast 3a5267a, real `dc_boot.bin` (md5
e10c53c2f8b90bab96ead2d368858623, no `dc_flash.bin`), player built from this checkout, default power mode.

| Game | Scene | Emulated frame (`run`) | Over 16.7 ms | Shown frame | Peak resident |
|---|---|---|---|---|---|
| Crazy Taxi (USA), Redump `.cue` | Boot, Sega logo (first 20 s) | 8-20 ms avg, worst 118 ms | 11-49 per window | 20-30 ms avg | |
| Crazy Taxi | "No VMU found" screen (static 2D) | 5.5-5.8 ms avg, worst 7-10 ms | 0 | 16.7-17.6 ms avg, one 33 ms frame per window | 122-124 MB |

- **Memory:** 122-124 MB resident for the whole player (Flycast's 16 MB RAM + 8 MB VRAM + 2 MB ARAM included), with
  the guest RAM on `/dev/shm` as planned. That leaves plenty on the Plus's 1 GB.
- **Boot:** the GD-ROM image (1.2 GB `.cue`/`.bin`) starts from the exFAT card in about 20 s to the first screen;
  shader compiles on the first run cause the slow frames above. The second run has the Mesa cache warm.
- **Bug found and fixed:** the game said "No VMU found". Flycast plugs the expansion slots in only once every port has
  had `retro_set_controller_port_device` (RetroArch sets all four); the player set port A only. It now sets ports
  B-D to none for `dc` (`play/src/core/libretro_core.cpp`). Confirmed on the device: the game then says "VMU in A1
  is in use" and offers to create its file, and the lower screen switches to the VMU view (128 MB peak over 150 s).
- **Gameplay, Crazy Taxi driving** (scripted input `RGDS_PLAY_INPUT=os/device-bench/dc_crazy_taxi.input`: menus with B,
  the Dreamcast's A, and Start, then R2 to drive; the road is reached ~140 s in): every frame over budget, **32.5-33.6
  ms a frame (about 30 fps)**, 155-165 MB resident. CPU at 1.992 GHz (performance governor), GPU at 800 MHz.
  - Per thread over 5 s of driving: the player's main thread (GL drawing) 68%, `Flycast-emu` (SH4) 65-67%, audio 4%.
    Neither core is full: the two take turns rather than overlap, each needing ~22 ms a frame.
  - 320x240 internal resolution gave the same 32.7 ms, so the GPU's fill rate is not the limit; the drawing cost is
    CPU-side (driver work per draw call).
  - `flycast_auto_skip_frame=more`, `flycast_sh4clock=160` and `flycast_threaded_rendering=disabled` each gave the same
    32.5-32.7 ms, and the `Flycast-emu` thread still ran with threading disabled. That is suspicious: these options may
    not have reached the core. The player now logs each `RGDS_PLAY_CORE_OPTIONS` override ("play: core option ...");
    rerun one of them to tell "no effect" from "not applied" before drawing conclusions.
- **Corrected after review:** the first scripted drive pressed Start into gameplay (pauses), but a clean drive and the
  owner's own hand-played drive agree: **28-35 ms a frame, about 30 fps, so the game runs at half speed** (Crazy Taxi's
  timer drops ~13 s per 30 s). The option overrides do reach the core (`play: core option ...` in the log); Flycast
  keeps its emulation thread with threaded rendering off.
- **Where the time goes** (sampling profile, `os/device-bench/rgds_prof.c` preloaded, 1 kHz CPU-time samples over 70 s
  of driving, names from an unstripped relink of the same Flycast objects and Mesa's staging copy; report with
  `rgds_prof_report.py --maps prof.livemaps`):
  - The drawing thread (the player's main thread) is the limit: computing 63%, waiting on GPU fences 35%. Its CPU time:
    **Mesa/Panfrost 64%**, Flycast's renderer 16% (`SetGPState`, TA parsing, triangle sort), libc 15% (mostly one
    copy routine). Top function: **`u_vbuf_get_minmax_index_mapped` 10.6%**, then `panfrost_draw_vbo` 5%, then texture
    and sampler state updates (`_mesa_update_texture_state`, `cso_set_samplers`, `set_tex_parameter*`).
  - That is Mesa scanning each draw's index buffer on the CPU for its vertex range, because Flycast draws with
    `glDrawElements` (no range), and doing it through `u_vbuf`, the translation path for vertex input the driver
    doesn't take directly.
  - The SH4 thread waits on the drawing thread half the time: SH4 JIT 63%, AICA sound ~20% (DSP 7%).
- **Fix 1 done: `glDrawRangeElements`** (`package/libretro-flycast/0002-gles-draw-range-elements.patch`; the libretro
  build reaches GL through libretro-common's glsm, which had no wrapper for it). Panfrost's own
  `panfrost_get_index_buffer_bounded` was doing the scan (via `u_vbuf_get_minmax_index`) because Flycast rewrites the
  index buffer every frame. Driving: **25-31 ms a frame, was 28.6-34.7 (about 10% faster, still ~35 fps)**; the scan
  is gone from the profile and computing the ranges costs 1%.
- **After fix 1, the drawing thread:** Mesa 58% (draw emission `panfrost_draw_vbo` 7%, then texture/sampler/shader
  state updates spread over many functions: `_mesa_update_texture_state`, `cso_set_samplers`,
  `panfrost_update_shader_variant`, `panfrost_emit_const_buf`, `st_update_*`), Flycast 20% (`SetGPState` 7%, TA
  parsing 3%), kernel time in `ioctl` 12% (GPU job submission and buffer work, all through RG DS Core's bridge's
  `ioctl` hook). The cost is now per-draw state churn, not one hot spot.
- **Fix 2 done: whole-buffer uploads** (`0003-gles-whole-buffer-uploads.patch`). The drawing thread's 35% GPU wait was
  Flycast's `GlBuffer::update` using `glBufferSubData` over the start of the vertex/index buffers: that range still
  held last frame's data, so Panfrost's `panfrost_ptr_map` took its "Synchronized write" path (flush, then wait for the
  GPU) on every upload. `glBufferData` each frame lets Mesa discard the whole resource (same size) or reallocate, and
  Panfrost takes a fresh BO from its cache. **Driving: 14.9-16.8 ms a frame (4-162 of 300 over budget), was 25-31:
  about 55-60 fps.** The drawing thread no longer waits on fences (running 71%, the rest idle in poll/futex).
- **After fix 2:** the two threads are about even. Drawing: Mesa 53%, Flycast 21%, kernel `ioctl` 11.5%, `memcpy` 3%.
  SH4 thread: JIT 67%, AICA sound ~17% (DSP 6.5%). Both are close to the 16.7 ms budget, so the next gains are on both
  sides.
- **Image builds:** patches 0002-0004 were added after the VM's Flycast build directory was made, and were applied
  there by hand, so the image built on 2026-11-20 (0.9.1-os8974f5f1) carries a Flycast without them (installed from the
  17:33 build of 11-19). The next image build needs `make libretro-flycast-dirclean` first (or `own_os_build.py
  --reconfigure`) so Buildroot applies every patch in `package/libretro-flycast/`.
- **2026-11-20 runs measured that unpatched image core by mistake:** `dc_spike.py` linked the image's
  `/usr/lib/libretro/flycast_libretro.so` before its own copy (it only linked its own when no Flycast existed). Those runs
  (32-34 ms, "0004 and DSP off change nothing") say nothing about the patches; fixed in the runner, to be redone.
- **Redone 2026-11-20 with the right core** (image 0.9.1-os8974f5f1, quiet device, profiler on, 10 driving windows =
  3000 frames each):

  | Core | Emulated frame | Frames over 16.7 ms | Shown frame interval |
  |---|---|---|---|
  | 0001-0003 | 16.12 ms | 1072 | 21.8 ms |
  | + 0004 (`0004-gles-skip-unchanged-poly-state.patch`) | 15.78 ms | 714 | 21.8 ms |
  | + 0004, `flycast_enable_dsp=disabled` | 15.94 ms | 894 | 21.9 ms |

  0004 cuts over-budget frames by a third; the DSP off buys nothing (the SH4 thread waits on drawing anyway), so the
  DSP stays on. **The shown interval (21.8 ms, ~46 fps) is the real gap:** the player's main thread draws (computing
  ~80%) and then waits in `poll` for the page flip (14-19%), so a frame that finishes a little late waits a whole
  refresh. Kernel time in GPU job submission (`DRM_IOCTL_PANFROST_SUBMIT`, ~10 a frame at ~84 µs) is 12% of the
  drawing thread; `WAIT_BO` dropped to 2 s per 200 s run (49 s on the unpatched core).
- **Memory clock and bandwidth:** the RAM is LPDDR3 and already at stock's 920-924 MHz (kernel/bootloader session,
  2026-11-20); no higher clock to try. Its 1 s DDR log over the three drives: driving at a median 1,981 MiB/s (read
  1,614, write 374), p95 2,240, max 2,287, about 30-35% of the ~7,000 MiB/s peak and half of what is practical. Memory
  is not the wall (a burst within a frame could still be, which 1 s averages can't show).
- **Pacing (2026-11-20):** the profile's "frame" is retro_run to retro_run, so ~21.8 ms means the core runs ~46
  times a second: the game itself runs at about 77% speed in the drive, not just fewer shown frames. Each loop pass
  spends ~3-6 ms outside the core, mostly the upper panel's `SDL_RenderPresent`: SDL's KMSDRM swap first waits for the
  previous page flip, so once a frame runs late the loop is pinned to the vblank grid (the main thread sits in `poll`
  ~19%). Not the cause: the lower panel (presenting it only every 10th pass changed nothing, 22.1 vs 21.8 ms) and the
  bridge (the owner's unbridged drive shows the same run + 3-6 ms). The loop's catch-up (`owed`, capped at 4 frames)
  can't win when the core alone takes ~95% of a frame. Ways out: (a) make the core's frame cheaper (~14 ms leaves room
  for the present), (b) a present that doesn't wait for the previous flip (drop the picture when a flip is pending and
  keep emulating: full game speed, fewer shown frames), (c) Flycast's own frameskip deciding with the present time
  included.
- **The present is not the limit (2026-11-20, tested and reverted):** a player-side skip of presents while a flip
  was probably pending changed nothing (21.7 vs 22.3 ms core interval). The main thread's 19-23% `poll` was on
  `/dev/dri/card0` with no timeout, SDL's KMSDRM page-flip wait (`dc_polls.py`). An SDL patch that dropped the frame
  instead of waiting (hint `SDL_KMSDRM_DROP_PENDING`) removed that wait entirely (main thread running 99%) but moved
  the time into Flycast's frame (15.4 -> 18.8 ms) at the same 21.5 ms core interval. **The GPU needs ~21.5 ms per
  frame in this drive**; the flip wait was only where it showed. Since 320x240 changed nothing, that GPU time is
  geometry, tiling and per-job overhead rather than pixels. The next lever is GPU work per frame: ~10 job
  submissions a frame (each a separate tile pass on Mali: the framebuffer stored and reloaded), and per-draw cost.
- **Early flushes are not it:** `PAN_MESA_DEBUG=perf` over a drive logged 63 early flushes in the 3,000 driving
  frames, all "AFBC write staging blit" (texture uploads; 2,561 over the whole run, mostly loading). Flycast already
  merges consecutive polys with equivalent state into one strip draw (`makePrimRestartIndex`, restart index between
  them), so the remaining draws differ in state. Next measured: half resolution again (the 11-19 test ran while the
  upload stall hid the GPU) and per-strip translucent sorting (fewer draws than per-triangle).
- **GPU time measured, 2026-11-23** (image 0.9.1-os0daf9ef0, same 0004 core and drive, `os/device-bench/dc_run_m.sh`:
  Panfrost's per-process engine time from `/proc/PID/fdinfo` with `.../fde60000.gpu/profiling` = 1, sampled by
  `dc_gpu.py`). In the drive the **fragment engine is busy ~780 ms of every second** (~17 ms per shown frame at ~45 fps)
  and the vertex/tiler engine ~145 ms/s (~3 ms). The results screen after the drive: 210 + 19 ms/s. **320x240 gives the
  same 775 ms/s fragment**. That looked like "not pixels", but **the option never applied** (see the `reicast_`
  note under the settings table): the stats core printed a 640x480 target with 320x240 asked for. Core interval in the drive per case: baseline
  22.4 ms, 320x240 22.1, per-strip sorting 23, `auto_skip_frame=more` 23: no setting moves it. Frameskip can't help as
  built: `retro_run` loops until a frame is rendered and the SH4 waits on that, so game speed follows the GPU. A
  1280x960 run didn't start (another session had bind-mounted a new `rgds-play` over the release's, which fails the
  release SHA-256 check).
- **With the options really applied (2026-11-23, image 0.9.1-os0daf9ef0, same drive, `dc_run_o.sh`; stats core
  `RGDS_FLYCAST_STATS=2` counting guest vblanks per second = game speed, 59.94 = full):**

  | Settings (plus the player's set) | Game fps | Shown fps | Translucent draws | GPU fragment ms/s |
  |---|---|---|---|---|
  | frameskip off (what ran before 11-23) | ~45 | ~45 | ~750 | 821 |
  | frameskip some (the intent) | ~52 | ~44 | ~750 | 741 |
  | per-strip, frameskip off | ~54 | ~54 | ~365 | 775 |
  | **per-strip + some (new default)** | **~57** | ~53 | ~365 | 726-740 |
  | per-strip + some + DSP off | ~58.5 | ~54 | | 695 |
  | 320x240 + some | ~57.5 | ~56 | ~750 | 247 |

  320x240 cuts the fragment time by 70%, so the pixels are most of the GPU's cost after all, yet the game stays at
  ~57: past the GPU the **SH4 thread is the limit** (`Flycast-emu` 82-89% of a core, the drawing thread 64-66%). A timed
  split per list (`RGDS_FLYCAST_STATS=1`, a fence wait after each list) gave translucent ~53%, opaque ~30%, modifier
  volumes ~14%, final blit ~1.2 ms. The stats core is a hand edit in the VM's build tree (`*.pre-stats` beside each
  file), not a package patch.
- **The owner's own drive, 2026-11-23 09:43** (launched from the launcher, image 0.9.2-os2f9cf014 with the
  `reicast_` player, watched read-only by `os/device-bench/dc_watch_wait.sh`): the same picture as the scripted drive,
  only heavier. `Flycast-emu` (SH4) 90-97% of a core, the drawing thread 63-66%, the GPU 840-905 ms/s, nothing else
  busy (rgds-music 2-3%, the session's python 6%), all threads on CPUs 0-3 at normal priority, 1992 MHz performance,
  GPU 800 MHz, 57-64 C. The presentation stats (`RGDS_FRAME_STATS`): average 42 fps over 147 s, p50 16.7 ms but p95
  64 ms and p99 83 ms, 2,004 of 6,042 frames over 20 ms. So the shown frames come in bursts with gaps of 3-5 vblanks
  (frameskip "some" dropping draws while the SH4 is behind), which reads as 20-30 fps. Both the SH4 thread and the GPU
  are at their limit in the drive; no setting left gets both under 16.7 ms.
- **Where the SH4 thread goes (2026-11-23, rgds_prof on the drive, unstripped Flycast):** SH4 JIT code 63%, the sound
  chip ~25% (DSP `DynCode` 7.7% + pack/unpack 1.5%, `AICA_Sample` 6.6% + channel steps ~1.5%, the ARM7 recompiler 4.9%
  + its interpreter and memory ops ~2.5%), the scheduler and interrupts ~3%. Underclocking the SH4
  (`reicast_sh4clock` 180 or 160) changed the game speed by only ~0.5 frames a second. The drawing thread: Mesa 38%, kernel
  time in `ioctl` ~20% (~10 GPU job submits a frame at ~100 us, ~20 `WAIT_BO` busy checks a frame), Flycast's TA
  parsing and sorting ~10%.
- **One option per drive (2026-11-23, per-strip + frameskip some):** `delay_frame_swapping` off and fog off change
  nothing; DSP off 58.1 game / ~54 shown; **modifier volumes off 58.3 / 57.8** (GPU fragment 539 vs ~650-730 ms/s).
- **Patch 0005, depth from the vertex shader (2026-11-23):** every GLES 3 pixel shader wrote `gl_FragDepth` (log2 of
  1/w), which turns off Mali's early depth test and hidden-pixel removal for every draw. The vertex shader now writes
  1/w scaled by `depth_scale` (already computed for the GLES 2 path), linear on screen because w is 1. Drive: game 57 ->
  58.8, shown 54 -> 58; captures at 95 s identical (shadow, hedges, road). Linear depth in 24 bits may z-fight far away
  in other games: `RGDS_FLYCAST_VTXDEPTH=0` turns it off. Package patch written and dry-run applied on 0001-0004; not
  in an image yet. Built with 0001-0005 after a dirclean on 2026-11-23 (md5 be4f8708...; the VM's build tree now holds
  the package build, the measuring edits are kept as `os/device-bench/flycast-stats.patch`) and bound over
  `/usr/lib/libretro/flycast_libretro.so` from `/data/rgds/dc-core/` on the handheld until the next reboot (launches only
  verify the release folder, not `/usr/lib/libretro`). A direct `make` in the VM must set
  `BR2_DL_DIR=$HOME/own-os/dl` as `own_os_build.py` does: without it Buildroot cloned Flycast's 1.4 GB repository into
  `buildroot/dl` and filled the VM disk (removed).
- **Patch 0006, the game's wait loop (2026-11-24):** a ptrace sampler for translated code
  (`os/device-bench/jit_peek.c`: program counters of the `Flycast-emu` thread plus the busiest 4 KiB pages of JIT
  code, with `dc_guest_ram.py` for the guest RAM to disassemble; run by `dc_run_t.sh`) showed one loop of four tiny
  blocks at `0x8c1583f8` taking 16.6% of that thread's running time in the drive (25.7% over the whole 11-23 profile):
  Crazy Taxi's wait for the next frame (a call through an empty hook, then two compares against the vertical blank
  counter, 34 cycles a pass). The block with the loop's backward branch now gives the rest of the time slice back, so
  the loop runs once a scheduler tick (448 cycles) instead; nothing is skipped and guest time is unchanged
  (`core/rec-ARM64/idle_loop.h` says which loops qualify). Same drive, same stats core, off against on
  (`dc_run_u.sh`): **`Flycast-emu` 88-94% -> 79-81% of a core**, same picture, the drive itself 55.6-57 -> 58-58.9
  game frames a second. The detector finds 34 such loops in Crazy Taxi's RAM (host test in `build/dc-spike/idle`),
  8 of them run. `RGDS_FLYCAST_IDLESKIP=0` turns it off, `=2` logs each loop.
- **After 0006 the GPU is the limit in this drive:** the SH4 thread waits 20-25% of the time and the profile is flat
  (the busiest page of translated code is 3.5%). `reicast_auto_skip_frame=more` showed the headroom: 66-72 game
  frames a second, because "more" never waits for the renderer and nothing else paces the emulation thread in the
  libretro build (sound is dropped when its buffer is full, not waited on). So "more" is not usable as it is.
- **Patch 0007, which frames to drop (2026-11-24):** "some" waits for the renderer while the SH4's average speed is
  85% or more, so a scene the GPU can't draw in a refresh slows the whole game to the GPU's pace, and frames go in
  bursts once the average drops. The emulation thread now tracks how far the game is behind the wall clock: under
  8 ms it waits as before, over it the frame is not drawn. Same drive (`dc_run_w.sh`, twice): **the game holds
  59.5-60.2 frames a second through the GPU-limited stretch (58.2-58.5 before), same frames drawn (53-58)**.
  `RGDS_FLYCAST_PACE=0` gives the old rule back, `RGDS_FLYCAST_PACE_MS` sets the limit. How evenly the drawn frames
  are spaced was not measured (the bridge wrote no frame statistics in the spike runs).
- **Installed 2026-11-24:** core with 0001-0007 (md5 2db907c5...) in `/data/rgds/dc-core/` on the RG DS, bound over
  `/usr/lib/libretro/flycast_libretro.so` until the next reboot (the 0005 core is kept beside it as
  `flycast_libretro.0005.so`). The VM's build tree holds the same source (hand-applied, backups `*.pre-idle` and
  `*.pre-pace`; both patches checked to reproduce it), so the next image build carries it with or without a dirclean.
  Not tried on the Plus, in other games, or in a hand-played drive.
- **The GPU is not what limits the drive (2026-11-25).** A stats core with switches (`os/device-bench/flycast_gpux.py`
  applies them to the VM tree on top of `flycast-stats.patch`, `flycast_gpux_restore.sh` undoes both; run by
  `dc_run_x.sh`, table by `dc_x_report.sh`), one drive each, GPU time from fdinfo over the drive:

  | Case | Fragment ms/s | Per drawn frame | Drawn a second |
  |---|---|---|---|
  | as installed (0001-0007) | 601 | 10.6 ms | 56.9 |
  | no translucent list (`RGDS_X_SKIP=t`) | 483 | 8.5 ms | 56.7 |
  | no opaque list (`o`) | 395 | 7.0 ms | 56.7 |
  | no modifier volumes (`m`) | 478 | 8.4 ms | 56.8 |
  | no punch-through list (`p`) | 585 | 10.3 ms | 56.8 |
  | colour cleared at the start, depth and stencil dropped at the end (`RGDS_X_CLEAR`, `RGDS_X_INVAL`) | 580 | 10.2 ms | 56.9 |
  | the GPU's own perspective correction, real w (`RGDS_X_PERSP`) | 615 | 10.8 ms | 56.9 |
  | the same with mediump colours (`RGDS_X_MEDIUMP`) | 614 | 10.7 ms | 57.2 |

  So opaque is ~34% of the fragment time, translucent and the shadows ~20% each, punch-through ~3%, and ~23% is there
  with nothing drawn (the copy to the player's target and the player's own two passes). The perspective rewrite gains
  nothing, the clear and discard ~3% (one drive each, so within the noise). And with half the GPU's work removed the
  drawn frames did not move from 56.8: the fragment engine is about 60% busy, not the limit. None of these became a
  package patch.
- **What held the picture at 57: the player's sound steering (2026-11-25).** The player's loop period was 17.56-17.61
  ms in every case, menus with a 4 ms core frame included: one doubled refresh in twenty. That is the loop holding a
  frame back (5% a pass) while its sound queue is over twice its target. It works for a core whose sound follows the
  frames it is asked for; Flycast's emulation thread makes sound at the game's rate whatever the loop does, so the
  queue never empties and the hold never ends (until the old frameskip rule slowed the game itself: Crazy Taxi's
  results screen ran at 56.8 game frames a second). The Dreamcast now gets its frames by the clock alone, and sound
  that arrives too late is dropped (`play/src/frontend/main.cpp`, `RGDS_PLAY_PACED` 0 steered / 1 one frame a pass /
  2 clock; `dc` defaults to 2). Same drive, `clock` against `steered`: **menus and the results screen 59.8-60 game
  and drawn frames a second (56.8 before), loop 16.72 ms (17.61)**; in the drive the loop is 16.9-17.3 ms against
  17.1-17.5. One frame a pass, as the PSP has it, was worse in the drive (loop ~20 ms, ~50 pictures a second: after
  each frame Flycast drops, the pass waits a whole emulated frame and misses a refresh). Note the stats core's
  "shown" counts frames Flycast drew, not refreshes: a pass that runs two frames shows one.
- **Not on the handheld yet:** the player is checked against the release's manifest at launch, so this needs an image.
  The spike folder on the RG DS has the new player (`rgds-play`, the 11-23 one kept as `rgds-play-1123`) and a current
  `runtime/`.
- **The main thread (2026-11-25, afternoon).** Measured with `os/device-bench/dc_run_y.sh` (the player's own loop
  numbers; `PROF=1` adds `rgds_prof.so`, which now counts ioctls from its start time only and, with
  `RGDS_PROF_STACKS=<request hex>`, records who called one ioctl by walking the frame-pointer chain;
  `rgds_prof_report.py --stacks N` and `--callers TEXT` read it; `dc_y_report.sh` gives one line a case over the
  drive's nine windows of 300 passes).
  - **Where the kernel time went:** in the drive the thread made 10-16 `PANFROST_SUBMIT` calls a frame at 110 us
    each (1.8 ms a frame), 33 `WAIT_BO` checks (0.3 ms), two page flips (0.17 ms). The submits by caller: almost half
    came from two full-screen copies. Flycast drew the scene into a framebuffer of its own and blitted it, upside
    down, into the player's (`PostProcessor::render`); the player blitted that into the panel's texture and turned it
    back over. Each copy is a GPU job (two submits) and forces the one before it out.
  - **Patch 0008, the scene into the frontend's framebuffer:** Flycast binds the frontend's framebuffer for the
    scene and declares `bottom_left_origin = false`. Not with the PowerVR2 filter (decided at start); a frame with a
    video shift and the 2D framebuffer path still copy, the same way up. `RGDS_FLYCAST_DIRECT=0` turns it off.
  - **Player, the panel's texture as the framebuffer's colour** (`aim_framebuffer` in
    `play/src/core/libretro_core.cpp`): for a core that draws top row first, when the last frame was the texture's
    size, the texture replaces the colour renderbuffer and nothing is copied. Only for `dc`
    (`RGDS_PLAY_FBO_TEXTURE=0/1` forces it; note `RGDS_PLAY_DIRECT` is the older switch for sharing the context at
    all). Capture at 95 s: right way up, same picture.
  - **Bridge hooks** (`src/platform/emulator_bridge.cpp`): the `ioctl` hook searched the environment for
    `RGDS_SHARED_MENU` on every successful call (a thousand DRM calls a second here) and the `read` hook read the
    descriptor's path back from `/proc` on every read. Now only evdev requests go past the first line of the ioctl
    hook, and an input node is told by `fstat` (character device 13, minor 64 and up: checked against the RG DS's
    `/dev/input`). Built on its own for the bench (`bridge-1125.so` in the spike folder); the menu key filter was
    not exercised by the scripted drive.
  - **Drives (two each, RG DS):**

    | Case | Core's run a pass | Passes over 16.7 ms, of 300 | Loop |
    |---|---|---|---|
    | 0001-0007, new player | 12.6 ms | 10.8 / 11.7 | 17.5 ms |
    | + 0008 and the framebuffer texture | 10.2 / 10.4 ms | 3.6 / 3.3 | 16.89 ms |
    | + the bridge fix | 10.0 / 10.2 ms | 3.2 / 3.1 | 16.89 ms |
    | 0008 with the player still copying | 12.9 ms | 25.7 | 18.7 ms |

    The last row is why 0008 is **not** bound over the installed core: with the image's player (which still copies)
    it is worse than 0007. Core, player and bridge go into an image together. Menus: 4.3 -> 3.95 ms.
  - **Installed 2026-11-25:** the package rebuilt from patches 0001-0008 after a dirclean gives the same core as the
    bench build (md5 9a418f2a... before the image's strip). Image `0.9.2-2d1a6fa0f6e1-osf1d60aec` pushed to the RG
    DS's slot b (slot a keeps `os86393bed`); it boots, and the installed core, player and bridge carry the changes.
    No drive was run from the installed image (the panels were left to the owner after the install).
  - **After:** 8.5 submits a frame over the run (6.8 in the drive): the scene, the upper panel and the lower panel,
    each flushed where it must be (swap, context switch, the panel sampling the scene). `WAIT_BO` 23 a frame (0.2 ms),
    nearly all the batch pools probing the buffer cache (`panfrost_pool_alloc_backing`); not worth a Mesa patch yet.
- **What is left for Crazy Taxi:** about 1% of refreshes still missed in the drive (3 passes of 300 over, worst
  windows 12 ms average). The main thread's time is now Mesa's per-draw state and Flycast's list building: joining
  consecutive polygons with the same state into one draw is the next lever. The lower panel is cleared, drawn and
  flipped every pass even when its picture has not changed (two submits and a flip). After that the sound chip's
  ~25% of the SH4 thread.
- **Next fixes, in order:** (1) and the buffer stall done, above. (The `u_vbuf` name in the first profile was Panfrost borrowing u_vbuf's
  scan helper, not the vertex-translation path; no layout change needed.) (2) Cut redundant per-poly state in
  `SetGPState`: skip texture binds, `glTexParameter` calls and uniform updates when they equal the previous poly's
  (each one makes Mesa revalidate texture and sampler state). (3) Batch: consecutive polys with identical state as one
  draw (strips joined by the restart index), the biggest lever on a per-draw-cost driver. (4) Look at the ioctl time
  (job submits per frame: one batch, or flushes mid-frame from FBO switches?). (5) SH4 side: try
  `flycast_enable_dsp=disabled` per game (6.5% of that thread; some games need the DSP for sound effects).
  Re-profile after each, and test heavier games (only Crazy Taxi is on hand).
- Test runs keep vibration off (`RGDS_RUMBLE=0` in `dc_spike.py`).
- **VMU screen orientation:** Flycast keeps the VMU picture's rows bottom first (its own overlay draws the texture
  flipped). The lower-screen view now flips the rows; checked against Crazy Taxi's logo from a device capture
  (`play/src/companion/keypad.cpp`), not yet seen on the panel.
- Spike tooling: a run of several variants goes detached on the handheld (`os/device-bench/dc_variants.sh` with `dc_threads.py`, staged into the spike folder, `setsid
  nohup`), since the USB link dropped once mid-run and took the SSH session with it.
- The shown-frame average above 16.7 ms with 0 emulated frames over budget points at presentation pacing (one doubled
  vsync per window), not the emulator; compare with PSP's pacing before changing options.
- Staging notes: `/data` is exFAT, so the runner keeps its core symlinks in `/run/rgds/dc-spike-cores`, and the spike
  folder needs a copy of the release's `assets/shaders` beside `runtime/`.

## Sources

- Flycast source, <https://github.com/flyinghead/flycast> (core options in `shell/libretro/libretro_core_options.h`,
  memory in `core/linux/posix_vmem.cpp` and `core/types.h`, JIT size in `core/hw/sh4/dyna/driver.cpp`, VMU picture in
  `core/rend/osd.cpp`).
- Panfrost GLES 3.1 conformance on Mali-G52: <https://www.collabora.com/news-and-blog/news-and-events/panfrost-achieves-opengl-es-3.1-conformance-on-mali-g52.html>
- RG DS Plus specs (RK3568, 1 GB, 1024×768): <https://www.notebookcheck.net/Anbernic-RG-DS-Plus-full-spec-sheet-and-pricing-revealed-ahead-of-launch.1394454.0.html>
- RG353 Dreamcast reports: <https://retrohandhelds.gg/rh-reviews-the-anbernic-rg353m/>,
  <https://pocket-gaming.org/device/anbernic-anbernic-rg-353vs>
- Our own measurements: `docs/PERFORMANCE.md` (PSP, N64, heat, power modes).
