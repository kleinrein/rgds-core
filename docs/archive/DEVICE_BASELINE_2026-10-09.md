# Read-only standard RG DS baseline — 2026-10-09

Captured over verified-key SSH while the stock frontend was running. The owner
confirmed this is a standard RG DS. No deployment, launcher/game start, suspend,
restart, shutdown, mount, service change or SD write was dispatched.

Evidence lives in ignored local build output:

- `build/os-audit/20261009-baseline/audit.json` and its SHA-256 sidecar.
- `build/os-foundation/20261009/source-snapshot.json` and 19 selected upstream
  reference files, pinned to ROCKNIX commit
  `c445081a59518f37d9776e5412dd7b14910696f7`; not executed or installed.

## Observed facts

| Item | Observation |
| --- | --- |
| Architecture / board | AArch64; `rockchip,rk3568-deep-lp3-v10`, `rockchip,rk3568`. Stock's generic model string does not distinguish every retail model. |
| Linux base | Buildroot 2024.02, kernel `6.1.141`; PID 1 is `init`. |
| Runtime | Python 3.11.8; glibc 2.41. |
| Memory | `MemTotal` 3,032,700 KiB. This is reported available system memory, not a measurement of launcher use. |
| Panels | DSI-1 and DSI-2 both advertise connected 640×480 modes. No physical rendering test was performed. |
| Storage | `/userdata` ext4 and `/mnt/mmc` VFAT both report `rw` at capture. This inventory did not write-test either mount. Earlier filesystem repair is documented in BOOT.md. |
| Active stock services | Weston, NetworkManager, wpa_supplicant and `muos2.bin` observed. |
| Graphics | Stock processes map `/usr/lib/libSDL2-2.0.so.0.3200.10`, EGL/GLES, `libmali.so.1.9.0` and `libmali-hook.so.1.9.0`. This is an installed dependency observation, not permission to bundle the libraries. |
| Lid / suspend | Hall reading `3`; advertised `freeze mem`, with `deep` selected. No suspend/wake test performed. |
| Battery | `battery` reports 55%, `cw2015-battery` reports 65%. Identify the authoritative supply before changing the UI or drawing battery conclusions. |

## Installed releases differ

| Entry | Installation | Selected release | Previous release |
| --- | --- | --- | --- |
| Stock APPS | `/mnt/mmc/rgds` | `0.0.1-dev-9a9278b47f60` | `0.0.1-dev-52da1812c483` |
| Configured startup target described in BOOT.md | `/userdata/rgds-device-test/rgds` | `0.0.1-dev-f385dc83edc8` | `0.0.1-dev-6cdfaf6225be` |

The read-only probe found each selected session entry and recorded its manifest
hash. It did not verify every installed release file or execute the autostart
hook. Read the actual startup routing in the next session before comparing
results. Do not attribute an APPS test to the startup build or silently deploy
over either selection. Existing previous-release pointers were left intact.

## Local ELF dependency audit

The existing launcher, RGDS bridge and source-built DSperate artifacts require
glibc 2.17. DSperate directly links SDL2; the launcher and bridge also load
graphics/audio APIs at runtime. Supplied NNDDSS loader/shims require up to glibc
2.34 and include SDL2_ttf, EGL/GLES and zlib dependencies.

These are structural ELF metadata from existing build/provisioning files, not
fresh builds or a proof that the installed release matches current source.
Direct linkage is not recursive dependency closure; `dlopen` paths, firmware and
driver compatibility still need separate checks. The source-built SDL SDK uses
dummy backends for linking and is unsuitable as a device display/audio runtime.

Current migration boundaries identified by source inspection are:

- `runtime/session.py`: stock Wayland/socket discovery and SDL selection.
- `runtime/stock_handoff.py` / `runtime/boot_control.py`: vendor supervisor and
  autostart lifecycle.
- `runtime/device_services.py`: NetworkManager commands and vendor integration.
- `runtime/system_controls.py`: vendor pm-utils suspend hooks and hall sysfs.
- `runtime/emulators.py`: DraStic's stock binary/touch dependencies.

They are useful future portability targets. Full OS migration is currently
deferred; existing stock behavior remains the active target.

## Reproduce without changing the device

```sh
# Existing host artifacts only; creates a fresh local evidence directory.
make os-audit

# Read-only SSH; DEVICE is the current verified target, not a hard-coded address.
make os-audit DEVICE=root@DEVICE SD_ROOT=/userdata/rgds-device-test

# Record both installations explicitly when needed.
python3 tools/os_audit.py --device root@DEVICE \
  --install-root /mnt/mmc/rgds \
  --install-root /userdata/rgds-device-test/rgds

python3 -m unittest discover -s tests -p 'test_os_audit.py' -v
```

The tool runs Python on SSH stdin with bytecode disabled and requires an existing
verified host key. It prints device JSON to the host and writes evidence only
locally. Existing evidence directories are never overwritten. Unavailable SSH,
unexpected architecture/schema or malformed output return failure; an error
cannot be mistaken for device acceptance. Missing or oversized read-only facts
remain unavailable. Credentials and saved Wi-Fi profiles are not inspected.

The Wi-Fi/black-screen restart incident remains open. SSH reachability today and
advertised kernel interfaces do not close it. Next steps are in
[the experience plan](EXPERIENCE_PLAN.md) and
[the physical test checklist](../NEXT_DEVICE_TEST.md).
