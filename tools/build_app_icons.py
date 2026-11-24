#!/usr/bin/env python3
"""Draw Home's app icons that have no generated artwork: assets/icons/{media,album,files}.bmp (128 x 128, alpha).

Same family as the PictoChat and Music icons (whose primitives and painting they borrow): silver bodies with a
graphite edge, medium-blue glass accents, restrained gloss, no text and no enclosing tile. Their simple-pixel
counterparts are the Play, Art and Folder glyphs (category_icon in src/launcher/ui_widgets.hpp).

    python3 tools/build_app_icons.py [--png-dir previews/]
"""
import argparse
from pathlib import Path

import numpy as np

from build_pictochat_icon import SIZE, SUPER, blur, cover, grid, hexa, paint, rrect, triangle, write_bmp, write_png

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / 'assets/icons'


def shade(canvas, shape, dx=1, dy=5, alpha=.26):
    shadow = blur(cover(np.roll(np.roll(shape, int(dy * SUPER), 0), int(dx * SUPER), 1)) * alpha)
    canvas[..., :3] = canvas[..., :3] * (1 - shadow[..., None]) + hexa('#16202b') * shadow[..., None]
    canvas[..., 3:] = canvas[..., 3:] * (1 - shadow[..., None]) + shadow[..., None]


def gloss(canvas, mask, strength=.34):
    canvas[..., :3] = canvas[..., :3] * (1 - mask[..., None] * strength) + 255 * mask[..., None] * strength


def turned(px, py, cx, cy, angle):
    c, s = np.cos(angle), np.sin(angle)
    return cx + (px - cx) * c + (py - cy) * s, cy - (px - cx) * s + (py - cy) * c


def finish(canvas):
    small = canvas.reshape(SIZE, SUPER, SIZE, SUPER, 4).mean((1, 3))
    alpha = small[..., 3:]
    colour = np.where(alpha > 1e-4, small[..., :3] / np.maximum(alpha, 1e-4), 0)
    return np.dstack([np.clip(colour, 0, 255), np.clip(alpha * 255, 0, 255)]).round().astype(np.uint8)


def files():
    """A folder: a silver back with its tab, a sheet of paper in it, a blue glass front flap."""
    px, py = grid()
    canvas = np.zeros(px.shape + (4,))
    back = np.minimum(rrect(px, py, 10, 26, 118, 108, 12), rrect(px, py, 10, 16, 56, 40, 9))
    front = rrect(px, py, 10, 44, 118, 110, 12)
    shade(canvas, np.minimum(back, front))
    paint(canvas, cover(back), '#55616d', '#2f3a45', py, 16, 108)
    paint(canvas, cover(back + 3), '#e9eef3', '#b3bec9', py, 16, 108)
    sheet = rrect(px, py, 22, 30, 104, 70, 5)
    paint(canvas, cover(sheet), '#8c97a3', '#6d7885', py, 30, 70)
    paint(canvas, cover(sheet + 2), '#ffffff', '#eef2f6', py, 30, 70)
    paint(canvas, cover(front), '#123f78', '#0d2f5c', py, 44, 110)
    paint(canvas, cover(front + 3), '#55adf5', '#1f6fc9', py, 44, 110)
    gloss(canvas, cover(rrect(px, py, 14, 47, 114, 70, 10)) * np.clip((72 - py) / 24, 0, 1))
    return finish(canvas)


def album():
    """Two photo prints, the back one turned: white paper, a graphite edge, a blue sky over silver hills."""
    px, py = grid()
    canvas = np.zeros(px.shape + (4,))
    bx, by = turned(px, py, 64, 64, .2)
    back = rrect(bx, by, 22, 14, 110, 92, 7)
    fx, fy = turned(px, py, 60, 70, -.09)
    front = rrect(fx, fy, 10, 30, 104, 114, 7)
    shade(canvas, back, 1, 4, .2)
    paint(canvas, cover(back), '#59646f', '#36404b', py, 10, 100)
    paint(canvas, cover(back + 3), '#f4f7fa', '#c9d1da', py, 10, 100)
    paint(canvas, cover(rrect(bx, by, 30, 22, 102, 74, 3)), '#9fb0c2', '#8798ab', py, 20, 80)
    shade(canvas, front)
    paint(canvas, cover(front), '#2f3a45', '#1f2831', py, 28, 118)
    paint(canvas, cover(front + 3), '#ffffff', '#e4ebf2', py, 28, 118)
    picture = rrect(fx, fy, 19, 39, 95, 96, 3)
    inside = cover(picture)
    paint(canvas, inside, '#8fcaf9', '#2f84d8', fy, 39, 96)
    sun = np.hypot(fx - 76, fy - 55) - 8
    paint(canvas, cover(np.maximum(sun, picture)), '#ffffff', '#e8f2fb', fy, 47, 63)
    hill = np.maximum(picture, (80 - 18 * np.exp(-((fx - 38) / 17)**2)) - fy)
    paint(canvas, cover(hill), '#e9eef3', '#b3bec9', fy, 64, 96)
    hill = np.maximum(picture, (88 - 13 * np.exp(-((fx - 78) / 15)**2)) - fy)
    paint(canvas, cover(hill), '#8c97a3', '#55616d', fy, 72, 96)
    gloss(canvas, inside * np.clip((62 - fy) / 22, 0, 1), .22)
    return finish(canvas)


def media():
    """A screen with film perforations along it and a blue glass play mark: films, books and music in one place."""
    px, py = grid()
    canvas = np.zeros(px.shape + (4,))
    body = rrect(px, py, 8, 18, 120, 110, 14)
    shade(canvas, body)
    paint(canvas, cover(body), '#55616d', '#2f3a45', py, 18, 110)
    paint(canvas, cover(body + 3), '#e9eef3', '#b3bec9', py, 18, 110)
    holes = np.full(px.shape, np.inf)
    for x in range(15, 106, 18):
        holes = np.minimum(holes, rrect(px, py, x, 25, x + 9, 33, 2))
        holes = np.minimum(holes, rrect(px, py, x, 95, x + 9, 103, 2))
    paint(canvas, cover(holes), '#55616d', '#3d4853', py, 25, 103)
    screen = rrect(px, py, 18, 40, 110, 88, 6)
    paint(canvas, cover(screen), '#2f3a45', '#1f2831', py, 40, 88)
    paint(canvas, cover(screen + 2), '#3a4652', '#26303a', py, 40, 88)
    play = triangle(px, py, (54, 46), (54, 82), (84, 64)) - 2
    paint(canvas, cover(play), '#123f78', '#0d2f5c', py, 44, 84)
    paint(canvas, cover(play + 2.5), '#6bb8f7', '#1f6fc9', py, 44, 84)
    gloss(canvas, cover(rrect(px, py, 12, 21, 116, 48, 11)) * np.clip((50 - py) / 26, 0, 1), .3)
    return finish(canvas)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--png-dir', type=Path, help='also write enlarged previews on the light theme background')
    args = parser.parse_args()
    for name, render in (('media', media), ('album', album), ('files', files)):
        image = render()
        write_bmp(ICONS / f'{name}.bmp', image)
        print((ICONS / f'{name}.bmp').relative_to(ROOT))
        if args.png_dir:
            args.png_dir.mkdir(parents=True, exist_ok=True)
            write_png(args.png_dir / f'{name}.png', image)


if __name__ == '__main__':
    main()
