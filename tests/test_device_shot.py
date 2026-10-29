"""Check the screenshot tool's BMP reader, PNG writer and compositor without a device."""
from pathlib import Path
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

try:
    import numpy as np
    import device_shot
except ImportError:
    np = None


def bmp(pixels, depth=32, top_down=False):
    rows, width = pixels.shape[:2]
    step = (width * depth // 8 + 3) // 4 * 4
    data = bytearray()
    for row in (pixels if top_down else pixels[::-1]):
        line = bytearray()
        for r, g, b in row:
            line += bytes((b, g, r)) + (b'\xff' if depth == 32 else b'')
        data += line.ljust(step, b'\0')
    header = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, width, -rows if top_down else rows, 1, depth, 0, len(data), 0, 0, 0, 0)
    return header + info + bytes(data)


@unittest.skipUnless(np, 'numpy required')
class DeviceShotTests(unittest.TestCase):
    def test_bmp_orientations_and_depths(self):
        pixels = np.arange(2 * 3 * 3, dtype=np.uint8).reshape(2, 3, 3)
        for depth in (24, 32):
            for top_down in (False, True):
                np.testing.assert_array_equal(device_shot.read_bmp(bmp(pixels, depth, top_down)), pixels)

    def test_png_roundtrip(self):
        import tempfile
        pixels = np.random.default_rng(1).integers(0, 256, (5, 7, 4), dtype=np.uint8)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'x.png'
            device_shot.write_png(path, pixels)
            data = path.read_bytes()
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        idat = data[data.index(b'IDAT') + 4:data.index(b'IEND') - 8]
        rows = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(5, 1 + 7 * 4)
        self.assertTrue((rows[:, 0] == 1).all())
        np.testing.assert_array_equal(np.cumsum(rows[:, 1:].reshape(5, 7, 4), axis=1, dtype=np.uint8), pixels)

    def test_compose_sizes(self):
        top = np.full((240, 320, 3), 200, np.uint8)
        bottom = np.full((240, 320, 3), 40, np.uint8)
        self.assertEqual(device_shot.compose(top, bottom, 'plain', 'stack', 'auto', 2).shape, (960, 640, 3))
        self.assertEqual(device_shot.compose(top, bottom, 'plain', 'side', 'auto', 1).shape, (240, 640, 3))
        for style in ('float', 'device'):
            for layout in ('stack', 'side'):
                self.assertEqual(device_shot.compose(top, bottom, style, layout, 'auto', 1).shape[2], 3)
                clear = device_shot.compose(top, bottom, style, layout, 'transparent', 1)
                self.assertEqual(clear.shape[2], 4)
                self.assertEqual(clear[0, 0, 3], 0)

    def test_device_shells(self):
        top = np.full((240, 320, 3), 120, np.uint8)
        sizes = {
            shell: device_shot.compose(top, top, 'device', 'stack', 'auto', 1, shell).shape
            for shell in device_shot.SHELLS
        }
        self.assertEqual(len(set(sizes.values())), 1)
        self.assertGreater(sizes['black'][0], sizes['black'][1])  # an open clamshell is taller than wide


if __name__ == '__main__':
    unittest.main()
