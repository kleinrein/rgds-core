# Offline stability and startup work — 2026-10-07

At the end of this investigation, the device was shut down. No deployment, boot installation, filesystem repair or additional hardware test was performed during this pass. The earlier upload attempt failed on its first SSH connection, before uploading or activating anything.

## Regression evidence captured before shutdown

The original `/mnt/mmc` card was mounted read-only. Games failed while writing `userdata/session/core.json`, before starting the emulator. Settings and brightness writes also failed there. A separate internal test installation remained writable.

The Goodix panel exposes Type A multitouch without `ABS_MT_SLOT`, with X 0–640 and Y 0–480. Touch decoding now handles this framing and allows SDL finger fallback while a raw device is open. Duplicate routes cannot trigger the same gesture twice.

Saves/settings were preserved in a separate backup: 168 files, 25,003,233 bytes, with no copy errors. The damaged FAT filesystem has not been repaired.

## Resulting source behavior

- Stock owns frontend restart. RGDS releases only its own stop marker before returning to the original supervisor, avoiding competing frontend launch paths and inherited integration environments.
- Cleanup uses recorded PID/start identities and protects NetworkManager, wpa_supplicant, D-Bus and Weston. Status polls use cached Wi-Fi information without rescanning or changing the radio.
- Normal Restart/Power off waits for owned worker and journalled hardware cleanup. It uses stock init commands, without forced reset or SysRq. Failed requests cannot become a later surprise action.
- System → Startup can select Anbernic or direct RG DS, with an optional DS/DSi-inspired Touch to continue screen. Both direct startup paths display before game scanning and hardware status work; scanning runs separately. The original kernel splash and vendor initialization remain intact.
- Startup failure returns to stock once per boot. Recovery can disable routing even with a read-only card. Selecting Anbernic does not depend on healthy emulator payloads or writable game storage.
- All adapters use the owned RGDS bridge/menu/audio/input/filter integration. SimpleOS hooks, boot scripts, OS helpers and compatibility profiles are excluded. External engine binaries/shims, vendor DraStic touch support and community preset data remain third-party components.
- Packaging uses a separate 38-file engine bundle with exact inventory/SHA-256 validation, copies only verified manifest entries and checks the copied hashes again. Original license/notice files remain present.

## Offline validation

The complete offline suite passed 330 tests in 208.512 seconds, and the host launcher self-test and Linux AArch64 build passed. Final recovery review added targeted checks after that full run; their results and the final archive are recorded in `build/stability-validation.json`.

Both light/dark warning previews were inspected. ELF dependencies of the provided engine payloads contain no SimpleOS compatibility library. GL tests use a typed driver and software SDL fixtures; actual Mali rendering, game FPS, physical touch, audio, Wi-Fi handoff, stock reboot and power-on timing remain hardware checks. Source hashes and logs are under `build/stability-source-hashes.json` and `build/stability-tests-20261007.log`.

Use the writable internal installation for the next deployment, and follow [NEXT_DEVICE_TEST.md](../NEXT_DEVICE_TEST.md). The new boot option remains uninstalled until that device session.
