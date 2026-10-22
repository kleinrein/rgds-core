"""Build small antialiased status glyphs at their exact native display size."""
from pathlib import Path
import struct
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1] / 'assets/status'
ROOT.mkdir(parents=True, exist_ok=True)
S = 4
for theme, ink, quiet in [('light', '#2d3944', '#bfccd7'), ('dark', '#e5eef7', '#405368')]:
    for name in ['sound', 'muted', 'wifi-off', 'wifi-0', 'wifi-1', 'wifi-2', 'wifi-3']:
        im = Image.new('RGBA', (28 * S, 28 * S))
        d = ImageDraw.Draw(im)

        def line(points, fill=ink, width=2):
            pts = [(round(x * S), round(y * S)) for x, y in points]
            d.line(pts, fill=fill, width=round(width * S), joint='curve')
            for x, y in (pts[0], pts[-1]):
                d.ellipse((x - width * S / 2, y - width * S / 2, x + width * S / 2, y + width * S / 2), fill=fill)

        if name in ['sound', 'muted']:
            d.polygon([(x * S, y * S) for x, y in [(3, 10), (7, 10), (13, 5), (13, 23), (7, 18), (3, 18)]], fill=ink)
            if name == 'sound':
                for radius in [6, 11]:
                    d.arc(
                        ((12 - radius) * S, (14 - radius) * S, (12 + radius) * S, (14 + radius) * S),
                        -48,
                        48,
                        fill=ink,
                        width=2 * S
                    )
            else:
                line([(18, 11), (24, 17)])
                line([(24, 11), (18, 17)])
        else:
            level = 0 if name == 'wifi-off' else int(name[-1])
            for i, radius in enumerate([6, 11, 16], 1):
                d.arc(
                    ((14 - radius) * S, (21 - radius) * S, (14 + radius) * S, (21 + radius) * S),
                    -137,
                    -43,
                    fill=ink if level >= i else quiet,
                    width=round(2.3 * S)
                )
            # Off: the dot goes quiet with the arcs, so only the slash reads as ink.
            d.ellipse((12 * S, 19 * S, 16 * S, 23 * S), fill=quiet if name == 'wifi-off' else ink)
            if name == 'wifi-off':
                line([(5, 5), (23, 23)], quiet, 4)
                line([(5, 5), (23, 23)], ink, 2)
        im = im.resize((28, 28), Image.Resampling.LANCZOS)
        pixels = im.tobytes('raw', 'BGRA')
        dib = struct.pack('<IiiHHIIiiII', 108, 28, -28, 1, 32, 3, len(pixels), 0, 0, 0, 0)
        dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
        (ROOT / (theme + '-' + name + '.bmp')
         ).write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(pixels), 0, 0, 122) + dib + pixels)
