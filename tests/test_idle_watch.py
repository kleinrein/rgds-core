import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))

from idle_watch import EV_ABS, EV_KEY, IdleWatch, PanelDim, timers


class IdleWatchTest(unittest.TestCase):
    def test_timers_follow_system_ini_with_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(timers(folder), (60, 0))
            Path(folder, 'system.ini').write_text('schema=1\ndim=0\ngame_sleep=3\n')
            self.assertEqual(timers(folder), (0, 30 * 60))
            Path(folder, 'system.ini').write_text('schema=1\ndim=9\ngame_sleep=x\n')
            self.assertEqual(timers(folder), (60, 0))

    def test_buttons_hats_touch_and_big_stick_moves_count_noise_does_not(self):
        with tempfile.TemporaryDirectory() as folder:
            watch = IdleWatch(Path(folder))
        watch.devices = {3: {0: (0, 450)}}
        self.assertTrue(watch.counts(3, EV_KEY, 304, 1))
        self.assertTrue(watch.counts(3, EV_ABS, 0x10, -1))
        self.assertFalse(watch.counts(3, EV_ABS, 0x10, 0))
        self.assertTrue(watch.counts(3, EV_ABS, 0x35, 200))
        self.assertFalse(watch.counts(3, EV_ABS, 0, 120))
        self.assertTrue(watch.counts(3, EV_ABS, 0, -1500))
        self.assertFalse(watch.counts(3, EV_ABS, 1, 1500))  # an axis with no known range

    def test_idle_time_and_touch(self):
        now = [100.0]
        with tempfile.TemporaryDirectory() as folder:
            watch = IdleWatch(Path(folder), clock=lambda: now[0])
        now[0] = 145
        self.assertEqual(watch.idle(), 45)
        watch.touch()
        self.assertEqual(watch.idle(), 0)

    def test_dim_lowers_every_panel_and_restores_exactly(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'class/backlight'
            for name, value in (('backlight0', '200'), ('backlight1', '37'), ('off', '0')):
                (root / name).mkdir(parents=True)
                (root / name / 'brightness').write_text(value)
            panels = PanelDim(Path(folder))
            panels.dim(25)
            self.assertTrue(panels.dimmed)
            self.assertEqual((root / 'backlight0/brightness').read_text(), '50')
            self.assertEqual((root / 'backlight1/brightness').read_text(), '9')
            self.assertEqual((root / 'off/brightness').read_text(), '0')
            panels.dim(10)  # already dim: the saved levels stay the real ones
            panels.restore()
            self.assertFalse(panels.dimmed)
            self.assertEqual((root / 'backlight0/brightness').read_text(), '200')
            self.assertEqual((root / 'backlight1/brightness').read_text(), '37')


if __name__ == '__main__':
    unittest.main()
