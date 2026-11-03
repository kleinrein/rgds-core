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
| Hotel Dusk: Room 215 | — | — | — | crashes during boot |

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
restores the renderer for A/B runs, and a chosen screen filter keeps the
renderer too: the filter is the bridge's GL pass over the renderer's game
draw, which the direct present has no stage for, so a filter picked in-game
on the direct present is saved and the menu offers the restart), and the
engine's new direct path
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

### Input latency on RG DS OS, 30 October

Measured on the handheld (RG DS OS 0.8.0, 2x, renderer sink, `DS_PACE=display`),
25 B presses on the pad node per run with `os/banana-probe` (`game_latency.py`,
`press_to_flip.py`; the scratch `trial.sh` is `pace_trial.sh` with
`DS_PRESENT_PROFILE=1`), median time from a press to the completed flip of the
frame whose poll read it:

| Run | Before | After |
| --- | ---: | ---: |
| New Super Mario Bros, HD off | 30.1 ms | — |
| New Super Mario Bros, HD smooth (`rgds_2d=5`) | 40.4 ms | 30.4 ms |
| Mario Kart DS, HD off, no filter | 30.9 ms | — |
| Mario Kart DS, HD smooth, no filter | 31.9 ms | — |

**HD smooth fell out of the display lock.** HD adds 1–2 ms of present (the
rules texture and a heavier draw), and that was enough to start the game
outside the lock's 1.5-period lead. Free pacing then held every swap for the
previous flip, the busy present lane made the emulation thread wait at the
hand-off, and the lock counted that wait as work (15.2 ms start-to-hand-off
free vs 8.6 locked), so the lead stayed at 25–28 ms. When it did lock, a run of
misses tripped the valve, and the band only relaxed on locked flips, so it
stayed at 6.3 ms and the lock never came back (120 of 1680 frames locked).
Fixed in `phase_lock.h`: the lane wait (`Display::take_lane_wait_ns()`,
`PhaseLock::queued()`) comes off the work sample, and the band decays on
free-running flips too (`dscore/tests/phase_lock_test.cpp` has both). After:
1751 of 1773 frames locked.

**The frame edge moved to VBlank** (`DS_FRAME_EDGE=vblank`, set by
`runtime/emulators.py` for DSCore). A frame used to end at line 0, but the
picture is complete at line 192 and games read keys and touch in VBlank,
showing the result in the next frame's lines; so a press read in frame N
appeared in frame N+1's picture, a refresh later. Ending the frame at line 192
(joining the lines' batch and the compose there, `Gpu::on_scanline_start`)
puts the poll just before the game's read and hands over the frame that
answers it. Measured with a per-frame picture hash (`DS_CADENCE_HASH=1`,
`press_to_change.py`) in NSMB's pause menu, Down/Up alternating, HD smooth at
2x: the menu answered 2 frames after the read with the edge at line 0 and 1
frame after with it at VBlank, 24 of 24 presses each; press to the answering
flip **74.2 ms → 52.1 ms** median. The picture is the same frame; the cost is
the join no longer overlapping VBlank (start-to-hand-off median 9.37 → 9.55 ms,
p95 11.5 → 13.9 in NSMB HD smooth, no more frames over 20 ms). Frameskip keeps
the line-0 edge. Save states are taken at either edge (`Gpu::at_frame_edge`).

**A heavy scene turned the lock off for good.** Once free, nearly every swap
is held for the previous flip (13 ms at 2x; 0-2 of 300 flips unheld in the
logs), and held swaps taught the lock nothing, so the work stayed what the
heavy stretch left it. Frames returned to 10 ms while the lead stayed over the
relock test for the rest of the run, at either frame edge. Free-running frames
now learn from held swaps too (`phase_lock.h`, host case in
`dscore/tests/phase_lock_test.cpp`). Same engine and edge, each run from the
same restored state, 1 November:

| Run | Before | After |
| --- | ---: | ---: |
| NSMB HD smooth, read to flip | 27.3 ms | 19.3 ms |
| NSMB HD smooth, frames locked | 850 of 1751 | 1535 of 1647 |
| NSMB pause menu, press to answering flip | 52.1 ms | 43.9 ms |
| Mario Kart filter 21, read to flip | 29.7 ms | 26.4 ms |
| Mario Kart filter 21, frames locked | 527 of 1739 | 865 of 1720 |

The "before" runs resumed from whatever the previous trial saved, so their
scenes are not identical to the "after" ones. Mario Kart with a filter still
trips the valve and takes more missed ticks (31 against 16); no presses landed
on a repeated flip.

**Run-ahead does not fit.** One rollback frame needs a hidden frame, a visible
frame, a save and a load inside 16.7 ms. The emulation thread takes 9-10 ms per
frame with or without drawing (`--frameskip 1 --frameskip-mode fixed`: NSMB
hidden 10.6, drawn 14.1; Mario Kart hidden 8.8, drawn 9.4), so two frames alone
are 18-25 ms before any state work. One timed save and load to memory: 19-32 ms
and 48 ms, the next frame 17-58 ms slower (the load flushes the JIT). A probe
doing this every 30 frames hung the engine after its first pass, so those are
single samples.

A chosen screen filter is a separate cost: Mario Kart with filter 21 flips
about 15 ms after its swap (the filter's GPU pass), the lock trips its valve
and comes and goes (34.4 ms with the installed engine). Not changed here.

### Real launches on RG DS OS, 2 November

The owner reported horrible lag and frame rates at 2x, while the hand runs above
looked fine. Hand runs differ from play in two ways that turned out to matter: an
SSH shell has all four CPUs where the launcher has three, and they rarely open the
menu. These runs go through `emulators.py launch` with the launcher's environment
and CPU mask (`os/banana-probe/real_trial.sh`, `session_hold.sh`, `cadence_lock.py`),
from the owner's own states, 20 B presses, the real menu for 4 s, 20 more presses,
each run started at 52 °C or below so none reached the throttle point. Median press
to completed flip of the reading frame, before and after the menu; "dry" counts
audio queue underruns over the 50 s run.

| Mario Kart DS, 2x, filter 21 | Before | After | Frames locked | Native holds | Dry |
| --- | ---: | ---: | ---: | ---: | ---: |
| Shipped (3 runs) | 36.9, 37.6, 37.9 | 42.7, 43.5, 41.4 | 114–584 of ~2850 | 6–7 | 116–138 |
| Shipped engine, all CPUs (1 run) | 45.7 | 42.1 | 126 of 2889 | 6 | 48 |
| All fixes (4 runs) | 33.6–42.2 | 35.9–39.6 | 1111–1687 of ~2900 | 4–6 | 98–120 |

| New Super Mario Bros, 3x, HD smooth | Before | After | Frames locked | Native holds | Dry |
| --- | ---: | ---: | ---: | ---: | ---: |
| Shipped (2 runs) | 39.6, 31.7 | 40.7, 42.4 | 154, 639 of ~2900 | 0 | 149, 193 |
| All fixes (2 runs) | 29.2, 31.8 | 26.6, 32.5 | 2281, 2791 of ~2900 | 0 | 82, 106 |

| Mario Kart DS, 2x, no filter (hand run, one each) | Before | After | Frames locked | Native holds | Dry |
| --- | ---: | ---: | ---: | ---: | ---: |
| Shipped, 3 CPUs | 34.5 | 28.3 | 645 of 2922 | 0 | 97 |
| Shipped, 4 CPUs | 31.6 | 35.6 | 830 of 2999 | 0 | 19 |
| Exact frames, 4 CPUs | 32.3 | 34.0 | 1439 of 2946 | 1 | 18 |
| All fixes, 4 CPUs | 26.9 | 26.9 | 2963 of 2984 | 0 | 27 |

**A launched game had three CPUs.** The session starts the launcher under
`taskset -c 0-2` to keep it off the emulation core (since 28 October), and a game
inherited that. DSCore counts the CPUs of its own mask: with three it reported
`host: 3 cores`, pinned nothing (`layout_cores` needs four) and left CPU 3 at 4–8 %
busy, with its real-time emulation and video threads on the cores the normal-priority
present threads need. `all_cpus()` (`runtime/linux_platform.py`) now starts a game on
every CPU, except RG DS Play with its main loop pinned, whose other threads are meant
to stay off that core. The launcher, its Python and the menu helper stay on 0–2.
DSCore also says so in its log when its mask stops it pinning. This mostly shows as
smoothness (fewer underruns, a shorter hand-off tail), not as lag.

**One menu turned the lock off until quit.** The lock guessed which frame a flip
belonged to: the start nearest the work learned so far. That guess has a false answer
at every whole refresh above the real work. A menu or fast forward sends flips with no
frame behind them, the first of them read as seconds of work, and every frame after
"confirmed" a figure a whole number of refreshes too high: the runs above ended on
62–100 ms of start-to-swap with frames taking 10–15, and the lead never fit again
until quit. The symptom is the "120 of 1680 frames locked" of 30 October again (the
owner's log read 120 of 1623, 64.77 ms). From the code, the 1 November change made it
stickier, since free-running held frames then fed the wrong figure too; not measured
apart. Flips now carry the frame's number (`PhaseLock::started()`,
`KmsFlips::TAG_FRAME_SHIFT`), a held picture or fast forward carries none, and the
menu's return resets the lock. The host test has a menu and a 990 ms stall.

**The lead limit sat where these games live.** Free-running over 1.5 periods of lead
(25 ms) dropped the lock for frames that were still far better locked: 18–23 ms start
to flip against 28–34 free in Mario Kart with a filter, because a free frame's swap is
held for the flip before it and then waits most of a refresh for its own. Now free over
2 periods, locked again under 1.8.

**The filter's "15 ms" was mostly that wait.** The paragraph above took Mario Kart's
swap-to-flip for the filter's GPU pass. Locked, the same frames flip as soon as 7 ms
after their swap; a free frame swaps right after a vblank and waits for the next. The
GPU is still close to full with the 3D job and two filter passes: hand-off runs 15–17 ms
at high resolution against 10 native, and this is where the native holds and most of
the remaining missed ticks come from. Filter off is the difference between about 38 and
27 ms here.

**Native holds no longer count present-lane waits as frame cost**
(`adaptive.frame`): that wait is free pacing's, and lower resolution would not shorten
it. A hold lasts up to 16 s in these runs, so it is visible as well.

### Power modes, 3 November

Three modes now, global, for every game and system and the menu too (`runtime/system_controls.py`, `POWER_MODES`). Stock
is gone: a saved `power=0` loads as Balanced, the new default. Performance's trips are written at run time, so it needs no
special device tree, and leaving it puts the device tree's back.

| Mode | CPU floor to cap | GPU floor to cap | Governors | Passive trips |
| --- | ---: | ---: | --- | ---: |
| Battery saver | 408–1416 MHz | 300–600 MHz | dynamic | 70/75 °C |
| Balanced | 1104–1608 MHz | 400–800 MHz | dynamic | 70/75 °C |
| Performance | 1992 MHz | 800 MHz | `performance` | 80/85 °C |

Why they changed. Before, Saver capped at 1104 and 400 MHz: Mario Kart DS 41 fps, OutRun 15. Balanced capped at 1800 MHz,
the same 1.15 V as 1992, so it saved little. And every launch forced the CPU's `performance` governor whatever the mode,
so a GBA game sat at its cap; a mode changed during a game was undone at its exit, which restored the governor from
before. Games now keep the mode's governors (`os/banana-probe/power_trial.py`, on a data copy, on USB, one run a cell from
about 52 °C, 25 s of the owner's save points; the current is the battery's net, so read differences, not absolutes):

| Game: fps / net mA | Saver | Balanced | Performance |
| --- | ---: | ---: | ---: |
| Mario Kart DS, 2x, DS Classic | 56.9 / −114 | 58.0 / −180 | 57.4 / −232 |
| Mario Kart 7 | 59.9 / −98 | 60.0 / −142 | 60.0 / −227 |
| OutRun 2006 (PSP) | 24.5 / +153 | 38.5 / +60 | 40.9 / −9 |
| Mario Kart 64 | 51.5 / +121 | 51.6 / +45 | 53.4 / +37 |
| Crash Team Racing (PSX) | 59.1 / +171 | 58.5 / +164 | 59.8 / +114 |
| 2048 Advance (GBA) | 59.7 / +187 | 59.9 / +175 | 59.9 / +130 |

Performance pays 50–130 mA for little in these short cold runs. Its case is sustained play: at 80 °C Mario Kart DS held 57
fps for ten minutes, against 54.3 by minute 9 at 70 °C (measured with a patched device tree, same trip values). Press to
flip, Mario Kart DS: Balanced 34.2, 35.9, 43.4, 41.5 ms, Performance 43.1, 43.7, 37.6, 44.0, Saver 43.8; the spread within a
mode is as large as the difference, so no mode shows less lag. The dynamic governors' floors did not add any here. Not
measured: absolute battery life unplugged, long play in the other systems, the shell's temperature at 80 °C.

### Mario Kart at 3x, 3 November

Steady fps over the middle 60% of a real launch, the kart parked on a course with fog, adaptive off, each run from
52–54 °C, one run a setting (3x no filter: two of each engine). `lat` is `DS_GPU_TRACE`'s latency, submit to the first
reader seeing the fence.

| Setting | Before | Readback and vertices | + cheaper DS Classic |
| --- | ---: | ---: | ---: |
| 3x, no filter | 53 (lat 12.8) | 58 (lat 11.1) | |
| 3x, DS Classic | 48 | 51 (lat 14.1) | 55 (lat 12.7) |
| 3x, HD smooth | 47 | 52 (lat 12.3) | |
| 3x, HD smooth + DS Classic | 38 (lat 19) | 39 (lat 18.2) | 43 (lat 16.2) |
| 2x, HD smooth + DS Classic | 50 | 59.5 (lat 10.1) | 59.9 |

**The emulation thread waits for the 3D image.** At 3x it took 18.5 ms a frame against 14.1 at 2x and about 10 native:
the helper composing the first row waits for the GPU, and the line-0 join waits for that helper. So GPU time is frame
time, and over about 11 ms the frame misses 16.7 before any present.

- **The store readback was never on.** `limits_.fragment_stores` sat behind a check PanVK on Bifrost (Vulkan 1.0, neither
  extension) did not pass, so every frame ran a copy job. Fixed in `vk_device.cpp`.
- **Fans ask for what they need.** A draw ran the vertex stage 24 times a polygon for the 8 possible triangles; now 6,
  12 or 24 by the run's largest polygon, with the clipped ones sorted apart.
- **Direct frames.** With no fog to apply, a frame needs no resolve: colour goes straight to a linear image in cached
  memory, the alpha as the record has it, the blend's factor from a second source. Mario Kart's ink scene at 3x, no fog:
  38 fps against 33 (lat 19.5 against 23.2). Fog frames keep the resolve, and this course fogs every frame.
- **DS Classic without powers.** Six `pow` a pixel on both screens became polynomials, within 4/255 on random colours;
  the grid and stripe pattern is unchanged, and the stripes are a closed form of the same coverage.

Byte-identical against the copy path under lavapipe (`build/frag-cost/sweep.sh`, 30 frames a window, with
`DS_LEAN_MSAA=0`: the host defaults to MSAA, which takes neither new path) in the ink scene and two later windows.

**Rejected, measured.** Fog in the tile (one dynamic render with `VK_KHR_dynamic_rendering_local_read`, no store and
reload of the attachments; byte-identical under lavapipe, ran clean on the Mali): lat 12.2 against 11.3, so polygons
cost more in it and the subpass was not the main cost. Packing the flat varyings from 5 loads to 2: lat 11.6 against
11.1. `DS_LEAN_D24`, `DS_LEAN_HALFPX`, `DS_LEAN_NODISCARD=1`: no change in this scene.

**`lat` has a floor.** It is 4.3 ms at 2x and at 3x alike with no polygons, because it ends when the first reader asks,
not when the GPU finished. A job shorter than that reads as 4.3, so empty-frame figures say little; only figures well
above the floor are GPU time. In the ink scene lat goes 10.7 at 2x to 19.5 at 3x, nearly all of it per pixel.

**2.5x, the panel itself** (`rgds_scale=25`). 3x renders 768×576 and samples it down to the 640×480 panel, discarding
about 30% of what it drew. 2.5x renders 640×480, one rendered pixel a panel pixel. Same engine and cheaper DS Classic, one
run a setting (the last row: Mario Kart's ink scene, no fog, no filter):

| Setting | 3x | 2.5x |
| --- | ---: | ---: |
| No filter | 56.5–58 (lat 11.2) | 59.7 (lat 8.0) |
| DS Classic | 55 (lat 12.7) | 58.2 (lat 10.7) |
| HD smooth | 52 (lat 12.3) | 56.9 (lat 10) |
| HD smooth + DS Classic | 43 (lat 16.2) | 47.9 (lat 13.7) |
| Heavy scene | 37.5 (lat 19.7) | 46.8 (lat 14.8) |

The scales 2x to 5x are byte-identical to before under lavapipe: compose positions moved to half units
(`rgds::half_units`), which round the same for whole scales. The polygon count of these runs drifted between 239 and 542
as the same state played on, so compare settings loosely. HD smooth with DS Classic still does not reach 60.

**Not the frame edge.** `DS_FRAME_EDGE=vblank` looked guilty, since a 3D job submitted
after the pacing sleep cannot use that sleep. Tested: with line 0 Mario Kart read
41.7/43.1 and 48.2/42.4 ms, locked 480 of ~2870 frames, hand-off no shorter. The edge
stays. An engine from 29 October was no better on the same state (49.0/44.2 ms, three
CPUs, one run at the throttle point), so this is not a recent engine change either.

**The cost of holding the lock:** after a menu, free pacing repeated about 1 frame per
10 s in New Super Mario Bros against 3–6 locked; Mario Kart with a filter is 3–10
against 4–11. Missed ticks are the remaining work (hand-off spikes against an 8 ms band
at its maximum), along with the valve, which still frees the lock for 600 frames after
6 misses in 120, and always trips in the first seconds after a state load.

**Heat, not fixed.** Back to back, 2x with a filter reached the 70 °C passive trip
(mainline's `rk356x-base.dtsi`, 70 and 75 °C, on the charger, about 3 minutes in): the
CPU cap then read 1416–1800 MHz, 1104 at lowest, in most samples. These runs held about
59 frames/s anyway, so it does not explain the report alone, though 3DS runs just before
heat the chip further. No run here averaged under about 58 frames/s, so what reads as a
bad frame rate in them is repeated frames, native holds and audio underruns.

**PGO has decayed.** `pgo/aarch64` dates from 19 October and 93 functions no longer match
it (`-DDSPERATE_PGO_STRICT=ON` shows them; the 0 in `build.json` counts nothing, since
the warning is off in that build), among them `Scheduler::run_until_impl`,
`NDS::run_frame_slice`, `Gpu::on_scanline_start` and the `Engine2D` background drawers.
`tools/pgo_refresh.sh`, named by `pgo/README.md`, is not in this repository.

### OutRun 2006 on the PSP core, 3 November

Where the time goes in a race, measured on the handheld (image 0.8.0-789980519979, Linux 7.2.7-rgds) with the
Cross-route script in `/data/rgds/psp-spike` (`run2.sh NAME SECS VAR=..`, `queue.sh LIST` waits for other
sessions' holds), the core's own per-pass profiler (package patch `0002-gl-pass-profile.patch`,
`RGDS_GL_PASS_PROFILE=300`), the kernel's GPU engine time (`gpubusy.sh`, fdinfo with profiling on) and a 1 kHz
sampler (`sampler.so`, writes on a timer because the player ends with `_exit`). The game runs at full speed
throughout with frame skip 1 + auto; the numbers are per *drawn* frame, in four 300-frame windows that repeat
within 1 ms from run to run, so one run per setting is enough.

| Setting | Drawn frame, four windows (ms) | GL thread busy | Fragment / vertex engine |
| --- | ---: | ---: | ---: |
| Performance (1992 MHz, GPU 800) | 20.6 / 24.1 / 25.8 / 32.4 | 63–65 % | 566 / 265 ms a second |
| Same, GPU capped at 600 MHz | 20.5 / 25.1 / 25.7 / 33.8 | 60 % | 612 / 287 |
| Same, AFBC off (`PAN_MESA_DEBUG=noafbc`) | 20.6 / 24.2 / 24.8 / 32.9 | | 597 / 242 |
| Uncapped schedutil (the launcher's leftover) | 22.8 / 28.8 / 30.0 / 31.4 | | |
| 1608 MHz cap (Balanced) | 21.7 / 26.0 / 26.3 / 34.8 | 52–55 % | 468 / 215 |

**The GPU is not the limit.** A quarter less GPU clock changed no window by more than 1.4 ms and raised the
fragment engine's busy time 8 %, not the 33 % a throughput-bound load shows; the engine sits at 47–60 % busy.
Its ~15 ms a drawn frame is latency and per-job overhead (tile loads and stores around every pass, job start-up),
not shading: `BIFROST_MESA_DEBUG=shaderdb` lists 91 fragment shaders with a median of 0.4 cycles and none above
2.7, little texture work. Framebuffer compression costs nothing. An overclock would buy nothing; a lower render
resolution would not either, since the pixels are not what is slow.

**The GL thread is.** It burns about 17 ms of CPU a drawn frame at 1992 MHz: 12–17 ms issuing the passes'
commands plus about 5 ms submitting the batches (inside the query calls, outside the pass scopes). The frame-slot
fence of patch 0001 waits 0.02 ms, so nothing here is waiting for the GPU. By sampling, the thread is 58 % Mesa
(GL front end and Panfrost draw emission: uniforms `panfrost_emit_const_buf` + `_mesa_uniform` + validation about
9 %, `panfrost_draw_vbo` 3 %, shader state 4 %, vertex-buffer binding 4 %, texture state 3 %), 24 % in `ioctl`
(the two job submits of every pass, about 48 a frame at roughly 85 µs each), 4 % `memcpy` (push-buffer uploads),
5 % PPSSPP's queue runner and 2 % SDL and the player.

**The cost is the pass count, not the draw count.** A frame has about 24 passes: one 480x272 scene pass of
about 2600 commands (2.4 ms to issue), six 64x64 render-to-texture passes of about 200 commands each followed by a
128x128 pass of 14 commands, a 565-to-4444 reinterpret of the 64x64 buffer the game uses in three formats, and
the back buffer. The 128x128 passes cost 0.2 ms for 14 commands and the 64x64 ones 0.3–0.5 ms, so each pass
carries about 0.45 ms of fixed cost (0.25 inside, 0.2 in the submit) against about 1 µs a command: roughly 11 ms
of the thread's time is per pass and 3 ms per draw. The GL backend never invalidates a framebuffer and binds every
pass keep/keep/keep, so Panfrost also preloads and stores all attachments of every pass.

**The other thread is fine.** The emulation thread is at 50 % (8 ms a game frame): JIT code 42 %, the display
list runner 8 %, bounding-box tests 3 %, texture hashing 2 %. Four vertex formats with morphing fail the vertex
decoder JIT and decode in C++ every frame (`Vertex decoder JIT failed` in the log), a small cost.

**The kernel pays on every submit.** `linux-rocknix.config` carries `CONFIG_DEBUG_SPINLOCK`,
`CONFIG_INIT_ON_ALLOC_DEFAULT_ON`, `CONFIG_INIT_ON_FREE_DEFAULT_ON` and `CONFIG_SCHEDSTATS`, hardening and debug
defaults that zero every allocation twice and check every spinlock; 85 µs for a job submit is slow. A trial costs
no build: `init_on_alloc=0 init_on_free=0` on the `APPEND` line of `extlinux.conf.in`; the spinlock checks need a
kernel build. Both touch every system, so this is the owner's call.

**What was learned the hard way.** `RGDS_GL_PASS_FINISH=1` (a `glFinish` after every pass) died with SIGSEGV
at 42 s once and most likely kernel-panicked the handheld the next time (`panic=10` reboot ten seconds after the
run started, nothing of its output reached the card): never run it. Panfrost's timestamp queries are useless on
this job-manager GPU (the stamp job sits in the vertex chain of a fresh batch, so end stamps land before start
stamps); the profiler's CPU clocks are what the numbers above use.

**The fix that fell out of measuring.** Every run with the profiler on drew about 3100 frames in its 90 s, every run
without it about 2600, and the profiler's one side effect is that its end-of-pass query makes Panfrost submit the
pass's batch at once. Without it the driver holds all 24 batches until something forces a submit, so the GPU idles
while the CPU issues the frame. A `glFlush` after each render pass (patch 0002, on by default,
`RGDS_GL_FLUSH_PASSES=0` restores upstream) gave 3284 frames against 2575, 27 % more, with the lighter road at
17.3 ms a drawn frame; the heavy road still sits near 29 ms. Mesa's glthread, tried with a Panfrost build that
advertises the two capabilities it needs (`map_unsynchronized_thread_safe`,
`allow_mapped_buffers_during_execution`), gave nothing: 2598 frames against 2648, the marshalling thread as busy as
the direct calls had been (64 %) with the worker at 46 % on top. Measure runs by drawn frames in the fixed 90 s,
not by the 300-frame windows, which cover different stretches of the route once the frame rate differs.

**What is left to try, in order.** (1) The kernel options above. (2) PPSSPP's
Vulkan backend on PanVK would remove the GL front end's share (perhaps half the thread) but not the submits:
PanVK on this GPU also submits each pass's job chains separately. (4) Fewer passes is not in our hands: the game
switches targets. A steady 30 is already there with fixed skip 1 (every window at 33 ms), and 60 everywhere needs
the GL thread under 16.7 ms with margin, about a third off it.

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
| `DS_LID_PULSE=0` | No synthetic hinge pulse after a host suspend (the launcher sets it: the game carries on where it was frozen); the default pulse holds the emulated lid closed until the firmware sleeps, then opens it |
| `DS_CADENCE_LOG=<path>` | One line a frame (start, hand-off and next-start timestamps, locked, band) plus one per page flip (completion, swap, hold); `build/pace-lock/flipwait.py` reports the wait distribution and the panel period |

The headless engine (`dsperate-headless` from `dscore/`) runs games without a
display; with `--trace /dev/null --max 0 --interp` and `DS_TRAP_WILD=2L` it
stops at the first ARM9 jump into unmapped memory or an odd ITCM mirror and
prints the preceding instructions.

Earlier measurements, including other emulators, are in the git history.
