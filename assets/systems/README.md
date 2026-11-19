# RG DS Core system icons

One drawn icon per system of the stock Anbernic RG DS Linux card: the artwork
the launcher shows on the **Systems** page and in the system carousel, in both
icon styles.

The set covers the systems the stock card carries (Linux firmware V1.0, the
Buildroot image with RetroArch and the stand-alone emulators): Nintendo DS,
Nintendo 3DS, Game Boy Advance, Game Boy Color, Game Boy, NES, Super Nintendo,
Mega Drive, Master System, Neo Geo, Neo Geo Pocket, PC Engine, MSX,
PlayStation, PSP, Dreamcast, Saturn, Nintendo 64, Pico-8, Arcade, DOS PC,
Atari 2600, ColecoVision, Intellivision, Vectrex, WonderSwan and RPG Maker,
the home computers RG DS Play adds (the ZX Spectrum, the Commodore 64 and the Amiga)
and the two RG DS Play emulates (Game & Watch and ScummVM). The Amiga, the third
home computer RG DS Play adds, came later and is drawn in the same family.
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
per system, 2048×2048. The Amiga was rendered on 2026-11-18 from the same
brief (`build/systems/systems_prompts.py`), so it sits in the same family.

- `<id>.bmp`: the launcher's own asset, a 128×128 32-bit BGRA bitmap with a
  116 px optical bound, written by `tools/prepare_system_icons.py`.
- `pixel/<id>.svg`: the pixel glyph as a pixel-perfect SVG, for review.

The generated renders the bitmaps are cut from are kept in `artwork/systems/`.
They are the source, not a shipped asset: the handheld gets the bitmaps and the
glyphs, not 98 MB of studio photographs.

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

Each glyph is the system's most iconic object, and its silhouette does the
work: a solid body in that object's own outline, with screens as `+` and buttons
punched out of it. So the handheld itself for the handhelds, the controller for
most home consoles (the NES's sharp rectangle, the SNES's dog bone, the N64's
trident), the console for the Master System and PC Engine, whose pads look like
everyone else's, the cabinet for the arcade, the computer for DOS, MSX, the
Spectrum and the C64, and a sword for RPG Maker, which has no hardware. The grid
is 16×16 (not the launcher's 12×12) because a system's shape needs that room to
stay recognisable.

The two non-stock tiles follow the same rule: Ports is the cargo ship of
PortMaster's own icon, and Homebrew is a blank cartridge, the thing the shelf
actually holds.

`tools/build_system_icons.py` is the only writer: it turns the table into the
launcher's masks (`src/launcher/system_icons.hpp`), one pixel-perfect SVG per
system (`pixel/<id>.svg`) and a labelled review sheet, so the three can never
drift apart.

```sh
python3 tools/build_system_icons.py                 # masks + SVG
python3 tools/build_system_icons.py --sheet out.png # review sheet
python3 tools/prepare_system_icons.py               # artwork -> BMP
python3 tools/prepare_system_icons.py --only amiga  # one system
```

The matte needs rembg and onnxruntime. They are not in the repository's own
environment; they live in a local venv, `.cache/rembg-venv`, so the pipeline
runs the same way on every machine:

```sh
python3 -m venv .cache/rembg-venv
.cache/rembg-venv/bin/pip install rembg onnxruntime
.cache/rembg-venv/bin/python tools/prepare_system_icons.py
```

The models themselves are cached in `.cache/u2net` (birefnet-general and
u2net), so a second run does not download them again.

## Seeing one in the launcher

The launcher draws a system's own drawing, so a new icon can be checked headless
before it ships: point it at a folder with a game of that system, and it appears
in the Systems carousel with its own drawing.

```sh
mkdir -p /tmp/amiga && touch "/tmp/amiga/Super Skweek.adf"
rm -f /tmp/rgds-data/state            # the state file remembers the last page
SDL_VIDEODRIVER=dummy build/host/rgds-launcher --preview \
    --data /tmp/rgds-data --state /tmp/rgds-data/state \
    --system-dir amiga=/tmp/amiga --screenshot /tmp/shot.bmp
```

`ui.ini` in the data folder turns the Systems step on (`show_systems=1`), and an
input script with a single `key A` on the Games tile opens the carousel.
