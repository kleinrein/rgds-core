# RG DS Core icons

Generated with built-in imagegen on 2026-10-12, with true transparent backgrounds.
The source PNGs are retained here. The brief was a coherent, mature DS-era system
menu family: silver and graphite bodies, medium blue accents, restrained bevels,
clear front-facing silhouettes, no text, no enclosing tile and no neon/glow.

- `navigation-source.png`: settings sliders, cartridge, heart and clock.
- `settings-source.png`: displays, power bolt, library, sound, Wi-Fi, system,
  controls and shutdown.
- `game-menu-source.png`: resume, save, load, shaders, brightness, restart, home
  and artwork. These complete the ten main in-game actions with the existing
  sound, system and controls assets; grid icons render at 32px.
- `games-source.png`: final replacement cartridge. Its prompt specifies a square
  graphite Nintendo DS cartridge front, a small upper-right step, grip ridges,
  silver inset label with blue dual screens, and no exposed gold contacts.

`tools/prepare_icons.py` crops cell padding, preserves alpha and scales each image
onto a 128×128 canvas with a 116px optical bound. The launcher preloads these BMPs
outside drawing (20 icons total), with 64 KiB of pixel storage per icon per renderer. Directional
arrows, checkmarks and small control glyphs retain native geometry for legibility.

## Theme selection

Theme & display → Icon style selects Rich (these generated assets) or Simple
pixel (the hand-aligned 12px native masks in `src/launcher/pixel_icons.hpp`). This
choice is stored in `ui.ini` and shared with the game menu, independently of the
font and light/dark theme. Rich remains the default for existing installations.
Pixel mode skips preloading the generated category textures. Small action glyphs
retain native geometry in both styles.
