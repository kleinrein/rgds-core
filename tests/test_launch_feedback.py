"""Display ownership handshake and touch feedback through the real SDL renderer."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from emulators import release_launcher_display

BINARY = ROOT / 'build/host/rgds-launcher'


class DisplayHandoffTests(unittest.TestCase):
    def test_engine_waits_for_display_release(self):
        parent, child = socket.socketpair()
        completed = threading.Event()
        errors = []
        def prepare():
            try:
                release_launcher_display(child)
                completed.set()
            except Exception as error:
                errors.append(error)
        with parent, child:
            worker = threading.Thread(target=prepare)
            worker.start()
            parent.settimeout(2)
            try:
                self.assertEqual(parent.recv(1), b'R')
                self.assertFalse(completed.wait(.05), 'Engine must wait for closed SDL panels')
                parent.sendall(b'C')
                self.assertTrue(completed.wait(2))
            finally:
                parent.shutdown(socket.SHUT_RDWR)
                worker.join(2)
        self.assertFalse(errors)

    def test_disconnected_launcher_aborts_before_engine_start(self):
        parent, child = socket.socketpair()
        parent.close()
        with child, self.assertRaises(OSError):
            release_launcher_display(child)

    def test_direct_launch_needs_no_handshake(self):
        release_launcher_display(None)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class FeedbackUITests(unittest.TestCase):
    def exercise(self, script='', animations=True, seconds=1, mode='light', indicator=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'inputs').write_text(script)
            (root / 'preferences.ini').write_text(f'schema=1\nmode={mode}\nfont=rounded\n')
            (root / 'ui.ini').write_text(f'schema=1\nanimations={int(animations)}\ntouch_indicator={int(indicator)}\n')
            screen = root / 'screen.bmp'
            result = subprocess.run([str(BINARY), '--preview', '--library', '--data', str(root),
                '--input-script', str(root / 'inputs'), '--seconds', str(seconds),
                '--screenshot', str(screen), '--state', str(root / 'state'),
                '--cache-report', str(root / 'report')], cwd=ROOT,
                env=dict(os.environ, SDL_VIDEODRIVER='dummy'), capture_output=True, text=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return screen.read_bytes(), json.loads((root / 'report').read_text()), (root / 'state').read_text()

    def crop(self, bmp, x, y, width, height):
        offset = struct.unpack_from('<I', bmp, 10)[0]
        w, h = struct.unpack_from('<ii', bmp, 18)
        depth = struct.unpack_from('<H', bmp, 28)[0] // 8
        stride = (w * depth + 3) & ~3
        return b''.join(bmp[offset + (h-1-row)*stride+x*depth:offset + (h-1-row)*stride+(x+width)*depth] for row in range(y,y+height))

    def test_held_contact_is_visible_then_release_and_focus_loss_clear_it(self):
        base, _, _ = self.exercise(animations=False)
        held, counts, _ = self.exercise('100 tap 100 370\n', animations=False)
        released, _, _ = self.exercise('100 tap 100 370\n200 release\n', animations=False)
        lost, _, _ = self.exercise('100 tap 100 370\n200 focus-lost\n', animations=False)
        crop = lambda image: self.crop(image, 70, 820, 60, 60)
        self.assertNotEqual(crop(base), crop(held))
        self.assertEqual(crop(base), crop(released))
        self.assertEqual(crop(base), crop(lost))
        self.assertLessEqual(counts['frames'], 5, 'Stationary contact must return to idle')
        self.assertEqual(counts['render_file_reads'], 0)

    def test_disabled_indicator_hides_contact_and_still_allows_long_press(self):
        # Compare idle contact work without variable startup-animation frames.
        # The separate long-press run below retains animations.
        base, baseline_counts, _ = self.exercise(indicator=False, animations=False)
        held, counts, _ = self.exercise('100 tap 100 370\n', indicator=False, animations=False)
        self.assertEqual(self.crop(base, 70, 820, 60, 60), self.crop(held, 70, 820, 60, 60))
        self.assertLessEqual(counts['frames'], baseline_counts['frames'] + 3)
        _, _, state = self.exercise('100 tap 320 205\n850 release\n', seconds=2, indicator=False)
        self.assertIn('19 ', state)

    def test_hold_opens_options_but_drag_does_not(self):
        _, counts, state = self.exercise('100 tap 320 205\n850 release\n', seconds=2)
        self.assertIn('19 ', state)
        self.assertEqual(counts['render_file_reads'], 0)
        _, _, state = self.exercise('100 tap 320 205\n300 drag 184 205\n850 release\n', seconds=2)
        self.assertNotIn('19 ', state)

    def test_launch_blocks_repeated_start_and_navigation_then_returns_to_idle(self):
        _, counts, state = self.exercise('100 key A\n200 key A\n250 key Right\n300 key-up Right\n', seconds=3)
        self.assertEqual(int(state.split()[1]), 0)
        self.assertLess(counts['frames'], 105, 'One bounded preview, then idle')
        self.assertEqual(counts['render_file_reads'], 0)

    def test_overlay_preserves_library_and_fades_back_to_it(self):
        base, _, _ = self.exercise(animations=False)
        visible, _, _ = self.exercise('600 key A\n')
        # The library remains beneath the scrim: distinct carousel and map pixels
        # survive beneath the wash instead of becoming a flat page.
        outside = self.crop(visible, 10, 885, 620, 60)
        self.assertGreater(len(set(outside)), 8)
        self.assertNotEqual(self.crop(base, 106, 577, 428, 290), self.crop(visible, 106, 577, 428, 290))
        returned, counts, _ = self.exercise('100 key A\n', seconds=2)
        self.assertEqual(self.crop(base, 0, 480, 640, 480), self.crop(returned, 0, 480, 640, 480))
        self.assertLess(counts['frames'], 85)
        self.assertEqual(counts['render_file_reads'], 0)

    def test_reduced_motion_loading_and_cancel(self):
        baseline, _, _ = self.exercise(animations=False)
        loading, counts, _ = self.exercise('600 key A\n', animations=False, mode='dark')
        self.assertLessEqual(counts['frames'], 5)
        cancelled, _, _ = self.exercise('100 key A\n300 key B\n', animations=False)
        self.assertNotEqual(self.crop(baseline, 0, 480, 640, 480), self.crop(loading, 0, 480, 640, 480))
        self.assertEqual(self.crop(baseline, 0, 480, 640, 480), self.crop(cancelled, 0, 480, 640, 480))


if __name__ == '__main__':
    unittest.main()
