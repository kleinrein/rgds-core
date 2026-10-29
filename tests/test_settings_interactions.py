"""Gesture ownership, visible choices, mute and bounded micro-animation regressions."""
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class SettingsInteractionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\ncore=0\n')

    def run_ui(self, script, section=None, animations=True, screenshot=None):
        (self.root / 'input').write_text(script)
        if not animations:
            (self.root / 'ui.ini').write_text('schema=1\nanimations=0\n')
        command = [
            str(BINARY), '--preview', '--settings', '--data',
            str(self.root), '--state',
            str(self.root / 'state'), '--input-script',
            str(self.root / 'input'), '--cache-report',
            str(self.root / 'report'), '--seconds', '1'
        ]
        if screenshot:
            command += ['--screenshot', str(screenshot)]
        if section:
            command += ['--section', section]
        result = subprocess.run(
            command, cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER='dummy'), capture_output=True, text=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report').read_text())

    def test_top_panel_stays_identical_while_navigating_settings_rows(self):
        def panel(path):
            data = path.read_bytes()
            offset = struct.unpack_from('<I', data, 10)[0]
            width, height = struct.unpack_from('<ii', data, 18)
            bits = struct.unpack_from('<H', data, 28)[0]
            stride = ((width * bits + 31) // 32) * 4
            return b''.join(
                data[offset + (height - 1 - y) * stride:offset + (height - y) * stride] for y in range(48, 480)
            )

        for section in (None, 'library', 'controls'):
            with self.subTest(section=section):
                first = self.root / 'first.bmp'
                last = self.root / 'last.bmp'
                # Library's first rows (Scan, Systems, Artwork sources) have their own notes: start on Auto-load
                # and stay among the rows without one (through Browser layout).
                start = '30 key Down\n40 key Down\n50 key Down\n' if section == 'library' else ''
                moves = 3 if section == 'library' else 4
                self.run_ui(start, section, animations=False, screenshot=first)
                self.run_ui(
                    start + ''.join(f'{100 + 50 * n} key Down\n' for n in range(moves)),
                    section,
                    animations=False,
                    screenshot=last
                )
                self.assertEqual(panel(first), panel(last))

    def test_header_and_footer_back_leave_settings_and_subpages(self):
        for x, y in ((50, 42), (72, 452)):
            for section, expected in ((None, 0), ('theme', 1), ('sound', 1)):
                with self.subTest(position=(x, y), section=section):
                    report = self.run_ui(f'30 click {x} {y}\n', section)
                    self.assertEqual(report['page'], expected)

    def test_touch_indicator_can_be_changed_by_touch_and_keyboard_and_persists(self):
        # Settings > Display: Touch indicator is the third row.
        self.run_ui('30 click 180 290\n', 'display')
        self.assertIn('touch_indicator=0\n', (self.root / 'ui.ini').read_text())
        # A second run must load Off before toggling it back to On.
        self.run_ui(''.join(f'{30+i*30} key Down\n' for i in range(2)) + '260 key A\n', 'display')
        self.assertIn('touch_indicator=1\n', (self.root / 'ui.ini').read_text())
        self.assertIn('animations=1\n', (self.root / 'ui.ini').read_text())
        self.assertNotIn('touch_indicator', (self.root / 'preferences.ini').read_text())

    def test_retired_default_engines_fall_back_to_dscore(self):
        for core in (0, 1, 3):
            with self.subTest(core=core):
                (self.root / 'preferences.ini').write_text(f'schema=1\nmode=light\nfont=rounded\ncore={core}\n')
                self.assertEqual(self.run_ui('', 'library')['default_core'], 4)

    def test_swipe_scrolls_without_opening_the_pressed_card(self):
        report = self.run_ui('30 tap 210 375\n60 drag 210 250\n90 drag 210 115\n120 release\n')
        self.assertEqual(report['page'], 1)
        self.assertGreater(report['list_scroll'], 200)

    def test_tap_cancelled_by_focus_loss_does_not_activate(self):
        report = self.run_ui('30 tap 210 129\n60 focus-lost\n90 release\n')
        self.assertEqual(report['page'], 1)

    def test_scrollbar_grab_does_not_jump_until_moved(self):
        report = self.run_ui('30 tap 612 200\n60 release\n')
        self.assertEqual(report['list_scroll'], 0)

    def test_mute_cancels_held_slider_repeat_and_survives_restart(self):
        self.run_ui('30 key Right\n90 click 180 230\n', 'sound')
        self.assertIn('volume=0\n', (self.root / 'volume.ini').read_text())
        self.run_ui('', 'sound')
        self.assertIn('volume=0\n', (self.root / 'volume.ini').read_text())
        self.run_ui('30 click 180 230\n', 'sound')
        self.assertIn('volume=25\n', (self.root / 'volume.ini').read_text())

    def test_duplicate_mouse_delivery_after_finger_mute_does_not_unmute(self):
        self.run_ui('30 finger-down 0.28125 0.7395833\n60 finger-up 0.28125 0.7395833\n80 click 180 230\n', 'sound')
        self.assertIn('volume=0\n', (self.root / 'volume.ini').read_text())

    def test_open_picker_settles_and_reduced_motion_applies_immediately(self):
        report = self.run_ui('90 key Down\n100 key A\n', 'library', animations=False)
        self.assertLess(report['frames'], 12)
        self.assertEqual(report['render_file_reads'], 0)


if __name__ == '__main__':
    unittest.main()
