# Offline integration validation — 2026-10-07

Package snapshot: `RGDS-0.0.1-dev-7442440137c3` (the ZIP is a local ignored artifact, not included in the repository). This report records offline changes and validation only. No deployment or device tests followed this build; an earlier upload attempt timed out before activation.

## Changes

- In-game shader selection stays in the picker and sends a live command. DraStic draws the original game first, then its supplied effect; it no longer draws an unfiltered copy over the effect. Destinations account for logical display scaling, and intermediate render targets bypass the effect.
- NNDDSS uses a live GLSL pass on the completed display frame, before the RGDS overlay/HUD. Its native startup-only effect is disabled to avoid two shader passes. Programs, context resources and GL entry points are cached. A failed shader compilation keeps the previous selection and reports failure.
- DSperate switches native nearest/bilinear texture scaling. Missing live rendering capabilities report failure, without requesting a game restart.
- The launcher keeps an action busy until the runtime acknowledges the exact UI request token. A stale status file cannot unlock Save, and a second tap cannot replace an outstanding action.
- Checkpoints must be nonempty. Save detection also checks ctime and file identity, so a same-size rewrite with unchanged mtime is detected. Both checkpoint contents and the directory rename are flushed before acknowledgement.
- The supervisor owns hardware menu keys; the engine and embedded helper ignore duplicate menu events. Shared-frame readers retain the last complete frame during publication.

## Local validation

All 218 tests passed, plus the launcher self-test, a dark preview render and the Linux AArch64/glibc 2.17 source build. A subsequent focused shader test passed after adding logical destination scaling. ZIP members and all 86 release payload hashes were checked against the manifest; all three engines and 14 supplied `.dsd` presets are included.

The actual embedded launcher was exercised against delayed/stale status replies using host SDL. A save stayed pending through extra presses; a matching acknowledgement allowed the next action. A software SDL fixture verifies that a zero return from the legacy post-processing callback cannot cover the effect with a plain copy. DSperate's host software renderer verifies an actual nearest/bilinear pixel difference.

NNDDSS's GL pipeline lifecycle, caching, state restoration and failure handling use a typed test driver. Actual Mali shader compilation, visual output and frame pacing have **not** been validated. The Mac could not create an offscreen GL context; no GPU validation claim is made. Save tests verify protocol/files, not restored gameplay on the console.

The package was built from frozen inputs in `build/overnight-source-20261007/` to retain a consistent version while the other UI chat continued editing. Source hashes are recorded in that folder's `source-snapshot.json`; package verification is in `build/overnight-validation.json`. Logs are `build/overnight-tests.log` and `build/live-shaders-tests.log`.

## Next device session

Use a verified writable installation, confirm the device's current IP, and mute before launching. The original `/mnt/mmc` FAT damage remains unresolved; see [the previous report](DEVICE_TEST_2026-10-06.md). No filesystem repair was performed.

1. Open/close the overlay repeatedly with each hardware menu key. One press should toggle once, without blinking or leaking controls into the game.
2. Switch Off → LCD1x → NDS Color → Off on DraStic and NNDDSS at the same scene, and Off → bilinear → Off on DSperate. Confirm both panels and an unchanged engine PID/session.
3. Save the first checkpoint at an identifiable scene, then save again quickly. Advance the game, load and resume to prove restoration. Repeat after returning home and a clean reboot.
4. Verify normal in-game battery saves, muted startup/return, and deliberately chosen audible volume levels.

The full follow-up checklist is [NEXT_DEVICE_TEST.md](../NEXT_DEVICE_TEST.md).
