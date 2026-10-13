# Fourth engine and retired Armbian tooling — 2026-10-11

This is a local application candidate for the working Anbernic Linux card.
No handheld connection, deployment, flash, reboot or power action was performed.
The source-built melonDS frontend is selectable as engine 3; DraStic=0,
DSperate=1 and NNDDSS=2 retain their saved identities. DSperate remains the default.
Existing per-game choices and saves are preserved.

## What changed

- melonDS uses the pinned upstream core, ARM64 JIT, release LTO and Cortex-A55
  tuning, with an original GPL-3.0-or-later SDL frontend. It boots NDS software
  directly with FreeBIOS/generated firmware and needs no stock BIOS assets.
- Two native 256×192 screen textures feed separate SDL renderers. One DS frame
  clock paces both panels; per-window vsync cannot impose a second wait. ROM
  data moves into the core without an extra full-ROM copy. Audio queues are
  bounded and cleared after pauses/loads; the system clock initializes the RTC.
- Controller input, keyboard, bottom-screen touch and audio use the shared RGDS
  integration. Microphone capture opens on demand. Physical microphone behavior
  remains untested. The core's completed-frame counter supplies game FPS
  separately from presentation FPS, so dual displays cannot double the reading.
- Battery saves and checkpoints live in distinct per-game paths. Game identity
  prevents equal filenames in different collections from sharing a save. Atomic
  writes flush file and directory metadata. Automatic loading happens before
  gameplay rather than through a delayed four-second hotkey. Existing manual
  checkpoints are backed up before automatic checkpoint import.
- The shared menu, shaders, mapped fast-forward, volume, pause/resume, save/load,
  exit and lid watcher include melonDS. Native checkpoint loads report success
  only after the core accepts the state; corrupt data returns an error and retains
  the prior emulation state. Real game progress still needs device verification.
- melonDS Threaded 3D applies through its native renderer API between frames.
  The same game/process stays alive. The menu persists the choice only after a
  matching successful acknowledgement. DSperate manages its workers automatically;
  NNDDSS has no reviewed user threading API. DraStic's closed binary still requires
  restart for its startup configuration; no invented live toggle is presented.
- Unsupported melonDS high-resolution/frameskip settings are disabled. This
  frontend implements NDS direct boot, not DSi system mode, camera, multiplayer,
  Slot-2 or filesystem add-ons. Retail-game compatibility is pending.

## Local verification

Evidence: `build/engine-expansion-20261011/`.

- Host and Linux ARM64 launcher/bridge builds; current source/ABI records match.
- Native ARM64 frontend with original synthetic homebrew: both SDL screen pixel
  readbacks match the emulated buffers, button and pen states reach the emulated
  registers, audio samples queue, microphone capture starts/stops, battery bytes
  survive file readback, RAM restores from disk checkpoint, malformed state is
  rejected, and 20 live threading switches preserve progress.
- The actual ARM64 bridge exercises 18 acknowledged commands, including ten
  threading changes, save/load, failed corrupt-state load, successful retry,
  fast-forward and quit. A stopped process accrues no CPU ticks and resumes.
- The separate core probe passes 13 frames in interpreter and ARM64 JIT modes,
  with matching execution counters and distinct top/bottom framebuffer hashes.
- Targeted runtime, UI, engine-selection, save-ownership, bridge, shader pixel,
  simulated lid lifecycle, build provenance and packaging tests pass. An older
  three-choice touch-test coordinate was updated for the fourth choice and the
  affected test was rerun successfully.
- All 20 effects run through the four engine rendering paths on host GLES.
  The melonDS menu was inspected visually, including its immediate-threading
  explanation and live switch state.
- Corresponding source includes all 778 pinned upstream files, original frontend,
  tests and build drivers. The source archive was independently unpacked and its
  complete upstream fingerprint verified without Git metadata. `make build-melonds`
  and `make package` include the new engine; the dummy cross-link SDL is excluded.

These are synthetic/host/VM checks. They do not certify RG DS game speed,
physical dual displays, touch, sound, microphone or reliable shutdown/suspend.
There is no measured claim that this candidate beats SimpleOS or ROCKNIX DS.
Use the same games, shader Off, native resolution and power profile when comparing
actual game FPS, menu latency, clocks, temperature and memory on the handheld.

## Retired OS experiment

Removed the active `os/` image configuration, Armbian image/flash/recovery tools,
associated prototype tests and generated test images/base cache. Removed the OS
Make targets and moved old distribution receipts into the retirement archive.
The exact source was archived and reopened for verification before removal.
Historical documentation, hardware failure logs and stock backups are retained.

Archive: `build/retired-armbian-20261011/`. The local Ubuntu ARM64 builder remains
available for executable checks; it is not an Armbian image builder. Updates use
the application ZIP over SSH on the working stock installation.

## Next hardware session

Follow [the stock-card checklist](../NEXT_DEVICE_TEST.md) after deployment resumes.
The reported NNDDSS slowdown, Power off restarting on battery, early Anbernic logo
and cold-boot time remain unresolved. Do not claim those fixed by this package.
Check internal free space and preserve active/previous releases plus saves before
staging any application update. No replacement SD image is needed.
