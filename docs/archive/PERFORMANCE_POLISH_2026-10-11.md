> Follow-up: [melonDS now has a selectable SDL frontend](MELONDS_INTEGRATION_2026-10-11.md). The research and package record below describe the earlier three-engine candidate.

# Stock RG DS performance and UX review — 2026-10-11

## Scope and evidence

Local application changes only. No deployment, flash, reboot, suspend, hardware
clock write or shutdown was performed. A single read-only SSH attempt timed out.
The active stock-card build remains `31ae97d6bb7d` until the next authorized test.
The user reports slow NNDDSS in every game and power-off restarting on battery,
so USB charging is not sufficient to explain the shutdown failure.

## Changes prepared

- New global preferences and CLI launches default to DSperate. Engine IDs remain
  DraStic=0, DSperate=1, NNDDSS=2, preserving saves and per-game overrides.
  Existing explicit preferences survive upgrades: the saved device backup has
  `core=2`. Set Default emulator to DSperate during the next authorized session.
- The default in-game menu stops the owned emulator process, opens its own GPU
  windows and resumes the same process on exit. It no longer relies on the
  stopped emulator to display shared software-rendered menu frames. Saving,
  loading and live shader commands temporarily resume the core to service the
  request, then pause it again. Saves, fast-forward, lid sleep and exit recovery
  retain their existing ownership checks. Live shared rendering remains available
  to the integration tests but is not the normal launch policy.
- The menu acknowledges its first presented frame using its PID. A missing or
  stale acknowledgement restores controls after five seconds rather than leaving
  the game stopped behind a menu that never drew. Actual compositor focus,
  audio resume and menu latency on all three engines still need the handheld.
- Threaded 3D now explains engine capabilities. DraStic has an on/off switch that
  requires Save & restart. DSperate automatically allocates rendering workers;
  NNDDSS does not expose a reviewed user toggle. Its saved loader log reports a
  threaded raster pool. Read-only options have no misleading action arrow or
  Select button. The native/2x NNDDSS resolution setting explains its cost.
- Added Pocket green, Amber and Soft LCD grid: original single-texture-sample
  effects. There are 20 effects plus Off, with stable older IDs. All 63 previews
  (three original scenes × 21 choices) were regenerated through actual host GLES.
  Handheld GPU cost remains a measurement, not an assumed speed-up.
- Removed unreachable menu feedback branches and consolidated shader counts and
  names shared by the launcher, GPU pass, runtime and preview/package tools.
- Shutdown writes a durable action/command/boot-ID receipt and captures the stock
  helper output. An accepted command is never labelled physical shutdown. Errors
  remain failures; there is no retry as reboot or forced PMIC/register write.
- The existing isolated emulator benchmark now samples CPU/GPU clocks,
  temperatures and process CPU ticks once per second, and recognizes first-frame
  acknowledgements from the paused menu. It does not run as part of gameplay.

## Not yet fixed or established

**NNDDSS gameplay speed:** no cause has been confirmed. The previous stock-card
New Super Mario Bros test recorded 59.05 game FPS and 60 display FPS; that is one
older sample, not a rebuttal of the user's current experience. The saved config's
1024×768 window fields are not proof of oversized rendering: the same loader log
reports a correct 1280×480 dual-panel span. Native 3D and zero frameskip were set.
The crash-prone fast-upload patch stays disabled. Compare the affected current
installation, power profile, temperatures, shader Off and native resolution before
changing clocks or loader patches. Use game FPS separately from presentation FPS.

**Power-off reboot:** RGDS already dispatches the normal stock `poweroff` helper.
The saved audit lists `S99-auto-reboot`, but its contents and relevance are unknown.
At the next read-only connection, collect that script, `rcK`, `inittab`, stock
`loadapp.sh`, helper symlink targets and kernel power logs. Compare stock-menu
shutdown with RGDS shutdown after saving and returning home. Record whether the
kernel boot ID changes, and verify the LED remains off on battery. Do not infer a
fix from successful helper exit or copy the reference OS's raw PMIC writes.

**Boot artwork/time:** the earliest Anbernic logo remains unresolved in the signed
vendor boot image. Later userspace splash assets are separate. The last recorded
stock first frame was at 17.823 seconds of Linux uptime, 2.532 seconds after the
RGDS startup hook and 1.832 seconds after its session started. This excludes the
bootloader and physical button hold; no new cold-boot result is claimed.

## Additional emulator research

- [melonDS](https://github.com/melonDS-emu/melonDS) supports Linux, Windows and
  macOS. It is the best next integration candidate here because an ARM64 JIT
  headless probe already exists in this repository. It is not a selectable,
  playable RGDS engine yet; dual-screen video, touch, audio, pacing, checkpoints
  and device performance remain work. See [the probe evidence](MELONDS_EXPERIMENT_2026-10-09.md).
- [NooDS](https://github.com/Hydr8gon/NooDS) is an open-source cross-platform
  alternative with multicore software 3D. Its desktop frontend needs adaptation
  to this handheld; no claim that it will outperform DSperate is justified.
- [SkyEmu](https://github.com/skylersaleh/SkyEmu) includes DS emulation marked beta
  upstream. Consider it for future compatibility experiments after the required
  three engines are stable. No placeholder engine entries were added.

## Validation

Evidence is under `build/performance-polish-20261011/`. Host and Linux ARM64
builds, SDL interaction tests, owned-process pause/resume/save/exit tests, repeated
lid cycles, shader pixels through all three bridge paths, preview provenance,
shutdown ownership and package hashes are checked locally. These checks do not
constitute hardware acceptance. Device checks follow the explicit no-deployment
restriction and resume when the user is ready.

Final local package: `dist/RGDS-0.0.1-dev-24a6fc55337c.zip` (11547851 bytes).
SHA-256: `84240b7aff11c99f0c0f6c604391616ea6d8cf890c4b49fe8e504c5d23513e9f`.
All 190 manifest files and the runtime release verifier passed; 51 existing
emulator payload files match installed build `31ae97d6bb7d`. Only the RGDS bridge
changed and three owned shader files were added in the emulator payload tree.

The focused run passed 121 tests; the complete UI/integration run passed 71;
the lifecycle review passed 90. After visual review removed misleading controls,
the final UI subset passed 9 tests and package checks passed 19. These runs
overlap and must not be added into a unique-test count. Earlier shader tests with
hard-coded old counts were updated, then rerun successfully. All 20 shaders were
compiled and drawn through each of the three bridge paths on host GLES; the 63
scene/preset reference images passed source and pixel checks.
