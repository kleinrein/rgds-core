"""Holding up or down scrolls menu lists; a tap still moves one row and a release stops the repeat."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class MenuHoldRepeatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.count = len((ROOT/'assets/locales/languages.tsv').read_text().splitlines())
        (self.root/'ui.ini').write_text('schema=1\nlanguage=en-US\nanimations=0\nview_animations=0\n')
        (self.root/'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def focus(self, script):
        # The language picker opens on A from System settings; its focus is in the report.
        (self.root/'input').write_text('30 key A\n40 key-up A\n'+script)
        command = [str(BINARY), '--preview', '--settings', '--section', 'system',
                   '--data', str(self.root), '--state', str(self.root/'state'),
                   '--input-script', str(self.root/'input'), '--cache-report', str(self.root/'report'), '--seconds', '1']
        result = subprocess.run(command, cwd=ROOT, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        report = json.loads((self.root/'report').read_text())
        self.assertTrue(report['picker_open'])
        return report['picker_focus']

    def test_tap_moves_one_row(self):
        start = self.focus('')
        self.assertEqual(self.focus('100 key Down\n'), start+1)
        self.assertEqual(self.focus('100 key Down\n140 key-up Down\n'), start+1)

    def test_held_key_and_hat_repeat_until_released(self):
        start = self.focus('')
        expected = min(start+3, self.count-1)
        self.assertGreaterEqual(self.focus('100 key Down\n900 key-up Down\n'), expected)
        self.assertGreaterEqual(self.focus('100 joy-hat 4\n900 joy-hat 0\n'), expected)
        # Released before the first repeat: still one row, nothing more after release.
        self.assertEqual(self.focus('100 key Down\n300 key-up Down\n'), start+1)

    def test_held_up_repeats_towards_the_top(self):
        start = self.focus('')
        self.assertLessEqual(self.focus('100 key Up\n900 key-up Up\n'), max(start-3, 0))
