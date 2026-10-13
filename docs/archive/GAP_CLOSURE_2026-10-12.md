# Save management, resume, discovery and update controls

## Delivered behavior

- First installation enables RG DS Core direct startup on the supported stock
  supervisor. Upgrades retain an explicit Anbernic startup preference.
- Nine checkpoint slots per game/engine; captured previews, timestamps, overwrite
  confirmation, reversible overwrite undo and pre-load recovery. Import/export
  validates the source game and engine. Library settings expose save-data backups.
- Optional shutdown resume uses a verified one-shot ticket. It is consumed before
  launch, skips OTA health trials, and falls back to the menu for missing or changed
  ROM/state files. Normal exits do not hash ROMs when the feature is off.
- Text search, played/unplayed/missing-artwork filters and first-install ROM setup.
  Discovery uses the cancellable library worker; drawing performs no file reads.
- DSperate and melonDS expose real microphone, a two-second simulated blow,
  two-second emulated lid closure and the six DS firmware languages. UI language
  and DS firmware language are independent. Other engines retain native controls.
- Automatic update checks default to a six-hour interval once a trusted feed exists.
  Automatic download is optional. Installation still requires confirmation and uses
  the signed staging/rollback path. The private build has no public feed.
- New user-facing screens are translated in all seven bundled languages.

## Hardware checks

Initial feature matrix used `0.0.1-dev-f8636d764344` on the standard RG DS stock
Linux card. Disposable ROM copies and save directories isolated the user's progress.
Raw JSON, rendered screens and test logs are under `build/gap-closure/`.
The earlier `matrix-mario` run used the previous build and is excluded.

New Super Mario Bros. passed slot save, confirmed overwrite, overwrite undo,
load, load undo, export and clean exit on DSperate, melonDS, DraStic and NNDDSS.
DSperate and melonDS also acknowledged simulated blow/lid commands, returned to
play and reopened the menu. Each engine produced a slot screenshot. Separate
card-side checks re-imported exported slots, verified every backup file against its
SHA-256 inventory and accepted a resume ticket once, then rejected the second use.
These checks establish the adapter/file flows; they do not prove restoration at
every point in every commercial game.

Mario Kart DS was checked on all four engines, and Nintendogs: Labrador & Friends
on DSperate/melonDS. The automated scenes are startup/title-screen workloads,
not representative gameplay benchmarks. Frame telemetry distinguishes engine
FPS where available from display presentation FPS. The optional SuperDrastic
native menu was not included in this new matrix.

Physical ALSA capture returned 32,000 samples in two seconds at 16 kHz mono,
peak 354 and RMS 100.2, without a capture-device error. This confirms a working
capture path. A person blowing during a microphone-dependent game is a separate
acceptance check. The shared DS input adapter does not claim real microphone
support for standard DraStic or NNDDSS.

## Measurement limits and release acceptance

- Menu request-to-first-frame and resume/reap times are software interaction
  measurements. They are not physical button-to-photon latency.
- The benchmark can sample ALSA kernel playback delay and rate. Queued milliseconds
  exclude upstream engine buffers, DAC/speaker delay and physical sound travel.
  End-to-end input/audio latency needs a high-speed camera or synchronized capture.
- A ten-minute automated stability run is an initial soak, not an hours-long
  gameplay, battery-life or sleep/wake qualification.
- The one-shot ticket path is tested; repeated physical shutdown/cold-boot cycles,
  removable-card power-cut testing and the reported two-press cold start still need
  hardware acceptance.
- Portable checkpoint import is implemented. Cross-engine battery-save conversion,
  cloud synchronization and whole-backup restore UI are not implemented.
- Achievements, cheats, a game-freeze watchdog, Plus validation and a distributable
  standalone OS image remain outside this delivered application update.

## Reproduce

Build host/device targets, package the application and wrap it with
`tools/package_installer.py`. Run `python3 -m unittest discover -s tests`.

`tools/benchmark_emulators.py` accepts `--feature-checks` for slot/DS-control
exercises, `--capture-scene` for one-shot supported captures, and up to 3,600
seconds per phase for longer attended validation. Use disposable profiles;
never benchmark against a user's live saves. It refuses an existing RG DS Core
session. Frame/thermal/audio samples, exact build identity and engine logs remain
in the resulting JSON records.

## Host verification

The full host suite completed 913 tests with six environment-specific skips and
no failures. Final focused runs also passed 99 runtime/safety tests and seven SDL
checks covering gallery navigation, overwrite confirmation, DS language, search
filters, import selection, ROM setup, quick resume and automatic downloads.
The final candidate additionally moves the undo catalog update before replacing
the live checkpoint and skips resume hashing when the option is off. Those changes
have targeted failure-path coverage.

## Ten-minute device stability result

Cooking Mama on DSperate completed the 600-second measured phase, three pause/resume
cycles and save-on-exit with exit code 0. Presentation averaged 60.00 FPS; the last
engine sample was 60.01 FPS. Frame intervals were 16.7 ms at p50, 17.1 ms at p95 and
18.1 ms at p99. SoC temperature peaked at 65 °C. The ALSA kernel queue measured
84.833 ms median (78.167–85.333 ms range); this is buffering, not end-to-end audio
latency. The game remained in an automated startup/title-screen workload.

The installed delivery candidate is `0.0.1-dev-391c668a0918`. It includes the final
resume fast path and failure handling covered by the final 99-test runtime run.
The downloadable installer checksum is stored alongside the ZIP in `dist/`.

## Follow-up: discovery, reset and microphone simulation

The user reported that the Cooking Mama microphone test seems to work. This is
an informal game check; the report did not distinguish physical capture from the
simulator, and does not establish support in every engine or game.

Search/filter results opened from Library settings now leave the home carousel
and display the game collection. Search trims surrounding whitespace and matches
ROM, artwork and indexed banner titles. Active filters and empty results are
labelled; clearing search restores the collection. Background filtering remains
cancellable and does not read files while drawing.

Emulator settings now include a confirmed Reset state action. It first saves a
checkpoint, then restarts the same engine with auto-load disabled for one launch.
A failed save cancels the reset and leaves the game running. Saved slots and
battery saves are not deleted. Sound settings include a physical microphone test with live RMS and peak levels,
a dBFS reading and DC-offset rejection. PCM is discarded after measurement; only
levels pass through a pipe to the UI. Leaving the page stops capture. A running
game blocks the standalone test, avoiding contention with emulator capture.
The existing two-second simulated blowing action remains in DS controls.

The follow-up delivery is `0.0.1-dev-9b1a64591ec2`. Host checks passed for
search/filter/microphone navigation (13), runtime behavior (34), game-menu flows
(40), microphone samples and capture ownership (3), language coverage/glyphs (4),
and existing sound interactions (3). Earlier follow-up library and collection
regressions also passed. On the RG DS, a disposable Cooking Mama profile saved a
5,690,650-byte checkpoint, restarted without `--load-state`, then loaded normally
on a subsequent Save & restart; clean exit returned code 0. The initial probes
watched the supervisor rather than the engine and are excluded from this result.

The microphone test received physical 16 kHz mono PCM, emitted changing level
readings, and released ALSA capture after stopping. Only numeric readings were
retained as evidence. A vendor-SDL dummy-driver self-test could not run because
that SDL build lacks the dummy driver; it is not counted as a passing test.
