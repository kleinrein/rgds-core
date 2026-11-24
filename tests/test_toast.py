"""Toasts: the model under sanitizers (tests/toast_test.cpp), and in the preview launcher the USB drive notice from setup's
Add your games step: a request that fails with no cable shows as a warning card, pressing again does not stack a copy,
and a tap on the card sends it away."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RGDS_TEST_LAUNCHER', ROOT / 'build/host/rgds-launcher'))
WARNING = 2  # toast::Severity::Warning

# The real device helper, except that its USB drive request fails the way it does on the handheld with no cable.
NO_CABLE = '''import importlib.util, sys
sys.path.insert(0, {runtime!r})
spec = importlib.util.spec_from_file_location('device_services_real', {real!r})
real = importlib.util.module_from_spec(spec)
spec.loader.exec_module(real)
def no_cable(self):
    raise ValueError('Connect the handheld to a computer first.')
real.Services.usb_drive = no_cable
sys.exit(real.main())
'''


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class ToastModelTests(unittest.TestCase):
    def test_the_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'toast_test'
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/toast_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('toast: ok', run.stdout)


def tap(at, x, y):
    return f'{at} finger-down {x} {y}\n{at + 40} finger-up {x} {y}\n'


USB_ROW = (0.3, 0.60)  # setup's Use as USB drive row, on the preview's lower half
CARD = (0.5, 0.535)  # the top toast, just under the lower screen's top edge


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class ToastUITests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='rgtoast'))
        self.addCleanup(shutil.rmtree, self.root, True)
        # A copy of the app tree by links, with the device helper swapped for the no-cable stand-in.
        self.app = self.root / 'app'
        (self.app / 'runtime').mkdir(parents=True)
        for entry in ROOT.iterdir():
            if entry.name != 'runtime': (self.app / entry.name).symlink_to(entry)
        for entry in (ROOT / 'runtime').iterdir():
            if entry.name != 'device_services.py': (self.app / 'runtime' / entry.name).symlink_to(entry)
        (self.app / 'runtime/device_services.py').write_text(
            NO_CABLE.format(runtime=str(ROOT / 'runtime'), real=str(ROOT / 'runtime/device_services.py'))
        )
        self.data = self.root / 'data'
        self.data.mkdir()
        (self.data / 'setup.ini').write_text('step=8\n')  # Add your games

    def run_ui(self, script, seconds=3):
        (self.data / 'input.txt').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--boot-startup', '--first-run', '--data',
                str(self.data), '--input-script',
                str(self.data / 'input.txt'), '--cache-report',
                str(self.data / 'report.json'), '--seconds',
                str(seconds)
            ],
            cwd=self.app,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.data / 'report.json').read_text())['toasts']

    def test_no_cable_is_a_warning_card(self):
        toasts = self.run_ui(tap(600, *USB_ROW))
        self.assertEqual(
            toasts, [{
                'text': 'Connect the handheld to a computer first.',
                'severity': WARNING,
                'leaving': 0
            }]
        )

    def test_pressing_again_does_not_stack_a_copy(self):
        toasts = self.run_ui(tap(600, *USB_ROW) + tap(1600, *USB_ROW), seconds=3)
        self.assertEqual(len(toasts), 1, toasts)

    def test_a_tap_sends_it_away(self):
        toasts = self.run_ui(tap(600, *USB_ROW) + tap(1800, *CARD), seconds=3)
        self.assertEqual(toasts, [])


if __name__ == '__main__':
    unittest.main()
