# RG DS Core system icons

One drawn icon per system of the stock Anbernic RG DS Linux card: the artwork
the launcher shows on the **Systems** page and in the system carousel, in both
icon styles.

The set covers the systems the stock card carries (Linux firmware V1.0, the
Buildroot image with RetroArch and the stand-alone emulators): Nintendo DS,
Nintendo 3DS, Game Boy Advance, Game Boy Color, Game Boy, NES, Super Nintendo,
Mega Drive, Master System, Neo Geo, Neo Geo Pocket, PC Engine, MSX,
PlayStation, PSP, Dreamcast, Saturn, Nintendo 64, Pico-8, Arcade, DOS PC,
Atari 2600, ColecoVision, Intellivision, Vectrex, WonderSwan and RPG Maker.
`src/launcher/system_icons.hpp` keeps their names, their cartridge badges and
the folders the stock card uses (`FC`, `SFC`, `PS`, `NEOGEO`, …), so a new
system only needs its icon and one line.

Two of the launcher's tiles are not stock-card systems and so have no folders
to scan: **Ports** (PortMaster's ports) and **Homebrew** (the curated free DS,
GBA and NES games). They carry icons too, drawn in the same family, and live
in the same assets: `ports.bmp` and `homebrew.bmp`, with the matching glyphs.

## The rich artwork

Generated with imagegen on 2026-10-23 as photorealistic studio product photos:
the real hardware, on a seamless studio sweep, one camera and one lighting
setup for the whole set, three-quarter view, no text and no logos. One picture
per system, 2048×2048.

- `<id>.bmp`: the launcher's own asset, a 128×128 32-bit BGRA bitmap with a
  116 px optical bound, written by `tools/prepare_system_icons.py`.
- `pixel/<id>.svg`: the pixel glyph as a pixel-perfect SVG, for review.

The generated renders the bitmaps are cut from are kept in `artwork/systems/`.
They are the source, not a shipped asset: the handheld gets the bitmaps and the
glyphs, not 89 MB of studio photographs.

The generator paints the "transparent" background as a studio sweep or as a
checkerboard of two flat tones, so the backdrop has to be taken off before the
launcher can draw the machine on its own theme. `tools/system_matte.py` does
that, in three steps:

- Two segmentation models are run over the picture. The sharp one finds the
  edge of the hardware but punches holes through pale panels; the blunt one
  keeps whole machines but leaves slivers of the backdrop standing along an
  edge. Where they disagree and the picture shows the studio, the studio wins.
- The parts of the machine the models lost are found by closing the confident
  silhouette, and closed with the pixels of the photograph. A missing part
  that shows the studio through it stays open - that is a real opening, the
  loop of a coiled cable or the gap between two buttons.
- The colour of the edge is taken from the machine next to it rather than from
  the studio behind it, so no milk of the backdrop survives around the
  silhouette.

Without rembg the tool falls back to a colour-model matte, which is rougher
but keeps the pipeline runnable.

## The pixel icons

`Theme & display → Icon style → Simple pixel` draws the same systems as the
hand-drawn 16×16 masks in `tools/system_pixel_art.py`: `#` is ink, `+` a
half-tone (screens and panels), `.` empty. They are drawn at whole-pixel steps
only, so 64, 72 and 112 px stay crisp.

Each glyph is the system's most iconic object: the handheld itself for the
handhelds, the controller for the home consoles, the cabinet for the arcade,
the computer for DOS and RPG Maker. The grid is 16×16 (not the launcher's 12×12)
because a system's shape needs that room to stay recognisable.

The two non-stock tiles follow the same rule: Ports is the cargo ship of
PortMaster's own icon, and Homebrew is a stack of three blank cartridges, the
thing the shelf actually holds.

`tools/build_system_icons.py` is the only writer: it turns the table into the
launcher's masks (`src/launcher/system_icons.hpp`), one pixel-perfect SVG per
system (`pixel/<id>.svg`) and a labelled review sheet, so the three can never
drift apart.

```sh
python3 tools/build_system_icons.py                 # masks + SVG
python3 tools/build_system_icons.py --sheet out.png # review sheet
python3 tools/prepare_system_icons.py               # artwork -> BMP
```
