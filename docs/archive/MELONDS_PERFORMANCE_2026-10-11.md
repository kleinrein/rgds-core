# melonDS performance investigation — 11 October 2026

The current RGDS frontend already uses the pinned upstream ARM64 JIT, fast
memory, release LTO, Cortex-A55 tuning, native resolution and threaded software
3D. The two SDL panels do not each wait for vsync; normal execution uses one
DS-clock deadline. Changing those existing settings without measurements would
not establish an improvement.

This candidate reduces unnecessary presentation during fast-forward. The core
still executes every DS frame and updates its actual emulated-frame counter,
while texture uploads and presentation are limited to approximately 60 Hz.
Normal mode presents every emulated frame as before. Fast-forward sound stays
muted as before. Returning from a pause or checkpoint rebases the presentation
clock, and a delayed frame never creates a burst of overdue presentations.

## Reproducible diagnostics

Set `RGDS_MELONDS_PROFILE=1` only during a measurement. The frontend writes a
JSON record to stderr approximately once per second with `type` equal to
`melonds-profile`:

- `fast_forward` and `threaded_3d` identify the active modes.
- `frames`, `presented`, `game_frames_total` and `wall_seconds` distinguish
  emulation from display work.
- `core_us_per_frame`, `audio_us_per_frame`, `draw_us_per_frame` and
  `pacing_us_per_frame` are average wall durations for each stage, divided by
  the number of emulated frames. They are **not process CPU measurements**.

The core duration includes emulation and any waits for its software renderer.
Draw includes obtaining the completed framebuffers, both texture uploads and
both SDL presentations. Pauses, checkpoint restores and mode changes start a
new measurement window; a final partial window is emitted on clean exit.
Profiling is off by default.

For an A/B experiment on the same candidate binary,
`RGDS_MELONDS_PRESENT_EVERY_FRAME=1` restores the previous presentation behavior
during fast-forward. Both variants must start from the same per-core checkpoint
with the same inputs, normal-speed settings, power profile and shader state.
Use separate disposable save directories. A cold-start attract sequence that
advances into different scenes is not a controlled throughput comparison.

Compare threaded 3D on and off using that same scene. Low core-stage time with
high draw-stage time points toward presentation or compositing; high core-stage
time warrants investigating the renderer or CPU JIT. This evidence should
precede wider changes to rendering accuracy or timing.

## Local validation

`tools/probe_melonds_frontend.py` now replays the original synthetic cartridge
for exactly 360 emulated frames with every-frame and limited presentation. Both
runs also exercise checkpoint save/restore, corrupt-state rejection, live
thread changes, input sampling and microphone lifecycle. CPU progress at the
end must match exactly. These tests use Linux ARM64 in the local VM with dummy
SDL drivers; they do not validate real audio or handheld graphics.

In the first local run, both variants ended at RAM progress `2459371`.
Every-frame presentation drew 360 frames; the limiter drew 33. The post-restore
355-frame measurement window fell from 0.892 to 0.533 seconds, with mean draw
cost declining from 1,051 to 95 microseconds per emulated frame. Complete test
times, including deliberately expensive save and recovery checks, were 1.230
and 1.116 seconds. Those are VM measurements, not RG DS speed claims.

The same run passed all 18 existing real frontend/bridge commands. A separate
core probe passed deterministic replay with both the interpreter and ARM64 JIT.
The host presentation-clock check covers full normal output, fast-forward
limits, pause recovery and the absence of catch-up presentation bursts.

Evidence is in `build/melonds-performance-20261011/`: the prior executable and
source archive are preserved in `baseline/`, and the new reports are
`frontend-probe-1/report.json` and `core-probe.json`. Retail-game and RG DS
measurements must be recorded separately before claiming a hardware speedup.

## Pixel format regression

A real RG DS capture revealed swapped red and blue in the melonDS frontend.
The pinned core's software renderer returns BGRA bytes, documented in
`GPU.h` and implemented by `SoftRenderer::ExpandColor`. The frontend had used
an ABGR8888 SDL texture, interpreting those bytes as RGBA on ARM64. The BMP
capture had standard BGRA masks; the screenshot conversion was correct.

The corrected texture uses SDL's explicit `SDL_PIXELFORMAT_BGRA32` byte
format. The native self-test reads the rendered output in independent RGBA
format and checks red, green and blue against the core's BGRA channels.
Comparing packed integers using the same incorrect format previously hid the
error. These assertions execute on the synthetic cartridge's distinct solid
red and green backgrounds and catch a red/blue inversion.

The stronger test failed against the previous ABGR texture with
`SDL RGB channels differ from emulated screen`, then passed with BGRA32.
The corrected build also passed all 18 native bridge checks, deterministic
360-frame fast-forward comparison and 17 host tests. Evidence is under
`build/melonds-performance-20261011/color-regression/`; the corrected executable
SHA-256 is `ac7afeff32ab8877526301bfa49e54c9cd44dc5c878dd8bda5e5262e7663c78a`.
Physical screen confirmation remains a separate device check.

## Linux semaphore experiment

The threaded software renderer hands completed scanlines between threads. The
RGDS platform implementation uses a mutex and condition variable for this
coordination. An optional Linux implementation uses POSIX `sem_t` to test
whether those handoffs contribute measurable overhead. It changes neither
renderer accuracy nor the JIT. **The condition-variable implementation remains
the default:** the controlled handheld comparison did not establish a useful
gain. See the [gameplay review](GAMEPLAY_REVIEW_2026-10-11.md) for device results.

Set `RGDS_MELONDS_POSIX_SEMAPHORE=1` to opt in. Only that exact value enables
the experiment, and non-Linux builds retain the condition-variable fallback.
The engine logs its actual selection once per process as
`RGDS melonDS semaphore=condition-variable` or
`RGDS melonDS semaphore=posix-experiment`. Runtime and benchmark environment
filters explicitly permit this diagnostic flag; unrelated control variables
remain filtered.

Both implementations are built into the same executable. Compare separate
disposable profiles restored from the same checkpoint, with identical inputs,
threaded 3D, shaders and power settings. Run the baseline without the variable
and the candidate with `--engine-env RGDS_MELONDS_POSIX_SEMAPHORE=1` in
`tools/benchmark_emulators.py`. Keep capture and warm-up outside measurement
windows, and check the actual backend log in both reports. A faster synthetic
handoff alone does not establish a faster emulator.

The implementation preserves counting, reset and timed-wait behavior. Linux
waits retry interrupted system calls without extending a timed wait's original
deadline. Reset drains at most the permits present when reset starts, rather
than chasing later producer posts indefinitely. Unexpected POSIX failures
produce an error instead of silently returning an unearned permit.

`tests/test_melonds_semaphore.py` compiles and runs the native fixture
`tests/fixtures/melonds_semaphore.cpp`. It checks both selections, immediate
and delayed waits, timeout duration, reset, 5,000 producer/consumer handoffs
with memory visibility, and 2,000 permits shared by four concurrent waiters.
On Linux it also interrupts blocking and timed waits with signals. The tests
passed on macOS for fallback behavior and on Linux ARM64 for both backends.
The standalone probe built against the handheld's GLIBC 2.17 ABI also passed
in the Linux VM. These are concurrency checks, not an emulation benchmark.

Both modes of the exact candidate executable passed all 18 frontend/bridge
commands in the Linux ARM64 VM, including repeated live threaded-3D changes,
save/load, corrupt-state rejection, pause, fast-forward and clean quit. Each
mode also passed the original cartridge's 360-frame comparison with both
presentation settings; all four runs reached RAM progress `2459371`.

The tested upstream pin is `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` and the
candidate executable SHA-256 is
`3fbaa6c5fc2ed2452aae9956049aa339e1b26afb86e5e18e72d8309c236a50cc`.
Local evidence is in `build/same-scene-20261011/`:

- `melonds-posix-candidate-summary.json` records the executable and source hashes.
- `melonds-posix-linux-concurrency.log` and `melonds-posix-linux-abi-probe.log`
  contain native Linux checks.
- `melonds-posix-default-vm/report.json` and
  `melonds-posix-enabled-vm/report.json` contain the matching full frontend runs.
- `melonds-posix-host-regressions.log` and
  `melonds-posix-python-regressions.log` cover the host fallback, environment
  filtering and build/integration regressions.
