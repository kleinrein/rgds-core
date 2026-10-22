"""Both semaphore modes preserve scanline handoff and permit semantics."""
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MelonDSSemaphoreTests(unittest.TestCase):
    def test_native_concurrency_and_non_linux_fallback(self):
        compiler = shutil.which('c++')
        if not compiler: self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as name:
            binary = Path(name) / 'semaphore'
            subprocess.run(
                [
                    compiler, '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT / 'src'),
                    str(ROOT / 'tests/fixtures/melonds_semaphore.cpp'), '-o',
                    str(binary)
                ],
                check=True,
                capture_output=True,
                text=True
            )
            run = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
            reports = [json.loads(line) for line in run.stdout.splitlines()]
            self.assertEqual(len(reports), 2)
            self.assertEqual(reports[0]['backend'], 'condition-variable')
            self.assertEqual(reports[1]['backend'], 'posix' if platform.system() == 'Linux' else 'condition-variable')
            self.assertTrue(all(row['counts_reset_timeout_visibility'] for row in reports))


if __name__ == '__main__': unittest.main()
