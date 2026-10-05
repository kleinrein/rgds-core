# Development package provenance

The original supplied package remains unchanged at `simpleos-source/`. It is ignored by Git. The SD artifact built here is a private development package for the owner's device, not a public firmware release.

| Component | Source | Treatment |
| --- | --- | --- |
| SimpleOS integration reference and profiles | [boorngos/SimpleOS](https://github.com/boorngos/SimpleOS), supplied package version `20260927`. | Published project license is MIT; preserve its contributor notice for imported material. |
| DraStic and Nintendo BIOS | Existing official Anbernic Linux card. | Not included in source or ZIP. The runtime locates existing stock files. |
| DSperate | Local `system/bin/dsperate`, with local `system/dsperate/LICENSE` and `NOTICE.txt`; upstream [beebono/DSperate](https://github.com/beebono/DSperate). | The supplied notice identifies GPLv3 and version 3.0.0. Its notice/license are copied with the binary. Establish matching source/build provenance before a public redistribution. |
| NNDDSS and its shim libraries | Local `system/nnddss_payload/`. | Copied for private testing with the device owner. This package supplies no separate license/source record for the payload; resolve component provenance before a public release. |
| Shaders | Local `system/shaders/` and NNDDSS payload shader files. | Keep supplied README/attribution; audit individual notices before public packaging. |
| SDL2 | Installed host library and headers; existing vendor device library. | Runtime library is not copied into the ZIP. SDL's own notices remain with its headers/library. |
| Nunito (DS rounded option) | [Google Fonts Nunito](https://github.com/google/fonts/tree/main/ofl/nunito); exact commit in `assets/fonts/source/PROVENANCE.txt`. | SIL Open Font License 1.1. Unmodified variable source font, full license and baked weight-600 atlases are bundled. This is a DS-inspired alternative, not Nintendo's original font. |
| New UI | Source in this workspace. | Original vector shapes, probe bitmap font and palettes; no Nintendo or ROCKNIXDS theme artwork/sounds copied. |
| ScreenScraper artwork and metadata | Downloaded on user request through [ScreenScraper API v2](https://www.screenscraper.fr/webapi2.php). | Cached per user and credited on the top screen. No downloaded artwork or account credentials are bundled in releases. |
| DS banner icons/titles | Read from the owner's local ROM files. | Displayed locally, with ZIP icon caching. No game icons or ROMs are included in releases. |

SimpleOS's published notice for imported profiles:

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
