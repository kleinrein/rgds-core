"""Prepare imagegen wordmarks as alpha-preserving BMPs for native SDL loading."""
from pathlib import Path
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
for theme in ('light', 'dark'):
    path = ROOT / 'assets/brand' / ('wordmark-' + theme)
    image = Image.open(path.with_suffix('.png')).convert('RGBA')
    bounds = image.getchannel('A').point(lambda a: 255 if a > 24 else 0).getbbox()
    image = image.crop(bounds)
    image.thumbnail((768, 108), Image.Resampling.LANCZOS)
    # V4 bitfields explicitly preserve alpha (ordinary 32-bit BMP does not).
    width, height = image.size
    pixels = image.tobytes('raw', 'BGRA')
    dib = struct.pack('<IiiHHIIiiII', 108, width, -height, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    dib += struct.pack('<5I', 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    path.with_suffix('.bmp').write_bytes(struct.pack('<2sIHHI', b'BM', 122 + len(pixels), 0, 0, 122) + dib + pixels)
