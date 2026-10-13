# ROCKNIXDS review and local adaptations — 2026-10-11

Reviewed JorreFog/ROCKNIXDS at commit
[`8b8f72cd70f0d198612ea8e6651fae71664aa97b`](https://github.com/JorreFog/ROCKNIXDS/tree/8b8f72cd70f0d198612ea8e6651fae71664aa97b).
The selected 15 source/documentation files were checked against Git blob hashes;
`build/rocknixds-review-20261011/reviewed-files.json` records their SHA-256 hashes.
Upstream installers were not executed. No handheld connection, deployment,
clock change, reboot, suspend or SD write was performed. Anbernic Linux remains
the supported device baseline.

## Changes implemented

### DraStic helper-thread initialization

The most relevant upstream fix is patch `5e14c95a`, included in the pinned
[SuperDrastic patch series](https://github.com/JorreFog/ROCKNIXDS/blob/8b8f72cd70f0d198612ea8e6651fae71664aa97b/dsflip/superdrastic-0.5.0-beta.1-rocknixds.7.patch).
DraStic starts its line helper and three raster helpers before initializing their
mutexes and condition variables. A worker can begin waiting before its creator
initializes that condition variable, losing its first wakeup.

Our owner's binary has GNU build ID
`7a5e0e5fc6e52e6e8f5499c3d4d667ef51db0748`, the exact upstream affected build.
SHA-256: `dab3275ef772451b9235b78e64b2d0d041a28ac22937668231088c76b709e218`.
Local disassembly confirms the order at `0x312ac` and `0x597b4`: thread creation,
two mutex initializations, then two condition initializations. Evidence is saved
as `drastic-identity.json`, `drastic-line-init.asm` and `drastic-bin-init.asm` in
the review build directory. This confirms the code defect; it does not establish
that every previously reported handheld freeze came from it.

`src/platform/drastic_threads.hpp` adapts the upstream barrier in our existing
bridge. It holds only the two recognized helper entry points (`0x2fa50` and
`0x58e50`) until their creating thread completes both condition initializations.
Activation requires `RGDS_CORE=drastic` and the main executable's exact build ID.
Unknown binaries, unrelated start routines and the other three engines pass
through. Thread attributes and CPU affinity are preserved. Each start owns its
gate, avoiding reused fixed-array entries, and only successful initialization
calls count. After a monotonic two-second timeout it logs a diagnostic and uses
the original startup behavior; this fallback is not a claim that initialization
succeeded. `RGDS_DRASTIC_HELPER_GATE=0` allows an explicit diagnostic comparison.
The MIT attribution and complete notice are included in `THIRD_PARTY.md` and in
the application package. This does not implement live DraStic Threaded 3D changes.

### Idle input and logging

ROCKNIXDS's [idle investigation](https://github.com/JorreFog/ROCKNIXDS/blob/8b8f72cd70f0d198612ea8e6651fae71664aa97b/docs/optimization-1.4.md)
identified SDL event waits polling open gamepads frequently. Our launcher had the
same API pattern. `wait_ui_event` now checks pending events, sleeps once for at
most eight milliseconds, then polls when a joystick or raw touch/volume input is
active. Desktop keyboard-only windows retain SDL's wakeable event wait. Animation
deadlines remain shorter when needed. This bounds the requested sleep, not total
physical input-to-screen latency.

The Python engine log worker used a 50 ms timeout even while the game was silent.
It now blocks on engine output and an explicit shutdown pipe. The existing
128 KiB RAM tail and bounded cleanup when descendants retain stdout remain.

### Memory and application integrity

- Emulator library comparison streams bounded chunks, preserving exact byte
  comparisons even if files have equal size and timestamps. Interrupted copies
  retain the installed component and remove the incomplete temporary file.
- Automatic-state/manual-slot comparison and backup hashing also stream rather
  than allocating multiple full checkpoint files.
- BMP dimensions and decoded size are checked before SDL decodes library artwork.
  Oversized, invalid or excessively large encoded files are cached as misses.
  Existing 64-texture / 16 MiB and 128-banner cache limits still apply. Normal
  covers, palette images and top-down BMP headers retain support.
- A release-integrity regression check found `activate.py` importing `engines`
  before disabling Python bytecode output. Direct verification could therefore
  create an unlisted `__pycache__` file in the installed release. Import order is
  fixed; package installation and direct verification are rechecked together.

## Measurements and validation

Host-only generated fixture, same Mac, one before/after run:

| Work | Before | After |
| --- | ---: | ---: |
| Compare identical 16 MiB code file: peak traced Python allocation | 16,777,547 B | 787,826 B |
| Same comparison: elapsed host time | 20.39 ms | 3.31 ms |
| Quiet log worker over 0.5 seconds: timer wakeups | 9 | 0 |
| Log shutdown with an inherited writer still open | 31.36 ms | 3.21 ms |

Timing varies with filesystem cache and host load. The memory bound and removed
timer polling are stronger evidence than the elapsed-time ratio. These numbers
are not device FPS, total launch time, battery runtime, or a comparison against
ROCKNIXDS. Reproduce current behavior with:

```sh
python3 -B tools/benchmark_runtime_idle.py --output build/runtime-overhead.json
```

Targeted host checks exercise real SDL virtual-gamepad press/release events,
keyboard wakeups, touch/menu behavior, cached artwork rendering, pre-decode
rejection, pipe draining, bounded shutdown, exact file refresh and save-slot
preservation. Native ARM64 Linux checks use the actual newly built bridge and
original synthetic ELF fixtures with deliberately delayed lock initialization:
repeated and concurrent helper starts, unrelated routines, mismatched identity,
other engine names, explicit opt-out and timeout fallback. They execute no
DraStic or game ROM code. The actual melonDS frontend is also rechecked with our
original homebrew and dummy SDL devices because the preload exports changed.

Validation logs, benchmark records and final native reports are in
`build/rocknixds-review-20261011/`. `thread-probe-final/report.json` is the final
thread report; earlier probe attempts are diagnostic evidence, not acceptance.
The final package and validation summary are recorded alongside these reports.

## Upstream ideas that need separate work

| Finding | RGDS decision |
| --- | --- |
| Direct KMS scanout and DMA buffer import avoid frame copies | Promising renderer experiment. Their session owns DRM and switches away from sway. Stock RGDS uses vendor SDL/Wayland; importing that lifecycle would risk panel/input regressions. Keep it separate until display handoff and recovery can be measured on stock. |
| Gengis rasterizer / NEON compositor | A DraStic-specific optimization, not another standalone DS emulator. Evaluate with the exact supported binary, pixel comparisons and matching native/2x settings before adopting its hooks. |
| Adaptive clocks and display queues | Upstream measurements show the latency/power trade-off. Do not copy MHz limits, affinity or GPU sysfs paths into a different kernel. Benchmark frame-time tails, clocks and temperature first. |
| Background artwork priority / retry limits | Useful next step. RGDS already uses a background worker and bounded caches; it does not repeatedly scrape automatically during rendering. Measure scanning/decoding contention before changing worker scheduling. |
| Resume state older than a native battery save | Real save-consistency concern. Their DraStic-specific timestamp check is not copied blindly across our four engines: save flush order and state contents differ. A follow-up should retain both files, validate per-engine paths and use checkpoint/save provenance, with explicit tests for later native progress and exit-time flushes. |
| Stall watchdog | Useful diagnostics, but a no-frame timeout alone can kill a legitimately loading game or a stopped menu/suspend session. Require engine progress, lifecycle awareness and a recoverable notice before enabling automatic termination. |
| ROCKNIX power/battery services | These target ROCKNIX paths and hardware policy; they cannot resolve our stock shutdown/restart or early boot-logo issues by substitution. The `powerstate` source itself retains GPL-2.0/JELOS notices despite the repository's general MIT license. |

Upstream's `SUPERDRASTIC` pin describes a binary needing glibc 2.38 and its own
libdrm stack. It is not bundled into this stock-targeted application. The new
RGDS launcher/bridge continue to target glibc 2.17. The ROCKNIXDS top-level MIT
license explicitly excludes third-party components and unlicensed portions of
the dii-ess-aye frontend; that frontend is not imported.

## Next handheld acceptance

Use the next authorized application update, preserving the active/previous
release and all saves. Repeat DraStic launch/quit with threaded 3D enabled and
checkpoint restores; capture any helper-timeout message. Compare stationary
launcher CPU and input response with a gamepad present. Run the same games,
power mode, brightness, resolution and shaders on each OS, recording game FPS
separately from presentation FPS, frame-time tails, menu latency and temperature.
DSperate remains the default; DraStic, NNDDSS and melonDS remain available.

NNDDSS's reported slowdown, physical sleep/wake, shutdown restarting on battery,
and the first Anbernic boot logo remain device acceptance items. This review
contains no measured claim that RGDS outperforms ROCKNIXDS or SimpleOS yet.
