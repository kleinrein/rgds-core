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
| Device spike | **Waiting**: the handheld was offline, and there are no Dreamcast games or BIOS on hand |

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
scp -O build/dc-spike/flycast_libretro.so build/play/device/rgds-play os/banana-probe/dc_spike.py os/banana-probe/session_hold.sh $D:/data/rgds/dc-spike/
tar -C runtime -cf - . | ssh $D tar -C /data/rgds/dc-spike/runtime -xf -
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

## Sources

- Flycast source, <https://github.com/flyinghead/flycast> (core options in `shell/libretro/libretro_core_options.h`,
  memory in `core/linux/posix_vmem.cpp` and `core/types.h`, JIT size in `core/hw/sh4/dyna/driver.cpp`, VMU picture in
  `core/rend/osd.cpp`).
- Panfrost GLES 3.1 conformance on Mali-G52: <https://www.collabora.com/news-and-blog/news-and-events/panfrost-achieves-opengl-es-3.1-conformance-on-mali-g52.html>
- RG DS Plus specs (RK3568, 1 GB, 1024×768): <https://www.notebookcheck.net/Anbernic-RG-DS-Plus-full-spec-sheet-and-pricing-revealed-ahead-of-launch.1394454.0.html>
- RG353 Dreamcast reports: <https://retrohandhelds.gg/rh-reviews-the-anbernic-rg353m/>,
  <https://pocket-gaming.org/device/anbernic-anbernic-rg-353vs>
- Our own measurements: `docs/PERFORMANCE.md` (PSP, N64, heat, power modes).
