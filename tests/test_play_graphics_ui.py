"""The in-game Graphics page of a 3DS game: the Internal resolution row, driven by script on the host launcher."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key, read_fields


@unittest.skipUnless(BINARY.exists(), 'Host launcher required')
class PlayGraphicsUiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        (self.data / 'ui.ini').write_text('schema=1\nlayout=1\nanimations=0\nbackground=plain\n')
        self.rom = self.root / 'mk7.cci'
        self.rom.write_bytes(b'')
        self.state = self.root / 'state'
        self.state.write_text('1 0 0 ""\n')

    def tearDown(self):
        self.temp.cleanup()

    def run_menu(self, script):
        inputs = self.root / 'input'
        inputs.write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--game-menu', '--game-core', 'rgds-play', '--game-rom',
                str(self.rom), '--game-system', '3ds', '--data',
                str(self.data), '--preferences',
                str(self.root / 'prefs.ini'), '--state',
                str(self.state), '--input-script',
                str(inputs), '--screenshot',
                str(self.root / 'screen.bmp'), '--seconds', '3'
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def play_ini(self):
        return read_fields(self.data / 'library' / game_key(self.rom) / 'play.ini')

    def test_internal_resolution_row_saves_2x_for_the_next_launch(self):
        # Quick menu: Resume is the wide tile, the grid below has Save, Load, Graphics in its first row.
        # Graphics page for a 3DS game: Game screen, Second screen, Bezel, Shader, Internal resolution, Save as default.
        self.run_menu(
            '200 key Down\n400 key Right\n600 key Right\n800 key A\n'
            '1100 key Down\n1300 key Down\n1500 key Down\n1700 key Down\n1900 key A\n'
            '2200 key Down\n2400 key A\n'
        )
        self.assertEqual(self.play_ini().get('scale'), '2')

    def test_internal_resolution_row_saves_3x_too(self):
        self.run_menu(
            '200 key Down\n400 key Right\n600 key Right\n800 key A\n'
            '1100 key Down\n1300 key Down\n1500 key Down\n1700 key Down\n1900 key A\n'
            '2200 key Down\n2400 key Down\n2600 key A\n'
            '2900 key A\n3200 key A\n'
        )
        self.assertEqual(self.play_ini().get('scale'), '3')
