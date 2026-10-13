# Emulator options — updated 2026-10-13

The development application has five selectable adapters: DraStic, DSperate, NNDDSS,
melonDS and RG DS Experimental. DSperate is the default. melonDS is a native Linux core with an RGDS SDL
frontend; it is not Android-only. Its source build and synthetic ARM64 checks
are recorded in [the integration report](archive/MELONDS_INTEGRATION_2026-10-11.md).
The [device review](archive/DEVICE_REVIEW_2026-10-11.md) records initial four-core smoke tests and substantial melonDS slowdown. Full physical acceptance is pending. RG DS Experimental now has on-device native and enhanced-resolution results; see the section below.

**SuperDrastic** is now bundled as an [optional experimental DraStic backend](SUPERDRASTIC.md),
with a pinned source build and native menu/display path. Standard DraStic remains
the default backend. It is an enhancement to DraStic, not a fifth core. Native startup, save/exit and Standard↔SuperDrastic checkpoint loading passed device smoke tests. This establishes no overall speed advantage. The earlier
[pinned review](archive/DRASTIC_ADDONS_2026-10-11.md) records the compatibility assessment.
Advanced DraStic remains unbundled pending source/license clarification.

## Standard DraStic screen order

For swapped panels, select the game, open **Game options → Screen order**, and
choose **Normal** or **Swapped**. This applies only to Standard DraStic and is
saved per game. The same option is in its in-game Emulator settings; use
**Save & restart** to apply it. DSperate remains the default emulator.

RGDS pins this choice in the main native config and an existing per-game
config, so an imported setting cannot override it. Native swap shortcuts are
disabled in both binding banks. Local builds/tests cover configuration and UI;
physical panel/touch validation and deployment are pending. See the
[screen-order follow-up](archive/GAMEPLAY_REVIEW_2026-10-11.md#standard-startup-follow-up).

## RG DS Experimental

Choose **Game options → Emulator → RG DS Experimental**. This is a
separate, source-built DSperate fork with independent saves and checkpoints. It
uses Cortex-A55 code generation, ARM64 JIT, NEON and LTO, omits network and
achievement integration, and fixes native SDL stylus contact ownership and edge
coordinates. See the [build and acceptance notes](../src/experimental/rgds/README.md).
The fork includes hidden-raster skipping during fast-forward and a fix for
emulated lid-resume scanline corruption. **In-game menu → Graphics** groups
3D resolution (Native through 5×), edge smoothing, screen filters and frameskip.
It shows the active renderer. Resolution, smoothing and frameskip now apply live
without restarting the game; Save & restart remains available as a separate action.
**DS controls → DS nickname** changes the console name, defaulting to RG DS Core,
on a fresh game boot. Existing game profiles and save states may retain names
that the game already stored.
The pre-launch **Game options → 3D resolution** picker is also available. Native is
the default. The tested NSMB scene reached about 60 FPS at 2×/3× and 57–59 FPS
at 5×, with fast-forward around 135/121/105 FPS respectively. These are scene
measurements, not universal performance claims. See the
[13 October benchmark and verification report](EXPERIMENTAL_BENCHMARK_2026-10-13.md)
and [enhanced-renderer documentation](RGDS_VULKAN.md). Zelda drawing remains
reserved for physical testing by the user.

NooDS is a useful source reference for renderer scheduling and touchscreen ADC
calibration. The locally pinned DSperate already has persistent band workers and
work sharing, so replacing that with NooDS scheduling needs measured evidence.
melonDS remains a compatibility reference. NNDDSS can be compared as a binary;
this checkout has no verified corresponding core source to import. No code from
those projects has been copied into this fork.

## Further candidates

NooDS and SkyEmu now have pinned source builds and successful headless probes
on the standard RG DS. These experiments are separate from the four installed
adapters. Their [builds, actual results and remaining checks](archive/CORE_CANDIDATES_2026-10-11.md)
include original-ROM CPU/input/checkpoint tests and the limits of headless
performance measurements. Neither has been added to the launcher or made default.

| Upstream | Evidence reviewed | RGDS decision |
| --- | --- | --- |
| [NooDS](https://github.com/Hydr8gon/NooDS) | GPL-3.0-or-later; portable interpreter with threaded software rendering. Pinned lightweight core probe passed on RG DS. | Continue controlled gameplay and frontend integration experiments. The measured 64.202 FPS initial New Super Mario Bros sequence excludes physical display/audio and is not gameplay acceptance. |
| [SkyEmu](https://github.com/skylersaleh/SkyEmu) | MIT core plus BSD-2-Clause replacement BIOS; upstream labels DS emulation beta. Pinned core-only ARM64 build passed 193 synthetic frame/input/checkpoint callbacks on RG DS. | Promising compatibility experiment. Physical output, touch, battery saves, menus and representative gameplay remain required before an installed option. |
| [DeSmuME](https://github.com/TASEmulators/desmume) | POSIX build implements ARM64 JIT; project/ARM JIT files declare GPL-2.0-or-later. The separate interface build still limits JIT to x86. | Next independent JIT candidate. Build its correct ARM64 target and verify generated code plus same-scene performance before an adapter; no speed result is established. |
| [DSVita](https://github.com/Grarak/DSVita) | GPL-3.0; newer source includes an AArch64 JIT despite the ARM32-focused README. Upstream documents unresolved A64 2D rendering and timing defects. | Track its ARM64 work, with no production adoption or speed claim while those defects remain. |
| [Dust](https://github.com/kelpsyberry/dust) | GPL-3.0 Rust DS emulator; ARM64 builds exist, but the reviewed JIT implementation is a TODO. | Compatibility/reference candidate, not an available JIT acceleration path. |
| [melonDS DS](https://github.com/JesseTG/melonds-ds) | Libretro frontend for melonDS, targeting parity and usability. | Shares the underlying emulator family already integrated. Adding a libretro frontend is a separate architecture choice, not automatically another faster core. |

The [SimpleOS NNDDSS fork](https://github.com/TiagosMs/SimpleOS-NNDDSS-support)
documents bundled NNDDSS and stock switching. This is useful integration context;
its README does not establish a source-built replacement for our closed NNDDSS
payload or prove a performance fix. No upstream installer was executed.

## Performance policy

Adding menu entries is not acceptance. A new engine needs a reproducible pinned
build, provenance, correct two-screen output and touch, controls/audio, normal
saves, checkpoints, pause/resume, shutdown and repeatable device timing. Compare
the same games, power profile, native resolution and shader setting. Preserve
save formats and timing correctness while optimizing.

NNDDSS remains a supplied binary core/loader with incomplete source provenance.
We can optimize our launch, input, menu and renderer integration, but cannot
claim core recompilation or arbitrary JIT improvements. Unsafe live upload
patching remains disabled. See [the current performance pass](archive/LAUNCH_MENU_POLISH_2026-10-11.md)
and [the hardware checklist](NEXT_DEVICE_TEST.md).
