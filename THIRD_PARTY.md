# Third-party components and provenance

RG DS Core is released under the GNU General Public License, version 3 or later (see [LICENSE](LICENSE)). Community shader presets are provisioned into the ignored `vendor/engines/` bundle; no DraStic, BIOS or other proprietary file is shipped.

| Component | Source | Treatment |
| --- | --- | --- |
| Network file sharing | [Samba](https://github.com/samba-team/samba/tree/samba-4.25.0), pinned version 4.25.0 and source SHA-256 in `tools/build_samba.py`. | GPL-3.0-or-later ARM64 source build. The private package includes the full corresponding source, a small relocation patch, build recipe, license, build/ABI records and copyright notices for private runtime libraries under `services/samba/`. SMB1, printing, directory services and firmware installation are disabled. |
| PortMaster compat layer (downloaded on the handheld, not bundled) | Ubuntu 20.04 (focal) arm64 and armhf packages from ports.ubuntu.com (a few from Ubuntu 18.04 and from Debian 10 on archive.debian.org where focal lacks a library), Rockchip's 32-bit `libmali-bifrost-g52-g24p0-wayland-gbm.so` from the JeffyCN/mirrors libmali branch (stock card); on RG DS OS, Debian 12 (bookworm) armhf packages from deb.debian.org with snapshot.debian.org as the pinned fallback, Mesa and SDL among them, and PortMaster's gl4es build from a PortMaster port zip; each pinned by SHA-256 in `runtime/portmaster_files/compat*.json`. | Fetched by `runtime/portmaster_compat.py` into `/mnt/mmc/portmaster/rgds/` (RG DS OS: `/data/portmaster/rgds/`) when the owner uses PortMaster, the way PortMaster fetches its own runtimes; nothing from them is in the RG DS Core package. The packages keep their Debian copyright and licence files in Ubuntu's archive. Ours: the stubs in `runtime/portmaster_files/armhf/` and `gl4es/` (`tools/build_compat_stubs.py`: empty libraries that name `libmali.so.1` or `libGL.so.1`) `gl4es/librgds-gl-attrs.so`, a small SDL preload for gl4es on Mali, and `kms/*/librgds-drm-lease.so`, the preload that hands RG DS OS's DRM panel leases to ports. |
| PortMaster's app icon (`runtime/portmaster_files/icon.png`, and `assets/systems/ports.bmp` made from it by `tools/prepare_ports_icon.py`) | PortMaster-GUI's `trimui/icon.png`, from PortsMaster/PortMaster-GUI on GitHub | MIT (PortMaster-GUI, notice below); shown as the PortMaster entry's cover before PortMaster is installed and as the Ports system's icon. |
| Realtek RTL8821CS Bluetooth firmware (RG DS OS image only) | linux-firmware 20251011 through Buildroot (`rtl_bt/rtl8821cs_fw.bin`, `rtl_bt/rtl8761b_config.bin`), the same files Debian's firmware-realtek ships; upstream links `rtl8821cs_config.bin` to `rtl8761b_config.bin` and `os/buildroot/board/rgds/post-build.sh` makes that link | Realtek's redistributable firmware licence (`LICENCE.rtlwifi_firmware.txt` in linux-firmware); loaded by the kernel's `btrtl` only when Bluetooth is switched on. |
| DSperate | Upstream [beebono/DSperate](https://github.com/beebono/DSperate), pinned commit `1b76c355109c9f7576363ccc023927b3137d3c6f`. | The GPL-3.0-or-later base DSCore is forked from (see RG DS Experimental fork). DSperate itself is no longer packaged. |
| mGBA (RG DS Play) | Upstream [mgba-emu/mgba](https://github.com/mgba-emu/mgba), pinned commit `143adf5aa2a60c6036810b0cd72445e13d74b0e6` in `tools/build_play.py`. | MPL-2.0. Unmodified libmgba (GBA, GB and GBC cores, with its bundled zlib on the handheld) linked statically into `play/`. Packaged at `emulators/rgds-play/` with `COPYING.mgba` (MPL-2.0), `COPYING` (GPL-3.0) and `source.tar.gz` (RG DS Play's sources and the pinned mGBA tree); `build.json` records the commit, file hashes and ELF ABI (`tools/build_play.py`). |
| libretro API header (RG DS Play's libretro core adapter) | `libretro.h` from the libretro project, MIT, as bundled in the pinned mGBA tree (`src/platform/libretro/libretro.h`). | Copied unmodified to `play/src/core/libretro/libretro.h`, with its licence text at the top. `play/src/core/libretro_core.cpp` (ours) loads a core through it. |
| libretro cores on RG DS OS (FCEUmm, Snes9x, PicoDrive, Mupen64Plus-Next, PCSX ReARMed) | Built by Buildroot from the pinned upstream commits in `os/buildroot/package/libretro-*` (recipes after BananaLinux's). FCEUmm, Mupen64Plus-Next and PCSX ReARMed GPL-2.0; Snes9x and PicoDrive non-commercial licences. | Installed as `/usr/lib/libretro/*_libretro.so` on RG DS OS only, never in a release package; RG DS Play loads one per game. Their licence files are kept by Buildroot's legal-info. |
| GBA DS (idea and profile facts) | [kyannon/gba-ds](https://github.com/kyannon/gba-ds), GPL-3.0. | No code copied. RG DS Play's companion screen is our own implementation of GBA DS's shadow-copy map idea; the memory addresses and pixel probes in `play/profiles/gba/AMTE.ini` and `A2CE.ini`, and the racing register checks in `AMKE.ini`, were first found by GBA DS and are credited in those files. |
| Game Boy disassemblies (profile facts) | [pret/pokered](https://github.com/pret/pokered), [pret/pokeyellow](https://github.com/pret/pokeyellow), [pret/pokecrystal](https://github.com/pret/pokecrystal), [pret/pokegold](https://github.com/pret/pokegold) symbols, [zladx/LADX-Disassembly](https://github.com/zladx/LADX-Disassembly), [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm), [metroidret/M2RoS](https://github.com/metroidret/M2RoS), [ElectroDeoxys/warioland3](https://github.com/ElectroDeoxys/warioland3). | No code or data copied. RAM addresses, record layouts, character sets and the locations of the games' own tables in `play/profiles/gb/*.ini` and `play/src/companion/party_gb.cpp` / `automap.cpp` come from these projects and are credited in those files; graphics are read from the player's own cartridge at run time. |
| Game RAM maps and facts (profile facts) | [Data Crystal](https://datacrystal.tcrf.net/) (Wario Land: Super Mario Land 3 and Wario Land 3 RAM maps; The Legend of Zelda, Metroid, Castlevania II, Duck Hunt, Super Mario Kart, F-Zero and Super Metroid RAM maps), [Super Mario Wiki](https://www.mariowiki.com/) (Wario Land: Super Mario Land 3's courses per world and ending money), [P.JBoy's Super Metroid bank logs](https://patrickjohnston.org/bank/), [maxim-zhao/psrp](https://github.com/maxim-zhao/psrp) (SMS Power!'s Phantasy Star disassembly), [n64decomp/mk64](https://github.com/n64decomp/mk64) (Mario Kart 64's Player struct and gPlayers) and [Skelux's MK64 memory map](https://wiki.skelux.root.sx/wiki/mario_kart_64/memory_map) (the course), the zelda3 reverse engineering of A Link to the Past and scawful's Hyrule Navigator RAM notes. | No code or data copied. The addresses and numbers are facts, credited in `play/profiles/gb/SUPERMARIOLAND3.ini`, `AW8A.ini` and the `play/profiles/nes`, `snes`, `sms` and `n64` profiles. |
| GBA colour model (GBA Color, GBA screen presets) | Pokefan531's GBA LCD colour model, public domain, as published in libretro's `handheld/shaders/color/gba-color`. | Its gamma, luminance and colour-mixing constants are used in our own presets `assets/shaders/gba_color.dsd` and `gba_screen.dsd`; no shader file copied. |
| Game Boy screen colours (Screen colours: Game Boy, Pocket, Light) | SameBoy's measured DMG, MGB and GBL palettes, MIT licence (Lior Halphon). | Four colour values per handheld in `play/src/ui/gb_palettes.hpp`; no code copied. The Game Boy Color's per-game colours come from mGBA's own table (`GBOverrideColorFind`), linked as part of mGBA. |
| Community shader presets | The `.dsd` pack in the ignored `vendor/engines/shaders/`. | Keep supplied README/attribution; audit individual notices before public packaging. |
| SDL2 | Installed host library and headers. DSCore and RG DS Play link against a source-built SDK pinned to official SDL2 2.32.10 (`tools/engine_toolchain.py --fetch`). | zlib licence. The SDK is link-time only; the handheld's own SDL is used at run time. |
| Nunito (DS rounded option) | [Google Fonts Nunito](https://github.com/google/fonts/tree/main/ofl/nunito); exact commit in `assets/fonts/source/PROVENANCE.txt`. | SIL Open Font License 1.1. Unmodified variable source font, full license and baked weight-600 atlases are bundled. This is a DS-inspired alternative, not Nintendo's original font. |
| New UI and native integration | Source in this workspace. | Original launcher, vector shapes, shared bridge/menu/audio/GL filters, input routing and startup warning. No Nintendo or ROCKNIXDS theme artwork/sounds copied. |
| ScreenScraper artwork and metadata | Downloaded on user request through [ScreenScraper API v2](https://www.screenscraper.fr/webapi2.php). | Cached per user and credited on the top screen. No downloaded artwork or account credentials are bundled in releases. |
| Shader studio reference scenes and UI icons | Original repository code in `tools/build_shader_previews.py` and `src/launcher/settings_visual.hpp`. | Three original scenes, rendered through the attributed supplied presets on host SwiftShader GLES2 and stored losslessly. No game screenshots or third-party character artwork included. Source/pixel hashes and renderer provenance accompany the previews. |
| DS banner icons/titles | Read from a user's local ROM files. | Displayed locally, with ZIP icon caching. No game icons or ROMs are included in releases. |

The DSperate v3.0.0 source is pinned at [commit `1b76c355109c9f7576363ccc023927b3137d3c6f`](https://github.com/beebono/DSperate/tree/1b76c355109c9f7576363ccc023927b3137d3c6f), including `src/frontend/sdl/main.cpp`, `input.cpp` and `configs/default.ini` for hotkeys and state filenames. DSCore (`dscore/`) is forked from this commit; DSperate itself is no longer built or packaged. Wi-Fi commands follow the [NetworkManager nmcli manual](https://networkmanager.dev/docs/api/latest/nmcli.html).

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

## RG DS Experimental fork

The `rgds-experimental` engine (DSCore), the only DS engine, is built from `dscore/`, a fork of
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

## RG DS OS hardware layer (BananaLinux, ROCKNIX)

`os/buildroot/` (RG DS OS, [docs/OWN_OS_PLAN.md](docs/OWN_OS_PLAN.md)) reuses BananaLinux's RG DS board layer from
[JorreFog/BananaLinux](https://github.com/JorreFog/BananaLinux) at `58380c28ae149db7d648da3565712d80bcdefef1`, under
its MIT licence (`os/buildroot/LICENSE.BananaLinux`). That covers `board/rgds/{dts,dts-patches,kernel,linux-patches,
uboot-patches,rkbin,linux-rocknix.config}`, the `mesa3d-panfrost` and `libudev-zero` package recipes, and
`patches/sdl2/0001-banana-panels.patch`. The kernel patches, device trees and the generic DSI panel and joypad drivers
come from ROCKNIX (GPL-2.0) at the commits in `os/buildroot/board/rgds/ROCKNIX`, and keep their own headers.
`rk3568_bl31_v1.45.elf` and `rk3568_ddr_1056MHz_v1.23.bin` (rockchip-linux/rkbin at `f43a462e`) are Rockchip's, under the licence beside them in `board/rgds/rkbin/LICENSE`. The init scripts,
the RG DS Core package and the image layout files are RG DS OS's own.

## PortMaster's app icon

`runtime/portmaster_files/icon.png` is PortMaster-GUI's `trimui/icon.png` (github.com/PortsMaster/PortMaster-GUI),
unchanged; `assets/systems/ports.bmp` is the same picture cropped and scaled by `tools/prepare_ports_icon.py`.

```
MIT License

Copyright (c) 2023-2025 Jacob

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
