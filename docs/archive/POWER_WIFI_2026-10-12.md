# Stock RG DS power and Wi-Fi investigation — 2026-10-12

Anbernic Linux remains the device baseline. No SD image, bootloader, kernel or
vendor service was replaced during this work. DSperate remains the default.

## Lid sensor correction

The owner's close/open test produced the following events on the stock
`rk805 pwrkey` input device:

| Action | EV_KEY code | Sequence |
| --- | --- | --- |
| Close | 110 | press, release about one second later |
| Open | 111 | press, release about one second later |

`/sys/class/anbernic_misc/hallkey` stayed at `3` throughout. The previous
assumption that this was a live 0/1/3 position caused the failed lid detection.
There is no SW_LID capability on this firmware's input devices.

The session guardian now discovers the named input device by capabilities,
reads it without grabbing input, and latches the two press edges. Releases do
not reopen the lid. A shared atomic record in `/tmp` (confirmed tmpfs on this
device) supplies the same state to the launcher, game runners and short-lived
suspend helper. Unknown initial state, dropped events, disconnected input and
stale records cannot trigger sleep. The guardian removes its record at exit;
its heartbeat also prevents stale state from a crashed guardian triggering sleep.

The existing debounce and pre-suspend recheck remain. Suspend still uses the
installed pm-utils hooks and journals wake-source changes. Detection tests do
not establish physical suspend/resume, audio recovery or game-save acceptance.
The owner confirmed screens turning off and returning after lid close/open on
`0.0.1-dev-045afff07ee9`; the kernel recorded two successful suspends and the
helper returned normally. SSH/Wi-Fi returned. Screen-off felt slow; gameplay,
audio and repeated-cycle acceptance remain pending. Host/device builds and
114 targeted regression tests passed before deployment.

## Wi-Fi changes and evidence

Scanning and connecting can recover an enabled but unavailable Wi-Fi interface;
explicit radio Off is respected. Duplicate access points retain their connected
indicator. A successful join now verifies the actual active access point's SSID,
not merely NetworkManager's saved connection name. Failed joins refresh status
instead of leaving a stale Connected/IP display. Timeout errors omit captured
command output so a password cannot leak through an error message.

The new verifier recognized the current access point on the RG DS in 0.307 s
(NetworkManager 1.44.2). This is not acceptance of changing networks, a wrong
password, sleep/wake reconnection or network loss. These require device tests.

## Persistent shutdown fault: stale RK817 saved state

The failure also occurred after a fresh Anbernic boot that never ran RGDS.
Returning all seven modified userspace splash files to their backed-up originals
and removing the inactive RGDS autostart hook did not fix a normal restart.
The stock boot partition matched the October 11 backup byte for byte; stock init
scripts matched their saved baseline. Shutdown traces reached init's cleanup
and SIGTERM stage. This narrowed the search beyond a stuck launcher process.

The device matched the exact signature in the upstream RG DS
[saved-rail-state fix](https://github.com/TheGammaSqueeze/rg_ds_kernel/commit/7ecfef9850b43d403c1038b5798c0e87dffecb54):
RK817 at I2C `0-0020`, board policy `not-save-power-en`, scratch register
`0x99 = 0x7f`, and `0xa4 = 0x00`. The affected kernel replays these saved values
into rail controls during restart/shutdown. An alternative bootloader can leave
them in battery-backed storage, so changing the SD card does not clear them.
Which earlier boot wrote this device's values was not directly observed.

A one-device, guarded repair cleared only those two saved-state bytes through
the kernel's `rk8xx_dbg` interface. It checked board/chip/policy, the exact stale
signature, boot identity and absence of an active RGDS/emulator session, saved a
durable before receipt, and read back both zeros. It did not write active rail
controls, force power off, replace the kernel or flash a partition. The register
definitions and debug-interface implementation were checked against the pinned
upstream source before use. This is a diagnostic repair, not an unconditional
launcher startup action or general-purpose PMIC writer.

**Ordinary stock reboot now succeeded without forced recovery.** Boot ID changed
from `48c2f3e3-2d20-49c8-a5f6-c7982ed2c700` to
`ba0d6bd1-b615-4d02-95e8-b4be0f3fcd91`; SSH returned and both scratch bytes remained
zero. The owner then used Anbernic's Power off and confirmed **the LED went out**.
This establishes one successful normal restart and one physical stock shutdown
after the repair. The owner also confirmed the subsequent start worked with a
normal Power press, without the previous long hold. The owner then confirmed
**RGDS Power off also extinguished the LED** on `0.0.1-dev-d02ae91ef0d6`.
Repeated battery/charger cycles remain untested. A different bootloader could
reintroduce the stale state; the stock
kernel itself has not been patched.

Device repair receipts are under
`/userdata/rgds-device-test/backups/pmic-scratch-1791793507421554343/`.
The original boot-customization backup is under
`/userdata/rgds-device-test/backups/power-baseline-1791792299539020304/`.
Anbernic startup and original userspace splash files were selected for the
hardware comparison. After that comparison passed, the owner requested direct
RGDS startup and boot branding; both were installed and physically confirmed in
the subsequent identity update described in [BOOT.md](../BOOT.md). RGDS releases, ROMs, saves and preferences are preserved.

## RGDS shutdown handoff correction

A separate code review found that RGDS restarted the stock frontend immediately
before asking init to shut down. The session guardian now completes worker and
hardware recovery, then dispatches the committed system request while retaining
its stock stop flag. Ordinary Return to stock still restores the frontend. A
failed system request restores stock; there is no forced-power fallback.
The boot-entry path acquires a stop flag before dispatch as well. After stock
restart/power-off and normal power-on passed, application release
`0.0.1-dev-d02ae91ef0d6` was hash-verified, deployed and launched for RGDS Power off
acceptance, which passed with the owner's physical LED-off confirmation. Both
display renderers initialized; DSperate remains the saved default.

Regression tests cover the real parent/guardian fork and pipe, cleanup ordering,
fragmented messages, ownership, rejected requests, stock and standalone entry,
and failure recovery. The final focused run passed 75 distinct tests: 44 stock
handoff, 12 device lifecycle and 19 system control tests. Acceptance of a command
alone never proves physical power-off.

Local evidence is in ignored `build/power-wifi-20261012/`; it is not shipped.

## Subsequent Wi-Fi and launcher hardening

Home and the optional boot warning no longer exit to Anbernic on B/Escape.
Settings → System → Return to stock remains a deliberate, confirmed recovery
route. The diagnostic input probe retains its B exit.

Wi-Fi scans identify saved station profiles by actual SSID, not profile label.
Saved networks offer Connect and Enter password. Connect activates a UUID and
prefers the highest-priority/recently used matching profile. A newly entered
password uses a unique candidate name: upstream NetworkManager 1.44.2 otherwise
skips its password prompt when a saved profile already has a secret, silently
ignoring stdin. Only a failed candidate created by that operation is deleted;
existing profiles remain intact. A verified candidate is preferred for future
autoconnect. Failure to set this preference never tears down a verified join.
Network status resolves the current SSID, so generated profile names do not
appear as the connected network. Passwords remain off argv and application logs.

These changes require a real changed-network/password acceptance test. Read-only
saved-profile enumeration and scanning do not prove a successful new join.
Boot-logo classification was corrected separately; see [the exact FIT analysis](../BOOT.md).

The new Wi-Fi path was exercised read-only on the installed release: scan took
1.115 seconds, detected the saved network and verified the actual current SSID.
No network switch, radio reset or saved-password replacement was performed in
that smoke check. The eight focused regression suites passed 170 tests in total
(boot image, branding, userspace branding, boot control, device services, library
UI, boot warning and settings). Both host and AArch64 builds passed.
