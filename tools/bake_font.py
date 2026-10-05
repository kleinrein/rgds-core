#!/usr/bin/env python3
"""Bake bundled Nunito into portable alpha atlases. Only regeneration needs Pillow."""
from pathlib import Path
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "assets/fonts"
CODEPOINTS = list(range(32, 127)) + list(range(160, 256)) + [0x2013, 0x2014, 0x2026]


def bake(size):
    font = ImageFont.truetype(str(DEST / "source/Nunito.ttf"), size)
    font.set_variation_by_axes([600])
    ascent, _ = font.getmetrics()
    records, x, y, row = [], 2, 2, 0
    for cp in CODEPOINTS:
        left, top, right, bottom = font.getbbox(chr(cp), anchor="ls")
        width, height = right - left, bottom - top
        if x + width + 2 > 512:
            x, y, row = 2, y + row + 2, 0
        records.append((cp, x, y, width, height, left, top, round(font.getlength(chr(cp)) * 64)))
        x += width + 2
        row = max(row, height)
    height = y + row + 2
    image = Image.new("L", (512, height))
    draw = ImageDraw.Draw(image)
    for cp, x, y, w, h, left, top, advance in records:
        draw.text((x - left, y - top), chr(cp), font=font, fill=255, anchor="ls")
    output = DEST / ("rounded-%d.rgf" % size)
    with output.open("wb") as stream:
        stream.write(struct.pack("<4s4I", b"RGF1", 512, height, ascent, len(records)))
        for record in records:
            stream.write(struct.pack("<I7i", *record))
        stream.write(image.tobytes())
    print(output.relative_to(ROOT), output.stat().st_size)


if __name__ == "__main__":
    for size in (16, 20, 24, 28, 36):
        bake(size)
