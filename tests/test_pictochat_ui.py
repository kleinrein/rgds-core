"""The chat pad in the real launcher: lobby, drawing, typing, sending, two handhelds, accent colour, scrolling."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
RED, BLUE = (251, 0, 24), (0, 89, 243)          # DS colours 2 and 11
INK_BLUE = (35, 110, 210)                       # ink 5
OPEN = '100 key A\n160 key-up A\n300 key A\n360 key-up A\n'      # open PictoChat from Home, join Chat Room A
SEND = (603, 271)                         # beside the keyboard, as on the DS


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def stroke(start, end, at):
    """A touch drag across the pad, starting `at` ms in."""
    (x0, y0), (x1, y1) = start, end
    lines = ['%d tap %d %d' % (at, x0, y0)]
    for i in range(1, 9):
        lines.append('%d drag %d %d' % (at + i * 12, x0 + (x1 - x0) * i // 8, y0 + (y1 - y0) * i // 8))
    lines.append('%d release' % (at + 120))
    return '\n'.join(lines) + '\n'


def tap(x, y, at):
    return '%d tap %d %d\n%d release\n' % (at, x, y, at + 30)


def swatch(index):
    return 58 + index * 36 + 16, 208


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class PictoChatUiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def handheld(self, name, colour, script, seconds=2.5, port=None, peer=None, arguments=None, ui='', mode='light'):
        """Start one launcher showing PictoChat's home tile; returns a handle for finish()."""
        folder = self.root / name
        folder.mkdir()
        (folder / 'pictochat.ini').write_text('name=%s\ncolour=%s\n' % (name, colour))
        (folder / 'state').write_text('1 0 0 ""\nhome 4\n')
        (folder / 'preferences.ini').write_text('schema=1\nmode=%s\nfont=rounded\n' % mode)
        (folder / 'ui.ini').write_text(ui)
        (folder / 'keys').write_text(script)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy')
        if port:
            env['RGDS_PICTOCHAT_ARGS'] = '--port %d --bind 127.0.0.1 --targets 127.0.0.1:%d' % (port, peer)
        if arguments:
            env['RGDS_PICTOCHAT_ARGS'] = arguments
        bmp = folder / 'screen.bmp'
        process = subprocess.Popen([str(BINARY), '--preview', '--home', '--data', str(folder), '--state', str(folder / 'state'),
                                    '--preferences', str(folder / 'preferences.ini'), '--input-script', str(folder / 'keys'),
                                    '--seconds', str(seconds), '--screenshot', str(bmp)], cwd=ROOT, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return folder, process, bmp

    def finish(self, handle):
        folder, process, bmp = handle
        out, err = process.communicate(timeout=60)
        self.assertEqual(process.returncode, 0, out + err)
        return folder, bmp.read_bytes()

    def run_one(self, *args, **keywords):
        return self.finish(self.handheld(*args, **keywords))

    @staticmethod
    def pixel(data, x, y):
        offset = struct.unpack_from('<I', data, 10)[0]
        width, height = struct.unpack_from('<ii', data, 18)
        index = offset + ((height - 1 - y if height > 0 else y) * width + x) * 4
        b, g, r = data[index:index + 3]
        return r, g, b

    def count(self, data, colour, x0, y0, x1, y1):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if self.pixel(data, x, y) == colour)

    # The newest message's card sits at the bottom of the top screen's log.
    BUBBLE = (70, 340, 90, 470)
    LOG = (70, 100, 90, 470)

    def test_lobby_then_room_then_a_drawn_and_typed_message_is_sent_and_the_pad_emptied(self):
        script = OPEN + tap(*swatch(5), 450) + stroke((120, 70), (440, 90), 500) + '700 text Hello!\n' + tap(*SEND, 900)
        folder, data = self.run_one('Anna', 2, script)
        self.assertGreater(self.count(data, RED, *self.BUBBLE), 100, "the message is in a bubble of the sender's colour")
        self.assertGreater(self.count(data, INK_BLUE, 80, 356, 560, 460), 40, 'drawn in the ink that was chosen')
        self.assertGreater(self.count(data, (255, 255, 255), 60, 534, 590, 634), 530 * 100 * .85, 'the pad is empty again after sending')   # the enlarged pad

    def test_colour_and_name_are_chosen_on_one_screen_and_remembered(self):
        # Lobby -> your profile -> clear the field, type, pick DS Blue, Done.
        script = ('100 key A\n160 key-up A\n' + tap(320, 384, 300) + tap(588, 58, 500) + '700 text Mia\n'
                  + tap(14 + 12 * 36 + 16, 132, 900) + tap(548, 452, 1100))
        folder, _ = self.run_one('Anna', 'auto', script)
        saved = (folder / 'pictochat.ini').read_text()
        self.assertIn('name=Mia\n', saved)
        self.assertIn('colour=11\n', saved)
        # The first chip hands the colour back to the launcher's accent.
        script = '100 key A\n160 key-up A\n' + tap(320, 384, 300) + tap(14 + 16, 132, 500) + tap(548, 452, 700)
        folder, _ = self.run_one('Bea', 4, script)
        self.assertIn('colour=auto\n', (folder / 'pictochat.ini').read_text())

    def test_the_pad_wears_the_launcher_accent_until_a_colour_is_chosen(self):
        # The pen button is lit in the pad's colour: the accent while automatic, the chosen DS colour otherwise.
        _, plain = self.run_one('Anna', 'auto', OPEN)
        _, red = self.run_one('Bea', 'auto', OPEN, ui='accent=2\n')
        _, chosen = self.run_one('Cleo', 11, OPEN, ui='accent=2\n')
        send = (14, 480 + 66)   # the pen button
        r, g, b = self.pixel(red, *send)
        self.assertGreater(r, g + 60, 'a red accent dresses the pad red')
        self.assertNotEqual(self.pixel(plain, *send), self.pixel(red, *send))
        self.assertEqual(self.pixel(chosen, *send), BLUE, 'a chosen colour wins over the accent')

    def test_two_handhelds_chat_both_ways_through_their_own_helpers(self):
        a_port, b_port = free_port(), free_port()
        a_script = OPEN + stroke((120, 70), (440, 90), 1700) + '1900 text Hi Bjorn\n' + tap(*SEND, 2100)
        b_script = OPEN + stroke((120, 60), (440, 80), 2500) + '2700 text Hi Anna\n' + tap(*SEND, 2900)
        a = self.handheld('Anna', 2, a_script, 4.5, a_port, b_port)
        b = self.handheld('Bjorn', 11, b_script, 4.5, b_port, a_port)
        _, a_screen = self.finish(a)
        _, b_screen = self.finish(b)
        self.assertGreater(self.count(b_screen, RED, *self.LOG), 100, "Bjorn sees Anna's message in her colour")
        self.assertGreater(self.count(b_screen, BLUE, *self.LOG), 100, 'next to his own')
        self.assertGreater(self.count(a_screen, BLUE, *self.LOG), 100, "Anna sees Bjorn's reply in his colour")
        self.assertGreater(self.count(a_screen, RED, *self.LOG), 100, 'and still her own message')

    def test_the_scrollbar_thumb_moves_when_the_log_is_scrolled_back(self):
        sends = ''.join('%d text %s\n' % (500 + i * 600, word) + tap(*SEND, 600 + i * 600) for i, word in enumerate(['One', 'Two', 'Three', 'Four']))
        _, newest = self.run_one('Anna', 2, OPEN + sends, 3.6)
        up = tap(603, 326, 3000) + tap(603, 326, 3150)
        _, older = self.run_one('Bea', 2, OPEN + sends + up, 4.4)

        def thumb_top(data):   # the thumb is drawn in the pad's colour, in a track at x 24-36
            rows = [y for y in range(130, 386) if self.pixel(data, 30, y) == RED]   # the glossy thumb, in the pad's colour
            return min(rows) if rows else None
        self.assertIsNotNone(thumb_top(newest))
        self.assertLess(thumb_top(older), thumb_top(newest) - 6, 'scrolling back moves the thumb up the track')

    def test_the_shoulder_buttons_scroll_the_log_since_the_dpad_belongs_to_the_keyboard(self):
        sends = ''.join('%d text %s\n' % (500 + i * 600, word) + tap(*SEND, 600 + i * 600) for i, word in enumerate(['One', 'Two', 'Three', 'Four']))

        def thumb_top(data):
            rows = [y for y in range(130, 386) if self.pixel(data, 30, y) == RED]   # the glossy thumb, in the pad's colour
            return min(rows) if rows else None
        _, newest = self.run_one('Anna', 2, OPEN + sends, 3.6)
        # L (button 4) steps to older messages; R (button 5) steps back.
        step = '3000 joy-down 4\n3040 joy-up 4\n'
        _, one = self.run_one('Bea', 2, OPEN + sends + step, 4.2)
        _, back = self.run_one('Cleo', 2, OPEN + sends + step + '3500 joy-down 5\n3540 joy-up 5\n', 4.6)
        # Held, it repeats until the oldest message.
        _, held = self.run_one('Dani', 2, OPEN + sends + '3000 joy-down 4\n4300 joy-up 4\n', 5.2)
        self.assertLess(thumb_top(one), thumb_top(newest) - 6, 'L scrolls back')
        self.assertEqual(thumb_top(back), thumb_top(newest), 'R returns to the newest message')
        self.assertLess(thumb_top(held), thumb_top(one) - 6, 'holding L keeps scrolling')

    def test_the_trigger_buttons_change_the_ink(self):
        # R2 (button 11) steps to the next ink: five steps from the first reach ink 5, the blue.
        steps = ''.join('%d joy-down 11\n%d joy-up 11\n' % (400 + i * 40, 420 + i * 40) for i in range(5))
        _, data = self.run_one('Anna', 2, OPEN + steps + stroke((120, 70), (440, 90), 800) + tap(*SEND, 1100))
        self.assertGreater(self.count(data, INK_BLUE, 80, 356, 560, 460), 40, 'the message is written in the ink the triggers chose')

    def test_a_controller_can_type_on_the_shared_keyboard(self):
        # Down then Right moves from the first key to q; A (button 0) presses it; Start (button 7) sends.
        script = OPEN + '500 key Down\n520 key-up Down\n560 key Right\n580 key-up Right\n620 joy-down 0\n640 joy-up 0\n800 joy-down 7\n830 joy-up 7\n'
        _, data = self.run_one('Anna', 2, script)
        self.assertGreater(self.count(data, RED, *self.BUBBLE), 100, 'a message made only of the typed letter was sent')

    def test_the_helper_stops_with_the_launcher(self):
        port = free_port()
        self.run_one('Anna', 2, OPEN + '600 key Escape\n660 key-up Escape\n', 1.5, port, port + 1)
        leftovers = subprocess.run(['ps', '-axo', 'command'], capture_output=True, text=True).stdout
        self.assertNotIn('--port %d' % port, leftovers)

    def test_chat_still_works_when_the_helper_cannot_run(self):
        script = OPEN + stroke((120, 70), (440, 90), 500) + tap(*SEND, 900)
        folder, data = self.run_one('Anna', 2, script, arguments='--port not-a-number')
        self.assertGreater(self.count(data, RED, *self.BUBBLE), 100)

    def test_leaving_goes_back_to_the_lobby_then_home_with_the_tile_selected(self):
        # In a room letters are typing, so Escape is the way out; the lobby's B and Escape both leave.
        script = OPEN + '600 key Escape\n660 key-up Escape\n900 key Escape\n960 key-up Escape\n'
        folder, data = self.run_one('Anna', 2, script)
        self.assertIn('home 4', (folder / 'state').read_text())
        # Home again: no pad, so neither the Pen nor the Send button (accent blue in a room) is there.
        self.assertNotEqual(self.pixel(data, 40, 480 + 40), (35, 110, 174))
        self.assertNotEqual(self.pixel(data, SEND[0], 480 + 40), (35, 110, 174))
        self.assertNotEqual(self.pixel(data, 330, 480 + 150), (255, 255, 255))   # and the pad's paper is gone


    GESTURES = 'schema=1\ngestures=1\n'

    @staticmethod
    def swipe(start, end, at):
        """An edge swipe: down on the edge, a few moves, release."""
        (x0, y0), (x1, y1) = start, end
        moves = ''.join('%d drag %d %d\n' % (at + i * 15, x0 + (x1 - x0) * i // 4, y0 + (y1 - y0) * i // 4) for i in range(1, 5))
        return '%d tap %d %d\n' % (at, x0, y0) + moves + '%d release\n' % (at + 90)

    def assert_home(self, folder, data):
        self.assertIn('home 4', (folder / 'state').read_text())
        self.assertNotEqual(self.pixel(data, SEND[0], 480 + 40), (35, 110, 174))
        self.assertNotEqual(self.pixel(data, 330, 480 + 150), (255, 255, 255))

    LOBBY = '100 key A\n160 key-up A\n'      # open PictoChat from Home; the room picker

    def assert_in_room(self, data):
        self.assertEqual(self.pixel(data, 330, 480 + 150), (255, 255, 255), "still in the room: the pad's paper is there")

    def test_swiping_in_from_the_left_in_the_room_picker_goes_home(self):
        self.assert_home(*self.run_one('Anna', 2, self.LOBBY + self.swipe((4, 300), (160, 300), 400), ui=self.GESTURES))

    def test_swiping_up_from_the_bottom_in_the_room_picker_goes_home(self):
        self.assert_home(*self.run_one('Anna', 2, self.LOBBY + self.swipe((320, 474), (320, 330), 400), ui=self.GESTURES))

    def test_a_tap_on_a_gesture_edge_is_still_a_tap(self):
        # The room picker's Back button reaches the bottom edge; a tap there leaves without a swipe.
        self.assert_home(*self.run_one('Anna', 2, self.LOBBY + tap(90, 470, 400), ui=self.GESTURES))

    def test_inside_a_room_edge_swipes_are_left_to_the_chat(self):
        script = OPEN + self.swipe((4, 300), (160, 300), 600) + self.swipe((320, 474), (320, 330), 900)
        folder, data = self.run_one('Anna', 2, script, ui=self.GESTURES)
        self.assert_in_room(data)


if __name__ == '__main__':
    unittest.main()
