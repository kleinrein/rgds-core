# Emulator and shader rebuild — 2026-10-08

This pass follows the report that DSperate launches while DraStic/NNDDSS fail and shaders do not work. All work and validation are on the development host. No device connection, emulator execution on hardware, deployment or filesystem repair was performed.

## Source build

`make build-engines` builds DSperate 3.0.0 at upstream commit `1b76c355109c9f7576363ccc023927b3137d3c6f`. Zig 0.13/Clang 18 targets Linux AArch64 with glibc 2.17, `-O3`, ThinLTO, Cortex-A55 tuning, JIT and NEON. Network and RetroAchievements support stay enabled. PGO remains off because the upstream GCC profile does not match this compiler.

The recorded portability patch corrects a callback const cast, NEON macro initializer arguments and missing libslirp endian macros. The original source checkout stays intact. Corresponding patched source, license, patch, build driver, compiler/SDK hashes and build options accompany the binary. The resulting stripped executable is 5,501,824 bytes with SHA-256 `c683fc864d0e7939e59c4b2bc413917ac5c744c23a504e4204c743fb61fdcd06`. Repeated cached builds produce the same engine bytes. Its 74 imported SDL symbols match those of the supplied engine; this checks the required interface, not hardware behavior.

Official SDL2 2.32.10 is source-built as a cross-link SDK. Its dummy-backend shared library is excluded from the package. The device continues using its existing SDL2. The source engine has no RPATH/RUNPATH or libstdc++ dependency. DraStic remains the proprietary stock binary; NNDDSS core/loader/shims remain supplied binaries with unavailable source/build records. Their performance has not been represented as source-rebuilt.

## Launch and storage fixes

- Reject ARM32 DraStic candidates for the AArch64 bridge. Missing DSperate cannot accidentally select NNDDSS.
- Keep the bridge and dynamic loader on the same selected SDL library. Preserve known vendor 64-bit dependency paths after clearing inherited compatibility hooks.
- Refresh NNDDSS executable/libraries inside its writable runtime root and resolve libraries there. Populate needed User directories, firmware aliases, private BIOS, fonts and available game database assets. Preserve saves and existing settings.
- Drain emulator stdout/stderr continuously into a bounded 128 KiB RAM tail. Write `userdata/logs/<core>.log` atomically when the engine exits, retaining exit/startup errors and counting discarded earlier output. This removes synchronous card log writes during gameplay.

These corrections address concrete adapter defects. The user's current startup failures have not been reproduced against a fresh device log in this pass.

## Native shader pipeline

The supplied `.dsd` fragments omit the GLES float precision declaration expected from their original engine. Submitting their raw text fails a real GLES2 compiler. The new pipeline supplies matching vertex/fragment precision, records compiler/linker errors and caches programs and uniform locations per context.

DraStic and DSperate filter native screen textures through their SDL copy path. NNDDSS substitutes its native screen-blit program while preserving its texture/vertex geometry. Effects use actual source size and per-screen destination size, including fractional scaling. Programs, GL state and texture filtering restore before engine/menu/HUD drawing. Duplicate startup effects are disabled. This replaces the scaled-framebuffer postprocess and removes its full-frame copy/readback. All three pickers expose Off plus the 14 presets.

Host SwiftShader EGL/GLES2 tests exercise the compiled C++ bridge and actual pixels through all three renderer routes. They check all preset compilation/linking, visible effects, sharp fractional scaling, logical scale/letterboxing, clipping, rotation, Off, original state restoration and capability failures. This provides host rendering evidence; Mali driver behavior and physical panel output remain for the later device session.

## Build and package integrity

Launcher/bridge builds use `-O3`, LTO and Cortex-A55 tuning. A linker export map exposes required interposed APIs and keeps private C++ runtime symbols local. ELF checks require the audio/input/render hooks and record actual dependencies/ABI.

Both outputs compile before publication. A failed compile, source edit during build or publication rename failure preserves the previous pair. Packaging rejects stale sources, mismatched executable hashes and changed copied build records. DSperate source/patch hashes are checked and the supplied DSperate executable is excluded. Corresponding source is verified during full installation verification and omitted from per-game hashing.

Host regression tests, launcher checks, cross-build and archive validation are recorded in `build/engine-rebuild-validation.json`. Hardware game FPS, audio/touch, save restoration, thermal behavior and power use remain unmeasured. Follow `NEXT_DEVICE_TEST.md` when device testing is authorized later.
