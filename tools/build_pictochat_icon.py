#!/usr/bin/env python3
"""Draw the PictoChat home icon: assets/icons/pictochat.bmp (128 x 128, alpha).

The other rich icons are generated artwork; this one is drawn here so it can be
reproduced exactly. It follows the same brief: a graphite and silver body with a
medium-blue accent, restrained bevels, no text and no enclosing tile. Two chat
bubbles, the front one carrying a hand-drawn stroke, which is what PictoChat is.
Original artwork; it copies nothing from Nintendo's icon.

    python3 tools/build_pictochat_icon.py [--png preview.png]
"""
import argparse
from pathlib import Path
import struct
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'assets/icons/pictochat.bmp'
SIZE, SUPER = 128, 4


def grid():
    n = SIZE * SUPER
    axis = (np.arange(n) + .5) / SUPER
    return np.meshgrid(axis, axis)


def rrect(px, py, x0, y0, x1, y1, r):
    """Signed distance to a rounded rectangle; negative inside."""
    cx, cy, hx, hy = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx, qy = np.abs(px - cx) - hx, np.abs(py - cy) - hy
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def triangle(px, py, a, b, c):
    """Signed distance to a triangle (after Inigo Quilez); negative inside."""
    p = np.stack([px, py], -1)
    a, b, c = (np.array(v, float) for v in (a, b, c))
    edges, sides = (b - a, c - b, a - c), (a, b, c)
    distance = np.full(px.shape, np.inf)
    for e, v in zip(edges, sides):
        w = p - v
        t = np.clip((w @ e) / (e @ e), 0, 1)
        distance = np.minimum(distance, np.sum((w - t[..., None] * e) ** 2, -1))
    cross = [e[0] * (p[..., 1] - v[1]) - e[1] * (p[..., 0] - v[0]) for e, v in zip(edges, sides)]
    inside = (np.all([k >= 0 for k in cross], 0)) | (np.all([k <= 0 for k in cross], 0))
    return np.sqrt(distance) * np.where(inside, -1, 1)


def stroke(px, py, points, width):
    """Coverage of a round-capped polyline."""
    distance = np.full(px.shape, np.inf)
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        ex, ey = bx - ax, by - ay
        t = np.clip(((px - ax) * ex + (py - ay) * ey) / (ex * ex + ey * ey), 0, 1)
        distance = np.minimum(distance, np.hypot(px - (ax + t * ex), py - (ay + t * ey)))
    return distance - width / 2


def cover(distance):
    return np.clip(.5 - distance * SUPER, 0, 1)   # one output pixel is SUPER samples wide


def blur(mask, passes=3, radius=3):
    for _ in range(passes):
        for axis in (0, 1):
            kernel = np.ones(radius * 2 + 1) / (radius * 2 + 1)
            mask = np.apply_along_axis(lambda line: np.convolve(line, kernel, 'same'), axis, mask)
    return mask


def hexa(value):
    return np.array([int(value[i:i + 2], 16) for i in (1, 3, 5)], float)


def paint(canvas, mask, top, bottom, py, y0, y1):
    """Composite a vertical-gradient fill through `mask` (premultiplied canvas)."""
    t = np.clip((py - y0) / (y1 - y0), 0, 1)[..., None]
    colour = hexa(top) * (1 - t) + hexa(bottom) * t
    mask = mask[..., None]
    canvas[..., :3] = canvas[..., :3] * (1 - mask) + colour * mask
    canvas[..., 3:] = canvas[..., 3:] * (1 - mask) + mask


def render():
    px, py = grid()
    canvas = np.zeros(px.shape + (4,))
    # Back bubble: graphite body with a silver rim, tail to the lower right.
    back = np.minimum(rrect(px, py, 40, 12, 118, 66, 14), triangle(px, py, (92, 60), (108, 60), (106, 86)))
    front = np.minimum(rrect(px, py, 10, 40, 94, 96, 15), triangle(px, py, (24, 90), (44, 90), (22, 118)))
    for shape, dx, dy, alpha in ((back, 1, 4, .22), (front, 1, 5, .28)):
        shadow = blur(cover(np.roll(np.roll(shape, int(dy * SUPER), 0), int(dx * SUPER), 1)) * alpha)
        canvas[..., :3] = canvas[..., :3] * (1 - shadow[..., None]) + hexa('#16202b') * shadow[..., None]
        canvas[..., 3:] = canvas[..., 3:] * (1 - shadow[..., None]) + shadow[..., None]
    paint(canvas, cover(back), '#c9d1da', '#8c97a3', py, 12, 86)
    paint(canvas, cover(back + 3.2), '#59646f', '#36404b', py, 12, 86)
    # A short stack of strokes on the back bubble: someone else's drawing.
    for row, (x0, x1) in enumerate(((56, 100), (56, 92))):
        paint(canvas, cover(stroke(px, py, [(x0, 28 + row * 14), (x1, 28 + row * 14)], 5)), '#9fb0c2', '#8798ab', py, 0, 1)
    # Front bubble: silver-white paper, graphite edge, a blue stroke drawn on it.
    paint(canvas, cover(front), '#2f3a45', '#1f2831', py, 40, 118)
    paint(canvas, cover(front + 3.4), '#ffffff', '#dfe7ef', py, 40, 118)
    wave = [(24, 72), (32, 56), (40, 82), (49, 54), (58, 80), (67, 56), (76, 70), (82, 64)]
    paint(canvas, cover(stroke(px, py, wave, 7.5)), '#4aa3f0', '#1f6fc9', py, 52, 84)
    # Restrained gloss across the upper half of the paper.
    sheen = cover(rrect(px, py, 14, 43, 90, 66, 11)) * .34 * np.clip((66 - py) / 24, 0, 1)
    canvas[..., :3] = canvas[..., :3] * (1 - sheen[..., None]) + 255 * sheen[..., None]
    # Downsample the premultiplied samples, then un-premultiply.
    small = canvas.reshape(SIZE, SUPER, SIZE, SUPER, 4).mean((1, 3))
    alpha = small[..., 3:]
    colour = np.where(alpha > 1e-4, small[..., :3] / np.maximum(alpha, 1e-4), 0)
    return np.dstack([np.clip(colour, 0, 255), np.clip(alpha * 255, 0, 255)]).round().astype(np.uint8)


def write_bmp(path, rgba):
    bgra = rgba[..., [2, 1, 0, 3]].tobytes()
    dib = struct.pack('<IiiHHIIiiII', 108, SIZE, -SIZE, 1, 32, 3, len(bgra), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    Path(path).write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(bgra), 0, 0, 122) + dib + bgra)


def write_png(path, rgba, background=(0xf2, 0xf5, 0xf8), scale=3):
    alpha = rgba[..., 3:] / 255.0
    flat = (rgba[..., :3] * alpha + np.array(background) * (1 - alpha)).round().astype(np.uint8)
    flat = np.repeat(np.repeat(flat, scale, 0), scale, 1)
    raw = b''.join(b'\0' + row.tobytes() for row in flat)
    chunk = lambda kind, body: struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    height, width = flat.shape[:2]
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


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
