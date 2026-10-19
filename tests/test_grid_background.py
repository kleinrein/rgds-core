"""The DSi grid background is the same paper behind settings as behind the library, on both screens."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class GridBackgroundTests(unittest.TestCase):
    def shot(self, background):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')
            (root/'ui.ini').write_text('schema=1\nbackground=%s\nanimations=0\n' % background)
            result = subprocess.run([str(BINARY), '--preview', '--settings', '--section', 'system', '--data', str(root),
                                     '--state', str(root/'state'), '--preferences', str(root/'preferences.ini'),
                                     '--frames', '3', '--screenshot', str(root/'s.bmp')],
                                    cwd=ROOT, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'}, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            return (root/'s.bmp').read_bytes()

    @staticmethod
    def row(data, y, x0=200, x1=640):
        offset = struct.unpack_from('<I', data, 10)[0]
        width, height = struct.unpack_from('<ii', data, 18)
        line = offset + (height - 1 - y if height > 0 else y) * width * 4
        return {tuple(data[line + x * 4:line + x * 4 + 3]) for x in range(x0, x1)}

    def test_settings_wear_the_grid_on_both_screens(self):
        grid, plain = self.shot('grid'), self.shot('plain')
        # Empty paper on the upper screen and above the lower screen's header: grid columns cross the row, plain is one colour.
        for y in (120, 490):
            self.assertEqual(len(self.row(plain, y)), 1, y)
            self.assertGreater(len(self.row(grid, y)), 2, y)


if __name__ == '__main__':
    unittest.main()
