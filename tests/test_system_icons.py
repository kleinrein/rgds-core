"""The system icon set: the drawn artwork, the pixel glyphs and the catalog behind them."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


def generator():
    spec = importlib.util.spec_from_file_location('build_system_icons', ROOT / 'tools/build_system_icons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SystemIconTest(unittest.TestCase):
    def test_every_stock_system_has_artwork_and_a_pixel_drawing(self):
        module = generator()
        for entry in module.CATALOG:
            name = entry[0]
            self.assertTrue((ROOT / 'assets/systems' / (name + '.bmp')).is_file(), name)
            self.assertTrue((ROOT / 'assets/systems/pixel' / (name + '.svg')).is_file(), name)

    def test_artwork_is_a_bounded_alpha_bitmap(self):
        for path in sorted((ROOT / 'assets/systems').glob('*.bmp')):
            data = path.read_bytes()
            self.assertEqual(struct.unpack_from('<ii', data, 18), (128, -128))
            self.assertEqual(struct.unpack_from('<I', data, 66)[0], 0xff000000)
            alpha = data[125::4]
            self.assertIn(0, alpha)
            self.assertTrue(any(a > 200 for a in alpha))

    def test_pixel_glyphs_are_square_and_drawn(self):
        module = generator()
        glyphs = module.art()
        self.assertEqual(len(glyphs), len(module.CATALOG))
        for name, rows in glyphs.items():
            self.assertEqual(len(rows), module.GRID, name)
            for row in rows:
                self.assertEqual(len(row), module.GRID, name)
                self.assertFalse(set(row) - set('.#+'), name)
            drawn = sum(row.count('#') + row.count('+') for row in rows)
            self.assertGreater(drawn, 40, name)

    def test_the_systems_page_draws_the_system_its_own_icon(self):
        source = (ROOT / 'src/launcher/systems_play.hpp').read_text()
        self.assertIn('systems/" + id + ".bmp', source)
        self.assertIn('draw_system_pixel(s, r, id', source)
        # A system without artwork still gets the cartridge and its badge.
        self.assertIn('SettingsIcon::Cartridge', source)


if __name__ == '__main__':
    unittest.main()
