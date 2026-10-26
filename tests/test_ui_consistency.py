"""Tester feedback fixes: volume keys scale the menu audio, the music volume popup mutes, A leaves the transition
choices alone, the D-pad reaches Home's bottom bar and PictoChat follows the typeface."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

from tests import test_pictochat_ui as chat_ui

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
DISPLAY_PAGE = 5  # Page::Display, the brightness page


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class UiConsistencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script='', prefs='', entry=('--home',), seconds=2):
        (self.root / 'ui.ini').write_text('schema=1\nanimations=0\n' + prefs)
        (self.root / 'input').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.root), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input'), '--cache-report',
                str(self.root / 'report'), '--seconds',
                str(seconds), *entry
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=12
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report').read_text())

    def test_system_volume_scales_the_menu_audio_and_mute_silences_it(self):
        gains = {}
        for level in (100, 75, 50, 25, 1):
            (self.root / 'volume.ini').write_text(f'schema=1\nvolume={level}\n')
            gains[level] = self.run_ui('', 'menu_sounds=1\n', seconds=1)['sound_master']
        self.assertAlmostEqual(gains[100], 1, places=3)
        self.assertAlmostEqual(gains[50], .5, delta=.02)
        self.assertEqual(sorted(gains.values(), reverse=True), [gains[level] for level in (100, 75, 50, 25, 1)])
        self.assertGreater(gains[1], 0)
        (self.root / 'volume.ini').write_text('schema=1\nvolume=0\n')
        self.assertEqual(self.run_ui('', 'menu_sounds=1\n', seconds=1)['sound_master'], 0)

    def test_music_volume_popup_has_a_mute_choice(self):
        # Sound -> Sounds & music -> Music volume (row 3) -> A -> up to Mute -> A.
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key A\n500 key Down\n600 key Down\n700 key Down\n800 key A\n'
            '1000 key Up\n1100 key Up\n1200 key A\n',
            'menu_music=1\nmusic_volume=40\n', ('--settings', '--section', 'sound'),
            seconds=3
        )
        self.assertEqual(r['music_volume'], 0)
        self.assertIn('music_volume=0\n', (self.root / 'ui.ini').read_text())

    def test_a_does_not_step_the_transition_choices_but_left_and_right_do(self):
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key A\n400 key A\n', 'view_animations=1\n',
            ('--settings', '--section', 'motion')
        )
        self.assertEqual(r['view_transition_style'], 'spring')
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key Right\n', 'view_animations=1\n', ('--settings', '--section', 'motion')
        )
        self.assertEqual(r['view_transition_style'], 'push')

    def test_a_still_toggles_the_two_switches_above_the_choices(self):
        r = self.run_ui('100 key A\n', 'animations=1\nview_animations=1\n', ('--settings', '--section', 'motion'))
        self.assertFalse(r['view_animations'])  # Animations off takes the transitions with it

    def test_dpad_reaches_the_bottom_bar_and_back(self):
        down = ''.join(
            f'{100 + i * 100} key Down\n' for i in range(4)
        )  # Home starts on Games: three rows, then the bar
        self.assertEqual(self.run_ui(down)['home_bar_focus'], 0)
        self.assertEqual(self.run_ui(down + '600 key Right\n')['home_bar_focus'], 2)
        self.assertEqual(self.run_ui(down + '600 key Right\n700 key Left\n')['home_bar_focus'], 0)
        self.assertEqual(self.run_ui(down + '600 key Up\n')['home_bar_focus'], -1)
        # Past the end of the bar nothing moves.
        self.assertEqual(self.run_ui(down + '600 key Down\n')['home_bar_focus'], 0)

    def test_stepping_onto_and_along_the_bar_plays_the_browse_cue(self):
        prefs = 'menu_sounds=1\nmenu_music=0\n'
        (self.root / 'state').write_text('1 0 0 ""\nhome 0\n')  # on Settings, the last row
        self.assertEqual(self.run_ui('300 key Down\n', prefs)['sound_counts'][0], 1)
        (self.root / 'state').write_text('1 0 0 ""\nhome 0\n')
        # The first cue opens the sink and can hold the loop up; later keys come well after it.
        r = self.run_ui('300 key Down\n1000 key Right\n', prefs, seconds=3)
        self.assertEqual((r['home_bar_focus'], r['sound_counts'][0]), (2, 2))

    def test_a_on_the_bar_opens_brightness(self):
        down = ''.join(f'{100 + i * 100} key Down\n' for i in range(4))
        r = self.run_ui(down + '600 key A\n')
        self.assertEqual(r['page'], DISPLAY_PAGE)
        self.assertEqual(r['home_bar_focus'], -1)

    def test_a_vanished_update_mark_hands_focus_and_a_to_brightness(self):
        status = self.root / 'update/status.ini'
        status.parent.mkdir()
        down = ''.join(f'{100 + i * 100} key Down\n' for i in range(4)) + '600 key Right\n'

        def run(script, seconds):
            # On the mark; the update then goes away (status is re-read every 2 s while idle).
            status.write_text('state=available\n')
            clear = threading.Timer(1.5, lambda: status.unlink(missing_ok=True))
            clear.start()
            self.addCleanup(clear.cancel)
            return self.run_ui(script, seconds=seconds)

        status.write_text('state=available\n')
        self.assertEqual(self.run_ui(down)['home_bar_focus'], 1)
        self.assertEqual(run(down, 5)['home_bar_focus'], 0)
        self.assertEqual(run(down + '4500 key A\n', 5.5)['page'], DISPLAY_PAGE)

    def test_controls_profile_answers_a_even_without_a_game(self):
        r = self.run_ui('100 key A\n', '', ('--settings', '--section', 'controls'))
        self.assertTrue(r['picker_open'])
        self.assertEqual(r['picker_title'], 'Profile')


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class PixelBrandTests(unittest.TestCase):
    def screen(self, icons):
        helper = chat_ui.PictoChatUiTests('run_one')
        helper.setUp()
        self.addCleanup(helper.doCleanups)
        folder = helper.root / icons
        folder.mkdir()
        (folder / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')
        (folder / 'ui.ini').write_text(f'schema=1\nanimations=0\nhome_layout=1\nicon_style={icons}\n')
        bmp = folder / 'screen.bmp'
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--home', '--data',
                str(folder), '--state',
                str(folder / 'state'), '--preferences',
                str(folder / 'preferences.ini'), '--seconds', '1.5', '--screenshot',
                str(bmp)
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = bmp.read_bytes()
        region = lambda x0, y0, x1, y1: [helper.pixel(data, x, y) for y in range(y0, y1) for x in range(x0, x1)]
        return region(84, 10, 214, 38), region(296, 900, 344, 948)

    def test_simple_pixel_style_sets_the_title_and_the_bar_mark_in_dots(self):
        rich_title, rich_mark = self.screen('rich')
        pixel_title, pixel_mark = self.screen('pixel')
        self.assertNotEqual(rich_title, pixel_title)
        self.assertNotEqual(rich_mark, pixel_mark)
        # Flat dots on the bar's own sheen use fewer colours than the shaded bitmap.
        self.assertLess(len(set(pixel_mark)), len(set(rich_mark)))


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class ChatTypefaceTests(unittest.TestCase):
    def message_text(self, font):
        helper = chat_ui.PictoChatUiTests('run_one')
        helper.setUp()
        self.addCleanup(helper.doCleanups)
        folder = helper.root / 'Anna'
        folder.mkdir()
        (folder / 'pictochat.ini').write_text('name=Anna\ncolour=2\n')
        (folder / 'state').write_text('1 0 0 ""\nhome 4\n')
        (folder / 'preferences.ini').write_text(f'schema=1\nmode=light\nfont={font}\n')
        (folder / 'ui.ini').write_text('schema=1\nhome_layout=carousel\nbackground=plain\n')
        (folder / 'keys').write_text(chat_ui.OPEN + '700 text Hello world 123\n' + chat_ui.tap(*chat_ui.SEND, 900))
        bmp = folder / 'screen.bmp'
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--home', '--data',
                str(folder), '--state',
                str(folder / 'state'), '--preferences',
                str(folder / 'preferences.ini'), '--input-script',
                str(folder / 'keys'), '--seconds', '2.5', '--screenshot',
                str(bmp)
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = bmp.read_bytes()
        x0, y0, x1, y1 = 76, 390, 300, 420  # the first line of the sent message's card
        return [helper.pixel(data, x, y) for y in range(y0, y1) for x in range(x0, x1)]

    def test_message_text_follows_the_typeface(self):
        smooth, pixel = self.message_text('rounded'), self.message_text('pixel')
        self.assertEqual(smooth, self.message_text('rounded'))  # a steady render, so a difference is the font
        self.assertNotEqual(smooth, pixel)
        # The same words in dots are set in capitals; the smooth line has lower-case ink below the cap line.
        dark = lambda pixels: sum(1 for r, g, b in pixels if r + g + b < 300)
        self.assertGreater(dark(smooth), 100)
        self.assertGreater(dark(pixel), 100)


if __name__ == '__main__': unittest.main()
