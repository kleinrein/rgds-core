#!/usr/bin/env python3
"""Draw the Music home icon: assets/icons/music.bmp (128 x 128, alpha).

Drawn here, like the PictoChat icon (whose shapes and painting it borrows), so it can be reproduced exactly and
stays in the same family: silver stems and beam with a graphite edge, two medium-blue note heads, restrained
gloss, no text and no enclosing tile. Its pixel counterpart is the `Music` glyph.

    python3 tools/build_music_icon.py [--png preview.png]
"""
import argparse
from pathlib import Path

import numpy as np

from build_pictochat_icon import SIZE, SUPER, blur, cover, grid, hexa, paint, stroke, write_bmp, write_png

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'assets/icons/music.bmp'


def ellipse(px, py, cx, cy, rx, ry, turn):
    """Roughly the signed distance to an ellipse turned `turn` radians; negative inside."""
    c, s = np.cos(turn), np.sin(turn)
    x, y = (px - cx) * c + (py - cy) * s, -(px - cx) * s + (py - cy) * c
    return (np.hypot(x / rx, y / ry) - 1) * min(rx, ry)


def convex(px, py, points):
    """Signed distance to a convex polygon given clockwise on screen (inside: exact; outside: a lower bound)."""
    distance = np.full(px.shape, -np.inf)
    for (ax, ay), (bx, by) in zip(points, points[1:] + points[:1]):
        ex, ey = bx - ax, by - ay
        distance = np.maximum(distance, ((py - ay) * ex - (px - ax) * ey) / -np.hypot(ex, ey))
    return distance


def render():
    px, py = grid()
    canvas = np.zeros(px.shape + (4,))
    # Two eighth notes under one beam, leaning the way a pen draws them.
    stems = np.minimum(stroke(px, py, [(52, 30), (52, 90)], 11), stroke(px, py, [(104, 19), (104, 79)], 11))
    beam = convex(px, py, [(46.5, 20.5), (109.5, 7.5), (109.5, 31), (46.5, 44)])
    body = np.minimum(stems, beam)
    heads = np.minimum(ellipse(px, py, 36, 93, 21, 15.5, -.38), ellipse(px, py, 88, 82, 21, 15.5, -.38))
    whole = np.minimum(body, heads)
    shadow = blur(cover(np.roll(np.roll(whole, int(5 * SUPER), 0), int(1 * SUPER), 1)) * .26)
    canvas[..., :3] = canvas[..., :3] * (1 - shadow[..., None]) + hexa('#16202b') * shadow[..., None]
    canvas[..., 3:] = canvas[..., 3:] * (1 - shadow[..., None]) + shadow[..., None]
    # Stems and beam: a graphite edge around silver.
    paint(canvas, cover(body), '#55616d', '#2f3a45', py, 8, 92)
    paint(canvas, cover(body + 3), '#f4f7fa', '#b9c3ce', py, 8, 92)
    # Heads: blue glass with a darker rim.
    paint(canvas, cover(heads), '#1b5fae', '#123f78', py, 66, 110)
    paint(canvas, cover(heads + 3), '#55adf5', '#1f6fc9', py, 66, 110)
    for cx, cy in ((36, 93), (88, 82)):
        gloss = cover(ellipse(px, py, cx - 3, cy - 5, 13, 6, -.38)) * .36
        canvas[..., :3] = canvas[..., :3] * (1 - gloss[..., None]) + 255 * gloss[..., None]
    small = canvas.reshape(SIZE, SUPER, SIZE, SUPER, 4).mean((1, 3))
    alpha = small[..., 3:]
    colour = np.where(alpha > 1e-4, small[..., :3] / np.maximum(alpha, 1e-4), 0)
    return np.dstack([np.clip(colour, 0, 255), np.clip(alpha * 255, 0, 255)]).round().astype(np.uint8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--png', type=Path, help='also write an enlarged preview on the light theme background')
    args = parser.parse_args()
    image = render()
    write_bmp(OUTPUT, image)
    print(OUTPUT.relative_to(ROOT))
    if args.png:
        write_png(args.png, image)


if __name__ == '__main__':
    main()
