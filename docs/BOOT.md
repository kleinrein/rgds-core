# RG DS startup and stock handoff

Current baseline: Anbernic Linux with the October 12 RK817 saved-state repair.
Normal restart, stock and RGDS Power off, and normal Power-button startup passed.
The owner now requests RGDS direct startup without an accidental Back-to-stock exit.
See [the power diagnosis](archive/POWER_WIFI_2026-10-12.md).

System → Startup offers Anbernic or RG DS, a Touch to continue switch, and Restart. Anbernic remains the default until RG DS startup is explicitly enabled on the device. Desktop preview simulates these choices. The optional RG DS startup path had not been installed on hardware at the end of the 2026-10-07 session.

The RG DS route uses the stock supervisor's `/mnt/mod/ctrl/autostart` hook. It runs one foreground RGDS session before the stock frontend. Returning to Anbernic returns from that hook; the original supervisor starts its own frontend. An APPS launch uses an exclusively owned `/tmp/stopAPP.ini` marker and releases it before returning control to the supervisor. RGDS no longer launches a captured stock frontend executable with its old environment.

The autostart hook executes after the early boot splash. Selecting RG DS in Startup alone therefore cannot remove the first Anbernic logo. On a supported unsigned boot image, the separate branding installer can replace both boot-image logo stages and existing userspace logo files. The October 11 inspection mistakenly classified an empty signing template as a signed image. The October 12 parser distinguishes the exact unsigned stock template from an actual signature; the current stock image can be patched without changing its kernel or device tree. Kernel startup, vendor init, Wi-Fi initialization and Weston retain their existing sequence. RGDS does not kill/restart network services or modify their startup scripts. An autostart hook written by someone else is not overwritten blindly: it is saved as `original-autostart` in the control folder, RGDS's hook runs first and then runs that saved hook, and `Uninstall RG DS Core` puts it back byte for byte. A symlinked hook is still refused.

## Fast entry

The warning uses original RGDS artwork and text inspired by DS/DSi: a small warm caution symbol, centered comfort reminders, open spacing, and a clean stylus above Touch to continue. The lower panel pairs the wordmark with a native dual-screen emblem: the displays settle into place, light sweeps across the glass, and small pixels orbit the hinge. Welcome motion refreshes at most 30 times per second; disabling animations keeps the entire scene static. The prompt remains fully legible throughout the animation. There is no mandatory countdown. A fresh touch release, A or Start continues; a key held during boot does not dismiss the screen accidentally. The screen can be disabled in Startup.

Startup hashes owned executable/runtime/theme/font files once and checks the complete release inventory with one directory traversal per verification. The normal boot route loads the verified session entry in the same Python interpreter and passes its receipt directly, avoiding a second interpreter and receipt-file handoff. A changed release path or mismatched cached dependency uses a fresh interpreter and private inherited descriptor instead. The session reuses hashes only when all inspected file identities, sizes and change times still match; a standalone boot session still verifies its release. Environment, import path and signal handlers are restored on return. Large engine payload hashes and installation-only boot artwork hashes are deferred until needed. Enabling a startup target and installing/activating any release still verify the full package. Hardware status helpers and the initial game scan wait until the warning is dismissed, or until Home's first frame when the warning is disabled. Game discovery runs in a separate process so scrolling and touch stay responsive.

Boot verification and session imports avoid the emulator, archive and game-menu modules. Shared process identities and small INI records live in lightweight modules while their previous APIs remain available to callers. Normal session launches use the built-in light/dark themes directly, avoiding repeated reads of an explicit default theme override.

The first Home frame prepares only the 16/20/28-point rounded atlases; the warning also prepares 36-point. Pixel mode prepares no rounded atlases. Remaining rounded sizes are checked before entering Home after the warning or switching from Pixel to Rounded. The shader catalog waits until after presentation/continuation. The warning also defers saved system preferences and timezone setup; Home still loads them before drawing its clock. All startup font hashes remain verified, so reduced font parsing does not imply an equivalent reduction in physical card reads.

No artificial sleep, Wi-Fi wait, full-card scan or emulator preload is added before the warning. After both panels' first frame is presented, the launcher acknowledges it over a private nonblocking socket. Only then does the session guardian start the reviewed Wi-Fi-only recovery worker. Existing adapters are left alone; failed or unavailable Wi-Fi is recorded without blocking the launcher or forcing a stock fallback. The guardian reaps/stops its owned worker during cleanup.

`userdata/session/boot-startup.json` records verification time, dispatch mode/session import time when available, hook-to-first-frame and session-to-first-frame timing. The guardian samples the acknowledgement, so these can include up to about 100 ms of acknowledgement scheduling delay. On Linux, kernel-to-first-frame uptime is also recorded; this excludes the bootloader and is not power-button-to-screen timing. `userdata/session/boot-network.json` records deferred network readiness separately. `/run/rgds-boot-result.json` records dispatch mode/overhead and the session's lifetime. Full power-on-to-warning/menu time has not been measured on hardware.

## Startup speed, SD trim and power-off (October 13)

With RG DS startup enabled, the kernel-to-menu time measured 12.3–13.6 s
(previously 25.6 s). Two causes were removed; see
[the dated report](archive/SPEED_POWER_2026-10-13.md):

- Stock `S99fstrim` re-trimmed every SD partition at boot (13 s of card I/O)
  and hourly. `runtime/system_tuning.py` clears only its execute bit while RGDS
  owns startup and runs one idle-priority trim per week instead.
- Startup and engine-launch verification memoize file hashes per release in
  `userdata/cache/verified/`, keyed on size and times; any change re-hashes,
  and a daily idle-priority audit re-checks memoized files.

The same module adds a final `::shutdown` step to the reviewed stock
`/etc/inittab`. For an RGDS-requested power-off only, once every block
filesystem is read-only, it powers the RK817 off by register (DEV_OFF) instead
of the SLEEP-pin path that left the power key unresponsive for 20–30 s.
Choosing Anbernic startup or APPS → Uninstall RG DS Core restores the trim script's
mode and the original inittab.

## One RG DS identity from early boot

`assets/boot/` contains original static RG DS artwork generated from the bundled brand wordmark and the dual-screen emblem by `python3 tools/build_boot_assets.py`. Both early logo slots receive identical 640×480, top-down, uncompressed 24-bit BMP pixels. Existing userspace boot PNGs receive the same composition; the narrow waiting renderer receives the wordmark. Firmware artwork remains static. With the early service installed, animation begins when Linux exposes the DSI graphics device; otherwise it begins when the native launcher owns the displays. There is no mandatory delay or minimum animation duration. The library-ready event can interrupt the boot scene immediately.

The SD installer applies both parts after activating a release through
`runtime/boot_identity.py` (best effort: a failure keeps stock artwork and the
installation still completes; skipped on the RG DS Plus), and APPS → Uninstall
RG DS Core runs its `remove` to put stock artwork back. Re-running the installer
with newer artwork refreshes both, keeping the original backups.

The early power-on animation is a separate, reversible userspace service. Install
with `python3 -B runtime/boot_splash_install.py apply` on the stock handheld;
remove with the same command and `remove`. It installs only owned files under
`/usr/local/share/rgds-boot-splash` plus `S12rgds-splash` and `S48rgds-splash`.
It never replaces a vendor script or writes a boot partition. Stock mode,
disabled RGDS routing, an installation in progress, a recovery marker, or an
already running Weston cause it to skip startup.

The service uses the two existing 640×480 DSI modes and one shared linear DRM
scanout buffer. At S48 it drops DRM master before stock Weston starts. The
firmware's `weston --warm-up` preserves the existing scanout until the first
application, so animation continues in that retained buffer while the compositor
and launcher prepare. The service exits only after both CRTCs replace its
framebuffer, or after its 40-second watchdog. It never restores display modes
after handing ownership to Weston. A bounded cleanup barrier waits for master
release, not for the animation's duration. The earliest firmware-only screen
remains static. UI reduced motion controls the launcher; remove the separate
early service to retain a static power-on splash throughout.

On October 12, this path was installed and reboot-tested at the user's supplied
handheld address. The verified run drew 264 frames on both panels from kernel
uptime 5.97 s to 21.15 s, released master at 6.66 s, and exited when both scanouts
were replaced. The launcher acknowledged its first frame at 20.833 s. These are
kernel-uptime observations, not power-button timing or a claimed speedup.
Temporary frame capture was disabled after validation. Evidence is saved in
`build/boot-ux/device/`; focused coverage is in `tests/test_boot_splash.py`.

`runtime/boot_image.py` parses the actual FIT tree and its Rockchip RSCE resource rather than guessing an offset or replacing matching hash bytes. It validates the original resource and entry hashes, patches only `logo.bmp`, `logo_kernel.bmp` and their corresponding hash fields, and verifies the output. Image sizes, kernel, device tree and other resources retain their existing bytes. Signed, encrypted, compressed, corrupt or unsupported layouts are rejected. The format follows the [FIT specification](https://fitspec.osfw.foundation/) and [Rockchip resource format](https://github.com/rockchip-linux/u-boot/blob/next-dev/tools/rockchip/resource_tool.c).

For a full boot-partition snapshot saved from the active device, prepare a separate image locally without writing to a device:

```sh
python3 tools/boot_image.py inspect --image /path/to/original-boot.img
python3 tools/boot_image.py prepare --image /path/to/original-boot.img --logo assets/boot/logo.bmp --output /path/to/rgds-boot.img
```

The device installer is `runtime/boot_branding.py`. Its read-only plan requires an explicit boot-device path proved against the active boot source and known boot alias; it never falls back to a guessed partition number. Before applying, it recomputes the logo patch from the supplied original image and rejects any other image changes. Durable original boot/userspace backups and an installation manifest are stored under `/userdata/rgds-boot/branding`. Applying requires the current partition to match the original snapshot, and completed writes are read back and checked. Restore checks ownership before returning the original bytes and file modes.

From the installed release directory, with complete original/prepared images already saved and an original copy retained on the development host:

```sh
python3 -B runtime/boot_branding.py plan --boot-device /dev/disk/by-partlabel/boot --original /path/to/original-boot.img --image /path/to/rgds-boot.img --assets assets/boot
# After reviewing the plan:
python3 -B runtime/boot_branding.py apply --boot-device /dev/disk/by-partlabel/boot --original /path/to/original-boot.img --image /path/to/rgds-boot.img --assets assets/boot
python3 -B runtime/boot_branding.py restore --boot-device /dev/disk/by-partlabel/boot
```

Use only a boot alias actually present on that device. The writer updates changed 512-byte sectors rather than rewriting the entire partition; all original bytes outside the approved logo/hash fields remain identical.

This changes the branding of the active boot image independently of the launcher routing marker. Stock startup can be chosen while leaving the RG DS splash in place; use the branding installer's restore operation to restore the vendor artwork. The installer has passed synthetic fixture tests, but the actual device FIT layout and a cold boot must still be checked before accepting the RG DS-only sequence as validated.

## Recovery and clean restart

B/Escape stays at Home and cannot exit the boot warning. Returning to Anbernic requires the confirmed System → Return to stock action; the diagnostic probe retains its explicit B exit.

The hook falls through immediately in Anbernic mode. An interrupted installation is disabled until complete. Failed startup falls back to stock, and `/run/rgds-boot-attempted` permits only one RGDS attempt per kernel boot, avoiding restart loops after a crash or Return to stock.

Restart and Power off prepare a request for the current session. It becomes eligible only after success is published and the frontend acknowledges that terminal action. Ordinary Back/Return to stock cannot dispatch a leftover request, including after a failed/cancelled worker or rollback error. The frontend exits, the guardian stops owned workers and restores journalled display/clocks/wake settings, and stock init performs the normal action. A running game, foreign request or incomplete cleanup blocks dispatch. There are no forced reboot, SysRq or broad `killall` paths.

Hardware writes and recovery share a process lock, including the full suspend/resume operation. A competing helper fails after a bounded lock wait instead of overwriting another helper's recovery state. New recovery journals include the kernel boot ID: after a hard reset, recovery discards the previous boot's values instead of replacing freshly initialized vendor defaults. Older journals remain readable for recovery.

Menu sleep uses the installed `pm-suspend` helper so the firmware's suspend/resume hooks can run. It never writes directly to `/sys/power/state` or manually blanks the panels. Missing helpers disable menu sleep and lid sleep; helper failure is reported without a raw-suspend fallback. Helper availability alone does not establish hardware reliability.

`APPS → Uninstall RG DS Core` puts the stock autostart hook back from its backup (or only removes `/userdata/rgds-boot/enabled` when the hook was changed by someone else) and removes the application; it leaves vendor init files and user saves intact. System → Startup is the everyday way to boot into Anbernic instead.

For an installed release, inspect or plan without changing startup:

```sh
python3 -B /path/to/rgds/releases/<build>/runtime/boot_install.py inspect --root /path/to/rgds
python3 -B /path/to/rgds/releases/<build>/runtime/boot_install.py plan --root /path/to/rgds
```

On the RG DS, `enable`, `stock` and `uninstall` configure the owned hook. `--no-warning` skips Touch to continue. Use the System screen for ordinary operation.

## Hardware validation still required

The tested `/mnt/mmc` card was read-only during the 2026-10-07 investigation. Games failed while writing session records before the emulator was started. Do not enable direct startup from that installation until the filesystem is repaired or replaced; use a verified writable installation for further tests. Existing saves/settings were backed up before shutdown.

On the next device session, compare stock boot with RG DS warning enabled/disabled, then repeat Return to stock, Wi-Fi reconnect, normal stock restart, RG DS restart, and power-off. Include a terminated launcher and a missing/damaged release. Verify touch and game launching separately before accepting the boot option as validated.


### Corrected stock FIT classification — 2026-10-12

The stock image contains `/configurations/conf/signature` with only `algo`,
`padding`, `key-name-hint` and `sign-images`. It has no `value`, `hashed-nodes`
or `hashed-strings`. This is an unpopulated signing template, not a completed
signature. The earlier blanket node-name rejection was overly conservative.
The parser now accepts only this exact metadata-only stock template, preserving
its bytes; actual signatures, partial signing output and unknown templates are
still rejected. See [U-Boot's signature format](https://docs.u-boot.org/en/latest/usage/fit/signature.html)
and the independent [RG DS stock-logo procedure](https://gist.github.com/shauninman/8398c1577cb362b3119e8c6a722a1bf2).

The October 12 candidate was prepared against the saved 64 MiB stock partition
with SHA-256 `d18c142e201666c6eb04cffda45808aab24afb72cdf81d1eb00b6aca1a5d2622`.
Only both logo payloads, their RSCE SHA-1 entries and the resource's FIT SHA-256
change. Kernel, both device-tree copies, battery artwork and all other bytes
remain identical. The installer must still match the live partition to that
original, preserve durable backups and verify the complete result before reboot.
Logo visibility is a physical boot check, separate from file/hash validation.

### Installed application and artwork — October 12

Release `0.0.1-dev-0d1d0e29995d` was deployed with DSperate still the default.
The live boot partition matched the saved original. The installer backed up the
complete partition and seven userspace logo files, applied the reviewed patch,
and verified the complete result after reopening it. Installed SHA-256:
`d91af3f3285198729db69e1174b3cc14ff82e066bd77d222babda3d21e41e117`.
Originals and the manifest are at `/userdata/rgds-boot/branding/`.
Direct startup now targets `/mnt/mmc/rgds`, with Touch to continue disabled.
The normal restart returned with boot ID `5a233cc2-3cdd-4281-bef7-e62da8d9f5d3`.
Only the RGDS launcher was running; neither stock frontend was present. The owner
confirmed the replacement logo, direct entry to RGDS and repeated B presses
staying in RGDS. Artwork is provisional: the owner will choose a new project name
before final branding. The first frame was acknowledged 18.633 seconds after
kernel startup (4.028 seconds after the stock hook, including 1.952 seconds of
verification). This is not a Power-button-to-menu measurement. Deferred Wi-Fi
found the existing interface in 0.003 seconds without resetting it.
Evidence is retained in ignored `build/identity-wifi-20261012/`.
