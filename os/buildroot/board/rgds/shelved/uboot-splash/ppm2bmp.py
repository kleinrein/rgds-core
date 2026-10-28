#!/usr/bin/env python3
"""The kernel's boot logo (board/rgds/boot-logo.ppm, plain P3, up to 256 colours) as an 8-bit BMP for U-Boot's splash
(uboot-patches/004 reads /splash.bmp from the boot partition). Standard library only; run by post-image.sh.

  ppm2bmp.py boot-logo.ppm splash.bmp
"""
import struct
import sys


def read_ppm(path):
    words = []
    for line in open(path):
        words += line.split('#', 1)[0].split()
    if words[0] != 'P3':
        raise SystemExit(f'{path}: not a plain (P3) PPM')
    width, height, top = map(int, words[1:4])
    values = list(map(int, words[4:4 + width * height * 3]))
    if top != 255:
        values = [v * 255 // top for v in values]
    return width, height, [tuple(values[i:i + 3]) for i in range(0, len(values), 3)]


def write_bmp(path, width, height, pixels):
    palette = sorted(set(pixels))
    if len(palette) > 256:
        raise SystemExit(f'{len(palette)} colours; an 8-bit BMP holds 256')
    index = {colour: i for i, colour in enumerate(palette)}
    stride = (width + 3) & ~3
    rows = b''.join(
        bytes(index[p] for p in pixels[y * width:(y + 1) * width]) + bytes(stride - width)
        for y in reversed(range(height))
    )
    table = b''.join(bytes((b, g, r, 0)) for r, g, b in palette) + bytes(4 * (256 - len(palette)))
    offset = 14 + 40 + len(table)
    with open(path, 'wb') as out:
        out.write(b'BM' + struct.pack('<IHHI', offset + len(rows), 0, 0, offset))
        out.write(struct.pack('<IiiHHIIiiII', 40, width, height, 1, 8, 0, len(rows), 2835, 2835, 256, 0))
        out.write(table + rows)


if __name__ == '__main__':
    write_bmp(sys.argv[2], *read_ppm(sys.argv[1]))
