"""Menu audio against a fake device with a slow close: a cue pressed during the idle close still plays."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'C++/SDL headers required')
class MenuAudioTests(unittest.TestCase):
    def test_a_cue_during_the_idle_close_waits_and_plays(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'menu_audio_test'
            includes = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', *includes, '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/menu_audio_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('menu audio: ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
