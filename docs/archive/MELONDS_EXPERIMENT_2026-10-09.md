> Superseded on 2026-10-11: [the selectable SDL integration](MELONDS_INTEGRATION_2026-10-11.md) now builds on this core probe. The report below records the earlier limited experiment.

# melonDS Linux ARM64 experiment — 2026-10-09

melonDS is not Android-only. Upstream provides Linux, Windows and macOS build
instructions. Its ARM64 JIT and frontend-independent core make it a candidate
for an additional lightweight RGDS engine. See [upstream build instructions](https://github.com/melonDS-emu/melonDS/blob/906e9ebb27da8c6a715cd7abab4abfe8a8d29427/BUILD.md).

The first safe piece is complete: an unmodified, pinned core builds and runs
under native Linux ARM64. **It is an experimental headless probe, not a playable
or selectable RGDS engine.** DraStic, DSperate and NNDDSS remain required; no
existing engine selection or package was changed for this experiment.

## Build and evidence

- Upstream commit: `906e9ebb27da8c6a715cd7abab4abfe8a8d29427`.
- Source tree: `60c6724f8695bdbe5e21c4367b18293f7f811fc2`.
- SHA-256 fingerprint of all 778 tracked source files:
  `17d56c66414d0d5d26fb1842b03772190931abc5bf54314684c67f223124f2da`.
- Release build, ARM64 JIT, LTO, Cortex-A55 tuning, GLIBC 2.17 baseline.
- Qt frontend, OpenGL renderer, GDB stub and Teakra tools/tests disabled for
  this software-rendering core check. No desktop UI dependency is introduced.
- Stripped probe: **2,042,200 bytes**; SHA-256
  `bc3d436badc4fe098537f4e75a4611b89db0705ca6a1179b1f3c957ec0f95426`.
- Full corresponding upstream source, original probe/platform, build drivers and
  upstream notices: `build/melonds-experiment/source.tar.gz`.

`tools/build_melonds.py` rejects changed source, missing JIT/LTO and an unexpected
ELF architecture or GLIBC requirement. `tools/probe_melonds.py` verifies the
executable against its build record and preserves existing result files.
Four focused build/probe guard tests passed. Existing runtime/UI/package code
was not changed, so the earlier 611-test three-engine run remains separate
evidence, rather than a claim of melonDS integration coverage.

The original synthetic cartridge exercises actual ARM9/ARM7 execution without
a downloaded game, Nintendo BIOS or stock installation files. The headless
platform refuses file access and unexpected persistent-save writes. It uses
upstream FreeBIOS and generated firmware.

Native Ubuntu ARM64 checks passed in both interpreter and JIT modes:

| Check | Result |
| --- | --- |
| Complete emulated frames | 13 in each mode |
| ARM9 execution and key sampling | Counter advances; A press and release observed |
| ARM7 JIT | Compiled blocks present, alongside ARM9 blocks |
| Pen input | Press/release visible through the ARM7 register |
| Separate software screen output | Distinct top/bottom hashes, matching across CPU modes |
| Checkpoint | 19,219,831-byte memory snapshot; RAM restored |
| Execution after checkpoint load | Four-frame replay returns exactly the same counter |

The probe's checkpoint exercise exposed a required frontend behavior: upstream
does not serialize live key/pen input. After loading, the frontend must reapply
current controls and stylus state. The probe does this explicitly.

Raw records: `build/melonds-experiment/build.json` and
`build/melonds-experiment/probe.json`; build log:
`build/melonds-build-20261009.log`. Both modes ended at counter 61,484 and produced
the same screen hashes. Peak child resident memory was 183,252 KiB in this VM.
This is a small synthetic workload and does not establish commercial-game
compatibility, RG DS FPS, battery use or a hardware memory budget.

## Reproduce

On the current Mac build host with Zig, CMake and Ninja:

```sh
make build-melonds-experiment
python3 -m unittest tests.test_melonds_build -v
```

Inside a Linux ARM64 environment, with the repository and build mounted at the
same location:

```sh
python3 tools/probe_melonds.py --report build/melonds-experiment/probe-rerun.json
```

Use a fresh report name for each run. The Linux builder was stopped after the
successful check to release its memory.

## Remaining pieces

1. Build the thin SDL frontend: named dual displays, correct touch/controller
   mapping, audio buffering and DS pacing. Reuse the shared RGDS integration
   interfaces and test menu/resume plus ordinary save/checkpoint persistence.
2. Add an optional engine adapter only once that executable is validated.
   Preserve stable IDs and behavior for the three required engines.
3. Compare familiar 2D/3D and touch-heavy games on the standard RG DS, with the
   same power/settings baseline. An upstream JIT does not establish speed here.

SSH userspace testing can use the existing Anbernic card. Testing a replacement
kernel and boot sequence requires a **spare microSD of at least 8 GB**; it need
not be brand new, but flashing erases it. Keep the working Anbernic card intact.
Do the independent image's spare-card boot after the three required engines and
recovery checks are ready. The earlier DSperate-only image is not that candidate.
