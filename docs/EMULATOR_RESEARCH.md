# Sources for RG DS Experimental — 12 October 2026

The initial fork is DSperate-based because this project already has its ARM64
JIT, NEON renderer, native input/audio integration and recorded RG DS gameplay.
That is a starting point, not a claim that it beats all other emulators. The
user's ten-project list is a research set, not a verified performance ranking.

The table separates observed project features from our proposed use. Repository
READMEs were checked for all ten projects. Detailed implementation inspection in
this pass covered the locally pinned NooDS renderer/touch code and DSperate
worker/input code; the other rows are investigation directions, not completed
source-level audits or code imports.

| Project | Observed basis | Useful direction for this fork |
| --- | --- | --- |
| [melonDS](https://github.com/melonDS-emu/melonDS) | Independent core; already pinned and built here. | Compare tricky rendering, firmware, microphone and lid behavior using the same guest tests. Treat discrepancies as investigations rather than assuming either core is always correct. |
| [NooDS](https://github.com/Hydr8gon/NooDS) | Portable DS/GBA emulator with threaded software rendering. | Study render work sharing and firmware-derived touchscreen ADC calibration. Compare output and frame-time variance before porting an algorithm. |
| [melonDS DS](https://github.com/JesseTG/melonds-ds) | Statically links standalone melonDS; provides libretro integration, microphone and layout support. | Keep platform integration and core changes separate, with a small reproducible patch set. Study lifecycle and microphone contracts. It is not an independent CPU core. |
| [DeSmuME](https://github.com/TASEmulators/desmume) | Independent DS emulator; existing project investigation identifies a POSIX ARM64 JIT candidate. | Use debugging/compatibility work as a reference; separately build and measure the correct ARM64 target before proposing JIT changes. No implementation imported in this pass. |
| [Dust](https://github.com/kelpsyberry/dust) | Rust core focused on accuracy; diagnostic and GDB-enabled configurations. | Reproducible guest traces and register/memory inspection for timing bugs. Its listed Linux desktop binaries do not establish RG DS performance. |
| [SkyEmu](https://github.com/skylersaleh/SkyEmu) | DS beta; debugger, save previews and asynchronous automation API. | Repeatable scripted input/capture/checkpoint tests. Keep instrumented runs separate from timing measurements; DS accuracy remains something to check. |
| [ntremu](https://github.com/burhanr13/ntremu) | SDL2 frontend, debugger and free camera; README says JIT is x86-64 with ARM64 planned. | Rendering inspection and minimal frontend ideas. Its JIT is not an available drop-in RG DS acceleration path on the reviewed evidence. |
| [NooDS Libretro](https://github.com/jonian/libretro-noods) | Unofficial libretro port of NooDS. | Study input, audio and serialization contracts. Avoid counting the same underlying emulator twice in compatibility comparisons. |
| [DualityDS](https://github.com/Wz-ozk/DualityDS) | Console-style Qt frontend powered by melonDS; documented packaged release is Windows. | Controller-first usability and clear screen/touch mapping. Useful UX reference, not a separate faster emulation core. |
| [nds-plus](https://github.com/annethereshewent/nds-plus) | Desktop/web/iOS emulator with microphone and save-state support; README lists CPU/rendering bugs. | Compare microphone and save lifecycle behavior; investigate specific bugs rather than treating it as an accuracy oracle. No code reuse without checking the specific files' licensing. |

NNDDSS remains useful for behavioral and performance comparisons, but the local
payload has no verified corresponding core source. It is not a code donor here.

## Findings applied to the experiment

The locally pinned NooDS revision is
`4cb1439c330248b26cdf5f8dbcfa4e5ac53dcda0`. In
`src/core/gpu/gpu_3d_renderer.cpp`, `getLine1` helps render ahead while a line is
pending. DSperate's pinned `src/core/gpu/render3d.cpp` already has persistent
band workers and work sharing. Replacing those with NooDS's per-frame thread
creation would need strong measurements; it is not an automatic optimization.

NooDS's `src/core/io/spi.cpp::setTouch` makes explicit the distinction between
frontend pixel coordinates and the emulated ADC coordinates derived from
firmware calibration. The first fork change addresses the frontend layer:
normalized panel edges, primary contact ownership and unrelated release events.
The native regression test sends real SDL events through the fork's Input class
and reads the resulting DS input frames, using stub panel geometry. It does not
exercise ADC conversion, physical latency or Zelda's drawing recognition.

The current fork retains normal guest timing, uses Cortex-A55 code generation,
ARM64 JIT, NEON and LTO, and omits achievements/network transport. No unrelated
core implementations have been merged. See [implementation and physical
acceptance](../src/experimental/rgds/README.md).

## Device measurements and drawing follow-up

The [12 October device report](EXPERIMENTAL_BENCHMARK_2026-10-12.md) records
the controlled FF optimization result, DSperate/NNDDSS comparisons and Vulkan
prototype status. The user will perform the Zelda pencil test later; it is
not a requirement for the current performance pass. The following is a future
drawing-test recipe, not a test already completed.

The existing benchmark tool now accepts the fork and an explicit test volume.
For example, from a device at its home screen, using its actual SSH address and
owned ROM path:

```sh
python3 tools/benchmark_emulators.py --device root@DEVICE_IP \
  --rom /path/on/device/PhantomHourglass.nds \
  --cores dsperate rgds-experimental --phase normal --seconds 600 \
  --repeats 2 --volume 50 --shader 0 --checkpoints \
  --output build/zelda-engine-comparison
```

This starts from a fresh profile unless compatible per-engine checkpoints are
provided through the tool's `--resume-from` workflow. A fresh boot/title sequence
is not the pencil test. Save a matching drawing scene for each engine first,
record ROM/build/checkpoint hashes, then repeat the scene with identical power
settings and no frameskip. Video of physical drawing and audible playback remains
necessary. Host tests and build settings do not prove a speedup or a Zelda pass.
