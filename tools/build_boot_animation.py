#!/usr/bin/env python3
"""Pre-render a lightweight, looping early-boot animation; no device image libraries.

The frames match the launcher's own boot scene (main.cpp boot_scene): the tile
and wordmark rest, the floor shadow breathes and a slow wave of light passes
through the loader dots, on the same 2.4 s period, so the hand-off from the
early KMS service to the launcher is seamless.
"""
import math
from pathlib import Path
import struct
import zlib
from build_boot_assets import boot_panel, dots, place, shadow, fill, BACKGROUND

ROOT = Path(__file__).resolve().parents[1]
FRAMES, FPS = 36, 15


def frames():
    base = bytearray(boot_panel(with_dots=False))
    fill(base, 640, 270, 236, 100, 10, BACKGROUND)  # the shadow is redrawn per frame
    for index in range(FRAMES):
        phase = index / FRAMES
        p = bytearray(base)
        shadow(p, (1 + math.cos(phase * 2 * math.pi)) * .5)
        place(p, 640, 'mark-120.bmp', 260, 112)
        dots(p, phase)
        bgra = bytearray(640 * 480 * 4)
        bgra[0::4] = p[2::3]
        bgra[1::4] = p[1::3]
        bgra[2::4] = p[0::3]
        bgra[3::4] = b'\xff' * (640 * 480)
        yield bytes(bgra)


def build(path):
    compressor = zlib.compressobj(6)
    data = b''.join(compressor.compress(frame) for frame in frames()) + compressor.flush()
    Path(path).write_bytes(struct.pack('<4s4I', b'RGA1', 640, 480, FRAMES, FPS) + data)


if __name__ == '__main__': build(ROOT / 'assets/boot/animation.rga')
