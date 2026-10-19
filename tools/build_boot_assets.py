#!/usr/bin/env python3
"""Build the static first frame of the native RG DS boot scene from the brand bitmaps, without deps."""
import argparse
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
BACKGROUND = (242, 245, 247)
INK = (45, 57, 68)
ACCENT = (44, 137, 218)
FOCUS = (224, 240, 252)


def atlas(path):
    data = Path(path).read_bytes()
    magic, width, height, ascent, count = struct.unpack_from('<4s4I', data)
    if magic != b'RGF1' or width * height != len(data) - 20 - count * 32:
        raise ValueError('Invalid RGDS font atlas')
    glyphs = {}
    for index in range(count):
        cp, *record = struct.unpack_from('<I7i', data, 20 + index * 32)
        glyphs[chr(cp)] = record
    return width, data[20 + count * 32:], glyphs


def sprite(name):
    """An alpha BMP V4 from assets/brand (tools/brand_logo.py): w, h, BGRA rows."""
    data = (ROOT/'assets/brand'/name).read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    if width <= 0 or height >= 0 or struct.unpack_from('<H', data, 28)[0] != 32:
        raise ValueError('Expected a top-down 32-bit brand bitmap: ' + name)
    return width, -height, data[offset:offset + width * -height * 4]


def place(pixels, width, name, left, top):
    """Straight-alpha composite of a brand bitmap onto RGB pixels, 1:1."""
    sw, sh, src = sprite(name)
    for y in range(sh):
        for x in range(sw):
            b, g, r, a = src[(y * sw + x) * 4:(y * sw + x) * 4 + 4]
            if not a:
                continue
            dest = ((top + y) * width + left + x) * 3
            pixels[dest:dest + 3] = bytes((c * (255 - a) + v * a + 127) // 255 for c, v in zip(pixels[dest:dest + 3], (r, g, b)))


def blend(a, b, percent):
    return tuple((x * (100 - percent) + y * percent) // 100 for x, y in zip(a, b))


def fill(pixels, width, left, top, w, h, colour):
    for y in range(top, top + h):
        pixels[(y * width + left) * 3:(y * width + left + w) * 3] = bytes(colour) * w


def shadow(pixels, breath=.5):
    """The launcher boot scene's floor shadow under the tile (main.cpp boot_emblem)."""
    for i in range(6):
        half = 44 - i * 6 + int(4 * breath)
        fill(pixels, 640, 320 - half, 240 + i, half * 2, 1, blend(BACKGROUND, (0, 0, 0), 7 - i))


def dots(pixels, phase=None):
    """Five round loader dots (main.cpp boot_scene); None: the resting state."""
    import math
    for i in range(5):
        wave = (1 + math.cos(phase * 2 * math.pi - i * .65)) * .5 if phase is not None else .4
        colour = blend(BACKGROUND, ACCENT, int(22 + 68 * wave))
        cx, cy = 288 + i * 16, 368
        for dy in range(-4, 5):
            half = int(math.sqrt(max(0.0, 16.5 - dy * dy)))
            fill(pixels, 640, cx - half, cy + dy, half * 2 + 1, 1, colour)


def boot_panel(with_dots=True):
    """The first frame of the launcher's boot scene: tile, wordmark, dots."""
    pixels = bytearray(bytes(BACKGROUND) * 640 * 480)
    shadow(pixels)
    place(pixels, 640, 'mark-120.bmp', 260, 112)
    place(pixels, 640, 'wordmark-light.bmp', 176, 268)
    if with_dots:
        dots(pixels)
    return bytes(pixels)


def waiting_panel():
    """The vendor waiting renderer's 330 x 50 strip: the wordmark, centred."""
    sw, sh, _ = sprite('wordmark-light.bmp')
    pixels = bytearray(bytes(BACKGROUND) * 330 * 50)
    place(pixels, 330, 'wordmark-light.bmp', (330 - sw) // 2, (50 - sh) // 2)
    return bytes(pixels)


def bmp(width, height, pixels, top_down=True):
    stride = (width * 3 + 3) & ~3
    header = struct.pack('<2sIHHI', b'BM', 54 + stride * height, 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height, 1, 24, 0,
                          stride * height, 3780, 3780, 0, 0)
    rows = []
    for y in range(height) if top_down else range(height - 1, -1, -1):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        bgr = bytearray(len(row))
        bgr[0::3], bgr[1::3], bgr[2::3] = row[2::3], row[1::3], row[0::3]
        rows.append(bytes(bgr) + bytes(stride - len(row)))
    return header + b''.join(rows)


def png(width, height, pixels):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def build(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    panel = boot_panel()
    waiting = waiting_panel()
    (destination / 'logo.bmp').write_bytes(bmp(640, 480, panel))
    (destination / 'logo.png').write_bytes(png(640, 480, panel))
    (destination / 'waiting.bmp').write_bytes(bmp(330, 50, waiting, top_down=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/boot')
    build(parser.parse_args().output)
