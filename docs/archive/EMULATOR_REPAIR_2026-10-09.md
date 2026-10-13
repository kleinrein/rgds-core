# Emulator input, independence and frame counts — 2026-10-09

The user's New Super Mario Bros report was reproduced on RG DS at 192.168.1.115.
The active card installation was `0.0.1-dev-9a9278b47f60` under `/mnt/mmc/rgds`,
not the older internal test installation. Its DraStic log recorded signal 11.

## Changes

DSperate no longer searches stock BIOS/firmware or vendor/ports library folders.
Explicit empty BIOS/firmware CLI values override stale native INI paths. Its
pinned, source-built engine uses embedded FreeBIOS and generated firmware for DS
direct boot. Linux, SDL2, the graphics driver and device services remain OS
requirements; this is engine independence, not a complete replacement OS or a
claim that DSi firmware boot works without dumps. Existing saves are preserved.

DSperate now reads the Goodix panel with RGDS's calibrated, non-grabbing touch
reader. It emits lower-panel SDL finger events, suppresses duplicate SDL touch
copies while that reader is available, releases a held stylus when entering the
menu, and drains menu gestures without replaying them into gameplay.

DraStic's vendor touch shim caches the bottom-screen rectangle in SDL_RenderCopy.
RGDS had bypassed that function while still calling the shim's SDL_PollEvent.
The latter dereferenced the uninitialized rectangle on finger input. Three
injected Goodix gestures reproduced exit -11 in an isolated candidate session;
unattended playback of a copied manual checkpoint had exited normally. RGDS now
preserves the adapter's draw/state update before applying an optional shader.
This retains one base draw before the shader pass when filtering is enabled.

NNDDSS's HUD now says GAME and measures its emulated-frame counter rather than
SDL swap calls. The pinned loader does not export this counter dynamically;
the bridge verifies its exact accessor instructions and readable ELF load
segment bounds before reading it. Unknown layouts fall back to DISP. Sampling
uses monotonic time, excludes the shared menu and handles 32-bit counter wrap.
Logs retain presentation histograms and add an explicit game_fps field.

## Measurements and limits

Separate test homes, muted audio, native resolution, zero frameskip and disabled
automatic saving protected the user's original progress. Tests that loaded
existing checkpoints used copies. No operating-system or firmware flashing was
performed.

With a copied NNDDSS gameplay checkpoint, shader preset 9 measured 59.46 emulated
frames/s over the sampling interval while its final presentation sample reported
86.2 FPS. Shader Off measured 59.71 emulated frames/s and 85.8 display FPS. These
runs demonstrate the misleading presentation metric. They did not reproduce the
reported extreme slowdown, and do not establish sustained or interactive
performance. No speculative fast-upload or CPU-affinity changes were made.

The first candidate booted New Super Mario Bros in DSperate with explicit logs
confirming built-in FreeBIOS and generated firmware, received the synthetic
finger events, presented about 60 FPS and completed three menu/resume cycles.
NNDDSS's new counter reported 59.97 game FPS in its equivalent short test.
Physical stylus accuracy and longer gameplay still need user confirmation.

Host tests cover stale stock paths, touch down/move/up and menu cancellation,
the vendor render callback, actual-frame cadence, pause exclusion and wrap.
Raw evidence and reproduction scripts are under build/emulator-repair-20261009.

## Fixed candidate

Build `0.0.1-dev-a1eab2fe6582` survived all three previously crashing touch
gestures in DraStic with shader preset 9, followed by three acknowledged
menu/resume cycles and a normal quit (exit zero). It averaged 59.82 display FPS
over 23.14 seconds. The post-fix focused suite passed 54 tests; the earlier
expanded suite passed 67. These overlap and are not added into a unique total.

The same fixed DraStic build also survived the gestures and three menu/resume
cycles with shaders Off, averaging 60.04 display FPS and exiting normally.
The final ZIP verified all 154 manifest entries; archive SHA-256 is
`2fce1da160adef3cacb7a6132b7fba6573a04d46bbcf03bfc043524a2947e350`.

Deployment completed to `/mnt/mmc/rgds`. Active release is
`0.0.1-dev-a1eab2fe6582`; previous remains `0.0.1-dev-9a9278b47f60`.
The live launcher process was verified against the new release path. Existing
userdata was retained. Physical touch confirmation and any remaining NNDDSS
slowdown are pending the user's next interactive gameplay check.
