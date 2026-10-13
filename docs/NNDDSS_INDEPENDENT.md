# NNDDSS without stock emulator assets

RGDS prepares NNDDSS from its own release and writable user-data directory. It
does not discover or copy stock DraStic BIOS, stock fonts, firmware dumps, game
databases, or an NNDDSS executable from an Anbernic installation.

This is engine-level independence. The current launcher session still runs on
the installed Linux system, using its compositor, hardware drivers and ordinary
shared libraries. This change does not create an independent OS image, rebuild
the closed NNDDSS core, or resolve its redistribution provenance.

## Included dependencies

- **BIOS:** Gilead Kutnick's BSD-2-Clause FreeBIOS, from melonDS commit
  `1124603369ce72986b7469bb6fa3520bcb58ab5e`. ARM7/ARM9 replacements are compiled
  from the included assembly, padded to their 16 KiB/4 KiB address spaces and
  installed in both loader and core BIOS directories. They contain no Nintendo
  logo or encryption table. The unpadded outputs match DSperate's embedded NTR
  FreeBIOS bytes. These are direct-boot replacements, not a Nintendo firmware menu.
- **Fonts:** the already bundled OFL Nunito font. The wrapper's fixed font names
  are aliases; they do not imply Orbitron or Source Han Sans is bundled. The
  wrapper language is explicitly English. CJK coverage is not provided.
- **Save-chip metadata:** the pinned melonDS game-code table supplies known
  EEPROM/Flash chip types and sizes. RGDS writes a small compatibility XML for
  the selected ROM using its exact header title and code. The GPL-3.0-or-later
  input sources, license and conversion script ship under
  `assets/nnddss/save-types/`; packaging verifies both original hashes and the
  complete generated table. NAND and unknown chip types are not guessed.
  Custom runtime databases and explicit user databases take precedence.
  The core uses a restricted line reader: the generated file uses its required
  single-quoted declaration/attributes, no comments and an actual ROM CRC32.
  A per-file metadata cache avoids repeating that CRC scan for unchanged ROMs.
- **Board/language settings:** owned `assets/board.ini` and `assets/language.ini`.
  The supplied loader otherwise refuses startup without a vendor board file.
  `runtime/nnddss_assets.py` checks the exact supplied loader SHA-256 and replaces
  only its two constant path strings, preserving all code bytes and ELF offsets.
  An unknown ELF loader fails explicitly instead of receiving a guessed patch.
  `RGds` is the reviewed wrapper's compatibility identifier, not a hardware
  identification claim. Plus hardware validation is still needed.
- **Engine and shims:** the existing verified private payload. Both core paths
  are explicitly set to the prepared runtime. Existing injected core/library
  settings cannot select an unrelated engine. GPU locking is disabled because
  RGDS already owns power policy.

No firmware dump is required for the tested direct-boot path. NNDDSS's default
firmware behavior was exercised on the device with both firmware files absent.
Existing user firmware and saves are retained, but never fetched from stock.
Optional user-supplied `game_database.xml` and `usrcheat.dat` can be placed in
`rgds/userdata/bios/nnddss/`; neither is required by the homebrew probe. For
known retail games without a custom database, the included table provides the
save-chip definition. A definition is not a claim of tested game compatibility.
The runtime leaves every battery save and checkpoint intact.

The October 11 retail check found that New Super Mario Bros. could not erase
its save when both database files were missing. Its chip is an 8 KiB EEPROM
(table type 2); the new fallback provides this definition for each known region.
Host tests verify exact XML, source provenance, launch integration, and save/user
database preservation, including the native parser's restricted dialect and CRC
cache invalidation. A fresh isolated hardware launch with the corrected XML
reached the New Super Mario Bros. intro and created an 8,314-byte `.dsv`
(8,192-byte save plus its 122-byte footer), replacing the previous erase-error
result. This confirms initial save creation; restoring game progress remains
a separate check. Start acceptance from a cold launch: an older emulator
checkpoint can retain a previous chip configuration even after a correct
database is installed.

## Build and verification

```sh
python3 tools/build_freebios.py
python3 tools/build_freebios.py --check
python3 tools/build_nnddss_database.py --check
python3 -m unittest discover -s tests -p 'test_nnddss_independent.py'
python3 -m unittest discover -s tests -p 'test_nnddss_database.py'
python3 tools/build_nnddss_probe.py
```

The BIOS builder needs LLVM clang, lld and llvm-objcopy. Set `LLVM_BIN` if they
are not on PATH or in `/opt/homebrew/opt/llvm/bin`. It builds entirely offline.
The original source and license, source hashes, syntax-only LLVM adaptations,
compiler version, commands, build-driver hash and output hashes are included in
the release. The packaged builder also runs beside its `freebios/` directory.
Packaging checks the source/build records and outputs; per-engine integrity
verification includes the replacement BIOS and source font.
The save-type table can be regenerated offline with
`python3 tools/build_nnddss_database.py --source .cache/melonds-source` using the
pinned source checkout. The shipped conversion script also accepts `--source`
and `--output` explicitly, so it can rebuild from its included `source/` tree.

The supplied loader requires Linux AArch64 and glibc **2.34 or newer**, plus
SDL2, SDL2_ttf, EGL, GLES2 and zlib, with their normal OS dependencies. NNDDSS's
library search uses its private shims and standard Linux library directories.
The engine does not add Anbernic/PortMaster library paths. An explicit stock
SDL override is rejected. A future Linux image must provide working SDL2,
graphics, audio and input backends; the cross-link SDK's dummy SDL is unsuitable.

## Device evidence — 2026-10-09

An isolated installation at `/userdata/rgds-nnddss-independent-20261009/rgds`
ran candidate `0.0.1-dev-617766bbb6b9` with fresh user data. No DS games were
found on the mounted storage, so the test used the original silent homebrew
probe built from `tests/fixtures/nnddss_probe/`, not a commercial ROM.

- NNDDSS initialized its real core, Wayland display, input and muted audio.
- The RGDS pause menu reported a live rendering acknowledgement.
- A 6,296-byte checkpoint was acknowledged and the normal return action exited
  with code 0. The earlier RGDS installation was restarted afterward.
- Decompressing the checkpoint produced 5,558,921 bytes, containing the probe's
  ARM9 program and a complete 256x192 RGB555 framebuffer of `0x840a` pixels.
  This proves the diagnostic executed and wrote the expected VRAM pattern; it
  is not a physical panel screenshot or proof of restored gameplay.
- File-access tracing recorded owned BIOS/configuration/font use and no vendor
  paths from the NNDDSS process. The outer session still inspected its stock
  frontend handoff and SDL discovery paths, as expected for the current OS.
- The core attempted its private firmware/database filenames, found them absent,
  and continued successfully. No stock copies were made.

Local evidence is retained under `build/nnddss-independent-evidence/` and
`build/nnddss-independent-device-result.log`. The compositor denied its
screenshot request; no physical screen capture is claimed. Commercial titles,
ordinary battery-save behavior, load restoration, long sessions, audible sound
and RG DS Plus still need follow-up device checks.

Host validation: 74 focused BIOS/runtime/integration/packaging/integrity tests
passed. The wider 553-test run had three UI failures; all three passed in a
12-test targeted rerun after rebuilding the host preview. Launcher source was
being edited elsewhere during this work, so the device candidate was built from
the frozen `build/nnddss-independent-source/` snapshot.
