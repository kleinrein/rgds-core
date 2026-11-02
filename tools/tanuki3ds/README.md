# Tanuki3DS for the RG DS

The Nintendo 3DS engine: our fork [kleinrein/Tanuki3DS-rgds](https://github.com/kleinrein/Tanuki3DS-rgds), branch
`rgds` (GPL-3.0), on [Tanuki3DS](https://github.com/burhanr13/Tanuki3DS) commit `879a1e7`, with SDL 3.2.24 and a
patched Mesa 25.2.8. It lives on the data partition, in `/data/rgds/engines/tanuki3ds`, because it needs about 380 MB
and the OS image has no room for it.

Still a spike. The fork mixes real changes with test hooks (the `SPIKE_*` variables, which the launch plan also uses
as settings):

- the renderer on its own thread, with command lists and vertex data copied when the game submits them;
- inline guest memory in the ARM JIT (`-DJIT_FASTMEM`);
- light tables cached by content, whole-tile updates of ETC1 atlases, per-draw GL state cached;
- JIT blocks that save nothing: `jit_enter` keeps the cpu, guest memory and the exit address in x27-x29 while guest code runs;
- Mesa's glthread on, for a third core (`map_unsynchronized_thread_safe` forced in Panfrost);
- the sound device opened at the mixer's 48 kHz, and a soft limiter instead of a hard clamp;
- a clean exit that skips teardown after flushing stdio (the teardown crashed under the renderer thread);
- each 3DS screen on its own panel, the RG DS's buttons and touchscreen read from evdev;
- a steady 60 Hz clock, with the sound queue held by a small resampling ratio;
- shader programs built on a helper thread in a shared context; a draw that needs an unbuilt one waits for it, because skipping such draws made backgrounds disappear;
- all JIT blocks side by side in one mapping, instead of a 16 KB mapping per block;
- RG DS Core's shared game menu without the SDL2 bridge (`src/rgds_bridge.c`): the files beside `RGDS_CONTROL_FILE`, the frame in `RGDS_OVERLAY_FILE`, the game held while `.blocked` exists;
- whole-machine save states (`src/savestate.c`, zstd, about 20 MB in Mario Kart 7): kernel objects in an arena at a fixed address (`src/kernel/karena.c`) and a binary that is not position independent, so pointers are stored as they are. A state only loads in the exact binary that wrote it; the game's own save data is separate and unaffected;
- the Graphics page's choices live: screen fit, second screen, RG DS Core's looks (`.dsd` presets adapted to GL 3.3 and fed the screen upright, as the 3DS screens lie on their side in their textures), and the 3DS lids as bezels (`rgds_bezel_draw`); the volume bar, fast-forward badge and FPS counter (`src/rgds_hud.cpp`, with copies of `src/platform/` headers);
- saves and loads on their own thread, so the menu's spinner goes on;
- a present only when the game has made a new picture (`SPIKE_PRESENT_ALL=1` presents every refresh), and textures rebuilt only when their bytes changed or more mip levels are asked for;
- ETC1 textures sent to the GPU as ETC2 blocks without decoding, each block flipped top to bottom (one step off in the about 2.6% of blocks that have no exact flip, `SPIKE_NOETC=1` decodes as before), and a tile table for the other formats' pixel order;
- frames skipped only when the renderer is 3 refreshes behind (`SPIKE_SKIP` in `run.sh`): at 2, a 30-picture game whose frame takes just over 33 ms lost every other picture, and 3D Land's levels showed 16-20 a second;
- lists per game in the game's home, `shaders/<rom>.list` (built ahead from the start of a run) and `jit/<rom>.list` (compiled ahead in spare frame time, once code with the same hash is in memory). Bump `PROGLIST_VERSION` in `renderer_gl.c` when the shader generators change.

Build in the Lima VM (`rgds-image`), under `~/3ds-spike`:

    sh build-tanuki.sh   # SDL3 and the fork's rgds branch (TANUKI_REF=<branch or commit> for another, TANUKI_KEEP=1 to build the clone as it is)
    sh build-mesa.sh     # Mesa with Panfrost's buffer cache patched
    sh bundle-tanuki.sh  # tanuki-bundle.tar

Settings the handheld needs are in `ctremu.ini`, copied to the game's home on first launch.

The engine folder holds Mesa twice: `lib2` is the patched install from `build-mesa.sh`, with the system `libLLVM.so.19.1` beside it, and `lib3/libgallium-25.2.8.so` comes from the same build with the glthread change. They are one library in practice; `lib3` only exists so `NO_GLTHREAD=1` in `run.sh` can compare without it.
