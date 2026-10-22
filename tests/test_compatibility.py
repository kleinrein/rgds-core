"""Known-problem titles start in a working engine unless the player chose one."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import library


def rom(folder, code, name='game.nds'):
    path = Path(folder) / name
    header = bytearray(0x200)
    header[0:12] = b'TEST TITLE\0\0'
    header[12:16] = code.encode()
    struct.pack_into('<I', header, 0x68, 0)  # no banner: indexing stops early
    path.write_bytes(bytes(header))
    return path


class CompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name) / 'data'

    def game_ini(self, path):
        return library.read_fields(self.data / 'library' / library.game_key(path) / 'game.ini')

    def test_hotel_dusk_defaults_to_drastic_in_every_region(self):
        for code in ('AWIP', 'AWIE', 'AWIJ'):
            with self.subTest(code=code):
                path = rom(self.temp.name, code, code + '.nds')
                library.index_banner(self.data, path)
                self.assertEqual(self.game_ini(path), {'core': '0', 'compatibility': '1'})

    def test_other_games_and_player_choices_are_untouched(self):
        other = rom(self.temp.name, 'AMCE', 'kart.nds')
        library.index_banner(self.data, other)
        self.assertEqual(self.game_ini(other), {})
        chosen = rom(self.temp.name, 'AWIP', 'dusk.nds')
        folder = self.data / 'library' / library.game_key(chosen)
        folder.mkdir(parents=True)
        library.write_fields(folder / 'game.ini', {'core': 4})
        library.index_banner(self.data, chosen)
        self.assertEqual(self.game_ini(chosen), {'core': '4'})

    def test_the_default_is_applied_once(self):
        path = rom(self.temp.name, 'AWIP')
        self.assertTrue(library.apply_compatibility(self.data, path, 'AWIP'))
        fields = self.game_ini(path)
        fields.pop('core')  # the player resets to Default later
        library.write_fields(self.data / 'library' / library.game_key(path) / 'game.ini', fields)
        self.assertFalse(library.apply_compatibility(self.data, path, 'AWIP'))


if __name__ == '__main__':
    unittest.main()
