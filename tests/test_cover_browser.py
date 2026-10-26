"""Cover framing, first-frame priority and physical shoulder navigation in SDL."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key
import test_visual_options as visuals

BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class CoverBrowserTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.roms = self.root / 'roms'
        self.roms.mkdir()
        self.data = self.root / 'data'
        self.prefs = self.root / 'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        for i in range(12):
            rom = self.roms / f'Game {i:02}.nds'
            banner = bytearray(0x640)
            struct.pack_into('<I', banner, 0x68, 0x200)
            banner[0x220:0x420] = bytes([0x11]) * 512
            struct.pack_into('<H', banner, 0x422, (31 | (31 << 5)) if i == 8 else 31 << 5)
            rom.write_bytes(banner)
            folder = self.data / 'library' / game_key(rom)
            folder.mkdir(parents=True)
            # Tall artwork: blue bands stay visible and the rounded frame follows the image.
            pixels = b''.join((b'\x00\x00\xff' if 20 <= y < 60 else b'\xff\x00\x00') * 40 for y in range(80))
            header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
            header += struct.pack('<IiiHHIIiiII', 40, 40, 80, 1, 24, 0, len(pixels), 0, 0, 0, 0)
            (folder / 'box.bmp').write_bytes(header + pixels)
            if i == 8:
                (folder / 'stats.ini').write_text('sessions=1\nlast_played=2026-10-12\n')

    def run_ui(self, script='', layout=1, animations=False, home=False, first=False, extra=(), home_layout='carousel'):
        (self.root / 'ui.ini').write_text(
            f'schema=1\nlayout={layout}\nanimations={int(animations)}\nhome_layout={home_layout}\nbackground=plain\n'
        )
        state = self.root / 'state'
        state.write_text('1 0 0 ""\n')
        inputs = self.root / 'input'
        inputs.write_text(script)
        screenshot = self.root / 'screen.bmp'
        report = self.root / 'report.json'
        result = subprocess.run(
            [
                str(BINARY),
                '--preview',
                '--home' if home else '--library',
                '--rom-dir',
                str(self.roms),
                '--data',
                str(self.data),
                '--preferences',
                str(self.prefs),
                '--state',
                str(state),
                '--input-script',
                str(inputs),
                '--screenshot',
                str(screenshot),
                '--cache-report',
                str(report),
                *(['--frames', '1'] if first else ['--seconds', '1']),
                *extra,
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=8
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.last_stdout = result.stdout
        counts = json.loads(report.read_text())
        self.assertEqual(counts['render_file_reads'], 0)
        self.assertEqual(counts['render_stamp_checks'], 0)
        return screenshot.read_bytes(), counts, int(state.read_text().split()[1])

    def test_selected_cover_fills_both_frames_on_first_draw_and_neighbors_follow(self):
        image, first, _ = self.run_ui(first=True)
        pixel = visuals.VisualOptionsTests.pixel
        # Top: the cover alone, centred and scaled to the stage (no animation
        # on the first draw; its upper part carries a faint glass sheen).
        near = lambda value, target: all(abs(a - b) <= 40 for a, b in zip(value, target))
        for x, y in [(320, 80), (320, 140)]:
            self.assertTrue(near(pixel(image, x, y), (0, 0, 255)), (x, y, pixel(image, x, y)))
        self.assertEqual(pixel(image, 320, 400), (0, 0, 255))
        self.assertEqual(pixel(image, 320, 260), (255, 0, 0))
        self.assertNotIn(pixel(image, 150, 240), ((0, 0, 255), (255, 0, 0)))  # Nothing beside it.
        # Bottom: the selected carousel tile.
        for x, y in [(320, 631), (320, 641), (320, 844)]:
            self.assertEqual(pixel(image, x, y), (0, 0, 255), (x, y))
        self.assertEqual(pixel(image, 320, 720), (255, 0, 0))
        # Border hugs the portrait, and the top-left image corner is rounded.
        self.assertEqual(pixel(image, 263, 740), (35, 110, 174))
        self.assertNotEqual(pixel(image, 264, 631), (0, 0, 255))
        image, settled, _ = self.run_ui()
        self.assertEqual(pixel(image, 478, 710), (255, 0, 0))
        self.assertGreater(settled['image_reads'], first['image_reads'])
        self.assertLess(settled['frames'], 35, 'Deferred covers must settle back to idle')
        self.assertEqual(settled['evictions'], 0, 'Unused icons must not evict covers')

    def test_shoulders_step_once_in_each_layout_and_repeat_while_held(self):
        for layout in (0, 1, 2):
            for animations in (False, True):
                with self.subTest(layout=layout, animations=animations):
                    _, counts, selected = self.run_ui('100 joy-down 5\n130 joy-up 5\n', layout, animations)
                    self.assertEqual(selected, 1)
                    self.assertFalse(counts['picker_open'])
        _, _, selected = self.run_ui(
            '100 joy-down 5\n150 joy-up 5\n200 joy-down 5\n250 joy-up 5\n300 joy-down 5\n350 joy-up 5\n'
        )
        self.assertEqual(selected, 3)
        _, _, selected = self.run_ui('100 joy-down 4\n150 joy-up 4\n')
        self.assertEqual(selected, 11)
        for source in ("joy-down 5", "tap 598 443", "key Right"):
            release = "joy-up 5" if source.startswith("joy"
                                                      ) else "release" if source.startswith("tap") else "key-up Right"
            _, _, selected = self.run_ui(f"100 {source}\n850 {release}\n")
            self.assertGreaterEqual(selected, 3)
            self.assertLessEqual(selected, 8)

    def test_l2_jumps_to_a_letter_and_r2_opens_search(self):
        _, counts, selected = self.run_ui('100 joy-down 10\n150 joy-up 10\n')
        self.assertTrue(counts['picker_open'])
        self.assertEqual(counts['picker_title'], 'Jump to letter')
        self.assertEqual(selected, 0)
        # A second press of L2 closes the sheet again.
        _, counts, _ = self.run_ui('100 joy-down 10\n150 joy-up 10\n400 joy-down 10\n450 joy-up 10\n')
        self.assertFalse(counts['picker_open'])
        _, counts, selected = self.run_ui('100 joy-down 11\n150 joy-up 11\n')
        self.assertFalse(counts['picker_open'])
        self.assertEqual(counts['page'], 8)  # the search keyboard
        self.assertEqual(selected, 0)

    def test_last_played_icon_comes_from_the_last_game(self):
        # The column Home's top tile carries the last game's banner icon (64 px at 44, 32 on the lower screen).
        image, _, _ = self.run_ui(home=True, home_layout='vertical')
        self.assertEqual(visuals.VisualOptionsTests.pixel(image, 76, 480 + 64), (255, 255, 0))

    def test_anbernic_opens_the_game_switcher_from_library_modal_settings_and_keyboard(self):
        for script in (
            '100 joy-down 8\n', '100 key Y\n300 joy-down 8\n', '100 key S\n300 joy-down 8\n',
            '100 key Q\n300 key Y\n500 joy-down 8\n'
        ):
            _, counts, _ = self.run_ui(script)
            self.assertEqual(counts['page'], 36, script)
            self.assertFalse(counts['picker_open'])

    def test_the_home_key_opens_the_game_search_and_closes_it_again(self):
        keyboard = self.run_ui('100 key Q\n300 key Y\n')[1]['page']
        for script in ('100 key Home\n', '100 key Y\n300 key Home\n', '100 key S\n300 key Home\n',
                       '100 joy-down 8\n300 key Home\n'):
            _, counts, _ = self.run_ui(script)
            self.assertEqual(counts['page'], keyboard, script)
            self.assertFalse(counts['picker_open'])
        # Home again while typing closes the search where it was opened: the library, or the switcher's page.
        self.assertEqual(self.run_ui('100 key Home\n300 key Home\n')[1]['page'], 0)
        settings = self.run_ui('100 key S\n')[1]['page']
        self.assertEqual(self.run_ui('100 key S\n300 joy-down 8\n500 key Home\n700 key Home\n')[1]['page'], settings)

    def test_the_launchers_game_switcher_plays_a_recent_game_and_goes_back(self):
        # Game 08 is the one played: A starts it, wherever the library was.
        _, counts, selected = self.run_ui('100 joy-down 8\n300 key A\n')
        self.assertEqual((counts['page'], selected), (0, 8))
        # B, or the button again, goes back to the page it was pressed on.
        for script in ('100 key S\n300 joy-down 8\n500 key B\n', '100 key S\n300 joy-down 8\n500 joy-down 8\n'):
            _, counts, _ = self.run_ui(script)
            self.assertEqual(counts['page'], self.run_ui('100 key S\n')[1]['page'], script)
        # Nothing played yet: the button leaves the page as it is.
        (self.data / 'library' / game_key(self.roms / 'Game 08.nds') / 'stats.ini').unlink()
        _, counts, _ = self.run_ui('100 joy-down 8\n')
        self.assertEqual(counts['page'], 0)

    def test_switcher_shows_games_and_keeps_game_menu_accessible(self):
        from library import write_fields
        write_fields(
            self.data / 'session/game-switcher.ini', {
                'count': 2,
                'game_0_rom': str(self.roms / 'Game 00.nds'),
                'game_0_title': 'Current game',
                'game_1_rom': str(self.roms / 'Game 08.nds'),
                'game_1_title': 'Recent game',
            }
        )
        extra = ('--game-menu', '--game-switcher', '--game-rom', str(self.roms / 'Game 00.nds'))
        _, counts, _ = self.run_ui(extra=extra)
        self.assertEqual(counts['page'], 36)
        self.run_ui('100 key Down\n200 key A\n', extra=extra)
        self.assertIn('Game switch (preview): switch:1', self.last_stdout)
        _, counts, _ = self.run_ui('100 key X\n', extra=extra)
        self.assertEqual(counts['page'], 12)
        for button in ('B', 'Home'):
            _, counts, _ = self.run_ui(f'100 key {button}\n', extra=extra)
            self.assertEqual(counts['page'], 12, button)
        menu = ('--game-menu', '--game-rom', str(self.roms / 'Game 00.nds'))
        # Game switcher is the bottom row's left tile.
        for script in ('100 key Down\n150 key Down\n200 key Down\n250 key Down\n300 key A\n', '100 click 170 388\n'):
            _, counts, _ = self.run_ui(script, extra=menu)
            self.assertEqual(counts['page'], 36, script)


if __name__ == '__main__':
    unittest.main()
