"""Screenshots: the album format, the Menu + R chord and the game runner's capture flow, with no device."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import screenshots  # noqa: E402
from controls import Shortcut  # noqa: E402
from game_menu import GameMenu  # noqa: E402
from library import game_key, read_fields, write_fields  # noqa: E402


def sdl_bmp(path, width, height, colour):
    """A capture as SDL writes it: 32-bit BI_BITFIELDS, bottom-up, bytes B G R A for each pixel."""
    r, g, b = colour
    pixels = bytes((b, g, r, 255)) * (width * height)
    head = struct.pack('<2sIHHI', b'BM', 66 + len(pixels), 0, 0, 66)
    head += struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    head += struct.pack('<III', 0xff0000, 0xff00, 0xff)
    Path(path).write_bytes(head + pixels)


class AlbumTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        sdl_bmp(self.data / 'top.bmp', 64, 48, (250, 10, 20))
        sdl_bmp(self.data / 'bottom.bmp', 64, 48, (5, 200, 30))

    def test_both_screens_stack_into_one_png_with_a_thumbnail_and_an_index(self):
        path = screenshots.save(self.data, self.data / 'top.bmp', self.data / 'bottom.bmp', key='k1', title='Game')
        picture = screenshots.decode_png(path.read_bytes())
        self.assertEqual((picture.width, picture.height), (64, 96))
        self.assertEqual(picture.rgb[:3], bytes((250, 10, 20)))
        self.assertEqual(picture.rgb[-3:], bytes((5, 200, 30)))
        self.assertTrue(path.with_name(path.stem + '.thumb.bmp').is_file())
        self.assertEqual(read_fields(path.parent / 'album.ini')['title'], 'Game')
        line = screenshots.write_index(self.data).read_text().splitlines()[0].split('\t')
        self.assertEqual(line[:3], [str(path), 'k1', 'Game'])

    def test_the_screens_setting_keeps_one_screen(self):
        screenshots.set_screens(self.data, 'bottom')
        path = screenshots.save(self.data, self.data / 'top.bmp', self.data / 'bottom.bmp', key='k1')
        picture = screenshots.decode_png(path.read_bytes())
        self.assertEqual((picture.height, picture.rgb[:3]), (48, bytes((5, 200, 30))))

    def test_a_combined_window_splits_and_a_lone_panel_is_kept(self):
        sdl_bmp(self.data / 'wide.bmp', 128, 48, (1, 2, 3))
        path = screenshots.save(self.data, combined=self.data / 'wide.bmp', key='k2')
        self.assertEqual(screenshots.decode_png(path.read_bytes()).height, 96)
        path = screenshots.save(self.data, top=self.data / 'top.bmp', key='k2')
        self.assertEqual(screenshots.decode_png(path.read_bytes()).height, 48)
        self.assertNotEqual(path.name[:15], '')  # same second: a -2 name, not an overwrite
        self.assertEqual(len(list((self.data / 'screenshots/k2').glob('*.png'))), 2)

    def test_view_cover_and_delete_stay_inside_the_album(self):
        path = screenshots.save(self.data, self.data / 'top.bmp', self.data / 'bottom.bmp', key='k3')
        screenshots.view(self.data, path, self.data / 'v-top.bmp', self.data / 'v-bottom.bmp')
        self.assertEqual(screenshots.read_bmp(self.data / 'v-bottom.bmp').rgb[:3], bytes((5, 200, 30)))
        (self.data / 'library/k3').mkdir(parents=True)
        (self.data / 'library/k3/box.bmp').write_bytes(b'old cover')
        cover = screenshots.cover(self.data, path)
        self.assertEqual(cover, self.data / 'library/k3/box.bmp')
        self.assertEqual(screenshots.read_bmp(cover).height, 48)  # the upper screen only
        self.assertEqual((self.data / 'library/k3/box.before-screenshot.bmp').read_bytes(), b'old cover')
        with self.assertRaises(ValueError):
            screenshots.delete(self.data, self.data / 'top.bmp')
        screenshots.delete(self.data, path)
        self.assertFalse((self.data / 'screenshots/k3').exists())

    def test_a_photo_is_saved_as_one_screen_whatever_the_setting(self):
        screenshots.set_screens(self.data, 'both')
        self.assertEqual(
            screenshots.main(
                [
                    'save', '--data',
                    str(self.data), '--top',
                    str(self.data / 'top.bmp'), '--key', 'k4', '--screens', 'top'
                ]
            ), 0
        )
        [png] = (self.data / 'screenshots/k4').glob('*.png')
        self.assertEqual(screenshots.decode_png(png.read_bytes()).height, 48)

    def test_titles_drop_tags_and_extensions(self):
        self.assertEqual(screenshots.title_for('/g/Mario Kart DS (Europe) [b].nds'), 'Mario Kart DS')
        self.assertEqual(screenshots.title_for('/g/cart.p8.png'), 'cart')


class ChordTests(unittest.TestCase):
    def test_a_held_menu_waits_for_r_and_the_chord_cancels_it(self):
        shortcut = Shortcut()
        shortcut.feed('menu', True, 10)
        self.assertIsNone(shortcut.poll(10.3))  # still held: maybe Menu + R
        shortcut.chord()
        shortcut.feed('menu', False, 10.4)
        self.assertIsNone(shortcut.poll(10.8))

    def test_a_tapped_or_long_held_menu_still_opens(self):
        shortcut = Shortcut()
        shortcut.feed('menu', True, 10)
        shortcut.feed('menu', False, 10.05)
        self.assertEqual(shortcut.poll(10.2), 'menu')
        shortcut = Shortcut()
        shortcut.feed('menu', True, 20)
        self.assertEqual(shortcut.poll(20.7), 'menu')


class Process:
    pid = 456
    result = None

    def poll(self):
        return self.result

    def send_signal(self, value):
        pass

    def terminate(self):
        self.result = 0

    def kill(self):
        self.result = -9

    def wait(self, timeout=None):
        return self.result


class Inputs:
    def __init__(self):
        self.queue = []

    def poll(self):
        return self.queue.pop(0) if self.queue else False

    def close(self):
        pass


class RunnerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.clock = 100
        timer = patch('game_menu.time.monotonic', side_effect=lambda: self.clock)
        timer.start()
        self.addCleanup(timer.stop)
        self.inputs = Inputs()
        self.rom = self.root / 'Some Game (USA).gba'
        self.menu = GameMenu(
            Process(), self.root / 'app', self.root / 'data', 'play', self.rom,
            {'RGDS_CONTROL_FILE': str(self.root / 'data/session/emulator.control')}, self.inputs
        )
        self.addCleanup(self.menu.close)
        self.capture = Path(str(self.menu.control) + '.capture')

    def finish(self):
        self.menu.shot_worker.join(5)

    def test_menu_and_r_asks_the_bridge_for_a_shot_and_saves_both_panels(self):
        self.inputs.queue.append('screenshot')
        with patch('sleep_sounds.play') as click:
            self.menu.poll()
            self.assertTrue(self.capture.read_text().startswith('shot-'))
            sdl_bmp(str(self.menu.control) + '.capture-top.bmp', 32, 24, (9, 9, 9))
            sdl_bmp(str(self.menu.control) + '.capture-bottom.bmp', 32, 24, (8, 8, 8))
            self.capture.unlink()  # the bridge acknowledges once both panels are out
            self.menu.poll()
            self.finish()
        click.assert_called_once()
        folder = self.root / 'data/screenshots' / game_key(self.rom)
        [png] = folder.glob('*.png')
        self.assertEqual(screenshots.decode_png(png.read_bytes()).height, 48)
        self.assertEqual(read_fields(folder / 'album.ini')['title'], 'Some Game')
        self.assertFalse(Path(str(self.menu.control) + '.capture-top.bmp').exists())

    def test_a_panel_that_never_presents_is_left_out_after_the_deadline(self):
        self.inputs.queue.append('screenshot')
        with patch('sleep_sounds.play'):
            self.menu.poll()
            sdl_bmp(str(self.menu.control) + '.capture-top.bmp', 32, 24, (9, 9, 9))
            self.menu.poll()
            self.assertIsNone(self.menu.shot_worker)
            self.clock += 1
            self.menu.poll()
            self.finish()
        self.assertFalse(self.capture.exists())
        [png] = (self.root / 'data/screenshots' / game_key(self.rom)).glob('*.png')
        self.assertEqual(screenshots.decode_png(png.read_bytes()).height, 24)

    def test_the_bridges_menu_press_inside_the_chord_is_dropped(self):
        self.inputs.chord_holds_menu = lambda now: True
        self.menu.trigger.write_text('1\n')
        with patch.object(self.menu, 'open') as opened:
            self.menu.poll()
        opened.assert_not_called()

    def test_the_game_menus_screenshot_row_saves_the_paused_frame(self):
        sdl_bmp(str(self.menu.control) + '.capture-top.bmp', 32, 24, (9, 9, 9))
        write_fields(self.menu.request, dict(token='7', action='screenshot'))
        self.menu.menu = Process()  # the menu helper is up
        with patch('sleep_sounds.play') as click:
            self.menu.poll()
            self.finish()
        click.assert_not_called()  # the menu plays its own cue
        status = read_fields(self.menu.status)
        self.assertEqual((status['message'], status['request_token']), ('Screenshot saved to the Album.', '7'))
        self.assertTrue(Path(str(self.menu.control) + '.capture-top.bmp').exists())  # still the menu's backdrop


if __name__ == '__main__':
    unittest.main()
