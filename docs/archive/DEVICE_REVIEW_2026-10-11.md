# Stock RG DS emulator review — 2026-10-11

The owner authorized deployment and tests over SSH to `192.168.68.118`.
Anbernic Linux remains the boot baseline. DSperate remains the default core;
Standard remains the default DraStic backend. No OS image, firmware or boot
configuration was changed. Test ROM copies and profiles lived under
`/mnt/mmc/benchmark-*`; existing user saves were not used or overwritten.

## Changes established by the review

- SuperDrastic initially aborted in touch discovery because a fixed 512-byte
  `realpath` buffer violates fortified glibc's requirements. The adapter now
  uses libc-allocated canonical paths. A native regression includes a path
  longer than 512 bytes.
- The next startup crashed because Anbernic's SDL 2.32.10 only provides Wayland
  and KMSDRM video drivers. SuperDrastic requires dummy video and was continuing
  after SDL initialization failed. The package now selects a private, pinned
  SDL runtime with dummy video, software rendering, Linux input and ALSA.
  Required drivers are checked before profile changes or display acquisition.
- A restored original VT could be incorrectly reported as a failed return when
  releasing the unused VT returned EBUSY. EBUSY is ignored only after the
  original console has been acknowledged; actual switch failures still fail.
- NNDDSS consumes keyboard state for fast-forward. A queued Tab event alone
  did not toggle it. The bridge now supplies the bounded synthetic key state;
  the emulator log and game-frame counter confirm on/off behavior.
- DSperate exports its actual completed-frame count, including frames skipped
  by its fast-forward display path. The HUD distinguishes GAME from DISP.
  NNDDSS and melonDS also report game frames; DraStic retains display-only FPS.
- Resume acknowledgements rebase frame timing even if SIGSTOP happened before
  the bridge observed the blocked-input transition. Menu pauses no longer
  contaminate the next frame interval.
- Launch verification skips the corresponding SDL source archive, while still
  hashing the executable runtime. Full installation verification includes both.

## Method and limits

`tools/benchmark_emulators.py` ran one copied ROM across disposable profiles:
native resolution, shader Off, no frameskip, performance power profile, muted
audio and automatic checkpoint on exit. The second comparison used New Super
Mario Bros with SHA-256
`dd2e519a9e46134e213e6e63dd2b478d9a258627769b777baddb2316dc21b81d`.
Each phase sampled ten seconds after settling: normal, fast-forward, then normal
again. Three menu cycles followed. CPU frequency was 1.992 GHz; sampled SoC
temperatures in this comparison ranged from 41.9 to 47.2°C.

These are launch/control/save and short attract-sequence smoke tests. The game
advances differently during fast-forward, so later phases are different scenes.
They are **not** a controlled same-scene speed ranking, proof of full-speed
gameplay, audio quality, physical input latency or superiority to another OS.
No competitor was measured. A fair follow-up needs fixed checkpoints/input
replays, matched elapsed emulated frames and repeated runs of demanding gameplay.

The initial Mario Kart comparison used `0.0.1-dev-d87f7db417c8`; the corrected four-core comparison used
`0.0.1-dev-33212e5ca829`. SuperDrastic's successful private-SDL test used
`0.0.1-dev-afb800fbf925`. `dist/latest.txt` identifies the final documented package.

## Corrected four-core results

GAME rates below are median sampled emulated frames/sec. DISP is presentations,
not necessarily unique game frames. Menu timing ends at the rendered-menu ACK;
it excludes the user's physical button and screen scan-out latency.

| Core | First presentation | Menu median (3 opens) | GAME normal / FF / restored | Outcome |
| --- | ---: | ---: | ---: | --- |
| DSperate | 4.074 s | 435 ms | 59.99 / 60.80 / 55.32 | Checkpoint saved, clean exit |
| NNDDSS | 3.039 s | 399 ms | 60.00 / 120.02 / 60.00 | Fast-forward on/off confirmed, checkpoint saved, clean exit |
| melonDS | 4.355 s | 416 ms | 53.37 / 32.46 / 23.54 | Significant slowdown in later scenes; checkpoint saved, clean exit |
| Standard DraStic | 3.381 s | 426 ms | unavailable | DISP stayed about 59.9; checkpoint saved, clean exit |

DSperate's display rate during fast-forward was only about 15.2 while emulation
ran at 60.8: its draw-skipping path illustrates why display FPS was misleading.
The scene change prevents attributing the small emulation-speed increase solely
to the toggle. NNDDSS explicitly logged `fast-forward x2: on` and `off`, matching
its measured 2× counter. Its previous reported slowdown in all games is not
resolved merely by this light test sequence.

Standard and SuperDrastic do not yet expose a validated emulated-frame counter.
An unchanged 60 DISP during fast-forward cannot prove either success or failure.
The current SuperDrastic test confirms command delivery, not actual acceleration
or physical mapped-button release behavior. Those remain pending.

## SuperDrastic and checkpoint review

With the two startup faults fixed, native DRM readiness arrived in 2.988 s.
The three native-menu acknowledgements took 25, 27 and 21 ms. These acknowledge
the native command and are **not equivalent** to the rendered-menu measurement
above. Native logs show both DSI panels initialized and receiving flips. Quit
saved a 1,450,583-byte checkpoint, reported a successful native receipt and
completed session recovery in 2.085 s. The native save itself logged 743 ms.

Reload checks reused only disposable profiles:

| Path | Evidence | Limitation |
| --- | --- | --- |
| DSperate → DSperate | Engine logged checkpoint loaded/autoloaded at frame 2486; clean exit | Expected visible progress still needs a person to confirm |
| NNDDSS → NNDDSS | Bridge dispatched autoload; loader logged F7 quick-load slot 8; clean exit | Dispatch/handling is not a positive core-level restoration receipt |
| melonDS → melonDS | Native autoload configured; session continued and exited cleanly with no load error | No explicit success log; visual progress remains unconfirmed |
| SuperDrastic → Standard | Standard logged loading the shared `.dss` and its saved RTC, then wrote a new checkpoint | Game-native battery-save behavior remains untested |
| Standard → SuperDrastic | Native backend logged `[resume] resumed`; clean no-save exit | Physical gameplay acceptance remains pending |

With Auto-save off, all four reload paths preserved their canonical checkpoint
SHA-256. Standard's intentionally enabled Auto-save produced a new checkpoint
for the reverse SuperDrastic test. The SuperDrastic test confirms that upstream's
consume-on-load behavior deletes only the staged copy, not the canonical file.
Host tests also cover failed-save preservation, backup promotion, autosave-off,
newer battery-save precedence and display restoration failures.

Cross-engine save conversion is not implemented. Standard and SuperDrastic use
the same DraStic engine/save home. DSperate, NNDDSS and melonDS retain separate
native battery saves and checkpoints. See [SAVES.md](../SAVES.md).

## Validation and evidence

- 102 relevant host regressions passed, including bridge metrics, synthetic
  controls, engine source records, launch preparation, pause menus, launcher
  return, SuperDrastic state policy and executable/source verification.
- Four actual Linux ARM64 native-library tests passed: symbol loading/config
  parsing, long canonical paths, no-save exit and save-failure/second-signal exit.
- Source builds and package validation check pinned inputs, ELF ABI, associated
  source, notices and every packaged payload hash. SDL source is zlib licensed
  and included separately from SuperDrastic's MIT source.
- Internal userdata was full. Eleven inactive releases were archived to
  `/mnt/mmc/rgds-maintenance-20261011/`, verified against every file hash and
  symlink target, then removed from internal storage. Active and previous
  releases, userdata, saves and boot backups were retained; about 317 MiB freed.

Local evidence is under `build/device-review-20261011/`: `mario-kart-first.jsonl`,
`new-super-mario-fixed.jsonl`, `superdrastic-private-sdl.jsonl`,
`restore-results.jsonl`, `final-regressions.log` and `native-final.log`.
Remote benchmark folders retain full per-engine logs, copies and receipts.

Physical dual-screen appearance, touch, sound, battery saves, mapped hold/toggle
release, lid sleep, Wi-Fi, shutdown and boot branding are still separate
acceptance work in [NEXT_DEVICE_TEST.md](../NEXT_DEVICE_TEST.md). No claim of fixing
those historical issues follows from these emulator tests.

NooDS and SkyEmu remain candidates, not installed engines. See the primary-source
review and frontend requirements in [EMULATOR_OPTIONS.md](../EMULATOR_OPTIONS.md).
