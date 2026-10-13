# Emulator recovery and device measurements — 2026-10-08

Installed build: `0.0.1-dev-f385dc83edc8`, in `/userdata/rgds-device-test/rgds`.
The device is left running that launcher. Its owned startup hook now selects this
internal installation, with the user's existing Touch to continue setting off.
System → Startup can select Anbernic again. The setting was verified after writing;
no reboot was performed. RGDS still uses the vendor kernel and hardware services.

## Failures and changes

The original `/mnt/mmc` filesystem was remounted read-only after FAT errors
(`fat_free_clusters: deleting FAT entry beyond EOF`). Volume/hardware lock files
and emulator diagnostics could not be written there. The launcher also latched a
failed volume restoration and refused subsequent launches even when retrying the
runner could recover. Launch now permits that retry, while retaining the mixer
preflight before starting emulator audio. A read-only data directory produces a
specific storage error. Crash-log write failure retains a bounded tail in a
private temporary file instead of losing the original error.

The verified vendor `/mnt/vendor/subscreen/subscreen.dge` process remained active
behind RGDS, displaying the stock Now Playing screen and consuming CPU. It is now
included in the existing scoped stock-frontend handoff. The executable path is
checked; an unrelated process with the same name is not selected. Its final
process inventory was empty while the repaired launcher ran.

The session guardian previously scanned every process twice at 10 Hz. It now
checks process groups once per second and reads names only for matching members;
pipe notifications retain their responsive wait. In-game menu hardware status is
queried asynchronously after its first frame, device-information work is skipped,
and only the required font atlases are parsed before first presentation.

Display FPS is available in Game menu → Emulator settings on every core. A matching
bridge acknowledgement precedes preference persistence. A storage failure reports
that the choice is session-only without stopping the game. Measurement counts one
presentation source, excludes blocked/menu intervals and publishes to tmpfs once
per second. Exit archives the statistics under `userdata/logs/<core>-performance.ini`.
The counter is off until selected. It measures presentation, not emulation accuracy
or unique game frames; NNDDSS can present duplicates. Histogram percentiles cap at
100 ms; `over_20ms` includes all longer intervals.

## Data recovery

302 files from the old userdata directory were copied and SHA-256 verified into
the internal installation, including saves, library records and preferences.
The obsolete `emulators/simpleos-compat` code directory was excluded after an I/O
error reading its old bridge library; the repaired release does not use it.
The original filesystem was not changed or repaired.

The previous internal userdata is preserved at
`/userdata/rgds-device-test/rgds/userdata-before-emulator-fix-20261008`.
Old session diagnostics were preserved under `userdata/session-card-diagnostics`
instead of being reused as current process records. ROMs still reside on the
original filesystem, so this recovery does not establish their integrity or cure
future reads from damaged storage.

## Filesystem repair follow-up — 2026-10-09

At the user's request, `/dev/mmcblk1p11` was repaired offline. The full-image
backup attempt was stopped when the user requested a lower-space approach; its
unfinished 1.7 GB file was deleted and the Mac returned to about 4 GB free.

A compact, verified backup preserves both FATs, boot sectors and all 744 live
directories: 36,601,856 raw metadata bytes compressed to **585,773 bytes**. It is
stored both at `build/filesystem-repair-20261008/metadata-before.tar.gz` and
`/userdata/rgds-filesystem-repair-20261008/metadata-before.tar.gz`. The existing
verified internal saves/settings copies were retained. This metadata backup
supports undoing allocation/directory repairs; it is not a full image or backup
of deleted/free file contents.

`fsck.fat -a -f -V -v` repaired the allocation chains, dirty flag and free-cluster
summary. The three already-broken old development/compatibility files were
truncated to zero; they are not used by the repaired internal release. It
preserved 279 orphaned clusters (9,142,272 bytes) as 38 `FSCK*.REC` files at the
game partition's root. No game-save chain was reported corrupt by fsck.

Two separate subsequent read-only checks returned success, including after
creating, syncing, renaming, reading and deleting a 1 MiB test file followed by a
clean unmount. The partition was remounted **read/write** with its original mount
options. Thus the earlier read-only limitation above is resolved. Repair and
verification output is in `build/filesystem-repair-20261008/repair-output.log`.

After remounting, all **62 DS game/archive files (2,801,795,072 bytes)** were read
and hashed without errors. All **three original save files** matched their
preserved internal copies byte-for-byte by SHA-256. The game partition remained
writable, the internal build remained active, and its RG DS startup setting
remained enabled. Details are in
`build/filesystem-repair-20261008/content-verification.json`.

## Device measurements

The menu comparison used six fresh native menu launches for each build on the
same device. Median time to the first published menu frame fell from **751 ms to
263 ms (65% less)**. With a running core, measured request-to-presented-menu times
were **262–385 ms** across the two comparisons below.

An idle CPU sample, expressed as percentage of one CPU core, found the guardian at
**46.66% before and 2.8% after**. The old stock subscreen added **9.67%** before and
was absent afterward. These are brief idle samples, not battery-life measurements.

All core comparisons used the same local New Super Mario Bros ROM, native
resolution, zero frameskip, isolated userdata, muted audio and automatic save/load
off. Each started a complete fresh RGDS session, sampled the unattended opening
sequence, opened/resumed the menu three times and quit normally. All six sessions
exited with code zero, with no volume-restore or shader-compile failure.

| Core | Shader Off: average display FPS | Preset 9: average display FPS | Preset 9: first presentation | Preset 9: median menu open |
| --- | ---: | ---: | ---: | ---: |
| DraStic | 60.13 | 59.80 | 3.010 s | 287 ms |
| DSperate | 59.84 | 56.60 | 3.886 s | 320 ms |
| NNDDSS | 71.70* | 78.29* | 4.194 s | 290 ms |

\* NNDDSS redraws frames; these values cannot rank its emulation speed above 60 Hz.
The counter intentionally says Display FPS. First-presentation times include
session startup and initial emulator preparation, not just the launch animation.

Shader Off used 20-second samples on intermediate build `6cdfaf6225be`; preset 9
used 30-second samples on final build `f385dc83edc8`. The latter adds synchronized
counter toggling and first-presentation timestamps. These are short sequential
opening-sequence checks, not matched gameplay replays, thermal endurance tests,
audio-quality checks or proof of compatibility across games. Shader Off and
preset 9 samples cover different durations and should not be interpreted as a
precise shader-cost experiment.

DraStic is the provisional choice for the user's current preset 9, holding about
60 presentations per second here. DSperate also reached about 60 with shaders off.
NNDDSS now passes launch/menu/exit checks, but its duplicate presentations prevent
a fair speed ranking from this metric alone. The user's saved core and shader
choices were preserved.

## Reproduction and validation

`tools/benchmark_emulators.py` runs comparisons without loading or overwriting
existing saves. It refuses an active RGDS session and retains isolated data/logs:

```sh
python3 tools/benchmark_emulators.py --device root@DEVICE \
  --rom '/mnt/mmc/Roms/NDS/New Super Mario Bros.nds' \
  --cores nnddss dsperate drastic --seconds 30 --shader 9 \
  --output build/emulator-comparison.jsonl
```

Host and device builds plus package verification passed. The frozen suite ran
544 tests: one source/compiler-dependent test was skipped; three fixture failures
were corrected (two tests still assumed the old immediate-choice UI, and one
mocked abstract `Path` incompatibly with Python 3.9). The five affected tests then
passed on rerun. The suite covers FPS cadence/menu exclusion, acknowledged setting
persistence and write failures, launch retry, exact stock-process selection,
read-only diagnostics, menu lifecycle, shaders and audio preflight.

Other UI work was happening concurrently in the shared checkout, so compilation,
packaging and device tests used a frozen copy at
`build/emulator-fixes-source-20261008`. The package and source fingerprint record
are there. The working checkout retains all unrelated edits. Later UI edits are
not part of this installed build.

Raw evidence is under `build/emulator-fixes-20261008`: `cpu-before.json`,
`cpu-after.json`, `menu-startup-comparison.json`, `device-bench-first.jsonl`,
`device-bench-shader9.jsonl`, `device-final.json`, `frozen-all-tests.log` and
`recheck-failures.log`. The FPS settings preview is `preview/fps.png`.

Outstanding hardware validation: controlled gameplay/unique emulated-frame
comparisons, sustained temperatures, audible sound, save/load behavior with real
user progress, and a cold boot. Filesystem consistency was repaired and verified;
this does not establish long-term physical storage health.
