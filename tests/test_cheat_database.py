import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
import emulators


class CheatDatabaseTests(unittest.TestCase):

    def test_independent_image_seeds_from_the_release(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            app, data, rom = root / 'app', root / 'data', root / 'roms/game.nds'
            (app / 'emulators/cheats').mkdir(parents=True)
            (app / 'emulators/cheats/usrcheat.dat').write_bytes(b'cheats')
            rom.parent.mkdir(parents=True)
            with mock.patch.dict(os.environ, {'RGDS_PLATFORM': 'independent'}), \
                    mock.patch('linux_platform.independent', return_value=True):
                self.assertIsNone(emulators.dscore_cheat_database(data, rom, prepare=False, app=app))
                found = emulators.dscore_cheat_database(data, rom, app=app)
            self.assertEqual(found, data / 'emulators/rgds-experimental/cheats/usrcheat.dat')
            self.assertEqual(found.read_bytes(), b'cheats')


if __name__ == '__main__':
    unittest.main()
