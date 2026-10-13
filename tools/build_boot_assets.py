#!/usr/bin/env python3
"""Build the static first frame of the native RG DS boot scene, without deps."""
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


def wordmark(width, height, scale, *, logo_width=None, logo_top=None):
    # Use the same image-generated identity as the launcher. SDL-ready V4 BMP
    # keeps this boot builder independent of Pillow or an image decoder.
    data=(ROOT/'assets/brand/wordmark-light.bmp').read_bytes()
    offset=struct.unpack_from('<I',data,10)[0]
    sw,sh=struct.unpack_from('<ii',data,18)
    if sw<=0 or sh>=0 or struct.unpack_from('<H',data,28)[0]!=32:
        raise ValueError('Expected top-down 32-bit wordmark')
    sh=-sh;tw=logo_width or round(width*.66);th=round(tw*sh/sw)
    left=(width-tw)//2;top=(height-th)//2 if logo_top is None else logo_top
    pixels=bytearray(bytes(BACKGROUND)*width*height)
    for y in range(th):
        for x in range(tw):
            src=offset+((y*sh//th)*sw+x*sw//tw)*4
            b,g,r,a=data[src:src+4];dest=((top+y)*width+left+x)*3
            pixels[dest:dest+3]=bytes((BACKGROUND[i]*(255-a)+v*a+127)//255 for i,v in enumerate((r,g,b)))
    return bytes(pixels)


def boot_panel():
    pixels = bytearray(wordmark(640, 480, 2, logo_width=356, logo_top=264))

    def rounded(x, y, width, height, radius, colour):
        for row in range(height):
            for col in range(width):
                dx = max(radius - col - .5, 0, col + .5 - (width - radius))
                dy = max(radius - row - .5, 0, row + .5 - (height - radius))
                if dx * dx + dy * dy <= radius * radius:
                    offset = ((y + row) * 640 + x + col) * 3
                    pixels[offset:offset + 3] = bytes(colour)

    for index, colour in enumerate((INK, ACCENT)):
        top = 140 + index * 48
        rounded(278, top, 84, 42, 9, colour)
        rounded(281, top + 3, 78, 36, 6, BACKGROUND)
        rounded(286, top + 8, 68, 26, 3, FOCUS)
    rounded(311, 184, 18, 2, 1, ACCENT)
    for x, y, size, strength in ((390,185,4,.7),(320,247,3,.28),(250,185,3,.28),(320,123,3,.28)):
        colour = tuple(round(a * (1 - strength) + b * strength) for a, b in zip(BACKGROUND, ACCENT))
        rounded(x-size//2, y-size//2, size, size, 0, colour)
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
    waiting = wordmark(330, 50, 1)
    (destination / 'logo.bmp').write_bytes(bmp(640, 480, panel))
    (destination / 'logo.png').write_bytes(png(640, 480, panel))
    (destination / 'waiting.bmp').write_bytes(bmp(330, 50, waiting, top_down=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'assets/boot')
    build(parser.parse_args().output)
