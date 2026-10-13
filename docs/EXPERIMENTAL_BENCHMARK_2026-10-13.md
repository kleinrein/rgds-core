# RG DS Experimental: Vulkan and correctness investigation, 13 October 2026

The new candidate implements the lid-resume fix, selectable 2×–5× gameplay,
and RGDS-owned FF/volume overlays. Native remains the default. Tests use private
profiles and copied checkpoints with a process-local Vulkan driver. System
graphics libraries, clocks and original user saves are unchanged.

## Latest local candidate — native UI removal and cached sampling

The fork no longer contains the old SDL pause/settings painter: `menu.cpp`,
`menu.h`, `settings.cpp` and `settings.h` are deleted from its modified source.
Its game picker, loading notice, gameplay OSD and screenshot flash are removed
from the frontend. Pause publishes an atomic request to the RGDS supervisor;
RGDS supplies the menus, loading/error presentation, save gallery, game FPS,
fast-forward and volume UI. Emulated firmware remains guest content. The
modified source archives retain upstream copyright and GPL attribution.

The enhanced compositor now caches its horizontal sample addresses until scale,
smoothing or the nine-bit 3D scroll offset changes. This removes repeated
division, wrap and bounds arithmetic from each displayed scanline. Exhaustive
tests of 3,145,728 tap pairs match the previous sampler, including the transparent
half of the DS scrolling plane. An isolated ARM64 Linux VM sampling benchmark
measured median times of 68.527 → 38.857 ms at 3× and 70.895 → 39.051 ms at 5×
for its fixed batch. That is 43.3%/44.9% less time in this loop only. These are
not handheld measurements or whole-frame speedups; actual 3×, 5× and FF rates
remain to be measured after the hardware reservation is released.

Validation of this candidate:

- Native and Vulkan AArch64 binaries compile; payload hashes, ABI and complete
  corresponding-source archives validate.
- Fifteen source-build/frontend/stylus/launch tests pass. Earlier Graphics,
  overlay and bridge checks remain applicable; actual new light/dark renders
  of Graphics, resolution, smoothing, Sound and emulator settings were inspected.
- The native SDL frontend boots Mario Kart for 120 frames in the Linux VM.
  Injecting its actual pause key at frame 30 publishes the RGDS menu request,
  and the run finishes cleanly without entering the removed native menu.
- Fresh ARM64 `test_io`, `test_gpu` and 4,096 compositor cases pass in the VM.
- From the private Mario Kart race checkpoint at frame 5300, 120 interpreter
  frames preserve native framebuffer hashes, audio bytes and serialized state
  byte-for-byte against the previous native headless build. Vulkan is disabled
  in that comparison; enhanced device parity remains a separate pending test.

Native binary SHA-256:
`6231187282bd0872691d21dde7711f5e3feb988eaf305727c4e7084f8753e483`.
Native source fingerprint:
`660fd61bab9830a008487f2dad8b570bcaf0e2f26bbfdb8a20afe7d3f8666346`.
Vulkan binary SHA-256:
`43fef40708cd743e8991b2103216f46defccc29b522ba427c4981d4155b93f2c`.
Vulkan source fingerprint:
`9f121ef2181aaf5b93ae1efcf5a9fec0ad1249ddeb9ab80aad7b67b6877da078`.
Evidence is under `build/rgds-ui-retirement/`: `source-tests-final.log`,
`arm64-core-tests.jsonl`, `native-menu-smoke.log`, `native-parity/results.json`,
`sample-bench-vm.jsonl` and `preview/`. The preview HUD uses illustrative values;
it is an actual render of the RGDS HUD functions, not a device capture.

Updated payloads are in `build/engines/rgds-experimental` and
`build/engines/rgds-vulkan`. The verification headless binary and
`build/experimental-device-20261013/test_hires_filtered` were rebuilt too.
The full launcher package must be rebuilt after concurrent launcher/bridge
changes settle: the earlier `6d176774245f` package is stale. No deployment or
handheld operation was performed for this local follow-up. The recovery chat
still reserves the handheld for physical tests; an idle turn is not release.
Inspect its latest status and other active RG DS chats before any deployment.

## Earlier Graphics/UI candidate — superseded locally, device validation pending

The follow-up removes the fork's gameplay OSD implementation (FF/FPS labels,
state notices and screenshot flash), including the previous suppression guard.
RGDS provides the FF badge, volume card and game counter. A direct in-game
Graphics tile groups resolution, edge smoothing, screen filters and frameskip,
with active-renderer information and Save & restart. The preferences parser fix
from the concurrent recovery work is preserved.

Smooth mode uses four coverage-weighted samples from the larger 3D target. At
5× this is an exact 2×2 box resolve to the 640×480 panel; hidden RGB in transparent
samples cannot darken the edge. At 3×/4× the samples approximate the footprint.
Sharp mode retains point sampling. Native 2D composition and guest readbacks
stay independent. A related bridge bug was fixed: filters now recognize the
fork's 640×480 enhanced game texture and receive its actual texture dimensions.
This support is scoped to the experimental enhanced renderer.

Final local checks: 57 UI/overlay/bridge/launch/benchmark regressions and six
real GLES shader-pixel checks passed. ARM64 Linux VM runs of `test_io`, `test_gpu`
and 4,096 differential compositor cases passed, including transparent-edge
filter cases. Host and device binaries compile. These results do not establish
handheld performance or visual acceptance for the filtered candidate.

Vulkan binary SHA-256: `22182941f9d600ae4ec377495a0add7db29a52273faa524df57ea2491574c335`.
Modified Vulkan source fingerprint: `3a8d6fa35043b1d70ea2c1898c5f90d42c874df3dfc90f1b624edf48f4cacfb5`.
Evidence: `build/rgds-graphics-final-tests.log`,
`build/rgds-enhanced-shader-tests.log`, `build/rgds-filter-arm64-tests.json`,
`build/rgds-ui-preview/graphics.png`, and `build/rgds-ui-preview/hud.png`.
Prepared hardware scripts are `run_filtered_verification.py` and
`run_filtered_benchmarks.py` under `build/experimental-device-20261013`.
The combined candidate is packaged as `dist/RGDS-0.0.1-dev-6d176774245f.zip`.
It includes the recovery chat's lid/audio changes present at build time. The
device build and full provenance checks passed after regenerating the 63 shader
previews for the enhanced-texture pipeline change. Another 53 preference,
GameMenu/startup and preview-validation tests passed in
`build/rgds-graphics-combined-tests.log`. Recheck source freshness if the
recovery chat makes further changes before deployment.
Deployment is waiting for the separate physical lid/audio recovery investigation
to release the handheld. The measurements below belong to the previous build.

A private Mario Kart Figure-8 Circuit race checkpoint is prepared at frame
5300 in `build/experimental-device-20261013/mariokart-benchmark.dss`.
It was generated from the local ROM in the Linux VM using interpreter mode;
the A55 JIT's instruction requirements are incompatible with this VM.
This is workload preparation, not a handheld performance result. The input
replay and screenshots are beside it. `prepare_mariokart_benchmark.py` stages
the seed after device handoff, and `benchmark-graphics.py` exercises the actual
in-game menus through Linux input events within an isolated benchmark profile.

## First deployed high-resolution candidate (before filtered resolve)

Vulkan binary SHA-256:
`d35b1d8e319c10daf9da067d19f0ab3f17334d0a98129083ce2a9f6da7ce9200`.
Native binary SHA-256:
`4aeff6d3a816d71cc977849ed3a8b28136739906070cb5e836ccf0fc2eff1abc`.
The complete source, shader, driver and package inputs are recorded in
`build/engines/rgds-vulkan/vulkan-build.json`.

Real gameplay was measured from the same NSMB checkpoint, with 15 seconds per
normal/fast-forward/restored phase. Audio emulation was active, playback muted;
FF skips hidden raster frames and normal play renders every frame. Approximate
rates below use the phase's recorded frame count and duration, not monitor Hz.

| 3D scale | Normal FPS | Fast-forward FPS | Restored normal FPS |
| --- | ---: | ---: | ---: |
| 2× (512×384) | 60 | 135 | 60 |
| 3× (768×576) | 60 | 121 | 60 |
| 5× (1280×960) | 57 | 105 | 59 |

All three runs exited 0, resumed two shared-menu cycles and wrote checkpoints.
Actual 5× gameplay screenshots contain sharper 3D Mario geometry and clean 2D
backgrounds. Evidence: `game-culled-gameplay-{2,3,5}.jsonl` and
`culled-gameplay-{3,5}-initial-top.png` in
`build/experimental-device-20261013`. GPU traces report exact-frame waits and
zero lagged frames. This is one scene, not a general 5×/60 FPS claim.

The first full compositor delivered roughly 55/52/46 FPS at 2×/3×/5×.
Caching layer priorities alone regressed to 48/47/42 FPS and was rejected.
Checking both rasters for transparent rows, reusing their 2D output and avoiding
large uploads for the 2D-only panel recovered the performance above. Native DS
capture/readback still uses the independent CPU raster; the GPU image only
enhances display output. Upstream GPU visual approximations remain.

ARM64 device verification passed `test_io`, `test_gpu` (including late capture
after FF raster skip), and 4,096 compositor combinations of BG/OBJ priority,
alpha, windows, blending and master brightness. The new compositor was compared
against the original full native layer-selection path and preserved native
output. Source: `src/experimental/rgds/hires_compositor_test.cpp`.

A deterministic 120-frame NSMB comparison, native vs 5× with
`DS_VERIFY_HIRES=1`, produced byte-identical native framebuffer hash sequences,
audio and checkpoint files. Their SHA-256 values were respectively
`ba541b2195e89086ac21f7556db5477912e1c922538c833fef373079a31e0c69`,
`c75421de06daed50a9ef6a3255c28fbdb646a565ce52fb9987638301ca21f108`, and
`b89dea767f61212c43bbc08e459c8b86a2db77f41bb9f104a803d9a8df0bc033`.
Evidence: `verify-hires4.jsonl`. This establishes equivalence for that run,
not universal emulator or GPU visual accuracy.

The lid failure was caused by treating STOP (0xC0) as ARM7-only HALT (0x80).
Video and HBlank DMA kept advancing during sleep, losing their phase. The fix
freezes CPU/video/timer/DMA time, advances only RTC events, and supplies silence
for frontend pacing. The on-device regression checks a mid-scanline stop,
65 sleeping frames, RTC advancement, disabled-IRQ rejection and lid-IRQ wake.
The formerly corrupted NSMB emulated-lid sequence now exits cleanly and its
post-resume screenshot has clean clouds. Evidence: `game-cpu-lid-fixed.jsonl`
and `cpu-lid-fixed-top.png`. Physical hinge/system-suspend acceptance and the
Zelda pencil test remain distinct; Zelda is reserved for the user.

The RGDS menu persists Native through 5× per game and selects the separate
Vulkan binary on restart. Both library and in-game pickers are exercised with
real SDL input. This initial candidate disabled native FF text under the shared menu, which
supplied the blue/navy FF badge and volume HUD. The follow-up removes the
native gameplay OSD drawing code entirely and adds the owned game counter. SDL pixel tests verify their
appearance, removal and cached texture updates.

Installed release `57573bdc7c06` additionally completed a 180-second 5× run,
two menu cycles and checkpoint/exit with status 0. The recorded presentation
average was 56.37 FPS across the full 184.985-second stats window; the final
game-rate sample was 57.76 FPS. This sustained result is below 60 FPS.
Its separate 5× emulated-lid action resumed and reopened the menu successfully.
A second-game deterministic Mario Kart 600-frame startup comparison also
preserved native frame hashes, audio and state byte-for-byte. Startup parity
does not establish gameplay performance. Evidence: `game-packaged-5-soak.jsonl`,
`game-packaged-5-lid.jsonl` and `verify-mariokart.jsonl`.

The following sections retain earlier investigation and comparisons in their
original experimental context.

## Resolve shader optimization

The single-sample shader accumulated one covered sample and then divided by
the dynamic coverage count. The optimized branch packs that sample directly,
after the same fog and alpha calculations. Transparent output remains zero;
four-sample MSAA still uses its existing averaging path.

Three variants were compared: upstream SPIR-V, unmodified GLSL freshly compiled
with the same compiler/options, and the optimized GLSL. All used the same
scaled renderer, pinned private PanVK driver, no MSAA, and 160 measured
iterations per run. Startup is excluded; upload, recording, submission,
completion and readback are included. No overclock or governor change.

| Recorded geometry | Scale | Fresh control median ms | Optimized median ms | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Original 128-polygon scene | 1× | 1.50 | 1.30 | 13.3% |
| Original 128-polygon scene | 3× | 8.69 | 6.12 | 29.6% |
| Original 128-polygon scene | 5× | 22.86 | 15.67 | 31.5% |
| Original 1,536-polygon scene | 1× | 3.36 | 3.26 | 3.0% |
| Original 1,536-polygon scene | 3× | 10.99 | 7.86 | 28.5% |
| Original 1,536-polygon scene | 5× | 24.92 | 17.79 | 28.6% |
| NSMB captured 3D layer | 1× | 1.61 | 1.31 | 18.6% |
| NSMB captured 3D layer | 3× | 7.73 | 4.94 | 36.1% |
| NSMB captured 3D layer | 5× | 19.75 | 11.66 | 41.0% |

All **27 runs exited 0**. Every scale/scene had byte-identical complete RGB666
and alpha5 output across all three variants. The freshly compiled control
tracked the upstream shader, so this is not merely a compiler-flag comparison.
The NSMB recording contains 235 opaque polygons, 707 vertices and 6,656 decoded
texture words. These tests do not cover every DS effect or establish DS hardware
accuracy. The renderer still has upstream hardware-rasterization approximations.

Evidence: `build/experimental-device-20261013/resolve-control.jsonl`,
`resolve-compare.jsonl`, and the two shader build logs. Temperature during the
27-run comparison was approximately 43–45°C. That script's GPU clock path was
incorrect; do not infer fixed clocks from it. The separate alternating repeat
uses the observed `fde60000.gpu` path and records samples during each process.

The alternating 5× repeat (optimized/control/control/optimized, 240 iterations
each) confirmed the gain: heavy-scene medians 17.98/24.96/24.83/17.75 ms;
NSMB 11.73/19.76/19.76/11.65 ms. All eight runs exited 0 and retained identical
output per scene. Recorded CPU speed was 1.992 GHz. GPU samples ranged from
200 MHz during initialization to 800 MHz under load (one run also sampled
600 MHz); temperatures were 40–46.1°C. Evidence: `resolve-repeat.jsonl`.

Build with `python3 tools/build_rgds_vulkan.py --jobs 4`. The private build
record is `build/rgds-vulkan-game/build.json`; it includes the compiler version,
patch, shader and source hashes. The optimized native binary SHA-256 is
`fd02e3be057e4632aad5a16e996ee762f3f27d95af2fb382489060d20196a9d1`.
Its source fingerprint is
`35a68457872d38a4c4efdc195eac56d3e814f9fd01b14e71572e0c8c66c53360`.

## Full-game baseline

Before the shader optimization, the same NSMB checkpoint and private Vulkan
binary were tested with GPU rendering disabled/enabled, respectively. Both ran
15-second normal, fast-forward and restored-normal phases. Audio emulation was
enabled, playback muted; normal rendering and FF skipping used profile defaults.

| Renderer | Normal emulated FPS | FF emulated FPS | Restored FPS |
| --- | ---: | ---: | ---: |
| Software | 59.93 | 159.13 | 59.93 |
| Vulkan, original resolve | 59.33 | 158.60 | 59.93 |

Both exited 0, resumed two menu cycles and saved checkpoints. Vulkan did not
improve FF in this scene: most raster work is already skipped during FF. The
GPU path's startup was 10.82 seconds versus 4.56 for software. Separate shader
timings must not be reported as whole-game speedups.

The optimized binary was then staged as the separate private release
`0.0.1-dev-bbbd2663b02b-vulkan-resolve`. A fresh 20-second-per-phase pair used
that identical binary with GPU disabled/enabled:

| Renderer | Normal emulated FPS | FF emulated FPS | Restored FPS |
| --- | ---: | ---: | ---: |
| Software | 59.95 | 162.81 | 59.95 |
| Vulkan, optimized resolve | 60.00 | 164.05 | 59.95 |

Both exited 0, acknowledged all speed transitions, completed two menu round
trips, and saved checkpoints. Preserved initial screenshots were inspected:
both backgrounds were clean. This one pair does not demonstrate a meaningful
FF advantage; the 0.8% difference is too small to separate from run variation.
Normal mean emulation time was 5.982 ms for software and 6.211 ms for Vulkan.
Vulkan startup remained 10.83 seconds versus 4.54 for software.

Evidence: `game-optimized-cpu.jsonl`, `game-optimized-gpu.jsonl` and
`optimized-*-initial-top.png`. GPU trace confirms actual GPU rendering, without
refused submits. It also confirms the upstream previous-frame fallback is
frequently used. Exact current-frame composition and input latency therefore
remain acceptance work; these rates are not evidence of equal accuracy.

Evidence: `game-cpu.jsonl`, `game-gpu.jsonl`. The Vulkan run additionally
acknowledged save/overwrite/undo/load/undo-load/export and microphone/lid
actions. Those acknowledgements do not establish correct pixels or audible
audio. Short smoke runs are not sustained stability acceptance.

## Lid-resume regression and capture evidence

Horizontal background streaks initially appeared in a screenshot retrieved
after the Vulkan feature checks. Subsequent investigation found two problems
with attributing that image to Vulkan:

1. Menu/checkpoint operations overwrite the bridge's capture filenames. The
   initial result hashes no longer matched the files later retrieved. The
   benchmark now preserves the initial pixels under `benchmark-scene/`, and
   its test explicitly overwrites the bridge files to verify preservation.
2. The **software renderer reproduces the same streaking after the emulated
   lid close/open action**. Isolated save/load and microphone actions produced
   clean backgrounds; the lid-only run reproduced the failure.

Evidence: `game-cpu-features.jsonl`, `game-cpu-save.jsonl`,
`game-cpu-blow.jsonl`, `game-cpu-lid.jsonl` and their retrieved PNGs. Each uses
the same private checkpoint; original user saves are unchanged. These are
post-action screenshots, separate from the preserved initial scene.

Disabling scanline lag or changing the worker layout yielded clean images in
runs without the lid action. A default-layout repeat was also clean. Therefore
these toggles are **not demonstrated fixes** and are not enabled in the fork.
At this stage, ARM7 HALTCNT sleep (0xC0) was handled like ordinary halt
(0x80). The subsequent scheduler fix and regression evidence are documented
in the current-candidate section above.

A deterministic 240-frame headless comparison also found the CPU outputs
identical with scanline lag enabled/disabled. The GPU differed only around the
small 3D character in the inspected frames, not the 2D backgrounds. The fixed
3D recording rendered correctly in isolation. This narrows the investigation;
it does not prove complete GPU accuracy.

The benchmark CLI now returns failure when a remote emulator record reports
an error or nonzero exit, even if SSH itself succeeded. This caught an unrelated
private-release staging issue: a generated `activate.pyc` had been copied from
the live release. The unlisted cache was preserved outside both release trees;
no source or manifest exception was added. The launch checks were rerun after
cleaning the staging inputs. The benchmark and fork test suites pass 26 tests.

## Resolution and remaining acceptance

The earlier awake retest completed eight 240-iteration runs at 1×, 2×, 3× and
5×, for 128 and 1,536 polygons. Original-shader medians were respectively
1.82/5.95/10.57/25.29 ms and 4.05/6.72/14.25/29.71 ms. GPU samples reached the
stock 800 MHz maximum; temperatures were 37–41°C. The 5× attachment, viewport
and readback are 1280×960. Inspection found 5,376 native-pixel-sized 5×5 blocks
with internal variation, confirming newly rasterized detail.

Evidence: `panvk-awake-scaled.jsonl`, `pixel-check.json`, and
`build/experimental-device-20261012/graphics128-5.png`.

These original scale measurements used the standalone probe. Full-game
composition and the lid fix were implemented subsequently, as recorded above.
The heavy 5× raster alone still exceeds the 16.7 ms frame budget. Wider game
coverage, physical lid cycles and audible audio testing remain acceptance work.
Zelda drawing is reserved for the user.

## Mario Kart wheel, live graphics and identity corrections (afternoon)

The missing wheel was a geometry cache invalidation bug, shared by the software
and Vulkan paths: mode-2 matrix operations changed the position matrix without
marking the combined clip matrix dirty. Commands 0x16, 0x17, 0x18, 0x19, 0x1A
and 0x1C now invalidate it. The new command-level regression fails all six cases
on the old implementation and passes on the handheld after the fix. Captures
from the same Mario Kart race checkpoint show all four wheels at native, 2×,
3× and 5×. Evidence: `build/rgds-wheel-fix/regression-red.log`,
`device-profile.jsonl` and `mk-{2,3,5}-top.png` in that directory.

Graphics scale, smoothing and frameskip now have a transactional live command.
The frontend joins rendering workers and replaces display resources while
preserving the emulated machine. A failure retains the previous renderer;
preferences are saved only after a successful acknowledgement. The bridge
queues resource changes until SDL event polling, avoiding resource replacement
between drawing the top and bottom panels. Native launch uses the optional
Vulkan-capable executable when available, allowing native-to-enhanced changes
without restarting the process. The standalone native fallback remains usable.

DS controls now include an editable ten-UTF-16-unit nickname, defaulting to
`RG DS Core`. Firmware generation and identity edits now encode UTF-8 names as
UTF-16 correctly, including Norwegian letters and complete surrogate pairs.
The old native menu/settings sources were removed earlier. Required upstream
copyrights, licenses and compatibility identifiers remain; visible engine and
console identity use RG DS Core. A game's own existing save/profile can retain
a name it stored previously; changing console identity does not rewrite saves.

The enhanced compositor compiles native layer/effect rules once per DS pixel,
then varies only 3D color/coverage for each enlarged sample. The exact native
blend, brightness, sprite and window behavior is checked against original
layer selection in 12,288 cases; 262,144 weighted filter comparisons also pass.
At 2×, repeated nearest samples and repeated output rows reuse their identical
results. An exhaustive sample-plan check covers every horizontal scroll offset.

Private 300-frame hardware comparisons from the same checkpoint have identical
native framebuffer, PCM audio and complete save-state bytes at 1×/2×/3×/5×:

- Native framebuffer: `eed6b4878865ac08463f19a57ff98c86f03c31ee02c594c7a94c3cdc0f90bab5`
- PCM: `cc732713c535b354efa2d5eea3d7ae84d5c7a3646b929d1a1c32b4f88cb160e0`
- State: `b946f6245832cb0d08e3c28c064750c42a4939b679f749abbf52799f7873371e`

Optimized enhanced captures are also byte-identical to the corrected prior
renderer at all three scales. Evidence:
`build/rgds-output-profile/optimized/device-profile.jsonl` and
`build/rgds-output-profile/fused-test.log`. Copying mapped GPU output to a
separate CPU allocation did not help and was rejected. These profiling runs
were not exclusive fullscreen measurements; they must not be presented as
accepted gameplay FPS. Other chats' package transfer/install activity overlapped
some runs. Final exclusive gameplay/UI verification follows device handoff.

Current native source fingerprint:
`e2b42f9e803d1b8ca0a6922336c5e883d65c781e22359a279446c82dcbe9901f`.
