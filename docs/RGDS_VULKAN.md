# RG DS Experimental enhanced 3D

RG DS Experimental offers Native and experimental 2×–5× 3D rendering in
**In-game menu → Graphics → 3D resolution** (also available before launch in
Game options). Restart the game after changing it. Native
is the default. A 5× target rasterizes at 1280×960; its 3D detail is composed to
the RG DS's 640×480 panel alongside native-grid 2D graphics. Stock Mali,
Weston, kernel, firmware and power policy are not replaced.

The CPU raster remains authoritative for guest-visible framebuffer capture,
VRAM writes and state. The larger GPU raster enhances the displayed image.
This costs extra work but avoids changing game logic to chase a benchmark.
The hardware rasterizer has visual approximations and the kbase driver remains
experimental. Select Native for compatibility or maximum fast-forward speed.
See [measured results](EXPERIMENTAL_BENCHMARK_2026-10-13.md).

The standard RG DS exposes Mali-G52 through `/dev/mali0`, with a kbase JM
11.47 interface. Its stock `libmali.so.1.9.0` has no Vulkan entry points. The
Chromium Vulkan loader exists at `/usr/lib/chromium/libvulkan.so.1`; a loader
alone does not provide a GPU driver.

## Reproduce the private driver

Source: [PanVK kbase fork](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/tree/efd07bab401b263a9ac114560ecdb9a2934f5d43),
pinned to `efd07bab401b263a9ac114560ecdb9a2934f5d43`. The recorded
`tools/engine_patches/panvk-kbase-linux.patch` limits Android's `-llog`
dependency to Android builds. Clang 18 is used because an upstream feature
check unconditionally supplies Clang warning flags.

`python3 tools/build_panvk.py` builds in the existing ARM64 Linux VM on macOS,
or directly on ARM64 Linux. It requires the pinned checkout at
`.cache/panvk-kbase-source`, Meson 1.7.2 in `.cache/panvk-build-venv`, Ninja,
Clang/LLVM 18 development libraries, libclc 18, the LLVM/SPIR-V translator,
SPIRV-Tools, libdrm, libudev, expat, zlib, zstd, Python Mako/YAML/packaging,
Flex and Bison. The venv uses system site packages. It never installs on a device.
The first build needs several GB of compiler dependencies and temporary objects.

The private output is `build/panvk-private`, including its exact build record
and a private zstd library/copyright notice. The driver itself is about 15 MB;
it does not require the LLVM compiler on the handheld. Device testing uses an
ICD JSON with an absolute `library_path` and process-local
`VK_ICD_FILENAMES` / `LD_LIBRARY_PATH`. `PANVK_KBASE_DVFS=none` prevents the
candidate's optional frequency-policy changes. No system ICD is registered.

On this RG DS, set **`PANVK_ATOM_STRIDE=56`** explicitly. Automatic detection
selected 64, resulting in a job configuration fault and a timeout. A 56-byte
submission rendered the original 128-polygon recording successfully. This is
a tested per-device override, not a proposed default for other Mali devices.
The private library directory is included in `LD_LIBRARY_PATH` so that the
bundled Vulkan loader and private zstd dependency can be found.

The 13 October retest completed 1×, 2×, 3× and 5× with both synthetic scenes.
The 1280×960 image was inspected and contains new subpixel detail, not enlarged
native pixels. At 5× the unoptimized isolated renderer needed 25–30 ms in that
run, already beyond a 60 FPS budget before CPU emulation and 2D composition.
Subsequent controlled shader comparisons are recorded in
[the 13 October report](EXPERIMENTAL_BENCHMARK_2026-10-13.md).
JIT-allocation and non-conformance warnings remain. Earlier connection losses
included confirmed system suspends; private diagnostic profiles now disable
idle/lid sleep without changing the user's profile. This does not establish
driver stability or explain every earlier disconnect.

## Rasterizer and scaling probe

The fork's upstream `tests/vk_probe.cpp` draws a recorded polygon list through
its actual `vk_lean.cpp` renderer. The original RGDS graphics ROMs provide light
128-polygon and heavy 1,536-polygon inputs, without commercial game assets.
Their initial changed frames can be recorded with the headless frontend's
`--dump-gpu-frame` option. Later identical frames are cached and may not dump.

`tools/engine_patches/rgds-vulkan-scale-probe.patch` applies to the portable
DSperate source. It parameterizes GPU attachments, viewport, resolve buffers,
readback and output dimensions from 1× to 5×, rejecting invalid scales and GPU
limits. `RGDS_VK_SCALE=5` is a real 1280×960 offscreen rasterization. This original
probe patch is separate from `rgds-vulkan-hires.patch`, which now implements
scaled attachments, display composition and SDL presentation for actual games.
The latter retains native CPU pixels for the game's own readbacks.

Compile the standalone probe with C++17, `DSPERATE_VULKAN=1`, Vulkan headers,
`DSPERATE_VK_SHADER_DIR` pointing to the pinned shaders, `vk_device.cpp`,
`vk_lean.cpp`, `vk_shaders.cpp`, `-ldl` and `-pthread`. Keep AA and scale explicit
in comparisons. The hardware rasterizer has upstream DS accuracy approximations;
resolution alone cannot establish texture, shadow, edge or capture correctness.

## Private full-game build

`python3 tools/build_rgds_vulkan.py --jobs 4` builds the same verified fork
source with `DSPERATE_VULKAN=ON` into `build/rgds-vulkan-game`. It preserves
the shipping build and records source, headers, recipe, options and binary
hashes. It does not install anything. The recipe also calls
`tools/build_rgds_vulkan_shaders.py` in the existing Linux VM on macOS, or
locally on Linux. That step requires `glslangValidator` and `spirv-val`, applies
`tools/engine_patches/rgds-vulkan-resolve.patch`, compiles the single-sample
resolve with `-Os`, validates SPIR-V, and records input/output/compiler hashes.
The shader skips coverage accumulation and division when there is exactly one
sample; fog and alpha calculations stay intact. Four-sample MSAA is unchanged.
Only the private build receives the patched source and generated shader.

The runtime switch is `DS_GPU3D=1`,
along with the private ICD/library environment and 56-byte job stride above.
`RGDS_VK_SCALE=2` through `5` selects scaled 3D. The supported launcher path
writes the per-game `rgds_scale` setting, selects the separate
`rgds-experimental-vulkan` binary and generates a private ICD file. Missing
enhanced binaries or drivers produce a clear launch error, not a silent native
fallback. Saves and checkpoints remain under the same experimental engine ID.

The enhanced compositor caches the 2D priority stack once per active native
scanline, then inserts each higher-resolution 3D sample with the native DS
blending, window and master-brightness kernels. Fully transparent scanlines are
checked against both rasters before reusing the native output. The panel needs
only 640×480 composition samples even when 3D rasterizes at 1280×960. The 2D-only
panel keeps its small native texture. GPU display frames are fence-synchronized;
no one-frame lag is permitted. Smooth mode resolves four 3D samples per panel
pixel using premultiplied coverage, an exact 2×2 average at 5×. Native 2D
art is not filtered by this pass. `rgds_smooth=0` selects Sharp; the runtime
passes `RGDS_3D_SMOOTH` only to the enhanced fork. `DS_VERIFY_HIRES=1` compares cached composition
against the original full layer-selection path during diagnostic runs.

After building, `python3 tools/package_rgds_vulkan.py` validates the compiled
source and binary, assembles the private libraries and complete corresponding
source, and writes `build/engines/rgds-vulkan`. `tools/package.py` requires this
validated payload. The native binary remains independently built. See the
benchmark report for on-device results and limitations.

## Acceptance

Record the device, driver/probe hashes, geometry hashes, power/thermal state,
AA, scale, elapsed render time and rendered image. Test 1× first, then larger
scales, repeated creation/destruction and real games. Check pixels, not only a
successful return code. A fast isolated rasterizer does not establish full-game
speed or stability. The Zelda drawing test is reserved for the user.


## Differential compositor test

After building the Vulkan core, compile
`src/experimental/rgds/hires_compositor_test.cpp` against
`build/rgds-vulkan-game/build/src/core/libdsperate_core.a` using that build's
`toolchain/cxx`, C++17, Cortex-A55, and `DSPERATE_JIT=1`, `DSPERATE_NEON=1`,
`DSPERATE_VULKAN=1`. Include `build/rgds-vulkan-game/source/src`. Run the ARM64
executable on the RG DS. It checks 4,096 combinations against the original
native render path and verifies that enhancement leaves native output intact.
The corresponding-source archive includes this test.
