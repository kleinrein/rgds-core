# Raster rules test

A way to prove a change to DSCore's GPU raster keeps the DS rendering rules,
without a game: an original test ROM draws one rule per screen cell, the
headless engine renders it with the CPU raster (the reference) and with the
GPU raster, and a tool compares the two per cell. It runs on the host, under
lavapipe in the Linux VM, in about ten seconds; no handheld is needed.

```sh
python3 tools/raster_rules.py all --engine /path/to/dsperate-headless
python3 tools/raster_rules.py all --engine ... --gpu-env DS_LEAN_NODISCARD=1 --out build/raster-rules/run-nd1
```

`--engine` is a Linux `dsperate-headless` built with `DSPERATE_VULKAN=ON`
(for example `build/frag-cost/zbuild.sh NAME SRC headless` from a copy of
`dscore/`). The ROM needs only the Homebrew LLVM cross compiler, as the
graphics probe does. `--gpu-env` passes engine switches to the GPU run, so a
candidate renderer change can be held against the unchanged CPU raster.
Results land in `build/raster-rules/run/` (or `--out`): `cpu.raw` and
`gpu/*-h.raw` are the two 3D layer records, `cpu.png`, `gpu.png` and
`diff.png` show them (red: rule differences, blue: rounding, yellow: edges).
The exit status is 1 when any cell fails.

## The ROM

`tests/fixtures/raster_rules/arm9.c` is register-level code, like the
graphics probe: an identity projection, axis-aligned quads on pixel
coordinates, flat colours, three 16×16 textures (an A3I5 checker of alpha 31
and 0, A5I3 bands of alpha 0, 6, 16 and 31, direct-colour stripes with an
alpha-0 half) mapped one texel per pixel, the rear plane cleared to alpha 0,
alpha test at reference 10, fog and a toon table on. Everything is static, so
any frame after the first is the same. The sixteen 64×48 cells:

| Cell | Rule |
| --- | --- |
| (0,0) | Marker: one opaque red quad, checks orientation and colour decode |
| (1,0) | Translucent polygon of the same id over another's alpha-0 texels: blends only there |
| (2,0) | The same with another id: blends everywhere |
| (3,0) | Translucent cut-out with depth write: a later polygon behind it shows only in the holes |
| (0,1) | The same without depth write: the later polygon blends over the cut-out's pixels too |
| (1,1) | Depth-equal decal on a surface at the same depth; without the bit it is invisible |
| (2,1) | Shadow volume over polygon id 0 (darkened) and over its own id (skipped) |
| (3,1) | Alpha test on a modulated A5I3 texture, then the same id over it |
| (0,2) | Alpha-31 polygon with A5I3 texels: its opaque pixels take a later same-id polygon |
| (1,2) | Fog by depth, three fogged quads and one unfogged |
| (2,2) | Toon shading from flat vertex reds |
| (3,2) | Decal blend of an A5I3 texture over the vertex colour |
| (0,3) | Repeat and flip texture wrap on an opaque cut-out over a backdrop |
| (1,3) | Translucent polygons over the empty rear plane |
| (2,3) | Opaque overlap order, including an opaque cut-out submitted last |
| (3,3) | A3I5 polygon of alpha 31: opaque texels take a later same-id polygon |

## What counts as a difference

Inside a polygon the DS corner sample and every GPU sample see the same
flat colour, texel and depth, so the GPU's 2×2 pixels for a DS pixel must
all match it. Two things differ by design and are reported apart:

- **Edge pixels**: the GPU has no DS edge-row emulation and draws a quad's
  last column, which the DS leaves out. A pixel whose 3×3 neighbourhood
  differs in either image counts as an edge pixel.
- **Rounding**: the GPU blends in float with the source alpha (a + 1) / 32
  to within 1/256 and the attachment rounds, where the DS blends in integers
  and truncates, so a blended pixel can differ by one 6-bit step per layer.
  Up to `--tolerance` steps (default 2, two layers stack in the tests) is
  rounding, not a rule.

Anything else inside a polygon is a rule difference and fails the cell.

## Results, 16 October 2026

The current renderer passes all sixteen cells (about 11,000 pixels of
rounding, 4,700 edge pixels, no rule differences). The same ROM shows what the
[fragment-cost candidates](PERFORMANCE.md) do: `DS_LEAN_NODISCARD=2` passes
every cell, `DS_LEAN_NODISCARD=1` fails five (same-id over alpha-0 texels,
depth-write cut-out, alpha test, both alpha-31 cases) with hundreds of rule
pixels each, as its stencil and depth side effects predict.

Lavapipe is not the Mali: rounding may fall differently on the device, and a
timing-dependent bug never shows here. The ROM runs on the handheld too (it
boots like the graphics probe), and `DS_HIRES_DUMP` gives the same `h` record
there for the same comparison.
