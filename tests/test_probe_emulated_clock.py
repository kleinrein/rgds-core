import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from tools.probe_emulated_clock import EmulatedClock, MAGIC, MASK, _mapping


class EmulatedClockTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name) / 'benchmark-fixture'
        self.root = self.base / 'drastic-0/rgds'
        (self.root / 'userdata/session').mkdir(parents=True)
        self.rom = self.base / 'RGDS-benchmark.nds'
        self.rom.write_bytes(b'RGDS PROBE  ####00')
        self.proc = self.base / 'proc'
        for pid, parent, start in ((100, 1, 10), (101, 100, 11)):
            (self.proc / str(pid)).mkdir(parents=True)
            self.stat(pid, parent, start)
        (self.root / 'userdata/session/core.json').write_text(json.dumps(dict(pid=100, start='10')))
        (self.proc / '101/cmdline').write_bytes(b'drastic\0' + bytes(self.rom) + b'\0')
        (self.proc / '101/maps').write_text('1000-9000 rw-p 00000000 00:00 0\n')
        (self.proc / '101/mem').write_bytes(bytes(0x9000))
        self.counter(100)

    def stat(self, pid, parent, start):
        fields = ['S', str(parent), *(['0'] * 17), str(start)]
        (self.proc / str(pid) / 'stat').write_text(str(pid) + ' (test process) ' + ' '.join(fields))

    def counter(self, value):
        with (self.proc / '101/mem').open('r+b') as memory:
            memory.seek(0x2000)
            memory.write(MAGIC + struct.pack('<QQ', value, value ^ MASK))

    def clock(self, **options):
        clock = EmulatedClock(101, self.root, self.rom, _proc_root=self.proc, **options)
        self.addCleanup(clock.close)
        return clock

    def discover(self, clock):
        with patch('tools.probe_emulated_clock.time.sleep', side_effect=lambda _: self.counter(110)):
            return clock.find()

    def test_owned_probe_scans_then_samples_only_the_validated_clock(self):
        clock = self.clock()
        found = self.discover(clock)
        self.assertEqual(found['address'], '0x2000')
        self.assertEqual(found['vblank_count'], 110)
        self.assertLessEqual(found['scanned_bytes'], 128 * 1024 * 1024)
        self.counter(200)
        self.assertEqual(clock.sample()['vblank_count'], 200)

    def test_device_maps_are_excluded_and_zero_requires_explicit_opt_in(self):
        for name in ('/dev/dri/card0', '/dev/shm/game', '/dev/zero_other', '/usr/lib/libc.so'):
            self.assertIsNone(_mapping('1000-5000 rw-s 0 00:00 0 ' + name, True))
        self.assertIsNone(_mapping('1000-5000 rw-s 0 00:00 0 /dev/zero', False))
        self.assertTrue(_mapping('1000-5000 rw-s 0 00:00 0 /dev/zero (deleted)', True)['zero'])
        self.assertIsNone(_mapping('1000-5000 r--p 0 00:00 0', False))

    def test_original_triangle_workload_uses_the_same_validated_clock(self):
        self.rom.write_bytes(b'RGDS 3D TEST####00')
        self.assertEqual(self.discover(self.clock())['vblank_count'], 110)

    def test_unrelated_process_and_wrong_rom_are_rejected_before_memory_open(self):
        self.stat(101, 1, 11)
        with self.assertRaisesRegex(ValueError, 'not descended'):
            self.clock()
        self.stat(101, 100, 11)
        self.rom.write_bytes(b'A commercial game!')
        with self.assertRaisesRegex(ValueError, 'original RGDS'):
            self.clock()

    def test_pid_reuse_mapping_change_and_state_restore_end_measurement(self):
        clock = self.clock()
        self.discover(clock)
        self.stat(101, 100, 12)
        with self.assertRaisesRegex(ValueError, 'identity changed'):
            clock.sample()
        self.stat(101, 100, 11)
        self.counter(1)
        with self.assertRaisesRegex(ValueError, 'backwards'):
            clock.sample()
        (self.proc / '101/maps').write_text('1000-9000 rw-s 0 00:00 0 /dev/dri/card0\n')
        with self.assertRaisesRegex(ValueError, 'mapping changed'):
            clock.sample()

    def test_nonadvancing_marker_is_not_treated_as_emulation(self):
        clock = self.clock()
        with patch('tools.probe_emulated_clock.time.sleep'), self.assertRaisesRegex(ValueError, 'No advancing'):
            clock.find()

    def test_drastic_ram_requires_opt_in_and_matching_owned_regular_fd(self):
        backing = self.base / 'ram'
        backing.write_bytes(bytes(4 * 1024 * 1024))
        info = backing.stat()
        identity = f'{os.major(info.st_dev):02x}:{os.minor(info.st_dev):02x} {info.st_ino}'
        fd = self.proc / '101/fd/19'
        fd.parent.mkdir()
        fd.symlink_to(backing)
        mapped = '/dev/shm/drastic_mapped_memory.dat (deleted)'
        (self.proc / '101/maps').write_text(
            '1000-401000 rw-s 00000000 ' + identity + ' ' + mapped + '\n'
            '501000-901000 rw-s 00000000 ' + identity + ' ' + mapped + '\n'
            'a00000-b00000 rw-p 00000000 00:00 0\n'
        )
        with (self.proc / '101/mem').open('r+b') as memory:
            memory.truncate(0xb00000)
        self.assertIsNone(_mapping((self.proc / '101/maps').read_text().splitlines()[0], True))
        clock = self.clock(allow_drastic_mapping=True)
        with patch('tools.probe_emulated_clock.os.readlink', return_value=mapped):
            self.assertEqual(len(clock._maps()), 1)  # Never scan mirrored RAM twice.
            found = self.discover(clock)
            self.assertTrue(found['drastic_shared_ram'])
            self.assertLess(found['scanned_bytes'], 4 * 1024 * 1024 + 256)
            fd.unlink()
            with self.assertRaisesRegex(ValueError, 'owned DraStic'):
                clock.sample()

    def test_drastic_ram_rejects_wrong_inode_and_device_mappings(self):
        identity = ('00:14', '1037')
        prefix = '1000-401000 rw-s 00000000 '
        self.assertIsNone(_mapping(prefix + '00:14 1038 /dev/shm/drastic_mapped_memory.dat', False, identity))
        self.assertIsNone(_mapping(prefix + '00:14 1037 /dev/dri/card0', False, identity))
        self.assertIsNone(_mapping(prefix + '00:14 1037 /tmp/drastic_mapped_memory.dat', False, identity))
        self.assertTrue(
            _mapping(prefix + '00:14 1037 /dev/shm/drastic_mapped_memory.dat', False, identity)['drastic_ram']
        )


if __name__ == '__main__':
    unittest.main()
