# Performance

How DSCore is measured on the RG DS, and the latest results.

## Results, 15 October 2026

Standard RG DS, stock Linux, DSCore, shader Off, frameskip 0, audio muted.
"Game fps" is emulated frames per second (not display flips), averaged over
the run; the budget is 16.7 ms a frame. "2×" is plain 2×; "2× adaptive" is the
default, where heavy GPU stretches drop to native resolution for a moment.

| Game, scene | Native | 2× | 2× adaptive | Notes |
| --- | ---: | ---: | ---: | --- |
| Mario Kart DS, race checkpoint | 60 | 60 | 60 | 3×: 60, 4×: 52, 5×: 35 |
| Mario Kart DS, Cheep Cheep Beach lap 3, Blooper ink | 60 | 56 | 60 (min 56) | GPU-bound: about 18 ms of GPU work a frame at 2× |
| Metroid Prime Hunters, intro | 60 | 60 | 60 | |
| Kingdom Hearts 358/2 Days, intro | 60 | 60 | 60 | |
| Kingdom Hearts Re:coded, intro | 60 | 60 | 60 | |
| Golden Sun: Dark Dawn, intro | 60 | 56 | 56 | 53–54 in the 3D cutscene |
| Zelda: Phantom Hourglass, intro | 60 | 54 | 54 | display capture keeps the CPU rasterizer on |
| Zelda: Spirit Tracks, intro | 60 | 47 | 48 | as above; about 13 ms of CPU raster a frame |
| COP: The Recruit, intro | 41 | 45 | 45 | CPU-bound at every scale |
| Hotel Dusk: Room 215 | — | — | — | crashes during boot; starts in DraStic by default |

Intro scenes are the first 40 s after boot, so they mostly show logos, menus and
cutscenes. Gameplay can be lighter or heavier. Checkpoints from demanding scenes
are the most useful thing a tester can share.

On 14 October the ink scene ran at 42 fps at 2×. What changed: translucent
polygons skip a pass that could never draw anything (46 fps), and a leaner
fragment shader (56 fps). Both produce byte-identical images, checked against
dumps of the old renderer in six scenes. Adaptive 2× covers the rest.

Where the time goes now: the 3D fragment shader is the GPU's limit (about
6 cycles a pixel on the Mali-G52, 4× the pixels at 2×); translucent surfaces
such as the sea in Cheep Cheep Beach are a third of the GPU frame. Weston, the
stock compositor, redraws both screens on the GPU every frame (about 8% of
the GPU and 27% of one CPU core), because the stock kernel's display driver
offers no atomic modesetting and Weston then never scans a window out
directly.

### Fragment cost at 3×, 15 October (evening)

Two ways to cut the per-fragment GPU cost were built behind switches, measured
at 3× in the Cheep Cheep Beach ink checkpoint (plain 3×, muted, 30 s, GPU time
is the fence latency from `DS_GPU_TRACE=300`, since the driver's timestamp
queries report nothing on this device) and checked for byte-identical output
in ten `DS_HIRES_DUMP` windows: the ink checkpoint (30 frames) and the nine
Phantom Hourglass and Spirit Tracks windows of the lavapipe harness
(`build/frag-cost/sweep-all.sh`, headless engine under lavapipe in the VM,
old against new per frame by hash; two identical runs give identical dumps).
With both switches off the new build matches the previous renderer in every
window.

| Variant | GPU ms/frame | Game fps | Output |
| --- | ---: | ---: | --- |
| Current renderer (two runs) | 25.0 | 32.4 | reference |
| `DS_LEAN_HALFPX=1`: viewport moved half a pixel, no `interpolateAtOffset` | 24.5 (−1.6%) | 32.8 | differs in every frame of every window |
| `DS_LEAN_NODISCARD=1`: translucent pass B without discard, every run | 23.1 (−7%, repeat 23.2) | 35.4 | differs in 7 of 10 windows |
| `DS_LEAN_NODISCARD=2`: same, only runs where nothing can be discarded | 24.6 (−1.2%) | 33.5 | identical in all 10 windows |
| `DS_LEAN_SKIP=8` (no translucent polygons, for scale) | 11.2 | 58.8 | — |

What the numbers mean:

- The two `interpolateAtOffset(-0.5)` reads cost about 1.6% of the frame. Moving
  the viewport instead moves the coverage sample with the attribute sample, so
  sloped edges rasterize differently; there is no way to read the corner
  sample at the centre's coverage without the explicit interpolation.
  Rejected.
- Pass B of a translucent run carries the alpha test and the "alpha 31 pixels
  were drawn opaque already" discard, so Mali updates depth and stencil late
  and serializes overlapping fragments of the same pixel. Without the discard
  (early ZS, a dropped fragment emitted as a blend no-op) the frame is 7%
  cheaper, but a fragment the alpha test would drop then writes the pixel's
  stencil id (and depth, in depth-write runs): a later polygon of the same id
  is refused where the DS would blend it, and cut-out holes occlude what lies
  behind them. The dumps show it.
- The provably safe subset (`DS_PF_NO_ATEST`: polygon alpha, or alpha
  modulated by the texture's lowest alpha, is above the alpha reference, and
  the run has no alpha-31 pixels under a depth-equal test) covers only the sea
  in this scene: the ink splats have alpha-0 texels. It stays available
  (`DS_LEAN_NODISCARD=2`) but saves 1.2%. The winner, such as it is.
- Translucent polygons are 13.8 of the 25.0 ms at 3× here, more than the third
  measured at 2×.

Where the remaining 7% could still come from, exactly: in this scene every
translucent id code appears in one run per frame and no shadow shares one, so
the stencil side effect is unobservable except where two polygons of one run
overlap, and the depth side effect only in depth-write runs. A per-frame check
(no later run or shadow with the same code, no code both before and after the
run, no overlapping polygon boxes within the run, no depth write) would let
most runs go discard-free with identical output. The [raster rules test](RASTER_RULES.md)
draws those cases (same-id overlaps with alpha-0 texels, depth-write cut-outs,
depth-equal decals, shadows sharing an id) and holds the GPU raster against the
CPU raster per cell: the unrestricted variant fails five of its sixteen cells,
the safe subset passes all, so a wider variant can be proven there first.

### Display phase lock, 18 October

Input-to-display latency on the direct KMS display, measured as the time from
a frame's hand-off to the present until its page flip completed
(`DS_CADENCE_LOG` now stamps every frame and every flip; `build/pace-lock/flipwait.py`
reduces a log). Two things turned up before any pacing change. The panels'
flips sit on a clean grid, but the grid's period wanders between 16.59 and
16.90 ms (59.2–60.3 Hz) over tens of seconds; the 60.4 Hz quoted earlier was
the shortest gap ever seen, not the refresh. And a swap cannot happen before
the window's previous flip has completed, so with the pacer at the profile's
60 Hz, usually faster than the panel, the present ran a whole frame behind:
each swap waited for the flip before it and was then taken a full refresh
later. Free-running, a finished frame waited **22 ms** on average for its
flip in Mario Kart 2× (sd 4.5, 5th–95th percentile 14–29 ms) and 18 ms in
Peggle, the per-second mean sweeping 15–28 ms with the beat between the two
clocks.

`video.pace = display` (`DS_PACE=display`; `auto` stays `free` for now) runs
the emulator one frame per refresh at the panel's own period, hands the rate
difference to the audio resampler, and starts each frame at its target tick
minus the learned start-to-swap time (emulation plus the present thread's
upload and draw, 5–6 ms of it) minus a band for the GPU and jitter; the band
widens by 0.5 ms whenever a frame that started on time still missed its tick
and decays slowly otherwise. The tick arithmetic is in `phase_lock.h` and is
covered by `tests/phase_lock_test.cpp` (a simulated pipeline with the swap
hold, flips delivered a frame late, two panels, a wandering period and
emulation that cannot keep up). Steering the deadline from the swap-to-flip
latency instead, the obvious loop, does not work here: the sample arrives one
or two frames late, the present adds its own jitter, and every overshoot past
a tick is a repeated frame; the best build of that kind reached a 14.7 ms wait
in Peggle but repeated about two frames a second.

Measured on 19 October (40 s runs, first 10 s skipped, upper panel; the lower
panel is within 2 ms of it), free pacer against the lock:

| Game | Wait, mean | sd | 5th–95th pct | Repeated frames per 40 s |
| --- | ---: | ---: | ---: | ---: |
| Peggle 2×, free | 18.3 ms | 5.8 | 9.4–26.9 | 32 |
| Peggle 2×, display | 17.5 ms | 3.2 | 14.6–20.3 | 79 |
| Mario Kart 2× adaptive, free | 19.6 ms | 5.6 | 11.3–29.0 | 5 |
| Mario Kart 2× adaptive, display | 17.0 ms | 3.1 | 13.1–23.2 | 31 |
| Mario Kart 2×, no fallback, free | 21.4 ms | 4.7 | 13.7–29.5 | 59 |
| Mario Kart 2×, no fallback, display | 18.7 ms | 6.8 | 6.7–29.9 | 139 |

The lock does what it was built to do, the spread halves and the beat is
gone, but the band it has to learn settles near 7 ms (plus a 2–4 ms
allowance for the spread of the present's own time), so the mean gain is
1–3 ms and the frames it still misses show as two to six times the repeats.
The latency on this device is set by the present pipeline, not by the pacer's
phase: 5–6 ms from hand-off to swap, the swap's wait for the window's
previous flip, and the present thread's tail. In plain 2× Mario Kart with the
native fallback off there is nothing to gain either way: emulation takes
14–16 of the 16.7 ms and the lock starts every frame at once, as the free
pacer does. So the default stays free; the lock is there to try with
`DS_PACE=display` or `[video] pace = display`, and the measurements and the
cadence log are the tools for the next step, which is the present's cost.

**Where the present's time goes** (19 October, `DS_PRESENT_PROFILE=1`, which
now times the texture upload, the draw, an optional `glFinish`
(`DS_PRESENT_FINISH=1`) and the swap per window). Per window and frame on the
SDL renderer: the upload is 0.3 ms for a 2× screen in Mario Kart and 1.8–2.3 ms
in Peggle, the draw 0.5 ms, and the swap 4–6 ms. With a `glFinish` before the
swap the swap itself drops to 0.7–1.0 ms and the finish takes 4.4–4.8 ms: the
swap is the GPU completing the frame. Leaving the upload out, or drawing a stale
texture instead of uploading, still costs 2.7–4.0 ms, so any job the Mali is
given takes 2.5–4 ms from submit to completion whatever its size. It is not
power gating (`power_policy` `always_on` changes nothing), not the GPU clock
(the launcher pins it at 800 MHz), and not interrupt placement (the Mali
interrupts forced onto CPUs 0–2, away from the emulation core, change nothing).
Textures are not recreated per frame. That latency sits inside the vendor
driver's job path and cannot be cut from the SDL renderer side; the one avenue
inside this path is a zero-copy texture (an EGLImage over a dma-buf written by
the compose) that removes the upload and one of the two GPU jobs, worth 1–3 ms.
The direct scanout tier (`video.sink = auto`: dma-heap buffers page-flipped
without GL, with the RGA blitter or the Vulkan import stage) avoids the GPU
round trip entirely, but today every tier except the SDL renderer presents
the native 256×192 frames (the 2× compose is only consumed by the renderer
path), and the RG DS launcher draws its pause menu and reads its frame
telemetry through the bridge's `SDL_RenderPresent` hook, so moving the RG DS
onto that tier means routing the 2× compose into the present stage and
blending the launcher's overlay frame in the engine: the real fix, and a
project of its own.

**Direct present, 19 October** (session "direct present"; runs in
`build/pace-lock/runs/`, scripts `measure.sh`, `queue-direct*.sh`,
`summary.py`). The RG DS now presents through the direct KMS scanout tier
instead of the SDL renderer: the launcher's KMS plan writes `video.sink = auto`
and `gpu_present = false` (runtime/emulators.py; `DS_RGDS_SINK=renderer`
restores the renderer for A/B runs), and the engine's new direct path
(`Display::draw_scanout_async`, dscore `display.cpp`) composes each panel's
finished frame on a cached canvas on the present thread, hands it to the
bridge's new hook `rgds_present_canvas` (src/platform/emulator_bridge.cpp: the
frame telemetry, the launcher's capture and the menu/HUD blend that used to
hang off `SDL_RenderPresent`), then copies it into a dma-heap buffer and
page-flips it. The 2× compose is a CPU buffer, so a Vulkan hand-off would only
have added a 1.2 MB upload and one more Mali job (2.5–4 ms each, see above);
the CPU path costs 2.3 ms per panel (compose 0.8–1.5, hook 0.1–0.2, copy and
flip 0.8–1.0) with no GPU job between a frame and its flip, and the direct
display's stray window-surface fallback (a hidden GLES renderer with an 8 ms
synchronous swap) is skipped on KMSDRM. Two things had kept the tier from
opening on this device: the DSI encoders are unbound once Weston stops, and the
tier's fallback picked the other panel's CRTC (DSI-1 may only use CRTC 130,
DSI-2 only CRTC 90; `SETCRTC` refused both with EINVAL), and the tier was not
safe for the two present threads on the shared DRM fd. Queueing a frame behind
the panel's pending flip, as the tier did, is what the SDL path effectively
does and leaves every frame a refresh behind (Peggle 2× 23.7/22.0 ms done-to-flip
wait, adaptive Mario Kart 18.8/19.3), so the tier now presents as a mailbox: a
frame that waited longer than `DS_KMS_MARGIN_MS` (2 ms) in the queue is dropped
at the pending flip's vblank, because its successor is then due before the next
one and goes straight onto it; a frame that started late, or follows one, is
never dropped (`DS_KMS_DROP=0/1`). Deciding at the vblank matters: deciding at
end_frame from a "pending" flag dropped 5–10 % of the frames, since a flip's
completion event is read some way after its vblank. 40 s runs, free pacing,
direct display, two panels:

| run | wait done→flip, mean (p5–p95) | doubled gaps | skipped frames |
| --- | --- | --- | --- |
| Peggle 2× SDL renderer | 17.4 / 17.6 ms (8–25) | 41 / 59 | 119 / 152 not presented |
| Peggle 2× direct, queue | 23.7 / 22.0 ms (13–32) | 3 / 4 | 3 |
| Peggle 2× direct, mailbox | **10.5 / 11.4 ms (3–19)** | 69 / 45 | 64 / 41 dropped |
| Mario Kart 2× adaptive, SDL renderer | 22.0 / 21.5 ms (14–29) | 9 / 14 | 47 / 63 not presented |
| Mario Kart 2× adaptive, direct, queue | 18.8 / 19.3 ms (5–31) | 15 / 15 | 1 / 1 |
| Mario Kart 2× adaptive, direct, mailbox | **10.9 / 11.8 ms (4–19)** | 34 / 34 | 46 / 44 dropped |

The wait halves (-7 to -11 ms mean) and the whole distribution moves down
(p95 19 vs 25–29); the frames the panel does not get are about as many as
before (the renderer skipped them at its present lanes), the doubled gaps are
up by about one in two seconds in Mario Kart. The pause menu is held and
re-presented through the hook (28–31 presents per hold), the scene capture
gives both panels, and the benchmark tool sees "game + presentation"
telemetry on the new path. The remaining wait is the phase between the
frame's completion and the panel's vblank (uniform over a refresh as the
60.00 Hz pacer beats against the 60.05 Hz panel); the display lock
(`DS_PACE=display`) is the lever for that, and with a 2 ms present instead of
a 5–6 ms one its margin can be far narrower than measured on 18 October.

## What made DSCore fast

- **3D on the GPU.** Polygons are rasterized by Vulkan on the Mali-G52 at the
  chosen scale. The CPU rasterizer is skipped unless a game reads the 3D image
  back (display capture or the display FIFO).
- **Composition off the emulator thread.** Helper threads compose each 640×480
  screen from the GPU image and the DS's 2D layers, in batches of 16 lines.
- **Presentation off the emulator thread.** Texture upload, shaders and the
  buffer swap for both screens run on a present thread, overlapping the next
  frame's emulation. Busy frames save about 2.5 ms; a light frame may simply
  wait for the previous present, which costs nothing at 60 fps.
- **A lean fragment shader.** Each polygon's constants (texture wrap masks,
  atlas corner, texcoord range, blend mode) are decoded once per vertex and
  arrive as flat varyings; the colour combine runs in float, where every DS
  step is exact; textures sit in an RGBA8 atlas, so a texel arrives as four
  channels. The single-sample resolve writes straight to the readback buffer.
- **Cheaper 2D scaling.** The optional Sharp and Smooth 2D filters scale
  each source line once and blend only where pixels differ (NEON for Smooth):
  in a Mario Kart race at 2×, Sharp costs about a fifth of a core instead of
  three fifths. All stay off by default (Native pixels). Crisp (Scale2x cells,
  16 October) first scans each line and takes Sharp's path where Scale2x
  changes nothing, and the compose job now runs every 2D filter before it
  waits for the GPU's 3D, so in a GPU-bound scene the filter overlaps the 3D.
  Measured on the ink checkpoint at 2× (plain, game fps / median emulation
  ms): Native pixels 60.0 / 13.9, Crisp 56.1 / 15.1 (58.8 with adaptive);
  before those two changes Crisp ran 47.9 / 16.5 against 56.8 / 14.8. Crisp
  still costs about a millisecond a frame on the critical path there, so
  Sharp is the cheaper choice for the heaviest scenes.
- **No empty passes.** A translucent polygon below alpha 31 can never produce
  an opaque pixel, so its run skips the opaque-pixel pass. A modulated polygon
  of alpha 31 reaches 31 only through a texel of alpha 31, so the texture cache
  records whether a texture has one (A3I5 and A5I3 water and glass textures
  often do not) and the pass is skipped for those runs too. The 3× fragment
  study (above) confirmed the renderer with it is byte-identical to the one
  without in all ten dump windows; its own GPU saving was not isolated.
- **Clock floor during GPU games.** The stock governor raises the Mali from
  200 MHz to 800 MHz only after load appears, so each light-to-heavy scene
  change at 2×–5× starts a few frames at a low clock. While such a game runs,
  `runtime/game_clocks.py` raises the GPU and memory-controller floors to
  their current ceiling through the same journalled writes the power profiles
  use, and puts them back when the game ends (a crash restores them with the
  rest of the hardware journal). A power profile's cap stays a cap. Opt out
  with `gpu_floor=0` in `performance.ini` or `RGDS_GPU_FLOOR=0`. Verified on
  the device on 15 October (the GPU and memory floors held at 800 and 920 MHz
  through a 2× Mario Kart session and back at 200 and 324 MHz after it); the
  frame-rate effect is not yet measured.
- **Adaptive 2×.** When at least 6 of 30 frames miss the budget while
  composition waits on the GPU, DSCore takes the 3D from its native raster for
  2 s, then probes 2× again (a relapse doubles the hold, up to 32 s). A hold
  that is no faster backs off for a minute. `DS_ADAPTIVE=0` or
  `video.adaptive = false` turns it off. Players choose it per game under
  Graphics → Heavy 3D scenes ("Always sharp" saves `rgds_adaptive=0`).
- **Quick starts.** The GPU driver's two-second warm-up per context is gone, so
  the engine presents its first frame in about 1.6 s at 2×.

## Measuring

`tools/benchmark_emulators.py` runs a game in a private copy on the device,
muted, with the same settings every time, and reports per-frame statistics.
Stop RG DS Core first (the tool refuses while a session runs).

```sh
python3 tools/benchmark_emulators.py --device root@<ip> --root /mnt/mmc/rgds \
  --rom "/mnt/mmc/Roms/NDS/<game>.nds" --cores rgds-experimental \
  --phase normal --seconds 30 --volume 0 --rgds-scale 2 \
  --resume-from /mnt/mmc/<seed>/results.json --output result.json
```

A seed is a `results.json` that names a checkpoint and the ROM's SHA-256, so a
saved state from a heavy scene can be replayed exactly. Useful engine switches
(`--engine-env KEY=1`):

| Switch | Shows |
| --- | --- |
| `DS_PROFILE` | Time per emulation stage and the slowest frames |
| `DS_GPU_TRACE` | GPU submit, latency and draws per frame |
| `DS_HIRES_DEBUG` | Composition, GPU waits, capture and deferral counts |
| `DS_JIT_CHURN` | Recompiler invalidations and retranslations |
| `DS_RENDERER_SYNC` | Presents on the emulator thread (for comparison) |
| `DS_ADAPTIVE=0` | Plain 2×: no native fallback in heavy stretches |
| `DS_LEAN_SKIP` | Leaves polygon kinds out to measure their GPU cost: 1 opaque, 2 cut-out, 8 translucent, 16 shadows |
| `DS_LEAN_FLAT` | GPU cost bisection: 1 flat colour, 2 no texture |
| `DS_LEAN_NODISCARD` | Translucent pass B without discard: 1 every run (not output-preserving), 2 only runs where nothing can be discarded (identical output, measured) |
| `DS_LEAN_HALFPX` | Viewport moved half a pixel instead of `interpolateAtOffset` (measured: changes coverage, rejected) |
| `DS_HIRES_DUMP=F:N` | Writes N frames from frame F to `/tmp/hd/` (`h` is the GPU image), to compare renderers byte for byte |
| `DS_PACE=free` / `display` | Free-running pacer at the limiter's rate, or the display phase lock (KMSDRM); `DS_PACE_BAND=<ms>` sets its starting margin |
| `DS_CADENCE_LOG=<path>` | One line a frame (start, hand-off and next-start timestamps, locked, band) plus one per page flip (completion, swap, hold); `build/pace-lock/flipwait.py` reports the wait distribution and the panel period |

The headless engine (`dsperate-headless` from `dscore/`) runs games without a
display; with `--trace /dev/null --max 0 --interp` and `DS_TRAP_WILD=2L` it
stops at the first ARM9 jump into unmapped memory or an odd ITCM mirror and
prints the preceding instructions.

Earlier measurements, including other emulators, are in the
[archive](archive/PERFORMANCE_2026-10-11.md).
