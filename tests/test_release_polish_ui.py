"""Render the shipped assets and screen-level layout changes through SDL."""
import os
from pathlib import Path
import struct
import sys
import unittest
import test_visual_options as helpers

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key


class ReleasePolishTests(unittest.TestCase):
    setUp = helpers.VisualOptionsTests.setUp
    run_ui = helpers.VisualOptionsTests.run_ui
    pixel = staticmethod(helpers.VisualOptionsTests.pixel)

    def test_every_main_game_menu_action_has_a_rich_icon(self):
        data = self.run_ui(extra=('--game-menu', '--game-core', 'dsperate', '--game-rom', '/tmp/Mario.nds'))
        for i in range(10):
            x = 60 if i == 0 else 120 + ((i - 1) % 3) * 200
            y = 604 if i == 0 else 656 + ((i - 1) // 3) * 68  # the icon in each tile of the three-column grid
            colors = {self.pixel(data, cx, cy) for cy in range(y - 12, y + 12) for cx in range(x - 12, x + 12)}
            self.assertGreater(len(colors), 20, f'Action {i} fell back to a plain glyph')

    def test_library_stats_use_one_row_and_provider_is_not_drawn(self):
        roms = self.root / 'roms'
        roms.mkdir()
        rom = roms / 'Test.nds'
        rom.write_bytes(bytes(512))
        folder = self.root / 'data/library' / game_key(rom)
        folder.mkdir(parents=True)
        (folder / 'stats.ini').write_text('seconds=9180\nsessions=12\nlast_played=2026-10-12\n')
        (folder / 'info.ini').write_text('title=Test game\nprovider=GameTDB\n')
        pixels = b'\x80\x80\x80' * 32 * 32
        bmp = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0,
                          54) + struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels
        (folder / 'box.bmp').write_bytes(bmp)
        data = self.run_ui(extra=('--library', '--rom-dir', str(roms)))
        ink = (45, 57, 68)
        # With a cover, plays and time share one small pill under it.
        self.assertGreater(sum(self.pixel(data, x, y) == ink for x in range(200, 440) for y in range(440, 468)), 20)
        # No provider credit beside it; the quiet Y/X tips keep to the line's ends.
        self.assertEqual(
            sum(self.pixel(data, x, y) == (94, 108, 120) for x in range(130, 510) for y in range(449, 475)), 0
        )

    def test_language_index_starts_with_english_and_sorts_native_names(self):
        rows = [l.split('\t') for l in (ROOT / 'assets/locales/languages.tsv').read_text().splitlines()]
        self.assertEqual(rows[0], ['en-US', 'English (US)'])
        self.assertEqual([r[1] for r in rows[1:]], sorted(r[1] for r in rows[1:]))

    def test_statistics_renders_ranked_native_and_cached_game_icons(self):
        roms = self.root / 'roms'
        roms.mkdir()
        colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 0, 255)]
        for i, color in enumerate(colors):
            rom = roms / f'Game {i}.nds'
            data = bytearray(0x640)
            if i % 2 == 0:
                # A real DS banner layout: tile index 1, one solid palette entry.
                struct.pack_into('<I', data, 0x68, 0x200)
                data[0x220:0x420] = b'\x11' * 512
                palette = sum((31 if channel else 0) << (j * 5) for j, channel in enumerate(color))
                struct.pack_into('<H', data, 0x422, palette)
            rom.write_bytes(data)
            folder = self.root / 'data/library' / game_key(rom)
            folder.mkdir(parents=True)
            (folder / 'stats.ini').write_text(f'seconds={(5-i)*3600}\nsessions={5-i}\n')
            if i % 2:
                pixels = bytes(reversed(color)) * 32 * 32
                bmp = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54) + struct.pack(
                    '<IiiHHIIiiII', 40, 32, 32, 1, 24, 0, len(pixels), 0, 0, 0, 0
                ) + pixels
                (folder / 'icon.bmp').write_bytes(bmp)
        for mode in ('light', 'dark'):
            with self.subTest(mode=mode):
                self.prefs.write_text(f'schema=1\nmode={mode}\nfont=rounded\n')
                screen = self.run_ui(extra=('--settings', '--section', 'statistics', '--rom-dir', str(roms)))
                for i, color in enumerate(colors):
                    # Both icon corners remain inside the ranked card, including row 5.
                    for x, y in [(78, 588 + i * 64), (122, 632 + i * 64)]:
                        self.assertEqual(self.pixel(screen, x, y), color, (mode, i, x, y))


if __name__ == '__main__': unittest.main()
