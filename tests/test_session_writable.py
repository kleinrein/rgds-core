"""Unwritable installations fail before acquiring the stock session."""
import errno
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
import session


class WritableSessionTests(unittest.TestCase):
    def test_probe_does_not_leave_files_or_change_preferences(self):
        with tempfile.TemporaryDirectory() as directory:
            data=Path(directory)/'userdata';data.mkdir()
            prefs=data/'preferences.ini';prefs.write_text('schema=1\nmode=dark\n')
            session.ensure_writable_data(data)
            self.assertEqual(list(data.iterdir()),[prefs])
            self.assertEqual(prefs.read_text(),'schema=1\nmode=dark\n')

    def test_internal_install_keeps_card_roms_and_deduplicates_card_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);card=base/'card';alias=base/'alias'
            (card/'Roms/NDS').mkdir(parents=True);alias.symlink_to(card,target_is_directory=True)
            internal=base/'userdata/test/rgds'
            roots=session.rom_roots(internal,cards=(card,alias))
            self.assertIn(card/'Roms/NDS',roots)
            self.assertIn(internal/'games',roots)
            self.assertIn(card/'rgds/games',roots)
            self.assertFalse(any('simpleos' in root.parts for root in roots))
            identities=[str(root.resolve()) for root in roots]
            self.assertEqual(len(identities),len(set(identities)))

    def test_readonly_storage_fails_before_lock_or_frontend_discovery(self):
        with tempfile.TemporaryDirectory() as directory, patch('session.sys.platform','linux'), \
             patch('session.release',return_value=Path(directory)/'app'),patch('session.environment',return_value={}), \
             patch('session.tempfile.mkstemp',side_effect=OSError(errno.EROFS,'Read-only file system')), \
             patch('session.lock') as lock,patch('session.discover_frontends') as frontends, \
             patch.object(sys,'argv',['session.py','--root',directory]):
            with self.assertRaisesRegex(ValueError,'userdata is not writable'):
                session.main()
            lock.assert_not_called();frontends.assert_not_called()


if __name__=='__main__':unittest.main()
