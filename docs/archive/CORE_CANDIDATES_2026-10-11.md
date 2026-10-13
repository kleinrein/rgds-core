# Additional DS cores: reproducible experiments, 11 October 2026

DSperate remains the default. NooDS and SkyEmu now have built and executed
experimental core probes. Neither is yet a launcher choice or established as
faster or more compatible on the RG DS.

## Candidate review

| Candidate | Evidence from primary source | Practical next step |
| --- | --- | --- |
| [NooDS](https://github.com/Hydr8gon/NooDS) | GPL-3.0-or-later portable DS/GBA core, software rendering and configurable rendering threads. The pinned code uses an interpreter for the emulated CPUs. Direct boot can use its BIOS replacements. | Headless handheld probe executed; compare owned games in controlled gameplay scenes before committing to dual-screen/audio/menu integration. |
| [SkyEmu](https://github.com/skylersaleh/SkyEmu) | MIT license; native Linux support; upstream labels DS emulation beta. Its build supports `RETRO_CORE_ONLY` and a `skyemu_libretro` target, so a desktop UI is not required for an experiment. | Core-only Linux ARM64 build and synthetic frame/input/checkpoint tests passed in the VM and on RG DS. Full presentation, gameplay and battery-save acceptance remain separate requirements. |
| [NooDS libretro port](https://github.com/jonian/libretro-noods) | An unofficial adapter around NooDS, with a dedicated libretro Makefile. | Alternative integration route, rather than another independent emulation engine. Compare its base revision and patches against our pinned upstream before selecting it. |
| [DeSmuME](https://github.com/TASEmulators/desmume) | GPL project with an implemented ARM64 JIT and a POSIX build that selects it. Its separate interface build needs attention: that target still enables JIT only on x86. | Next independent JIT candidate for an isolated core probe. No build, compatibility result or speed advantage is claimed here. |

SkyEmu source review used commit
`4ff8c56962054434daf18637818c44bc88b16d50`. License and build switches were
checked in the files at that exact revision, not inferred from an older release.
The core-only build and actual synthetic test are recorded below.

## NooDS source and build

Upstream source is pinned to
[`4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0`](https://github.com/Hydr8gon/NooDS/tree/4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0).
The build script refuses a dirty checkout or a different revision. It compiles
only the core and our small original probe; wxWidgets, PortAudio, SDL, Nintendo
BIOS/firmware and downloaded ROMs are not needed.

Files:

- `tools/probe_noods.py`: pinned-source build, provenance and optional host run.
- `src/experimental/noods/probe.cpp`: original synthetic cartridge, correctness
  checks and optional headless ROM throughput test.
- `src/experimental/noods/null-file-close.patch`: Linux exit fix described below.
- `build/noods-probe/{host,device}/build.json`: exact compiler, flags and hashes.
- `build/noods-probe/device/vm-results.json`: actual ARM64 Linux test results.

The output directory includes the complete upstream source archive, GPL license,
local patch, harness and build script. Preserve these alongside any distributed
experimental binary. The probe is GPL-3.0-or-later because it links NooDS.

```sh
python3 -B tools/probe_noods.py --fetch --target host --run --frames 180
python3 -B tools/probe_noods.py --target device
```

The device build uses Zig 0.13.0, `-O3`, Cortex-A55 tuning and the
`aarch64-linux-gnu.2.17` target. It does not enable `-Ofast`/fast-math. The
ARM64 executable depends only on normal Linux C/pthread/dl runtime libraries;
there is no external C++ standard library or graphics dependency.

## Checks actually executed

The authored ROM runs ARM9 instructions, repeatedly increments a RAM word and
samples the key register. ARM7 loops without BIOS calls. The harness selects
different display backdrop colors. It checks:

1. ARM9 executes and sees released buttons and a pressed A button.
2. Touch press reaches the emulated input register.
3. Both 256×192 output buffers exist and have different hashes.
4. A checkpoint is written, accepted by NooDS and restores the saved RAM value.
5. Four frames replayed with the same input reach exactly the same RAM counter.
6. Key release works after restore, and the process exits successfully.

This passed on macOS ARM64, then on actual Linux ARM64 in the local VM with
thread settings 0, 1 and 2. Each Linux run measured 180 completed GPU frames;
audio emulation was enabled, with output discarded. Rates were 285.327,
274.791 and 285.557 frames/s respectively. Those are synthetic VM measurements,
not RG DS results, and the simple scene does not exercise 3D rendering. The
small differences do not establish which thread setting is better for games.
Each checkpoint was 5,463,265 bytes.

The first Linux run exposed an upstream exit crash: `SdMmc::~SdMmc()` called
`fclose()` on two null pointers when DS mode had never opened DSi NAND/SD files.
GDB identified the exact destructor and libc call. The local two-line patch
checks each pointer before closing it. The clean upstream checkout is retained;
the patch is applied to a separate build copy and recorded in `build.json`.
All three Linux runs above include the fix and successful process exit.

## Actual RG DS candidate runs

The parent device test subsequently ran this exact ARM64 candidate on the
standard RG DS. Raw results are retained in
`build/same-scene-20261011/noods-device.jsonl`.

| Workload | Completed frames | Elapsed time | Unlimited core FPS | Startup through three frames |
| --- | ---: | ---: | ---: | ---: |
| Original synthetic ROM | 600 | 15.699058 s | 38.219 | 160.942 ms |
| New Super Mario Bros, initial frame sequence | 900 | 14.018318 s | 64.202 | 205.737 ms |

Both runs passed the probe's checkpoint checks and exited successfully. Both
used `threaded2D=1`, `threaded3D=1`, enabled audio emulation and disabled frame limiting.
Neither presented to physical screens nor played audio. The second run is a
fresh-ROM opening sequence, not a verified gameplay scene. These results must
not be compared directly with the other engines running through RGDS/SDL.
The synthetic ROM spends its emulated CPU time in busy loops; its lower rate
does not contradict the opening-sequence result or establish normal-game speed.

The pinned NooDS source has no JIT option: its CPU run-function choices all use
`Interpreter`. `arm7Hle` is an available emulation shortcut, but changes ARM7
behavior and needs compatibility testing. It was disabled in this probe and is
not proposed as a default. Rendering-thread changes need a real 3D workload;
this experiment gives no evidence for changing them.

This does not validate battery-save hardware protocols, physical controls,
audio playback, dual-panel presentation, touch calibration, launcher return,
suspend/wake, DSi mode or commercial-game compatibility. The candidate must pass
those checks before becoming an installed backend.

## Controlled device probe

After copying the ARM64 executable and verifying its `build.json` hash, run it
only while other emulator benchmarks are stopped:

```sh
./noods-probe /mnt/mmc/noods-probe-new-run 180 1
```

The directory must not already exist. Every save and checkpoint stays inside
it. A final optional argument accepts an explicitly supplied ROM:

```sh
./noods-probe /mnt/mmc/noods-probe-new-game 180 1 /path/to/owned-game.nds
```

That form opens the ROM read-only and uses new private saves. It reports core
throughput and a RAM restoration check, without claiming gameplay/input
correctness. It starts from a fresh cartridge and measures after the same
short frame sequence each time. It is useful to identify candidates, but does
not replace the launcher's controlled same-scene tests: headless runs exclude
display and audio output cost, and a game can remain on a boot screen.

`completed_frames` counts newly available GPU frames, not frontend loop
iterations. `startup_to_three_frames_ms` is the time to initialize the core and
produce three such frames; it is not time to visible gameplay on the console.

Record ROM hash, frame range, settings, binary hash, CPU frequency, temperature
and framebuffer evidence when comparing device runs. Do not merge these
headless numbers into full-launcher rankings.

## SkyEmu build and executed probe

SkyEmu's pinned core-only target compiled successfully with GCC 13.3.0 on Linux
ARM64. Only CMake 3.15+, a C/C++ compiler, a build tool and Git were needed.
The target compiles `libretro.c`, `shared.c` and `localization.c`; it skips the
desktop frontend and its SDL, X11, ALSA, curl, OpenSSL and UI dependencies.
CMake's optional OpenGL discovery reported missing OpenGL and configuration
still succeeded. The small test frontend also links the system math library.

A sparse checkout avoids downloading unnecessary desktop dependencies:

```sh
git clone --filter=blob:none --no-checkout --depth 1 \
  https://github.com/skylersaleh/SkyEmu.git build/skyemu-candidate/source
git -C build/skyemu-candidate/source fetch --depth 1 origin \
  4ff8c56962054434daf18637818c44bc88b16d50
git -C build/skyemu-candidate/source sparse-checkout set --no-cone \
  '/CMakeLists.txt' '/LICENSE' '/src/*.h' '/src/libretro.c' \
  '/src/shared.c' '/src/localization.c' '/src/freebios/'
git -C build/skyemu-candidate/source checkout --detach \
  4ff8c56962054434daf18637818c44bc88b16d50
```

Run the following inside the Linux ARM64 builder, from the repository root:

```sh
cmake -S build/skyemu-candidate/source -B build/skyemu-candidate/linux \
  -DCMAKE_BUILD_TYPE=Release -DRETRO_CORE_ONLY=ON \
  -DENABLE_RETRO_ACHIEVEMENTS=OFF -DCMAKE_C_FLAGS=-mtune=cortex-a55
cmake --build build/skyemu-candidate/linux --target skyemu_libretro --parallel 1
```

The original test frontend in `build/skyemu-candidate/probe.c` ran the same small
authored ROM used in the NooDS correctness probe. It verified 193 video callbacks
with the expected 256×384 combined frame shape, ARM9 execution, A press/release,
a 5,716,336-byte checkpoint, RAM restoration and deterministic replay of the
same four-frame input sequence. Unload and process exit succeeded. The log is
`build/skyemu-candidate/probe.log`.

The same bundle then passed on the physical standard RG DS, including all
193 callbacks, the 5,716,336-byte checkpoint, input/replay assertions and exit
status 0. The complete hardware log is
`build/same-scene-20261011/skyemu-device.log`. This establishes that the core
executes on the handheld; it does not establish commercial gameplay speed.

This is a correctness smoke test, not a throughput benchmark. It does not
exercise actual graphics presentation, audio playback, touch calibration,
battery-save persistence or game compatibility. SkyEmu's bundled replacement
DS BIOS has a separate BSD-2-Clause notice; preserve it alongside SkyEmu's MIT
license. The private test bundle
`build/skyemu-candidate/handheld-smoke.tar.gz` includes both notices, the probe
source, the original ROM, binaries and a manifest of exact hashes. It adds no
installed emulator option or default change.

## Other independent engines and ARM64 JIT feasibility

The review below checks implementation and license files at specific commits.
An ARM64 build target or a Cargo feature named `jit` alone is insufficient.

| Project and reviewed revision | Actual architecture evidence | License and decision |
| --- | --- | --- |
| [DeSmuME `1275dc64`](https://github.com/TASEmulators/desmume/tree/1275dc64f5d1f5ef18dc1bc1fa9e012024b6df1a) | POSIX `configure.ac` and `meson.build` recognize `aarch64`, enable `HAVE_JIT` and select `utils/arm_jit/arm_jit_arm.cpp`; that implementation includes an ARM64 emitter. | POSIX project and ARM JIT files declare GPL-2.0-or-later; preserve individual component notices when making a distributable build. A credible next JIT experiment, with no RK3568 performance result yet. |
| [DSVita `ae36f93a`](https://github.com/Grarak/DSVita/tree/ae36f93a231751c4b0ef2f6672d50e93a3a91dca) | Despite the ARM32-focused README, source has AArch64 assemblers/emitters and an `aarch64-unknown-linux-gnu` Cargo target. Its [development notes](https://github.com/Grarak/DSVita/blob/ae36f93a231751c4b0ef2f6672d50e93a3a91dca/DEVELOPMENT.md) report unresolved ARM64 2D corruption, a New Super Mario Bros timing divergence and unconfirmed performance parity. | GPL-3.0 license, with submodule/component notices to preserve. Useful research candidate; the known renderer/timing defects make it unsuitable as the next default or a quick production addition. |
| [Dust `76d79f25`](https://github.com/kelpsyberry/dust/tree/76d79f25e4ea3ce355c4ac868f5ac9a233e64b89) | Portable Rust DS emulator with ARM64 desktop builds. Its Cargo file defines a JIT feature, but the pinned [`core/src/cpu/jit.rs`](https://github.com/kelpsyberry/dust/blob/76d79f25e4ea3ce355c4ac868f5ac9a233e64b89/core/src/cpu/jit.rs) contains only a TODO. | GPL-3.0 license. Could provide another compatibility/reference implementation, but it offers no implemented JIT path for this performance objective. |

DeSmuME is the clearest additional independent ARM64 JIT candidate from this
review. It still needs a measured test; a JIT does not imply that its GPU, audio
or synchronization costs fit this handheld. We did not build it in this pass.
The existing Linux builder lacks Meson, Ninja, pkg-config and the GLib, SDL2 and
pcap development headers required by its POSIX target. The separate interface
target's architecture check would also silently leave ARM64 on the interpreter.

The intended next test uses the pinned POSIX static library, with
`frontend-gtk=false`, `frontend-gtk2=false`, `frontend-cli=false`, OpenGL/GLES
disabled and its actual ARM JIT enabled. A small original-ROM harness should
call `NDS_Init`/`NDS_exec`, assert that JIT code was generated for both CPUs, then
verify input, framebuffers, checkpoint replay and clean exit before trying
owned gameplay scenes. This requires a deliberate build setup; no GUI stack,
system installer or on-device changes were performed for this review.
