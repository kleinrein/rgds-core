"""The chat pad's message format and drawing, built with sanitizers and fuzzed."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ required')
class PictoChatCoreTests(unittest.TestCase):
    def test_codec_decoder_fuzz_and_drawing_under_sanitizers(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'core_test'
            build = subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                                    '-I' + str(ROOT / 'src'), str(ROOT / 'tests/pictochat_core_test.cpp'), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=240)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pictochat core: ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
