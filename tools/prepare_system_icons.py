"""Prepare the generated system artwork for native SDL: bounded alpha BMPs.

The source images are the imagegen renders kept in artwork/systems/. Each
one is cut free of its studio backdrop, cropped to its own artwork, scaled onto
a 128 x 128 canvas with a 116 px optical bound and written as a 32-bit BGRA
BMP, exactly like the launcher icons in tools/prepare_icons.py.

The generator answers "transparent background" with a painted checkerboard
rather than real transparency, so the backdrop has to be taken off. That is the
job of tools/system_matte.py, which runs two segmentation models and puts the
machine back together: the sharp model finds the edge of the hardware, the
blunt one says which of the studio the sharp one kept is really backdrop, and
the parts of the machine that were lost are closed with the photograph.

When rembg is not installed the tool falls back to a colour-model matte, which
is rougher but keeps the pipeline runnable.

    python3 tools/prepare_system_icons.py
    python3 tools/prepare_system_icons.py --sheet out.png   # labelled review sheet
"""
from pathlib import Path
import os
import struct
import sys
import zlib
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'artwork/systems'
TARGET = ROOT / 'assets/systems'
sys.path.insert(0, str(ROOT / 'tools'))

import system_matte

FIT_SIZE = 116
CANVAS_SIZE = 128
MODEL_DIR = ROOT / '.cache/u2net'


def _session(model):
    """The segmentation model, loaded once and kept for the whole run."""
    os.environ.setdefault('U2NET_HOME', str(MODEL_DIR))
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    from rembg import new_session
    # The model runs on the CPU: on this machine the accelerated backend
    # spends minutes compiling the graph and never gets to the picture.
    try:
        return new_session(model, providers=['CPUExecutionProvider'])
    except TypeError:
        return new_session(model)


def cut_background(image, session=None, session2=None):
    """Cut the studio backdrop away, leaving the hardware on transparency.

    The matte is the work of tools/system_matte.py. Two models are used: the
    sharp one finds the shape of the machine, the blunt one says which of the
    studio the sharp one kept is really backdrop.
    """
    image = image.convert('RGBA')
    if image.getchannel('A').getextrema()[0] < 250:
        return image
    if session is None:
        session = _session(system_matte.SEGMENT_MODEL)
    if session2 is None:
        session2 = _session(system_matte.SEGMENT_MODEL_SECOND)
    cut, _kind = system_matte.matte(image, session, session2=session2)
    return cut


def write_bmp(path, canvas):
    pixels = canvas.tobytes('raw', 'BGRA')
    dib = struct.pack('<IiiHHIIiiII', 108, 128, -128, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    path.write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(pixels), 0, 0, 122) + dib + pixels)


def sheet(path, columns=5, size=176):
    """A labelled review sheet of the artwork the launcher actually loads."""
    from build_system_icons import CATALOG, draw_text
    label_scale = 2
    cell = size + 16
    label_height = 5 * label_scale + 14
    width = columns * cell
    height = ((len(CATALOG) + columns - 1) // columns) * (cell + label_height)
    pixels = bytearray(b'\xf2\xf5\xf8' * width * height)
    muted = (0x7c, 0x8b, 0x99)
    for index, entry in enumerate(CATALOG):
        artwork = TARGET / (entry[0] + '.bmp')
        if not artwork.is_file():
            continue
        image = Image.open(artwork).convert('RGBA').resize((size, size), Image.Resampling.LANCZOS)
        ox = (index % columns) * cell + 8
        oy = (index // columns) * (cell + label_height) + 8
        for y in range(size):
            for x in range(size):
                red, green, blue, alpha = image.getpixel((x, y))
                if not alpha:
                    continue
                at = ((oy + y) * width + ox + x) * 3
                pixels[at:at + 3] = bytes((red, green, blue))
        draw_text(pixels, width, ox, oy + size + 6, entry[1].upper(), label_scale, muted)
    raw = b''.join(b'\0' + bytes(pixels[y * width * 3:(y + 1) * width * 3]) for y in range(height))
    chunk = lambda kind, body: struct.pack('>I', len(body)
                                           ) + kind + body + struct.pack('>I',
                                                                         zlib.crc32(kind + body) & 0xffffffff)
    Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    )


def main():
    TARGET.mkdir(parents=True, exist_ok=True)
    try:
        session = _session(system_matte.SEGMENT_MODEL)
        session2 = _session(system_matte.SEGMENT_MODEL_SECOND)
        print('segmentation models:', system_matte.SEGMENT_MODEL, '+', system_matte.SEGMENT_MODEL_SECOND)
    except Exception as error:  # noqa: BLE001 - the fallback keeps the run alive
        print('no segmentation model (%s); using the colour matte' % error)
        session = None
        session2 = None
    written = []
    for source in sorted(SOURCE.glob('*')):
        if source.suffix.lower() not in ('.png', '.jpg', '.jpeg', '.webp'):
            continue
        image = Image.open(source)
        if session is None:
            image = system_matte.fallback_matte(image)
        else:
            image = cut_background(image, session, session2)
        # Crop only empty padding; normalize optical size within a shared canvas.
        bounds = image.getchannel('A').point(lambda a: 255 if a > 24 else 0).getbbox()
        if not bounds:
            print('skipped (empty):', source.name)
            continue
        image = image.crop(bounds)
        image.thumbnail((FIT_SIZE, FIT_SIZE), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (CANVAS_SIZE, CANVAS_SIZE))
        canvas.alpha_composite(image, ((CANVAS_SIZE - image.width) // 2, (CANVAS_SIZE - image.height) // 2))
        write_bmp(TARGET / (source.stem + '.bmp'), canvas)
        written.append(source.stem)
    print('%d system icons: %s' % (len(written), ', '.join(sorted(written))))


if __name__ == '__main__':
    if '--sheet' in sys.argv:
        sheet(sys.argv[sys.argv.index('--sheet') + 1])
    else:
        main()
