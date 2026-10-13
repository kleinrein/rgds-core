"""NNDDSS prepares a fresh runtime without any stock BIOS, fonts or settings."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
sys.path.insert(0, str(ROOT))
import activate
import emulators
from nnddss_assets import LOADER_PATHS, LOADER_SHA256, loader_bytes
from tools.build_freebios import validate


class IndependentNNDDSS(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name); self.app = self.root / 'app'; self.data = self.root / 'userdata'
        shutil.copytree(ROOT / 'assets/nnddss', self.app / 'assets/nnddss')
        shutil.copytree(ROOT / 'assets/fonts/source', self.app / 'assets/fonts/source')
        self.binary = self.app / 'emulators/nnddss/nnddss'
        self.binary.parent.mkdir(parents=True)
        self.binary.write_text('#!/bin/sh\nexit 0\n'); self.binary.chmod(0o755)

    def test_fresh_launch_has_no_stock_lookup_and_no_dump_requirement(self):
        with patch.object(emulators, 'stock_file', side_effect=AssertionError('stock read')), \
             patch.dict(os.environ, {'NNDDSS_CORE_DEFAULT': '/mnt/vendor/foreign.so',
                                     'LD_LIBRARY_PATH': '/mnt/ports/libs', 'NNDDSS_GPU_MAX': '1'}, clear=True):
            plan = emulators.launch_plan('nnddss', self.app, self.data, self.root / 'game.nds')
        root = Path(plan['cwd'])
        self.assertEqual((root / 'assets/board.ini').read_text(), 'RGds\n')
        self.assertFalse((root / 'assets/bios/nds_firmware.bin').exists())
        self.assertFalse((root / 'User/game_database.xml').exists())
        self.assertNotIn('/mnt/', plan['env']['LD_LIBRARY_PATH'])
        self.assertEqual(plan['env']['NNDDSS_CORE_DEFAULT'], str(root / 'lib/libnnddss.so'))
        self.assertEqual(plan['env']['NNDDSS_GPU_MAX'], '0')
        self.assertEqual(plan['env']['NNDDSS_LANG'], 'en')
        self.assertEqual((root / 'assets/fonts/orbitron.ttf').read_bytes(),
                         (self.app / 'assets/fonts/source/Nunito.ttf').read_bytes())

    def test_missing_owned_bios_fails_even_if_a_previous_stock_copy_exists(self):
        root = self.data / 'emulators/nnddss'
        (root / 'assets/bios').mkdir(parents=True)
        previous = root / 'assets/bios/drastic_bios_arm7.bin'; previous.write_bytes(b'previous BIOS')
        (self.app / 'assets/nnddss/freebios/drastic_bios_arm7.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing replacement'):
            emulators.prepare_nnddss(self.app, self.data, self.binary)
        self.assertEqual(previous.read_bytes(), b'previous BIOS')
        self.assertFalse((root / 'nnddss').exists())

    def test_loader_patch_is_version_checked_and_changes_only_two_filename_slots(self):
        binary = ROOT / 'vendor/engines/nnddss/nnddss'
        if not binary.is_file(): self.skipTest('private NNDDSS payload is not provisioned')
        original = binary.read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), LOADER_SHA256)
        patched = loader_bytes(binary)
        restored = bytearray(patched)
        for before, after in LOADER_PATHS:
            offset = original.index(before + b'\0')
            self.assertNotIn(before, patched)
            self.assertEqual(patched[offset:offset+len(before)+1], after.ljust(len(before)+1, b'\0'))
            restored[offset:offset+len(before)+1] = before + b'\0'
        self.assertEqual(bytes(restored), original)
        self.binary.write_bytes(original + b'changed')
        with self.assertRaisesRegex(ValueError, 'Unrecognized NNDDSS loader'):
            loader_bytes(self.binary)

    def test_vendored_bios_is_built_from_recorded_source(self):
        record = validate()
        self.assertEqual(len(record['files']), 2)
        for arm, size in ((7, 16384), (9, 4096)):
            data = (ROOT / f'assets/nnddss/freebios/drastic_bios_arm{arm}.bin').read_bytes()
            self.assertEqual(len(data), size)
            self.assertEqual(data[3], 0xea)  # ARM branch vector, not an empty placeholder.

    def test_engine_verification_covers_bios_and_font(self):
        for relative in ('assets/nnddss/freebios/drastic_bios_arm7.bin', 'assets/fonts/source/Nunito.ttf',
                         'assets/nnddss/save-types/types.json', 'assets/nnddss/save-types/source/src/ROMList.cpp'):
            with self.subTest(relative=relative):
                selected = []
                def verify(app, predicate):
                    selected.append(predicate(relative))
                with patch.object(activate, 'verify_release', side_effect=verify):
                    activate.verify_engine(self.app, 'nnddss')
                self.assertEqual(selected, [True])

    def test_retail_launch_selects_known_save_chip(self):
        rom = self.root / 'game.nds'; rom.write_bytes(b'NEW MARIO\0\0\0A2DP')
        plan = emulators.launch_plan('nnddss', self.app, self.data, rom)
        database = Path(plan['cwd']) / 'User/game_database.xml'
        self.assertIn(b"size='0x2000' type='EEPROM'", database.read_bytes())


if __name__ == '__main__': unittest.main()
