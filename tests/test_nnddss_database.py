"""Known save chips reach NNDDSS without changing any user save or database."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
sys.path.insert(0, str(ROOT))
from nnddss_database import PREFIX, LEGACY_PREFIX, prepare
from tools.build_nnddss_database import COMMIT, validate


class SaveDatabase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.app = self.base / 'app'; self.data = self.base / 'data'
        self.engine = self.data / 'emulators/nnddss'
        self.target = self.engine / 'User/game_database.xml'
        self.rom = self.base / 'game.nds'
        shutil.copytree(ROOT / 'assets/nnddss/save-types', self.app / 'assets/nnddss/save-types')
        self.rom.write_bytes(b'NEW MARIO\0\0\0A2DP')

    def run_prepare(self):
        return prepare(self.app, self.data, self.engine, self.rom)

    def test_mario_chip_exact_header_and_unchanged_save_files(self):
        backups = [self.engine / 'User/backup/game.dsv', self.engine / 'User/savestates/game_8.dss']
        for file in backups:
            file.parent.mkdir(parents=True, exist_ok=True); file.write_bytes(b'user progress')
        result = self.run_prepare()
        self.assertEqual(result, dict(code='50443241', bytes=8192, chip='EEPROM'))
        xml = ET.fromstring(self.target.read_bytes())
        self.assertEqual(xml.find('.//rom').attrib, dict(name='rom', size='0x02000000',
                         crc32=f'{zlib.crc32(self.rom.read_bytes()):08x}', id='50443241', title='NEW MARIO'))
        self.assertEqual(xml.find('.//save').attrib, dict(name='save', size='0x2000', type='EEPROM'))
        before = self.target.stat().st_mtime_ns
        with patch('nnddss_database.zlib.crc32', side_effect=AssertionError('unchanged ROM reread')):
            self.run_prepare()
        self.assertEqual(self.target.stat().st_mtime_ns, before)
        self.assertEqual([p.read_bytes() for p in backups], [b'user progress'] * 2)

    def test_restricted_native_xml_dialect_and_v1_repair(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(LEGACY_PREFIX + b'<database/>')
        self.run_prepare()
        data = self.target.read_bytes()
        self.assertEqual(data.splitlines()[0], b"<?xml version='1.0' encoding='UTF-8'?>")
        self.assertEqual(data.splitlines()[1], b'<database>')
        self.assertNotIn(b'<!--', data)
        self.assertNotIn(b'="', data)
        self.assertTrue(all(len(line) < 256 for line in data.splitlines()))
        self.assertRegex(data, rb"crc32='[0-9a-f]{8}'")

    def test_crc_cache_invalidates_when_cartridge_changes(self):
        self.run_prepare()
        first = ET.fromstring(self.target.read_bytes()).find('.//rom').get('crc32')
        self.rom.write_bytes(self.rom.read_bytes() + b'new game contents')
        self.run_prepare()
        second = ET.fromstring(self.target.read_bytes()).find('.//rom').get('crc32')
        self.assertNotEqual(first, second)
        self.assertEqual(second, f'{zlib.crc32(self.rom.read_bytes()):08x}')

    def test_generated_entry_follows_rom_and_preserves_user_override(self):
        self.run_prepare()
        user = self.engine / 'User/game_database_user.xml'; user.write_bytes(b'personal overrides')
        self.rom.write_bytes(b'NEW MARIO\0\0\0A2DE')
        self.run_prepare()
        self.assertEqual(ET.fromstring(self.target.read_bytes()).find('.//rom').get('id'), '45443241')
        self.assertEqual(user.read_bytes(), b'personal overrides')

    def test_custom_and_explicit_databases_win(self):
        self.target.parent.mkdir(parents=True); self.target.write_bytes(b'custom engine database')
        self.assertIsNone(self.run_prepare())
        explicit = self.data / 'bios/nnddss/game_database.xml'
        explicit.parent.mkdir(parents=True); explicit.write_bytes(b'explicit user database')
        self.run_prepare()
        self.assertEqual(self.target.read_bytes(), b'custom engine database')
        self.target.write_bytes(PREFIX + b'<database/>')
        self.run_prepare()
        self.assertEqual(self.target.read_bytes(), b'explicit user database')

    def test_symlink_database_remains_untouched(self):
        custom = self.base / 'custom.xml'; custom.write_bytes(PREFIX + b'<database/>')
        self.target.parent.mkdir(parents=True); self.target.symlink_to(custom)
        self.run_prepare()
        self.assertTrue(self.target.is_symlink())
        self.assertEqual(custom.read_bytes(), PREFIX + b'<database/>')

    def test_unsupported_cartridges_do_not_guess_save_protocol(self):
        table = self.app / 'assets/nnddss/save-types/types.json'
        raw = json.loads(table.read_text()); raw['entries']['50443241'][1] = 8; table.write_text(json.dumps(raw))
        for header in (b'NEW MARIO\0\0\0A2DP', b'RGDS PROBE  ####', b'UNKNOWN     ZZZZ', b'bad', b'NEW MARIO\0\0\0A2D\x01'):
            with self.subTest(header=header):
                self.rom.write_bytes(header)
                self.assertIsNone(self.run_prepare())
                self.assertFalse(self.target.exists())

    def test_release_validation_checks_source_and_conversion(self):
        output = self.app / 'assets/nnddss/save-types'
        self.assertEqual(validate(output)['source_commit'], COMMIT)
        self.assertEqual(validate(output)['entries'], 6778)
        table = output / 'types.json'
        raw = json.loads(table.read_text()); raw['entries']['50443241'][1] = 6
        table.write_text(json.dumps(raw))
        with self.assertRaisesRegex(ValueError, 'differ from pinned'):
            validate(output)
        shutil.copyfile(ROOT / 'assets/nnddss/save-types/types.json', table)
        source = output / 'source/src/ROMList.cpp'; source.write_bytes(source.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            validate(output)


if __name__ == '__main__': unittest.main()
