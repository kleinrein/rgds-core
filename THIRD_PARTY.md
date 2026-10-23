# Third-party components and provenance

RG DS Core is released under the GNU General Public License, version 3 or later (see [LICENSE](LICENSE)). Community shader presets are provisioned into the ignored `vendor/engines/` bundle; their notices are retained below.

| Component | Source | Treatment |
| --- | --- | --- |
| Network file sharing | [Samba](https://github.com/samba-team/samba/tree/samba-4.25.0), pinned version 4.25.0 and source SHA-256 in `tools/build_samba.py`. | GPL-3.0-or-later ARM64 source build. The private package includes the full corresponding source, a small relocation patch, build recipe, license, build/ABI records and copyright notices for private runtime libraries under `services/samba/`. SMB1, printing, directory services and firmware installation are disabled. |
| SimpleOS (historical) | [boorngos/SimpleOS](https://github.com/boorngos/SimpleOS), package version `20260927`. | MIT; notice retained below for previously imported material. No SimpleOS code, scripts, profiles or hooks are used or bundled. |
| DraStic and Nintendo BIOS | Existing official Anbernic Linux card. | Not included in source or ZIP. The runtime locates existing stock files. |
| Private independent-image DraStic application | Owner's existing AArch64 executable/display-touch adapter, imported separately with `tools/capture_drastic.py` or `tools/provision_drastic.py`. | Not stored in Git. Exact inventory/ELF hashes; private testing only. OS libraries, BIOS dumps and user saves are excluded. This is a proprietary application import, not a source rebuild or resolved public redistribution license. |
| DSperate | Upstream [beebono/DSperate](https://github.com/beebono/DSperate), pinned commit `1b76c355109c9f7576363ccc023927b3137d3c6f`. | Source-built Linux AArch64 engine with JIT/NEON/LTO and Cortex-A55 tuning. GPL-3.0-or-later license, corresponding patched source, host build driver, portability patch and build hashes accompany the binary. The supplied binary stays in the reference bundle and is excluded from new packages. |
| melonDS engine and original RGDS frontend | Upstream [melonDS-emu/melonDS](https://github.com/melonDS-emu/melonDS), pinned commit `906e9ebb27da8c6a715cd7abab4abfe8a8d29427`. | Unmodified GPL-3.0-or-later core with ARM64 JIT/LTO and an original RGDS SDL frontend under GPL-3.0-or-later. Packaged at `emulators/melonds/` with upstream license, full corresponding core/frontend source archive and hashed build record. Uses upstream FreeBIOS/generated firmware; no Qt, downloaded games or Nintendo BIOS. Synthetic ARM64 VM checks are separate from pending handheld/retail-game acceptance. |
| mGBA (RG DS Play) | Upstream [mgba-emu/mgba](https://github.com/mgba-emu/mgba), pinned commit `143adf5aa2a60c6036810b0cd72445e13d74b0e6` in `tools/build_play.py`. | MPL-2.0. Unmodified libmgba (GBA, GB and GBC cores, with its bundled zlib on the handheld) linked statically into `play/`. Packaged at `emulators/rgds-play/` with `COPYING.mgba` (MPL-2.0), `COPYING` (GPL-3.0) and `source.tar.gz` (RG DS Play's sources and the pinned mGBA tree); `build.json` records the commit, file hashes and ELF ABI (`tools/build_play.py`). |
| GBA DS (idea and profile facts) | [kyannon/gba-ds](https://github.com/kyannon/gba-ds), GPL-3.0. | No code copied. RG DS Play's companion screen is our own implementation of GBA DS's shadow-copy map idea; the memory addresses and pixel probes in `play/profiles/gba/AMTE.ini` and `A2CE.ini`, and the racing register checks in `AMKE.ini`, were first found by GBA DS and are credited in those files. |
| Game Boy disassemblies (profile facts) | [pret/pokered](https://github.com/pret/pokered), [pret/pokecrystal](https://github.com/pret/pokecrystal), [pret/pokegold](https://github.com/pret/pokegold) symbols, [zladx/LADX-Disassembly](https://github.com/zladx/LADX-Disassembly), [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm), [metroidret/M2RoS](https://github.com/metroidret/M2RoS), [ElectroDeoxys/warioland3](https://github.com/ElectroDeoxys/warioland3). | No code or data copied. RAM addresses, record layouts, character sets and the locations of the games' own tables in `play/profiles/gb/*.ini` and `play/src/companion/party_gb.cpp` / `automap.cpp` come from these projects and are credited in those files; graphics are read from the player's own cartridge at run time. |
| GBA colour model (GBA Color, GBA screen presets) | Pokefan531's GBA LCD colour model, public domain, as published in libretro's `handheld/shaders/color/gba-color`. | Its gamma, luminance and colour-mixing constants are used in our own presets `assets/shaders/gba_color.dsd` and `gba_screen.dsd`; no shader file copied. |
| Shaders | Local `system/shaders/` presets. | Keep supplied README/attribution; audit individual notices before public packaging. |
| SDL2 | Installed host library and headers; existing vendor device library. DSperate links against a source-built SDK pinned to official SDL2 2.32.10. | DSperate's SDK is link-time only; standard adapters use the card SDL. SuperDrastic bundles a separate private SDL 2.32.10 runtime with dummy video, software rendering, Linux joystick/input and ALSA. Its zlib license, pinned source archive, build options, ABI and hashes accompany the library. |
| Nunito (DS rounded option) | [Google Fonts Nunito](https://github.com/google/fonts/tree/main/ofl/nunito); exact commit in `assets/fonts/source/PROVENANCE.txt`. | SIL Open Font License 1.1. Unmodified variable source font, full license and baked weight-600 atlases are bundled. This is a DS-inspired alternative, not Nintendo's original font. |
| New UI and native integration | Source in this workspace. | Original launcher, vector shapes, probe font/palettes, shared bridge/menu/audio/GL filters, input routing, startup warning and owned engine profiles. No Nintendo or ROCKNIXDS theme artwork/sounds copied. Profile keys were checked against engine source or its supplied configuration interface. |
| ScreenScraper artwork and metadata | Downloaded on user request through [ScreenScraper API v2](https://www.screenscraper.fr/webapi2.php). | Cached per user and credited on the top screen. No downloaded artwork or account credentials are bundled in releases. |
| Shader studio reference scenes and UI icons | Original repository code in `tools/build_shader_previews.py` and `src/launcher/settings_visual.hpp`. | Three original scenes, rendered through the attributed supplied presets on host SwiftShader GLES2 and stored losslessly. No game screenshots or third-party character artwork included. Source/pixel hashes and renderer provenance accompany the previews. |
| DS banner icons/titles | Read from a user's local ROM files. | Displayed locally, with ZIP icon caching. No game icons or ROMs are included in releases. |

Historical MIT notice retained for the material it covers:

Packages additionally record each ELF component’s SHA-256, source label and required GLIBC versions. Packaging reads the independent `vendor/engines` bundle with exact inventory verification. The one-time importer excludes OS scripts, compatibility hooks, Python/shell helpers, BIOS and ROMs. External engine binaries, loaders/shims and community preset data remain third-party components, rather than RGDS's emulation implementation.

The DSperate v3.0.0 source is pinned at [commit `1b76c355109c9f7576363ccc023927b3137d3c6f`](https://github.com/beebono/DSperate/tree/1b76c355109c9f7576363ccc023927b3137d3c6f), including `src/frontend/sdl/main.cpp`, `input.cpp` and `configs/default.ini` for hotkeys and state filenames. `tools/build_engines.py` applies recorded Clang portability edits in an isolated build checkout, builds with Zig, and packages corresponding source and notices alongside the executable. DraStic is not represented as a source-built RGDS engine. Wi-Fi commands follow the [NetworkManager nmcli manual](https://networkmanager.dev/docs/api/latest/nmcli.html).

```text
MIT License
Copyright (c) 2026 SimpleOS for RG DS contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```


## Optional public artwork services

GameTDB DS covers (https://www.gametdb.com/DS/Downloads) and Libretro thumbnails
(https://github.com/libretro-thumbnails/libretro-thumbnails) are fetched only on
request into the player's writable cache. They require no RGDS app identity or
player login. Provider attribution is retained per cover and shown in the UI.
Cover scans remain the property of their respective rights holders; public
availability is not a grant to relicense them as RGDS assets. Downloaded test
covers are not included in the release package. Renamed-title matching downloads
Libretro's public No-Intro DS serial/name catalog on demand from
https://github.com/libretro/libretro-database/blob/master/metadat/no-intro/Nintendo%20-%20Nintendo%20DS.dat
into the player's optional cache; it is not bundled or relicensed by RGDS.

`assets/shaders/soft_scanlines.dsd`, `warm_paper.dsd`, `cool_crisp.dsd`,
`pocket_green.dsd`, `amber.dsd`, `soft_lcd_grid.dsd`, `ds_classic.dsd`,
`ds_lite.dsd`, `natural_glow.dsd`, `vivid_lcd.dsd`, `natural_smooth.dsd`,
`vivid_smooth.dsd`, `pixel_beads.dsd` and `glossy_tiles.dsd` are
original RGDS single-sample effects, separate from the supplied shader presets.


## ROCKNIXDS review and DraStic startup adaptation

`src/platform/drastic_threads.hpp` adapts the helper-thread initialization barrier
from [ROCKNIXDS patch 5e14c95a](https://github.com/JorreFog/ROCKNIXDS/blob/8b8f72cd70f0d198612ea8e6651fae71664aa97b/dsflip/superdrastic-0.5.0-beta.1-rocknixds.7.patch).
It applies only to the recognized DraStic executable build ID and two helper
entry points. It does not include SuperDrastic’s display backend, rasterizer,
CPU governor or its binaries. RGDS adds exact identity gating, independent gate
lifetimes, successful-initialization counting and a monotonic timeout. The idle
polling investigation also informed RGDS’s independently written event wait.

This notice is included in every application package:

```text
MIT License

Copyright (c) 2026 JorreFog and the ROCKNIXDS contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Optional SuperDrastic backend

The private package includes a locally compiled SuperDrastic `0.5.0-beta.1` from
https://github.com/JorreFog/SuperDrastic at commit
`930c4c5bb3e904557cd8cc42a01a4e27834f5ba1`, tree
`6d2d72fdc27ac2010ca73f3d604f1e6e9e5d7722`. The tracked input hash is
`92d936303f349ba2a8f078c28e29d7a2f822b13894f503814b606c9b03075056`.

Upstream code is MIT licensed, copyright 2026 JorreFog. Its `LICENSE` and
`THIRD_PARTY.md` accompany the library, including rcheevos, stb, ARM rasterizer
references and shader/FSR notices as recorded by upstream. Its full pinned
source, RGDS patch/build driver and native adapter source accompany the build
in `emulators/superdrastic/source.tar.gz`; SHA-256s and ABI are in `build.json`.
The exact-build helper-thread adaptation above is also compiled into this
backend; its ROCKNIXDS MIT notice continues to apply. The optional library does
not include or relicense the proprietary DraStic executable.

RGDS adaptations add its command/exit protocol, fixed menu navigation, hardware
status/control bridge, spaced native profile compatibility, disabled implicit achievement login, readiness ordering,
independent auto-save policy and original launcher supervision. Upstream runner,
updater and service/clock scripts are not executed. The alternative rasterizer
is compiled as part of upstream but disabled in this profile. See
[SUPERDRASTIC.md](docs/SUPERDRASTIC.md) for integration boundaries and acceptance.

SuperDrastic's private SDL source archive is pinned to SHA-256
`5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165` from
[SDL release 2.32.10](https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10).
`emulators/superdrastic/sdl` contains the runtime, original `LICENSE.txt`, full
source archive and build record. SDL uses the zlib license. This runtime is
selected only for SuperDrastic; it does not replace the OS library. DSperate's
RGDS patch additionally exports a completed-emulation-frame counter; no frames
are fabricated from display refreshes.

## RG DS Experimental fork

The optional `rgds-experimental` engine (DSCore) is built from `dscore/`, a fork of
the same pinned GPL-3.0-or-later DSperate source (see [dscore/FORK.md](dscore/FORK.md)).
Original RGDS changes are GPL-3.0-or-later.
Packages include its LICENSE, NOTICE, the complete fork source and
build recipe. See [fork documentation](src/experimental/rgds/README.md). No NooDS,
or melonDS implementation has been copied into this fork.

The optional enhanced binary also includes the modified upstream Vulkan
rasterizer under GPL-3.0-or-later, its full source and build recipes in
`vulkan-source.tar.gz`. The process-local PanVK/kbase driver is built from
[FristOneRR-Admin/FristOneRR-Panvk-Source](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source)
at `efd07bab401b263a9ac114560ecdb9a2934f5d43`; complete modified driver source
is in `panvk-source.tar.gz`, with Mesa's per-file license headers and notices.
The Ubuntu arm64 Vulkan loader (`libvulkan1` 1.3.275.0-1build1) is Apache-2.0/MIT;
its copyright notice, full license texts and the private zstd notice are in the
engine's `lib` directory. No system driver or loader is replaced.

## Menu sound assets

`assets/sounds/*.wav` contains edited versions of four original source families generated with Suno v6-mini in the user’s account on 2026-10-12. Source URLs, the observed Free account plan and reproduction details are recorded in [docs/MENU_SOUNDS.md](docs/MENU_SOUNDS.md). No Nintendo audio recordings were used.
