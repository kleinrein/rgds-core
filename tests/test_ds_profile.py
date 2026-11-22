"""The rest of the DS profile and the clock the menus keep: src/launcher/ds_profile.hpp under sanitizers
(tests/ds_profile_test.cpp), and the birthday and favourite colour handed to DS games (runtime/emulators.py)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'C++/SDL headers required')
class DsProfileRulesTests(unittest.TestCase):
    def test_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'ds_profile_test'
            includes = [
                flag for flag in subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
                if flag.startswith('-I')
            ]
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(ROOT / 'src'), *includes,
                    str(ROOT / 'tests/ds_profile_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('ds profile: ok', run.stdout)


class DsFirmwareUserTests(unittest.TestCase):
    def test_birthday_and_colour_reach_the_firmware(self):
        from emulators import ds_firmware_user
        with tempfile.TemporaryDirectory() as tmp:
            player = Path(tmp)
            self.assertEqual(ds_firmware_user(player), {})
            (player / 'ds.ini').write_text('nickname=Andreas\nbirthday=02-29\n')
            (player / 'ui.ini').write_text('accent=2\nsound_toy=0\naccent=11\n')
            self.assertEqual(ds_firmware_user(player), {'birthday_month': 2, 'birthday_day': 29, 'colour': 11})
            (player / 'ds.ini').write_text('birthday=04-31\n')
            (player / 'ui.ini').write_text('accent=-1\n')
            self.assertEqual(ds_firmware_user(player), {})


if __name__ == '__main__':
    unittest.main()
