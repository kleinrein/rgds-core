# OTA foundation acceptance — October 12, 2026

Installed build: `0.0.1-dev-ffa1779feef6` on the standard RG DS, Anbernic
Buildroot 2024.02. Installed through the SD APPS installer entry invoked over SSH.
No public feed or production signing key was provisioned.

## Delivered

- Signed HTTPS update metadata, resumable downloads, package verification,
  staging and progress/retry/discard/restart controls.
- Stable supervisor outside releases, first-frame/heartbeat trial, automatic
  rollback and a persistent journal for interrupted startup.
- Settings → Statistics: library totals and five most-played games.
- Optically centered Wi-Fi status icon, Settings home tile without play history,
  framed cover art, and a shared rail geometry that contains the blue thumb within
  the grey viewport at every tested position.
- Rewritten README, installation/build/release guidance and a sourced comparison
  with GammaOS Nano and ROCKNIXDS.

## Validation

14 focused OTA tests passed: real Ed25519 signatures and tampering, metadata
expiry/replay/target/channel/protocol, resumed downloads, corruption, unsafe
paths, storage/game/lock gates, healthy startup, crashes/hangs, stopped heartbeat,
stale tokens, interrupted trial, corrupt staged release and cold-boot fallback.
Five SDL/geometry checks passed, including live asynchronous progress and rail
containment for 0–100,000 games. The 96-test UI pass found one outdated settings
scroll target after the Statistics row was added; its corrected focused rerun
passed. A further 88 backend/settings/startup/installer checks passed. The final 24-test OTA/UI/launcher-return pass also verifies that starting a game
during the trial keeps the heartbeat alive after SDL closes. The launcher
self-test and host/device builds passed.

The release-signing tool processed the complete final application package,
generated an OTA archive and signed feed, and verified the signature using a
temporary key. That key was removed; nothing was published. Device OpenSSL passed
valid and tampered RFC 8032 test vectors.

On the device, the exact installed launcher was re-trialed to verify its real
first-frame acknowledgement and full 15-second heartbeat period. It committed
successfully. A separate, verified test candidate deliberately raised an error
before startup. The supervisor restored the installed build and relaunched its
menu automatically. The test candidate was removed, the original manual rollback
target/status restored, and both preference files retained identical hashes.
Captured screens confirmed the recovered menu. No game was running or launched.

Evidence is in `build/ota-foundation/`: test/build logs, signing smoke artifacts,
`device-acceptance.json`, previews and `device-restored.png`.

## Remaining acceptance

This validates application startup recovery, not physical power-cut durability or
OS A/B recovery. Still required before public release: a real hosted signed feed,
clean spare-card install through the physical APPS menu, disposable-card power-cut
trials, signing custody and resolved redistribution licensing. The earlier
first-Power-press report is unchanged and unresolved. See [OTA](../OTA.md),
[release plan](../RELEASE_PLAN.md) and [feature comparison](../FEATURE_COMPARISON.md).
