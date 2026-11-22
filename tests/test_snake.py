"""Snake, the launcher's easter egg: the game under sanitizers (tests/snake_test.cpp), and the way in (five quick
taps on the battery on Home's upper screen) and its settings in the preview launcher."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RGDS_TEST_LAUNCHER', ROOT / 'build/host/rgds-launcher'))


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class SnakeEngineTests(unittest.TestCase):
    def test_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'snake_test'
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/snake_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('snake: ok', run.stdout)


def battery_taps(start, count, gap=200):
    # The preview's upper half is the top panel's touch screen: the battery sits at its top right.
    return ''.join(
        f'{start + i * gap} finger-down 0.93 0.025\n{start + i * gap + 40} finger-up 0.93 0.025\n'
        for i in range(count)
    )


def keys(start, *names, gap=150):
    return ''.join(f'{start + i * gap} key {name}\n' for i, name in enumerate(names))


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class SnakeUITests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='rgsn'))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.prefs = self.root / 'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        (self.root / 'ui.ini').write_text('schema=1\nhome_layout=column\nbackground=plain\n')
        self.data = self.root / 'data'
        self.saved = self.data / 'snake.ini'

    def run_ui(self, script, seconds=3):
        (self.root / 'input.txt').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input.txt'), '--seconds',
                str(seconds)
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def values(self):
        return dict(line.split('=', 1) for line in self.saved.read_text().split())

    def test_five_quick_taps_open_it_and_its_switches_are_kept(self):
        # Open, then Y (Walls), X (Classic screen) and R twice (speed 5 -> 7).
        self.run_ui(battery_taps(600, 5) + keys(1800, 'y', 'x', ']', ']'))
        self.assertEqual(self.values(), {'speed': '7', 'walls': '1', 'classic': '1', 'best': '0', 'best_walls': '0'})

    def test_four_taps_or_slow_taps_do_not(self):
        self.run_ui(battery_taps(600, 4) + keys(1800, 'y'))
        self.assertFalse(self.saved.exists())
        self.run_ui(battery_taps(600, 5, gap=800) + keys(4800, 'y'), seconds=5)
        self.assertFalse(self.saved.exists())

    def test_with_walls_the_edge_ends_it_and_b_leaves(self):
        # Walls on, start: the snake heads right from the middle and meets the wall in under two seconds. B on the
        # game-over card goes back to Home, where Y no longer reaches the game.
        script = battery_taps(600, 5) + keys(1800, 'y') + keys(2000, 'a')
        script += keys(5000, 'b') + keys(5500, 'y')
        self.run_ui(script, seconds=6)
        # Still moving, B would only have paused it and Y switched Walls off again; left, Y never reaches it.
        self.assertEqual(self.values()['walls'], '1')


if __name__ == '__main__':
    unittest.main()
