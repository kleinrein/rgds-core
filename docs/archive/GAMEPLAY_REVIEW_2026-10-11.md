# Gameplay review — 11 October 2026

## Method and scope

These checks use the standard RG DS on Anbernic Linux and private benchmark
profiles. Each run receives identical copied ROM bytes; the user's existing
ROM and save files are not used as writable test storage. The workload is
New Super Mario Bros, World 1-1, with Mario stationary near the entrance and
no gameplay input during measurement. The prepared checkpoint thumbnail and
live captures identify the scene.

This is a matched stationary gameplay scene, not a frame-identical replay
across different cores. Each core needs its own native checkpoint. Repeating
the same checkpoint within one core is stronger evidence than comparing
unrelated cold-boot or attract sequences; wall-clock measurement windows can
still advance through different numbers of emulated frames. Results below do
not establish performance in every game or a ranking against another OS.
Fast-forward uses each backend's configured speed policy, not a shared unlimited
throughput setting. Its measured rate verifies acceleration/restoration within
that backend; it is not a cross-core performance ranking.

## DSperate: verified checkpoint resume and normal gameplay

Build `0.0.1-dev-d53f1de6ba74` ran two fresh profiles from the same checkpoint,
SHA-256 `5248ec70c278e226bdd2924eace853b317938c5d998aaf4ff327d98ad4515fb8`.
Both engine logs confirm loading the canonical `auto-dsperate.dss` at emulated
frame **6667**. The ROM SHA-256 is
`dd2e519a9e46134e213e6e63dd2b478d9a258627769b777baddb2316dc21b81d`.

Settings were native resolution, shader Off, frameskip 0 and normal speed.
After first presentation, the runner allowed eight seconds for startup/resume
and two seconds after acknowledging normal speed, then collected an
eight-second measurement window. There were no menu cycles during gameplay.
`DS_PROFILE=1`, `DS_FRAME_STATS=1` and `DS_FPS=1` were enabled in both runs;
the light profiler estimates and subtracts its probe overhead from the
reported work durations. It does not turn wall durations into CPU timings.

| Measurement | Repeat 1 | Repeat 2 |
| --- | ---: | ---: |
| Mean sampled emulated GAME FPS | 59.98 | 59.95 |
| Sampled GAME FPS range | 59.91–60.10 | 59.90–59.98 |
| Samples in normal-speed window | 8 | 8 |
| DSperate process CPU usage, across all threads | 96.7% | 96.5% |
| Session start to first game presentation | 4.375 s | 4.195 s |
| Whole-run emulation work mean / p95 | 6.165 / 6.929 ms | 6.198 / 7.063 ms |
| Whole-run emulation + presentation mean / p95 | 8.239 / 9.231 ms | 8.313 / 9.300 ms |
| Clean save-and-exit duration | 6.593 s | 7.453 s |

CPU percentages come from `/proc` CPU-tick deltas over the sampled interval;
100% means one fully occupied CPU, rather than the whole four-core chip. The
CPU remained at 1.992 GHz and the GPU at 200 MHz in every sample. Maximum
sampled SoC temperatures were 58.3°C and 58.9°C. These runs show approximately
full-speed emulation in this scene, with room inside the normal frame budget.
They do not show sustained CPU, display or thermal saturation.

The work-duration rows cover the entire engine run, including its first frame,
rather than only the eight-second sampling window. The first frame took
69–73 ms of emulation and 141–147 ms including presentation. After warmup,
the native per-second logs generally show 6.1–7.0 ms emulation, 1.9–2.2 ms
presentation and 7.4–8.4 ms deliberate pacing per frame. Faster work here
would principally create headroom or reduce CPU use; the normal-speed cap
should continue to preserve DS timing.

The main-thread profile's typical-frame costs are about 2.8 ms in ARM9,
0.9 ms in ARM7 and 0.9 ms in audio synthesis. Parallel rendering workers add
about 3.7 ms of overlapping work; that cannot be added to the main-thread
wall duration. JIT translation costs 40–42 ms on the cold first frame but
only about 0.014–0.015 ms in the typical frame. This evidence supports
investigating launch setup and first-frame preparation separately. It does
not justify changing clock accuracy, dropping normal frames or disabling
audio work to advertise higher FPS. No speculative core tuning was applied
from this short scene alone.

Both runs exited with code 0 and logged confirmed manual-slot and native
automatic saves. The canonical checkpoints changed and were hashed after
exit. An ALSA underrun was logged around the final save/quit pause; these
muted tests do not certify audible resume quality. Save-and-exit latency
also remains substantial and should be measured separately from gameplay.

### Excluded earlier runs and the resume correction

`dsperate-world1-normal.jsonl` is **excluded from gameplay comparisons**.
Although it recorded the intended seed hash, DSperate did not load that
checkpoint: RGDS stored it under its per-game library directory, while
native autoload only searched `<GAMECODE>.auto.dss`. The repeated launches
instead advanced from cold boot. Its late profiler spikes also included
multi-second menu pauses inside arbitrary ARM9/DMA scopes, rather than
measuring equivalent compute work.

RGDS now chooses the freshest nonempty canonical or native automatic state
and passes its exact path with `--load-state`; `--no-autoload` prevents a
different native slot from bypassing that choice. A newer battery save
suppresses checkpoint loading, and disabling Auto-load suppresses both
automatic paths. Existing manual and automatic files remain intact. Six
new resume regressions and the wider 44-test runtime check pass; see
[save behavior](../SAVES.md).

Local evidence: `build/same-scene-20261011/dsperate-world1-verified.jsonl`,
`dsperate-world1-1.png` and `dsperate-resume-regressions.log`. The verified
JSONL includes build/ROM/checkpoint hashes, experiment settings, actual phase
boundaries, hardware samples and the engine logs confirming restoration.

## Standard and SuperDrastic

Both backends loaded the same DraStic World 1-1 checkpoint (SHA-256
`1792cab4d83e59b8f5f6657d34a3407b79cfbec755591473e25d6e460b8fa783`).
The final saved thumbnails confirm Mario stayed at the level entrance, with
zero score and coins, after normal/fast-forward/restored phases of eight
seconds each. Both saved and returned cleanly. Standard first presentation was
3.602 s; its three presented-menu acknowledgements were 410, 438 and 424 ms.
SuperDrastic native readiness was 3.105 s. Its 31, 25 and 21 ms menu command
acknowledgements are not measurements of a displayed menu and cannot be
compared with Standard's rendered acknowledgements.

A separate original, lightweight ROM provides an independent completed-VBlank
counter in emulated RAM. The read-only probe validates process ownership and
reads only the counter and its complement from the owned emulator's main RAM.
SuperDrastic measured **59.93 normal, 785.27 fast-forward, 59.97 restored
emulated frames/s**. This proves actual acceleration rather than a presentation
counter or command acknowledgement. The roughly 13.1× ratio belongs to this
synthetic workload, not NSMB or the entire game library. Native save-and-exit
reported `state=saved`, and session ownership was released.

Fast-forward button injection now lasts 160 ms of monotonic wall time rather
than 40 drawn frames, avoiding prolonged holds when fast-forward skips draws.
The shader-off native menu also avoids an unnecessary 50 ms sleep and duplicate
draw. Physical mapped hold/toggle acceptance remains separate from SSH-command
and synthetic-memory verification.

Evidence: `drastic-backends-world1.jsonl`, `drastic-world1-result.png`,
`superdrastic-world1-result.png`, `super-live-candidate.jsonl` and
`super-live-candidate-summary.json` under `build/same-scene-20261011/`.

## Additional actual core tests

NooDS and SkyEmu were built from pinned open sources and ran original-ROM
execution, input and checkpoint/replay probes on the actual RG DS. NooDS also
ran the first 900 frames of this retail ROM headlessly. They remain isolated
prototypes: neither result validates a full RGDS frontend, physical audio or
same-scene rendered gameplay performance. DeSmuME's ARM64 JIT is a further
candidate; its build dependencies and correct target are recorded, and no
untested integration option was added. See the [candidate report](CORE_CANDIDATES_2026-10-11.md).

## NNDDSS: save-chip correction and gameplay scheduling comparison

Cold gameplay testing exposed a missing save-chip definition: NSMB reported
“Could not erase data” instead of starting a new game. Its default 512 KiB
Flash assumption is wrong for this cartridge's 8 KiB EEPROM. RGDS now derives
save-chip facts from the pinned, licensed melonDS table and generates an exact
current-ROM database entry. The loader's restricted parser requires a
single-quoted declaration/attributes, no comments and a CRC32 field; generic
XML serialization did not work. ROM CRCs are cached against file identity,
size and modification/change timestamps. Custom databases and existing saves
are preserved. The generated source table, license and build driver ship with
the package.

The corrected entry was validated on hardware: the intro and World 1-1 run,
and the native battery file has the expected **8,314 bytes** (8 KiB plus its
122-byte footer). Actual in-game progress across a completed save point is
still unverified; simply returning from World 1-1 does not establish that.
Earlier checkpoints created with the wrong chip may retain that state, so an
affected game needs a cold launch with Auto-load disabled once. No old user
checkpoint or battery save was deleted.

Two runs used the same World 1-1 checkpoint, SHA-256
`040141ae3e8366f202ea22ccb7a8f0644cf2e7ed492c72e3c37f0b00bb756f06`,
on build `0.0.1-dev-cc9dfeb78beb`. Both final thumbnails show Mario at the
same entrance, zero score/coins and timers 333/334. These are single runs per
scheduling variant, with eight one-second samples per phase; small differences
should not be treated as a repeatable optimization.

| Scheduling | Normal GAME FPS | Fast-forward GAME FPS | Restored GAME FPS |
| --- | ---: | ---: | ---: |
| Existing pinned roles | 59.58 | 119.10 | 59.76 |
| `NNDDSS_CPU_AFFINITY=0` | 59.43 | 119.99 | 59.97 |

The opt-out was confirmed in the engine log. It gives no convincing normal
speed gain, so the shipped default is retained. Both runs saved and exited
with code 0. First presentation took 4.544/4.548 s. The default's three
presented-menu acknowledgements took 420, 436 and 413 ms. This scene does not
explain or resolve every previously reported slow game; no accuracy or
synchronization setting was disabled to raise the reported speed.

Evidence: `nnddss-world1-default.jsonl`, `nnddss-world1-free-affinity.jsonl`
and corresponding PNGs under `build/same-scene-20261011/`. The diagnostic
thumbnail decoder now handles v15 NNDDSS BGR565 separately from v14 DraStic
RGB565, verified using the original probe ROM's known framebuffer colors.
This decoder correction changes no emulator rendering.
## melonDS: confirmed core bottleneck and threading comparison

The same native checkpoint (SHA-256
`358fd80d436527d51f2740180a2d58e4b0c724a40600855eca2a82a89075a1ce`)
was restored in three fresh profiles on build `0.0.1-dev-cc9dfeb78beb`.
A live SDL capture confirms World 1-1 at the entrance. It also verifies the
corrected BGRA pixel format; the previous frontend swapped red and blue.
Every phase had eight one-second GAME FPS samples after warmup and settling.

| Configuration | Normal | Fast-forward | Normal restored |
| --- | ---: | ---: | ---: |
| Threaded 3D, presentation capped to 60 Hz in fast-forward | 44.84 | 45.01 | 45.86 |
| Threaded 3D, present every emulated frame | 46.40 | 46.33 | 46.31 |
| Threaded 3D off, normal presentation policy | 41.71 | 40.86 | 40.64 |

These are one run per configuration, not statistically established differences.
Both threaded variants actually presented every emulated frame: neither reaches
60 GAME FPS, so the fast-forward presentation cap is inactive here. The roughly
1–2 FPS inter-run difference cannot establish a cap regression or improvement.
Turning threading off is slower in this scene, so the default stays enabled.
Actual native profile records confirm the requested threading values.

Threaded `RunFrame` work costs roughly 19–20 ms/frame, versus about 1.7–1.8 ms
for drawing and 0.02 ms for audio queueing. Pacing time is effectively zero.
This is an emulation bottleneck. The ARM64 JIT is already enabled, with upstream's
maximum supported block size of 32, fast memory, branch/literal optimizations,
`-O3`, thin LTO and Cortex-A55 tuning. There is no missing JIT switch to turn on.
No emulated clock, accuracy or audio setting was disabled to increase FPS.

All three runs saved and exited with code 0. First presentation took
4.930 / 4.716 / 5.777 seconds respectively. Native resolution and frameskip 0
were used. This makes melonDS a compatibility option rather than a demonstrated
full-speed replacement for DSperate on this handheld.

Evidence: `melonds-world1-limited.jsonl`, `melonds-world1-every-frame.jsonl`,
`melonds-world1-threaded-off.jsonl` and `melonds-world1-restored.png` under
`build/same-scene-20261011/`. The capture is taken before measured phases;
readback work is followed by the normal two-second settle interval.

### Semaphore experiment

A second same-binary comparison used build `0.0.1-dev-56f763fdf979`,
with a Linux POSIX semaphore experiment selected only by an explicit diagnostic
environment variable. Native concurrency, interruption, reset and timeout tests,
and both 18-command Linux frontend/bridge checks passed before deployment.

| Backend | Normal GAME FPS | Fast-forward | Normal restored |
| --- | ---: | ---: | ---: |
| POSIX experiment | 45.45 | 46.00 | 45.62 |
| Existing condition variable | 45.43 | 45.14 | 46.40 |

Both engine logs identify the requested backend. Both resumed the same gameplay
checkpoint, saved and exited normally. This short scene shows no convincing
performance gain. The existing condition-variable backend remains the default;
this experiment is not exposed as a user tuning option. Threading remains on.
Evidence: `melonds-world1-posix.jsonl`, `melonds-world1-cv.jsonl` and
`melonds-posix-candidate-summary.json` in the local evidence directory.

## Final recorded DSperate run and remaining menu defect

Build `0.0.1-dev-0f53a502946c` restored the exact canonical checkpoint at frame
6667 after the ROM-identity selector correction. It measured 59.99 normal,
94.45 fast-forward and 60.11 restored GAME FPS. This run did not enable
`DS_PROFILE`; the earlier profiled run measured 132.64 in fast-forward. The
unexplained difference needs a controlled repeat; these runs cannot establish
a DSperate throughput improvement. CPU frequency stayed at 1.992 GHz, while
sampled SoC temperatures reached about 65.6°C in the later fast-forward phase.

The stricter runner confirmed eight fresh owned menu helpers, eight completed
resumes, and a normal save-and-exit (exit code 0, 6.240 seconds). However,
**every helper needed the two-second forced-close fallback**, including the
exit menu. Resume took 2.08–2.17 seconds; opening took 0.46–0.54 seconds with
process-ownership polling. Resuming the engine before teardown did not fix
this latency. The menu work is not complete, and these cycles are not evidence
of graceful SDL teardown. Native cleanup needs tracing on the device.

Evidence: `dsperate-world1-final.jsonl`. The current host menu/return/library
suite passed 33 tests; this does not close the hardware teardown issue.

## Screen-swap follow-up — 12 October 2026

A reported DraStic screen flip exposed an inherited-profile gap: Standard's
owned mapping reset gameplay and save/menu bindings, but left native
`SWAP_SCREENS` and `SWAP_ORIENTATION_A/B` bindings intact in both banks. The
local adapter now unbinds those actions and resets `screen_swap` to zero at
launch. SuperDrastic uses the same initial profile preparation. Unrelated
configuration and saves are retained; 47 focused adapter/backend tests pass.

This is a local correction for unwanted native swapping, not physical panel
mapping acceptance. SSH at the previously authorized device address was
unavailable during this follow-up, so the correction has not been deployed
or tested on the handheld. The user has now confirmed **Standard DraStic, swapped from launch**. The
physical routing remains unverified; disabling swap hotkeys alone does not
explain a launch-time swap. No image/bootloader/SD changes were made.


### Standard startup follow-up

The pinned native binary's `reset_system` loads the global config and then
`config/<ROM stem>.cfg`. RGDS previously updated only the global file. Standard
launch preparation now applies RGDS controls, tuning and screen order to an
existing native per-game file too, retaining unrelated fields. Unsupported
binary per-game configs fail with their path instead of being overwritten.
This closes a configuration override gap; the failed device's actual profile
has not been retrieved, so this is not a confirmed hardware root cause.

A per-game **Screen order: Normal / Swapped** picker is available under Game
options before launching Standard DraStic and under its in-game Emulator
settings. Changes apply on the next launch (use Save & restart in-game).
Normal is the default. The setting is retained across other preference edits,
disabled for SuperDrastic in Game options, and only consumed by Standard's
launch path. It does not alter battery saves or checkpoints. Touch alignment
with each native screen order still needs a physical device check.


Because Standard and SuperDrastic share the native save/config home, switching
to SuperDrastic also synchronizes its own control bindings and normal screen
order into the native per-game layer. The stored Standard screen-order choice
remains available when switching back. This prevents the new Standard option
from leaking into SuperDrastic's separate display path.


Validation: 51 focused runtime/backend tests and 20 emulator-settings UI tests
passed, including cancel, failed writes, before-launch access, persistence,
and Standard → SuperDrastic → Standard profile switching. Host and ARM64
device builds succeeded. The private application package is
`RGDS-0.0.1-dev-49abd08784c7.zip`; it has not been deployed because SSH at
the handheld timed out. These checks do not prove physical screen/touch order.
