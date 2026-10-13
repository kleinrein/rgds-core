# NNDDSS, launch and menu polish — 2026-10-11

Local application work on the working Anbernic Linux baseline. No device
connection, deployment, flash, reboot or power change was performed. The user
reports NNDDSS slow in every game; that report is unresolved until the next
authorized hardware measurement. Host work below does not establish game FPS.

## Changes

- NNDDSS's private libraries now link to the exact versioned, verified release
  on filesystems supporting symlinks. This avoids copying them on first setup
  and comparing both copies on every later launch. FAT/exFAT-style unsupported
  links use the existing bounded-memory copy path. I/O errors, full/read-only
  storage and redirected library directories still fail explicitly. Engine
  release verification remains before preparation, and release switches/rollback
  replace code links without changing saves or configuration.
- The loader's two constant path replacements remain pinned to its reviewed
  SHA-256. No new NNDDSS core binary or unsafe fast-upload patch was enabled.
  BIOS/font validation remains; the font is read once for its two aliases.
- NNDDSS bindings update in one pass; duplicate resolution-config updates are
  removed. DSperate INI updates retain user fields and skip unchanged writes;
  failed publication retains the prior file and cleans up the pending file.
- NNDDSS colour/LCD shader draws no longer query six pieces of vertex/viewport
  state that only sharp bilinear needs. Sharp and its combinations retain the
  actual destination-size calculation, including split layouts. The shared
  shader pipeline only changes/restores texture filters when needed.
- Pending/busy game-menu status checks use 20 ms instead of 100 ms, with the
  event wait capped accordingly. Idle polling stays at 100 ms. The first frame
  reads the supervisor's current checkpoint/FPS/threading status; exact request
  token matching still prevents stale acknowledgements from unlocking actions.
  Game-menu mode skips library scans/index jobs even if ROM roots are supplied.
- A successful or failed live Threaded 3D acknowledgement no longer changes the
  FPS preference. Only successful threading changes persist; the game returns
  to its paused state after the command.

## Measured locally

Evidence is under `build/launch-menu-polish-20261011/`, with a before-source
snapshot, benchmark scripts, fixtures and JSON reports.

| Workload | Before | After |
| --- | ---: | ---: |
| Duplicate NNDDSS library bytes read during one warm preparation | 8,597,588 | 0 on a link-capable home |
| Warm preparation, median of 12 runs on the Mac | 5.11 ms | 4.55 ms |
| NNDDSS NDS Color draw, GLES state queries | 17 | 11 |
| Same draw, texture-filter writes | 4 | 0 |

The library workload uses all eight actual packaged libraries (4,298,794 bytes).
Preparation timing excludes unchanged integrity verification, interpreter/core
startup, ROM loading and display setup. The small warm-host timing change is not
a prediction for SD storage or full game launch time. FAT copy fallback still
reads the two files to compare them.

The real host GLES comparison rendered all 20 effects through all four adapters:
80 before/after frame hashes match. Existing assertions also check shader
failure recovery, GL-state restoration, noninteger scaling, split layouts,
combined textures, BGR/alpha handling and Off. The renderer is a host software
pbuffer, not the handheld GPU or emulator core.

The host and Linux ARM64 builds pass, along with focused launch-preparation,
NNDDSS asset/loader, pause/menu, input/audio/lid, shader, startup and packaging
regressions. The local Ubuntu ARM64 probe passes all 18 melonDS/bridge commands,
including save/load, corrupt-state rejection, pause and live threading, against
the rebuilt bridge. It uses original synthetic homebrew and dummy SDL drivers.
The VM was stopped afterwards. All 63 regenerated preview images retain their
previous hashes while the manifest records the new pipeline source.

Reproduce preparation measurements against an unpacked release:

```sh
python3 -B tools/benchmark_runtime_idle.py \
  --nnddss-app /path/to/rgds/releases/BUILD_ID \
  --output build/runtime-overhead.json
```

For a before snapshot, add `--runtime /path/to/before/runtime`; it must retain
its matching `assets/shaders/catalog.tsv`. This tool does not execute engines.

## Still needs the device

Measure NNDDSS Game FPS with shader Off, native 3D, the same power profile,
clocks and temperature. Then compare NDS Color, Sharp and an LCD preset. Record
launch-to-first-game-frame and Menu-to-first-menu-frame times separately from
save/load latency. Test link-capable and card-only installations, all four
engines, real save restoration and repeated pause/resume. The menu timing change
is a polling bound, not a measured handheld latency guarantee.

See [the acceptance checklist](../NEXT_DEVICE_TEST.md), [save behavior](../SAVES.md)
and [other emulator candidates](../EMULATOR_OPTIONS.md).
