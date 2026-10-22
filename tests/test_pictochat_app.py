"""The chat pad's behaviour: layout, typing, drawing, rooms, and two apps talking through real helpers."""
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ required')
class PictoChatAppTests(unittest.TestCase):
    def test_app_logic_and_end_to_end_chat_under_sanitizers(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'app_test'
            flags = shlex.split(subprocess.check_output(['sdl2-config', '--cflags'], text=True))
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', *flags, '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/pictochat_app_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run(
                [str(binary), str(ROOT / 'runtime/pictochat_net.py')], capture_output=True, text=True, timeout=120
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pictochat app: ok', run.stdout)
            # Every key must show a real glyph, not the font's "?" fallback.
            keys = subprocess.run([str(binary), '--keys'], capture_output=True, text=True,
                                  check=True).stdout.split('\n')
            font = (ROOT / 'assets/fonts/rounded-24.rgf').read_bytes()
            count = struct.unpack_from('<I', font, 16)[0]
            glyphs = {struct.unpack_from('<I', font, 20 + i * 32)[0] for i in range(count)}
            typed = {key for key in keys if key}
            self.assertGreater(len(typed), 60)
            self.assertEqual([key for key in sorted(typed) if any(ord(c) not in glyphs for c in key)], [])


if __name__ == '__main__':
    unittest.main()
