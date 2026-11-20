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
- The checkout with every submodule is 865 MB with history; Buildroot's git download would clone all of it. The VM's
  `~/own-os/dl/libretro-flycast/` holds a hand-made tarball of a shallow checkout (172 MB, no `.git`), as was done for
  PPSSPP. A clean machine downloads the full checkout.
- The core is 33 MB before the image's strip step. The image's rootfs (235 of 768 MiB) and the 512 MiB OTA cap have
  room.
- **The next image build needs `--reconfigure`**: the defconfig now enables the package, and `check_cores` stops a
  build whose `output/rgds-os/.config` lacks it.

### Settings the player forces (`play/src/core/libretro_core.cpp`)

| Option | Upstream default | Ours | Why |
|---|---|---|---|
| `flycast_internal_resolution` | 640x480 | 640x480 | 1:1 on the RG DS; anything higher only costs fragment time |
| `flycast_threaded_rendering` | enabled | enabled | CPU and GL overlap; the main speed lever |
| `flycast_auto_skip_frame` | disabled | some | drops a frame only when the SH4 falls behind |
| `flycast_alpha_sorting` | per-triangle | per-triangle | per-pixel (OIT) needs more than GLES 3.1 |
| `flycast_anisotropic_filtering` | 4 | off | made no difference on PSP (G52) |
| `flycast_emulate_framebuffer` | disabled | disabled | slower, and N64's framebuffer copies crashed Panfrost |
| `flycast_texupscale` | 1 (off) | 1 (off) | xBRZ costs CPU and memory |
| `flycast_gdrom_fast_loading` | disabled | enabled | shorter loads; small accuracy cost |
| `flycast_digital_triggers` | disabled | enabled | the handheld's L2/R2 are buttons |
| `flycast_per_content_vmus` | disabled | VMU A1 | each game's saves in its own VMU file in the save folder, not the BIOS folder |
| `flycast_vmuN_screen_display` | (per VMU) | disabled | the VMU goes to the second screen, not over the game |

To try others without a rebuild: `RGDS_PLAY_CORE_OPTIONS="flycast_alpha_sorting=per-strip (fast, least accurate);flycast_enable_dsp=disabled"`.
Left at upstream for the spike to judge: `flycast_enable_dsp` (on), `flycast_delay_frame_swapping` (on),
`flycast_mipmapping` (on), `flycast_sh4clock` (200).

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
