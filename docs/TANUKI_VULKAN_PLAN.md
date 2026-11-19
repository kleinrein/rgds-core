# Tanuki3DS on Vulkan: plan and feasibility spike

Status 2026-11-19: plan written, spike run on the handheld. **Verdict: not with stock PanVK.** Vulkan
halves the CPU per draw and draws the same picture, but PanVK 26.2.5 turns every indexed draw into a
chain of five GPU jobs on Bifrost and the frame comes out 8x slower than GL. Next step is a PanVK patch
(below), not the backend. Nothing installed changed: GL stays the default.

## Why

On the RG DS the 3DS renderer is CPU-bound in the GL driver, not in the GPU. Measured in Mario Kart 7
races (see the 3DS notes, 2026-11-03/04): the renderer thread takes 47-57 % of a core and Mesa's
glthread `ctremu:gl0` another 32-50 % for about 370-430 draws a frame. Profiles of `gl0` show driver
work, not ours: `u_vbuf` re-scanning indices (14 %, patched), BO cache churn, `glTexImage` syncing
the driver thread, `glClientWaitSync` and fence syncs. After throttling (CPU at 1.4-1.6 GHz after
about 10 minutes) those two threads no longer fit in a frame and the race drops to 86-88 % of frames
drawn. A one-pixel scissor only raised the drawn share from 62 to 74 %: fragments are not the limit.

Vulkan removes the driver thread and most per-draw validation: a draw becomes a few command-buffer
words written by the renderer thread itself. The question for the spike is whether PanVK on the
Mali-G52 (Bifrost) gives that saving without losing it elsewhere (GPU time, pipeline builds,
stability).

## What we already know about PanVK on this device

- RG DS OS runs the panfrost kernel driver; the image ships upstream PanVK from Mesa 26.2.5 at
  `/usr/lib/rgds-panvk/libvulkan_panfrost.so` (`tools/build_panvk_rgds_os.py`, used by DSCore 2x-5x).
  Bifrost is not a conformant PanVK target: it needs `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
  The kbase fork in `tools/build_panvk.py` is for the stock kernel only.
- DSCore found PanVK on Bifrost reports Vulkan 1.0; core 1.1+ features must be asked for as
  extensions, and any `vkCmdPipelineBarrier` inside a frame starts a new GPU batch on the JM backend.
- The PSP session's Vulkan spike drew correctly but was not faster: the GPU's vertex-tiler time was
  about 4x GL's for the same scene (1930 vs 456 ms per second), cause unknown then. **Found here:**
  PanVK 26.x on JM (Bifrost) implements `vkCmdDrawIndexed` as an indirect draw. Each one becomes a
  write-value job, a compute job that searches the index range over the bound buffer to its end
  (`index_count = buffer_size / index_size`), an indirect-helper compute job with a prefetch barrier,
  then the vertex and tiler jobs, all chained (`panvk_cmd_draw_indirect` in
  `src/panfrost/vulkan/jm/panvk_vX_cmd_draw.c`). `is_indirect_draw()` counts every indexed draw.
  PanVK 25.0.7 still scanned the indices on the CPU and drew directly. `vkCmdBindIndexBuffer2`
  (exact size) exists but `VK_KHR_maintenance5` is only exposed on v10+.
- Job timeouts ("Job has been on the GPU for too long") were seen with DSCore's PanVK 3D; the runtime
  watches the kernel log for them.

## How the GL renderer maps onto Vulkan

The renderer stays one thread (the GPU worker in `gpu.c`), fed by the main thread exactly as now. A
new `src/video/renderer_vk.c` (plus `vk_glsl.c`, `vk_mem.c`) implements the same entry points that
`gpu.c` calls on `renderer_gl.c`: `gpu_*_draw`, `display_transfer`, `texture_copy`, `clear_fb`,
`render_lcd_fb`, `start_frame`, `present_finish`, `fbs_capture`, `state_loaded`. A small table of
function pointers chosen at start (`RGDS_3DS_RENDERER=vulkan`, default `gl`) picks the backend, so
`gpu.c` changes only at the call sites. `FBInfo`/`TexInfo` keep their `u32` handles, which become
indexes into the backend's own tables (VkImage, view, memory, layout state). The GL code is not
touched beyond that table.

### Shaders

The generators stay as they are (`shaderdec.c` for hardware vertex shaders, `shadergen_fs.c` for
fragment configs, `hostshaders/gpu.vert` for the software path). `vk_glsl.c` (written for the spike,
`tools/tanuki3ds/vkspike/vk_glsl.c`) rewrites their GLSL 330 into Vulkan GLSL 450 by text:

- varyings get fixed locations by name (`color`=0 ... `view`=7), the fragment output location 0;
- samplers and uniform blocks get `set=0, binding=N` from one table (`VertUniforms` 0, `FragConfig`
  1, `FragUniforms` 2, `FreecamUniforms` 3, `tex0..tex3`, `tex0shadow`, LUTs 9-12);
- the loose uniforms `depthOffset`, `depthScale`, `depthWBuffer` become one push-constant block;
- `main` is wrapped so GL's -1..1 clip depth (the generators map to it) is folded back to 0..1.

Images keep GL's memory layout: the viewport and scissor take Tanuki's GL numbers unchanged (positive
viewport, row 0 = GL's bottom row), `gl_FragCoord` comes out identical, and GL's counter-clockwise
front face becomes `VK_FRONT_FACE_CLOCKWISE`. Framebuffer copies, display transfers and texture
uploads therefore need no flips. On the host this gives a bit-identical picture against GL (below).

GLSL to SPIR-V runs with glslang linked statically (`glslang-dev` in the VM, about 4 MB of code) on
the existing program thread `ctremu:prog`. SPIR-V is cached on disk next to the game's
`shaders/<rom>.list` (keyed by the source text hash), so the list's warm step at boot produces
pipelines without glslang after the first run.

### Pipelines

A GL draw sets state freely; Vulkan bakes most of it into a pipeline. Key:

| Part | Source in `gpu_gl_draw` |
| --- | --- |
| vertex + fragment shader | `ProgSrc` pair (text-shared as today) |
| vertex input | `gpu_vbo_layout` per buffer (binding stride, attribute formats/offsets) |
| topology | `prim_mode` |
| cull, front face | `raster.cullmode` |
| depth test/func/write, stencil func/ops/masks | `fb.color_mask`, `fb.stencil_*` |
| blend equation/factors, colour write mask | `fb.blend_func`, `fb.color_mask` |
| logic op | only when not COPY |
| render pass formats | colour RGBA8, depth D24S8 |

Viewport, scissor, blend constants, stencil reference/masks are dynamic. If PanVK exposes
`VK_EXT_extended_dynamic_state` (2/3) the cull mode, depth and stencil state, and with
`VK_EXT_vertex_input_dynamic_state` the vertex layout also move out of the key (the spike reports
which exist). Blend state stays in the key: on Mali, unusual blend modes and logic ops compile into
blend shaders. Tanuki's GL path enables `GL_COLOR_LOGIC_OP` with `GL_COPY` whenever blending is
off; Vulkan maps COPY to "no blending, no logic op", otherwise it would run a blend shader for every
opaque draw.

Pipelines live in a hash table keyed by a 64-bit hash of the key; a miss queues a build on the program
thread, and the draw waits or uses the uber fragment shader of the same vertex shader, exactly like
`prog_request`/`prog_wait` and the `g_pb.nhot <= 2` rule today. `shaders/<rom>.list` gains pipeline
records (state key + pair) so the warm step builds the pipelines, not only the programs. A
`VkPipelineCache` per game is saved on exit and on idle (`pipelines/<rom>.vkcache`), and Mesa's own
disk cache (`MESA_SHADER_CACHE_DIR`, as the GL path) sits under it. A Mesa or engine change empties
it; the same "Preparing graphics" card covers the rebuild.

### Descriptors and uniforms

One descriptor set layout for every pipeline (13 bindings): three dynamic uniform buffers
(`VertUniforms`, `FragConfig`, `FragUniforms`), the freecam block, and nine combined image samplers.
One pipeline layout, so binding a new pipeline never disturbs the set. Uniform data goes into the
frame's ring (host-visible, coherent, mapped once, like `g_stream`), and the per-draw cost is the
dynamic offsets. Sets are cached by their texture/sampler tuple and written once; with
`VK_KHR_push_descriptor` the textures could be pushed per draw instead. The light, fog and proctex
LUTs keep their hash caches; each entry becomes a small image. GL leaves the fog/proctex textures
with a nearest minification filter (their `MIN_FILTER` is set on the wrong target), which the
Vulkan samplers must copy to keep the picture the same.

### Vertex and index data

The main thread keeps writing each draw's vertices and indices into a ring at submit
(`gpu_gl_vstream_put`, laid out by `gpu_vbo_layout`); the ring becomes a VkBuffer in host-visible
coherent memory, mapped persistently. A draw binds its buffers with one `vkCmdBindVertexBuffers`
and the index buffer at its offset. Panfrost's `u_vbuf` index scan has no Vulkan equivalent: PanVK
takes the index range from the draw, so the `glDrawRangeElements` Mesa patch is not needed there.

### Textures

`create_texture` makes a VkImage (RGBA8/RGB565/... as today; ETC1 as
`VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK` when `textureCompressionETC2` is there, with the same block
flip as the GL path). Data goes through a staging ring; `tex_upload_tiles` turns its runs into the
regions of one `vkCmdCopyBufferToImage` (one call per texture per diff instead of one GL call per
run).

The hard part: Vulkan forbids copies inside a render pass, and a tiler pays for ending one (tile
store and reload). Rules:
- an upload to a texture that the open pass has not sampled yet goes into a separate "upload" command
  buffer submitted before the pass's own (no split);
- an upload to a texture the open pass already sampled gets a new image version (copy-on-write: a
  fresh VkImage from a small free list, old one retired when its frame's fence passes). This is the
  "new texture name on a helper context" idea from the GL notes, which GL could not do safely.
- only when neither works (a render-to-texture read back in the same pass) is the pass split.

### Framebuffers, transfers and copies

`FBInfo` becomes a colour image (RGBA8) and a depth/stencil image (D24S8, fallback D32S8) with a
framebuffer object per pair. Draws to the same FB accumulate in one render pass; the pass ends on FB
switch, display transfer, texture copy from that FB, clear by a different path, or readback. Clears
at the start of a pass are load-op clears; mid-pass clears use `vkCmdClearAttachments`. Display
transfers and FB-to-texture copies are `vkCmdBlitImage`/`vkCmdCopyImage` between passes. The shadow
map mode (frag mode 3) and the "depth buffer bound as colour" hack keep their GL logic with explicit
image barriers. Save states and `fbs_capture` copy FB colours to a host buffer and wait on a fence,
as the GL path's `glReadPixels` does.

### Present

The panels are two KMS displays opened by SDL3's KMSDRM backend; the present thread draws both with
GL on a shared context (bridge menu, HUD, bezels, filters, preparing card). Options:

1. **dma-buf hand-off to the present thread (chosen).** The renderer's screen images are allocated
   exportable (`VK_EXT_external_memory_dma_buf`, linear or a modifier GL can import), imported once
   each into the present context as EGLImage textures (`EGL_EXT_image_dma_buf_import`), and the
   present sets keep working as today. Sync: a sync-file exported from the frame's semaphore
   (`VK_KHR_external_semaphore_fd`, `SYNC_FD`) and waited on with `EGL_ANDROID_native_fence_sync` +
   `glWaitSync`, or the implicit sync panfrost attaches to the BO. Everything around the panels stays
   GL and untouched. The spike tests the import (`--dmabuf`).
2. Vulkan WSI on the panels: `VK_KHR_display` needs the display controller's DRM node; panfrost is a
   render-only node (the panels belong to the Rockchip VOP), so PanVK has no displays to offer, and
   SDL3 is built without Vulkan. Not possible without a new WSI path.
3. Direct KMS (as DSCore's DrmOut): add the dma-buf as a KMS framebuffer and page-flip it. Fastest,
   but loses the GL compositing of the menu, HUD, bezels and shaders. Only worth it later, for the
   "no filter, no overlay" case.

### Threads and the frame

The renderer thread records into a command buffer per frame slot (2-3 in flight, fence per slot) and
submits once per guest frame or FB change, plus the upload command buffer. There is no driver thread,
so the CPU affinity plan simplifies: main thread, renderer thread, present thread, program thread.
The present-set wait (`pset_pick`, `SPIKE_PSETWAIT`) becomes a fence or semaphore wait on the exact
frame, which removes the blinking trade-off seen with the GL path.

## Milestones

1. **Spike (this document).** Device, one GLSL->SPIR-V pipeline from Tanuki's shaders, an offscreen
   3DS-sized frame, per-draw CPU cost against GL, GPU engine times, dma-buf import. Done on the
   handheld; found the indexed-draw cost.
1b. **PanVK patch: direct indexed draws with a CPU index range.** Gate for everything below.
2. Backend table in `gpu.c` + `renderer_vk.c` drawing NSMB2's map offscreen, compared with the GL
   picture frame by frame (`SPIKE_DUMP_AT` per-draw dump on both backends).
3. Textures with diffs, LUT caches, ETC; framebuffers, transfers, copies; save state capture.
4. Present through dma-buf; bridge, HUD and preparing card unchanged. Opt-in by
   `RGDS_3DS_RENDERER=vulkan` in run.sh (or a hidden play.ini key), GL default.
5. Pipeline list + VkPipelineCache warm step; uber fallback.
6. MK7 race A/B against GL on the userdata copy (`race-run.sh`), 10-minute throttled run, NSMB2,
   3D Land, Smash; owner hand test; only then a Graphics-page choice.

Estimate: milestones 2-4 are the bulk (the GL renderer is 3,350 lines); the shader and present parts
are mostly decided by the spike.

## Spike

`tools/tanuki3ds/vkspike/`: `vk_spike.c` draws the same synthetic frame through GL (EGL surfaceless,
desktop GL 3.3 core, Tanuki's call pattern: persistent stream buffer, `glBindBufferRange`, eight
`glVertexAttribPointer` and `glDrawRangeElementsBaseVertex` per draw, logic op COPY when blending is
off, glthread on) and through Vulkan (the ICD loaded directly, no loader). The frame: 400 draws of
128 triangles each (about 45x45 pixels) into a 240x400 RGBA8 + D24S8 target, 40 programs from Tanuki's
fragment generator (or a game's `shaders/<rom>.list`) with `gpu.vert`, 4 state variants, 3 textures
per draw from 24, the LUTs, 4 sub-texture updates a frame, two frames in flight. It reports build
times, per-frame wall time, the render thread's and the whole process's CPU time, GPU engine times
from panfrost's fdinfo, compares the two pictures and tries the dma-buf import.

- `build.sh` (from the Mac) builds it in the Lima VM against the fork's generators.
- `device.sh` copies it to `/data/rgds/vkspike` and runs the measurement set. It refuses while a game
  runs or a `/run/rgds/*-hold` exists, takes no panels and reads no game data;
  `SHADER_LIST=/path/to/shaders/<rom>.list` adds a game's real programs to the build timings.

### Host results (Lima VM, llvmpipe GL vs lavapipe Vulkan, 200 draws)

- Every generated fragment shader and `gpu.vert` convert and compile; GL and Vulkan pictures are
  **identical** (0 values differ), so the GLSL rewrite and the coordinate conventions hold.
- Render-thread CPU per frame: GL 2.10 ms vs Vulkan 0.24 ms (recording 0.22 ms, submit 0.002 ms).
  Process CPU is dominated by the software rasterizers there, so it says nothing about the Mali.
- Build: 40 pipelines for 20 programs in 57 ms (27 ms of it glslang), 28 ms from a warm in-process
  cache.

### Device results (RG DS, 2026-11-19)

Mali-G52 r1 MC1, PanVK from the image (Mesa 26.2.5, `/usr/lib/rgds-panvk`), GL through the engine's
own Mesa 25.2.8 (`lib2n`, glthread on). Performance mode: CPU 1992 MHz, GPU 800 MHz, 20-38 C. GPU
engine times read with panfrost's `profiling` knob on (restored to 0 afterwards). 400 draws a frame
unless noted, 60-300 measured frames each.

| | GL (glthread) | GL (no glthread) | Vulkan (PanVK 26.2.5) |
| --- | --- | --- | --- |
| frame wall time | 10.7-11.2 ms | 13.7 ms | **84 ms** |
| render thread CPU / frame | 4.8-5.5 ms (+ `gl0`) | 13.0 ms | 7.7 ms |
| process CPU / frame | 13.2-13.7 ms | 13.0 ms | 6.9-7.7 ms |
| CPU per draw (process) | 33-34 us | 33 us | **17-19 us** |
| GPU fragment / frame | 11.8-12.8 ms | 13.0 ms | 13.9 ms |
| GPU vertex-tiler / frame | 6.4-6.7 ms | 6.2 ms | **153-157 ms** |
| 1-pixel scissor: wall, CPU/draw, vertex-tiler | 10.1 ms, 32 us, 4.4 ms | 12.6 ms, 31 us, 4.4 ms | 80 ms, 19 us, 149 ms |
| 800 draws: wall, CPU/draw | 19.7 ms, 33 us | | 169 ms, 18.5 us |

- **Pictures**: Vulkan on the Mali matches GL (largest channel difference 1 of 255). No kernel faults
  with PanVK 26.2.5.
- **The vertex-tiler cost is the indexed draw path** (above). Bisected with the spike:
  - exact-size index buffers per draw (`--ibuf exact`, default): no change (153 ms), so it is not
    only the over-long index scan;
  - the same triangles without indices (`--nonindexed`, `vkCmdDraw`, the direct path): vertex-tiler
    20 ms for 4.7x the vertices (6.98 ms with the 1-pixel scissor, about GL's 4.4 ms for that
    vertex count), so the direct path is fine;
  - PanVK 25.0.7 (CPU index scan, direct draws; the VM's old build, copied to /data only): GPU work
    was low (fragment 5 ms, vertex-tiler 10 ms) but it faulted (`JOB_BUS_FAULT` at VA 0, 500 ms
    job timeouts) on this kernel, so it is no answer as it stands.
- **CPU**: Vulkan's 17-19 us per draw includes the vertex/index/uniform copies the GL path makes too
  (about 2 ms a frame here), so the driver part is roughly 13 us against GL's 26 us, and it runs on
  the render thread alone: no `gl0`. This is still the indexed (indirect) recording; a direct path
  records less.
- **Builds, 40 real Mario Kart 7 programs** from `shaders/mk7.list` (34 vertex, 402 fragment sources
  in the list; every one converts and compiles): cold (Mesa cache off) GL 10.1 s, Vulkan 8.5 s, of
  which glslang 1.7 s. With Mesa's disk cache warm: GL 0.5 s, Vulkan 1.6 s, of which glslang 1.5 s
  (about 14 ms per shader on the A55). So the SPIR-V must be cached on disk next to the list.
  Pipelines from a warm `VkPipelineCache`: 1.0 ms for 80. A new blend/depth variant of a known
  shader pair costs nothing measurable (0.2 ms for up to 40).
- **dma-buf hand-off**: a linear PanVK image exported as a dma-buf and imported into the engine's GL
  through EGL: 0 of 96,000 pixels differ, framebuffer complete, 10.8 ms for the first import (one
  time per image). The present plan stands.
- **Driver facts**: Vulkan 1.0.354, `logicOp`, `dualSrcBlend`, `depthClamp`, `independentBlend` yes,
  `shaderClipDistance` no; 4 descriptor sets, 16 dynamic uniform buffers, 256 bytes of push constants;
  no timestamp queries (`timestampComputeAndGraphics` 0); dynamic rendering, extended dynamic state
  1/2/3, push descriptors, graphics pipeline library, depth clip control, custom border colour yes;
  `VK_EXT_shader_object`, `VK_KHR_display` no. D24S8 is a valid attachment. Memory: one heap, a
  device-local host-visible coherent type (write-combined) and a host-cached type.

## Recommendation

**Do not start the backend on PanVK as shipped.** The CPU side delivers what was hoped (about half
the CPU per draw, and no driver thread), and the shader rewrite, pictures and dma-buf present all
work. But every indexed draw costs about 370 us of GPU job-chain time on Bifrost, and 3DS games draw
almost everything indexed: an MK7 race frame would take far longer than GL's.

Next step, about a day, and decisive: **a PanVK patch** in our own build
(`tools/build_panvk_rgds_os.py`, applied like the DSCore patches) that gives `vkCmdDrawIndexed` a
direct path again when the index buffer is host-mapped. Scan the indices on the CPU (as 25.0.7 and
the GL driver do; Tanuki's main thread already knows each draw's min/max), set `vertex_range` and
`raw_offset`, point the tiler job at the indices, and skip the min/max and indirect-helper jobs.
`is_indirect_draw()` must stop treating indexed draws as indirect. Then rerun the spike: it needs
vertex-tiler near GL's 6.5 ms and frame wall at or under GL's 11 ms, with no kernel faults, through
a 10-minute run. If that holds, build milestones 2-6 (weeks: the GL renderer is 3,350 lines); expected
gain in an MK7 race is the `gl0` thread's 0.3-0.5 of a core plus part of the renderer thread, which
is what is lost to throttling now. If the patch cannot be made stable, stay on GL and spend the time
on the GL-side ideas in the 3DS notes (fewer calls per draw, uploads off the draw path).

Housekeeping: the spike lives in `/data/rgds/vkspike` (128 MB, includes the old PanVK 25.0.7 used for
the bisect); it writes nowhere else, and its Mesa cache is `/data/rgds/vkspike/mesa-cache`. Remove
the folder when done.
