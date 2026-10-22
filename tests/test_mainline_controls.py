"""Exercise physical mainline button numbers through the real launcher."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class MainlineControlsTests(unittest.TestCase):
    def run_input(self, script):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            inputs = root / 'input'
            inputs.write_text(script)
            (root / 'ui.ini').write_text(
                "schema=1\nhome_layout=carousel\nbackground=plain\n"
            )  # the carousel home these scripts drive
            report = root / 'report.json'
            env = dict(
                os.environ,
                SDL_VIDEODRIVER='dummy',
                RGDS_SESSION_PLATFORM='independent',
                RGDS_INPUT_PROFILE='mainline-rg-ds'
            )
            result = subprocess.run(
                [
                    str(BINARY), '--preview', '--home', '--seconds', '1', '--app',
                    str(ROOT), '--data',
                    str(root), '--input-script',
                    str(inputs), '--cache-report',
                    str(report)
                ],
                env=env,
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(report.read_text())

    def test_physical_a_enters_category_instead_of_exiting(self):
        r = self.run_input('100 joy-down 14\n120 joy-up 14\n300 joy-down 1\n320 joy-up 1\n')
        self.assertNotEqual(r['page'], 0)  # first home tile opens settings

    def test_physical_b_at_home_does_not_exit_the_console(self):
        r = self.run_input(
            '100 joy-down 0\n120 joy-up 0\n250 joy-down 14\n270 joy-up 14\n450 joy-down 1\n470 joy-up 1\n'
        )
        self.assertNotEqual(r['page'], 0)  # later A must still be processed

    def test_dpad_is_buttons_and_can_return_home(self):
        r = self.run_input(
            '100 joy-down 14\n120 joy-up 14\n250 joy-down 1\n270 joy-up 1\n450 joy-down 0\n470 joy-up 0\n'
        )
        self.assertEqual(r['page'], 0)
