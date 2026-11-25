"""runtime/menu_snapshot.top_panel: the paused game's backdrop for the in-game menu. On the RG DS Plus the capture is
1024x768 and is brought down to 640x480 with nearest sampling; the strided-slice path must give exactly the pixels a
plain per-pixel loop gives (it replaced one that took 1.2 s on the handheld, 2026-11-24)."""
from pathlib import Path
import random
import struct
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
from menu_snapshot import top_panel  # noqa: E402


def capture(width, height, bits, top_down=False, seed=1):
    rng = random.Random(seed)
    stride = (width * bits // 8 + 3) & ~3
    body = bytes(rng.getrandbits(8) for _ in range(stride * height))
    signed = -height if top_down else height
    if bits == 32:
        return struct.pack('<2sIHHI', b'BM', 66 + len(body), 0, 0, 66) + struct.pack(
            '<IiiHHIIiiII', 40, width, signed, 1, 32, 3, len(body), 0, 0, 0, 0
        ) + struct.pack('<III', 0xff0000, 0xff00, 0xff) + body
    return struct.pack('<2sIHHI', b'BM', 54 + len(body), 0, 0, 54) + struct.pack(
        '<IiiHHIIiiII', 40, width, signed, 1, 24, 0, len(body), 0, 0, 0, 0
    ) + body


def reference(data):
    """The expected 24-bit top-down pixels, one source pixel per output pixel, as the original loop chose them."""
    offset = struct.unpack_from('<I', data, 10)[0]
    _, width, height, _, bits, _ = struct.unpack_from('<IiiHHI', data, 14)
    h = abs(height)
    stride = (width * bits // 8 + 3) & ~3
    crop_w = width // 2 if width >= h * 2 else width
    crop_h = h // 2 if h >= width * 3 // 2 else h
    out_w, out_h = min(640, crop_w), min(480, crop_h)
    out_stride = (out_w * 3 + 3) & ~3
    pixels = bytearray(out_stride * out_h)
    for y in range(out_h):
        sy = y * crop_h // out_h
        row = offset + (sy if height < 0 else h - 1 - sy) * stride
        for x in range(out_w):
            start = row + x * crop_w // out_w * (bits // 8)
            pixels[y * out_stride + x * 3:y * out_stride + x * 3 + 3] = data[start:start + 3]
    return bytes(pixels)


class TopPanel(unittest.TestCase):

    def check(self, width, height, bits, top_down=False):
        data = capture(width, height, bits, top_down, seed=width * 7 + height)
        with tempfile.TemporaryDirectory() as tmp:
            source, out = Path(tmp) / 'capture.bmp', Path(tmp) / 'menu-background.bmp'
            source.write_bytes(data)
            top_panel(source, out)
            self.assertEqual(out.read_bytes()[54:], reference(data), f'{width}x{height} {bits}-bit')

    def test_plus_panel_capture_matches_nearest_sampling(self):
        self.check(1024, 768, 32)
        self.check(1024, 768, 24, top_down=True)

    def test_other_sizes_match(self):
        for size in ((800, 600), (1280, 480), (1024, 1536), (640, 480), (997, 613)):
            self.check(*size, 32)

    def test_plus_capture_is_quick(self):
        data = capture(1024, 768, 32)
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'capture.bmp'
            source.write_bytes(data)
            began = time.monotonic()
            top_panel(source, Path(tmp) / 'out.bmp')
            # The per-pixel loop took ~100 ms on a Mac (1.2 s on the handheld); the sliced one ~5 ms.
            self.assertLess(time.monotonic() - began, 0.05)


if __name__ == '__main__':
    unittest.main()
