"""Connectivity's status card follows Wi-Fi joining a saved network by itself (runtime/wpa_wifi.py)."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
BASE = 'state=ready\nmessage=\nwifi_supported=1\nwifi=1\nwifi_radio=1\nwifi_connected=0\nconnection=Not connected\n'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class WifiStatusTests(unittest.TestCase):
    def render(self, fields):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = BASE + fields
            (root / 'data/device').mkdir(parents=True)
            (root / 'data/device/status.ini').write_text(content)
            # the device helper answers every request with the same status
            (root / 'bin').mkdir()
            helper = root / 'bin/python3'
            helper.write_text(
                '#!' + sys.executable + '\nimport sys\nfrom pathlib import Path\n'
                'p=Path(sys.argv[sys.argv.index("--data")+1])/"device"\np.mkdir(parents=True,exist_ok=True)\n'
                f'(p/"status.ini").write_text({content!r})\n'
            )
            helper.chmod(0o755)
            shot = root / 'screen.bmp'
            result = subprocess.run(
                [
                    str(BINARY), '--preview', '--settings', '--section', 'connectivity', '--data',
                    str(root / 'data'), '--seconds', '1.5', '--screenshot',
                    str(shot)
                ],
                cwd=ROOT,
                env=dict(
                    os.environ,
                    SDL_VIDEODRIVER='dummy',
                    SDL_AUDIODRIVER='dummy',
                    PATH=str(root / 'bin') + os.pathsep + os.environ['PATH']
                ),
                capture_output=True,
                text=True,
                timeout=8
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('Could not connect', (root / 'data/device/status.ini').read_text())
            return shot.read_bytes()

    @staticmethod
    def pixel(data, x, y):
        offset, width, height = struct.unpack_from('<Iii', data, 10)[0], *struct.unpack_from('<ii', data, 18)
        row = y if height < 0 else abs(height) - 1 - y
        b, g, r = data[offset + (row * width + x) * 4:offset + (row * width + x) * 4 + 3]
        return r, g, b

    def lit(self, data):
        # the card on the upper screen (40..600 x 296..412): accent-filled while joining, the plain panel otherwise
        return sum(
            1 for x in range(200, 560, 8) for y in range(330, 400, 6)
            if self.pixel(data, x, y)[2] > 140 and self.pixel(data, x, y)[0] < 90
        ) > 100

    def test_joining_a_saved_network_lights_the_card(self):
        self.assertTrue(self.lit(self.render('wifi_joining=Home\nwifi_looking=0\n')))

    def test_looking_and_not_connected_keep_the_plain_card(self):
        self.assertFalse(self.lit(self.render('wifi_joining=\nwifi_looking=1\n')))
        self.assertFalse(self.lit(self.render('wifi_joining=\nwifi_looking=0\n')))


if __name__ == '__main__':
    unittest.main()
