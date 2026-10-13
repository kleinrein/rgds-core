# RG DS Core identity

`rg-ds-core.png` is the original transparent image generated with the built-in
imagegen tool on 2026-10-12. `mark-light.bmp` and `mark-dark.bmp` are the cropped,
144px RGB runtime adaptations, composited onto the corresponding theme background.
The original alpha is retained in the PNG. Bitmap adaptation uses Lanczos scaling.
No runtime image decoder or extra device dependency is required.

Prompt (built-in imagegen, transparent background):

> Use case: logo-brand. Asset type: production logo for RG DS Core, a clean dual-screen handheld gaming OS. Create one exceptionally refined compact brand symbol only, no text. Two vertically stacked rounded rectangular display shapes integrated into a single geometric monogram/core emblem, a subtle central negative-space hinge. Crisp vector-like edges, restrained teal and deep navy, no gradients, no glow, no mockup, no shadows, no extraneous decoration. Flat front view centered on genuine transparent background with generous empty padding. Recognizable at 24 pixels. This symbol will accompany the typeset wordmark RG DS Core in the top status bar and animated boot screen.

## Current wordmark

`wordmark-light.png` and `wordmark-dark.png` were produced with built-in imagegen
on 2026-10-12. The generated lettering is used as an image in the status bar and
boot scene. The design brief was a mature, restrained DS-era identity with a
custom split-screen detail in the D, blue RG and contrasting DS Core lettering.
The dark variant preserves the same identity with light lettering.

`tools/prepare_wordmark.py` crops transparent padding and exports alpha-preserving
BMP V4 assets for SDL. The original PNGs preserve generation output. Regenerate
early boot art with `python3 tools/build_boot_assets.py` after preparing the BMPs.
The earlier symbol is retained as source history.
