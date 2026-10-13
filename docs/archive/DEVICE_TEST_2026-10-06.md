# RG DS hardware checks — 2026-10-06

Private development build `0.0.1-dev-34aa7430508d`, official Anbernic Linux, SDL 2.32.10 / Wayland / Mali-G52. Tested over SSH with New Super Mario Bros. This verifies that representative game and these code paths; it is not an all-games compatibility claim.

## Installation used

The game partition `/mnt/mmc` reported FAT filesystem damage and remounted read-only (`fat_free_clusters: deleting FAT entry beyond EOF`). A read-only `fsck.fat -n` found broken chains in older RGDS files. No filesystem repair or remount was performed. Selected game saves and non-credential settings were backed up to a local, ignored archive.

Hardware testing therefore used a separate writable internal installation, with ROMs read from the original partition. The stock OS, ROMs and original installation were not replaced. The original APPS shortcut still pointed to the older SD installation.

```sh
make run DEVICE=root@192.0.2.10 SD_ROOT=/path/to/writable-install
```

Replace the documentation-only address and install path with the values for your device.

Repair or replace the game partition before returning to the normal SD installation and development watcher.

## Confirmed

- The device operator confirmed touch, theme selection, both screens, cartridge dragging, held-button acceleration and first/last wrapping.
- Both backlights followed 20% → 80% → 60% (51 → 204 → 153 on both 255-step drivers).
- The hardware date/time uses Europe/Oslo. Actual battery and charging status appeared in captures.
- The menu is composed into the engine framebuffer. Its helper uses software canvases, opens no compositor window, and preserves renderer viewport/scale/clipping and GL state.
- Sound stayed at DAC 0 throughout the successful game tests. Direct test sessions can restart stock, whose frontend may reapply its own mixer setting; mute was restored after failed startup experiments.

| Engine | Overlay acknowledgement | First checkpoint | Load dispatch | Save and normal exit | Cleanup |
| --- | ---: | ---: | ---: | ---: | --- |
| DraStic | 713 ms | 1,526 ms | 818 ms | 3,045 ms | Game marker and guardian lock cleared |
| DSperate | 768 ms | 1,040 ms | 1,014 ms | 3,744 ms | Game marker and guardian lock cleared |
| NNDDSS | 662 ms | 2,937 ms | 811 ms | 3,039 ms | Game marker and guardian lock cleared |

The load UI deliberately reports dispatch. DSperate's native log also reports loading its checkpoint frame; visual gameplay restoration and battery-save persistence still require longer play/reboot checks.

## Fixes established during this session

- Separate transparent full-screen menu windows stalled the device. They were replaced by shared RGBA canvases rendered inside the emulator; a missing rendering acknowledgement now restores game controls after three seconds.
- The old hook handled menu input before outer filters. RGDS uses vendor input APIs and disables the legacy common-menu controller while retaining rendering support. Both observed Anbernic menu key codes (312 and 354) use the same menu.
- A tick sampled before a command's later start time could underflow an unsigned subtraction and immediately time out saving or prematurely acknowledge loading. Timers now clamp negative elapsed intervals; a clock-advancing regression test covers this.
- DraStic's virtual save button emits one polled press instead of repeatedly starting writes during a held pulse.
- NNDDSS finds BIOS/assets beside its executable despite `--root`; its executable now lives beside the writable prepared assets and privately copied stock BIOS.
- NNDDSS's fast-upload code patch was unstable with hooks on this card. `NNDDSS_FAST_UPLOAD=0` is the tested configuration.
- NNDDSS quick F5/F7 uses slot 8. Save acknowledgement, load, auto-load import and manual-slot backup now use that slot.
- Exclusive evdev grabs are suppressed for the managed shared menu so touch, menu and volume clients can coexist.

## Still to check

Final physical button/touch return-home feedback, sustained gameplay across more titles, sound at intentionally chosen audible levels, state restoration after a reboot, each shader preset's appearance/performance, fast-forward behavior in each engine, and hardware recovery after power loss. Lid sleep remains unavailable until the hall sensor's open/closed values are measured.

Raw smoke logs and native framebuffer captures are in the ignored `build/device-20261006/` folder. Account/network credentials were excluded from the backup.
