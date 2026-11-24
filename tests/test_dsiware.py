"""DSiWare: told apart from DS games by header, read through a .cia, started with the owner's four BIOS dumps."""
import os
import struct
import sys
import zipfile
from pathlib import Path
from unittest.mock import patch

import dsiware
import emulators
import library
import save_guard
from test_runtime import Fixture

DSIWARE, DS_UNIT, DSI_ONLY = 0x00030004, 2, 3


def srl(code=b'KABC', unit=DSI_ONLY, high=DSIWARE):
    """A 0x1000-byte header and a banner right after it."""
    header = bytearray(0x1000)
    header[12:16] = code
    header[0x12] = unit
    struct.pack_into('<I', header, 0x68, 0x1000)
    struct.pack_into('<I', header, 0x234, high)
    banner = bytearray(0x440)
    banner[0x240:0x24a] = 'Hello'.encode('utf-16-le')
    banner[0x340:0x34a] = 'Hello'.encode('utf-16-le')  # the English title, which the launcher shows
    return bytes(header) + bytes(banner)


def cia(content, encrypted=False):
    """A DSiWare CIA as DSCore reads one: header, certificates, ticket, a DSi TMD, the SRL, each 64-byte aligned."""
    align = lambda n: (n + 0x3f) & ~0x3f
    certs, ticket, tmd_size = 0xa00, 0x350, 0x208
    header = struct.pack('<IHHIIIIQ', 0x2020, 0, 0, certs, ticket, tmd_size, 0, len(content))
    tmd = bytearray(tmd_size)
    tmd[0:4] = (0x00010001).to_bytes(4, 'big')
    record = 0x1e4
    tmd[record + 6:record + 8] = (1 if encrypted else 0).to_bytes(2, 'big')
    tmd[record + 8:record + 16] = len(content).to_bytes(8, 'big')
    out = bytearray(header.ljust(0x2020, b'\0'))
    out = out.ljust(align(len(out)), b'\0') + bytes(certs)
    out = out.ljust(align(len(out)), b'\0') + bytes(ticket)
    out = out.ljust(align(len(out)), b'\0') + tmd
    out = out.ljust(align(len(out)), b'\0') + content
    return bytes(out)


class DsiWareTests(Fixture):
    def setUp(self):
        super().setUp()
        self.binary('rgds-experimental')
        self.bios = self.root / 'bios'
        self.bios.mkdir()

    def game(self, name, data):
        path = self.root / 'roms' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def dumps(self):
        (self.bios / 'bios9.bin').write_bytes(bytes(4096))
        (self.bios / 'bios7.bin').write_bytes(bytes(16384))
        (self.bios / 'nds').mkdir()
        (self.bios / 'nds/dsi_bios9.bin').write_bytes(bytes(65536))
        (self.bios / 'BIOSDSI7.bin').write_bytes(bytes(65536))

    def plan(self, rom):
        with patch.dict(os.environ, {'RGDS_BIOS_DIR': str(self.bios)}, clear=True):
            return emulators.launch_plan('rgds-experimental', self.app, self.data, rom, prepare=False)

    def test_header_tells_dsiware_from_ds_games_and_dsi_enhanced_carts(self):
        self.assertIsNone(dsiware.kind(self.game('ds.nds', srl(unit=0, high=0))))
        self.assertIsNone(dsiware.kind(self.game('enhanced.nds', srl(unit=DS_UNIT, high=0x00030000))))
        self.assertEqual(dsiware.kind(self.game('ware.dsi', srl())), 'dsi')
        self.assertEqual(dsiware.kind(self.game('blocks.nds', srl(unit=DS_UNIT))), 'both')
        self.assertEqual(dsiware.kind(self.game('ware.cia', cia(srl()))), 'dsi')
        self.assertEqual(dsiware.kind(self.game('short.nds', b'\0' * 512)), None)

    def test_cia_and_zipped_cia_read_the_game_inside(self):
        rom = self.game('ware.cia', cia(srl(code=b'KXYZ')))
        self.assertEqual(dsiware.game_code(rom), 'KXYZ')
        banner = library.banner_data(rom)
        self.assertEqual(banner[0x240:0x24a].decode('utf-16-le'), 'Hello')
        packed = self.root / 'roms/ware.zip'
        with zipfile.ZipFile(packed, 'w') as archive:
            archive.writestr('Ware.cia', cia(srl(code=b'KXYZ')))
        self.assertEqual(dsiware.kind(packed), 'dsi')
        self.assertEqual(dsiware.game_code(packed), 'KXYZ')

    def test_dsiware_without_the_four_dumps_is_refused_in_the_players_words(self):
        (self.bios / 'bios9.bin').write_bytes(bytes(4096))
        (self.bios / 'dsi_bios7.bin').write_bytes(bytes(100))  # the wrong size: not a DSi BIOS
        with self.assertRaises(dsiware.Unreadable) as refused:
            self.plan(self.game('ware.dsi', srl()))
        self.assertEqual(str(refused.exception), dsiware.NEEDS_BIOS)
        self.assertEqual(dsiware.missing_files(self.bios), ['bios7', 'bios9i', 'bios7i'])

    def test_encrypted_cia_says_so(self):
        self.dumps()
        with self.assertRaises(dsiware.Unreadable) as refused:
            self.plan(self.game('ware.cia', cia(srl(), encrypted=True)))
        self.assertEqual(str(refused.exception), dsiware.ENCRYPTED)

    def test_dsiware_gets_the_dumps_after_the_empty_ds_pair_and_an_sd_card(self):
        self.dumps()
        command = self.plan(self.game('ware.srl', srl()))['command']
        empty = command.index('--bios9') + 1
        self.assertEqual(command[empty], '')
        later = len(command) - command[::-1].index('--bios9')
        self.assertGreater(later, empty)
        self.assertEqual(command[later], str(self.bios / 'bios9.bin'))
        self.assertEqual(command[command.index('--bios9i') + 1], str(self.bios / 'nds/dsi_bios9.bin'))
        self.assertEqual(command[command.index('--bios7i') + 1], str(self.bios / 'BIOSDSI7.bin'))
        sd = Path(command[command.index('--dsi-sd') + 1])
        self.assertEqual(sd.name, 'dsi-sd')
        self.assertEqual(sd.parent.name, 'rgds-experimental')
        self.assertFalse(sd.exists())  # a dry plan writes nothing

    def test_ds_games_and_ds_compatible_homebrew_keep_freebios(self):
        self.dumps()
        for rom in (self.game('ds.nds', srl(unit=0, high=0)), self.game('blocks.nds', srl(unit=DS_UNIT))):
            command = self.plan(rom)['command']
            self.assertNotIn('--bios9i', command)
            self.assertNotIn('--dsi-sd', command)
            self.assertEqual(command.count('--bios9'), 1)

    def test_save_history_keeps_dsiware_saves_by_game_code(self):
        rom = self.game('ware.cia', cia(srl(code=b'KXYZ')))
        name, files = save_guard.game_saves(self.data, save_guard.DS, rom, rom)
        saves = self.data / 'emulators/rgds-experimental/saves'
        self.assertEqual(files, [saves / 'KXYZ.pub', saves / 'KXYZ.prv'])
        saves.mkdir(parents=True)
        (saves / 'KXYZ.pub').write_bytes(b'progress')
        save_guard.protect(self.data, save_guard.DS, rom)
        copies = list((self.data / save_guard.HISTORY / name).glob('*/emulators/rgds-experimental/saves/KXYZ.pub'))
        self.assertEqual(len(copies), 1)

    def test_a_refused_launch_leaves_its_words_for_the_launcher(self):
        rom = self.game('ware.dsi', srl())
        argv = ['emulators.py', 'launch', '--app', str(self.app), '--data', str(self.data), '--rom', str(rom)]
        with patch.object(sys, 'argv',
                          argv), patch.object(emulators, 'run', side_effect=dsiware.Unreadable(dsiware.NEEDS_BIOS)):
            self.assertEqual(emulators.main(), 1)
        message = self.data / 'emulators/rgds-play/launch-message.txt'
        self.assertEqual(message.read_text().strip(), dsiware.NEEDS_BIOS)
