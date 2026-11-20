"""The sound toy: its voices under sanitizers (tests/voice_toy_test.cpp), and its screens in the preview launcher."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RGDS_TEST_LAUNCHER', ROOT / 'build/host/rgds-launcher'))


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class VoiceToyEngineTests(unittest.TestCase):
    def test_voices_curve_and_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'voice_toy_test'
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/voice_toy_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('voice toy: ok', run.stdout)


def keys(start, *names, gap=150):
    return ''.join(
        f'{start + i * gap} key {name}\n{start + i * gap + 30} key-up {name}\n' for i, name in enumerate(names)
    )


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class VoiceToyUITests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='rgvt'))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.prefs = self.root / 'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        self.ui = self.root / 'ui.ini'
        self.ui.write_text('schema=1\nhome_layout=carousel\nbackground=plain\n')
        self.data = self.root / 'data'

    def run_ui(self, script='', extra=(), seconds=2):
        (self.root / 'input.txt').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input.txt'), '--seconds',
                str(seconds), '--cache-report',
                str(self.root / 'report.json'), *extra
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def open_toy(self):
        # Carousel: Settings, Games, Favourites, Recents, PictoChat, Sound toy (Music is off by default), so 5.
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')

    def test_the_tile_is_on_home_and_its_setting_hides_it(self):
        self.open_toy()
        self.run_ui(keys(400, 'a', 'b'))
        self.assertIn('home 5', (self.root / 'state').read_text())
        self.ui.write_text(self.ui.read_text() + 'sound_toy=0\n')
        self.run_ui()
        self.assertIn('home 4', (self.root / 'state').read_text())  # the tile is gone: back to the last one

    def test_recording_saves_a_sound_and_the_startup_sound_follows_it(self):
        self.open_toy()
        # Open, record the preview's stand-in "hello" (it stops by itself after 1.55 s and plays), pick Robot, make
        # it the startup sound with Start (joystick button 7), then leave.
        script = keys(400, 'a') + keys(800, 'x') + keys(3200, 'right', 'right')
        script += '3700 joy-down 7\n3730 joy-up 7\n' + keys(4000, 'b')
        self.run_ui(script, seconds=5)
        folder = self.data / 'sound-toy'
        wav = (folder / 'slot-1.wav').read_bytes()
        self.assertEqual(wav[:4], b'RIFF')
        rate, = struct.unpack_from('<I', wav, 24)
        self.assertEqual(rate, 16000)
        self.assertGreater(len(wav), 44 + 16000)  # more than half a second of sound
        self.assertIn('voice=2', (folder / 'slot-1.ini').read_text())
        self.assertEqual((folder / 'startup.ini').read_text(), 'slot=1\n')
        startup = (folder / 'startup.wav').read_bytes()
        self.assertEqual(startup[:4], b'RIFF')
        self.assertGreater(len(startup), 44 + 16000)
        self.assertIn('home 5', (self.root / 'state').read_text())  # back on Home, on the tile

    def test_clear_takes_the_drawing_then_twice_erases_the_sound(self):
        self.open_toy()
        # Record, then draw a stroke across the pad (lower screen), then Y: the drawing goes, the sound stays.
        script = keys(400,
                      'a') + keys(700, 'x') + '2600 tap 100 150\n2620 drag 300 120\n2640 drag 500 140\n2660 release\n'
        script += keys(3000, 'y')
        self.run_ui(script, seconds=4)
        folder = self.data / 'sound-toy'
        self.assertTrue((folder / 'slot-1.wav').exists())
        bend = (folder / 'slot-1.ini').read_text().split('bend=')[1]
        self.assertEqual({float(v) for v in bend.split(',')}, {0.0})
        # Nothing drawn now: Y once only asks, Y again erases the sound.
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        self.run_ui(keys(400, 'a') + keys(800, 'y'), seconds=2)
        self.assertTrue((folder / 'slot-1.wav').exists())
        self.run_ui(keys(400, 'a') + keys(800, 'y', 'y'), seconds=2)
        self.assertFalse((folder / 'slot-1.wav').exists())

    def test_right_walks_through_every_voice_onto_the_next_pages(self):
        self.open_toy()
        script = keys(400, 'a') + keys(700, 'x') + keys(2600, *['right'] * 13)
        self.run_ui(script, seconds=5)
        self.assertIn('voice=13', (self.data / 'sound-toy/slot-1.ini').read_text())  # Megaphone, page three

    def test_the_column_has_three_apps_in_its_last_row_and_walks_them(self):
        self.ui.write_text('schema=1\nhome_layout=vertical\nbackground=plain\nmusic_player=1\n')
        (self.root / 'state').write_text('1 0 0 ""\nhome 0\n')
        # From Settings (last row, left), Right twice reaches the Sound toy beside Music; Up goes to PictoChat's row.
        self.run_ui(keys(400, 'right', 'right'))
        self.assertIn('home 7', (self.root / 'state').read_text())
        self.run_ui(keys(400, 'up'))
        self.assertIn('home 4', (self.root / 'state').read_text())


if __name__ == '__main__':
    unittest.main()
