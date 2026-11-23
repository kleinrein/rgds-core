"""Rewind (docs/TIME_TRAVEL_PLAN.md): the timeline's own tests, then a real session on the test cartridge."""
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'build/play/host/play-timeline-tests'


@unittest.skipUnless(TESTS.exists(), 'Build RG DS Play first: tools/build_play.py host')
class Timeline(unittest.TestCase):
    def test_timeline_and_session(self):
        spec = importlib.util.spec_from_file_location('rgds_play_testrom', ROOT / 'play/tests/testrom/build.py')
        testrom = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(testrom)
        with tempfile.TemporaryDirectory() as temp:
            rom = testrom.build(Path(temp) / 'Play Test Rooms (World).gba')
            data = Path(temp) / 'userdata/play'
            data.mkdir(parents=True)
            result = subprocess.run(
                [str(TESTS), str(rom), str(data), str(Path(temp) / 'rewind.bmp')],
                capture_output=True,
                text=True,
                timeout=300
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((Path(temp) / 'rewind.bmp').stat().st_size > 0)


if __name__ == '__main__':
    unittest.main()
