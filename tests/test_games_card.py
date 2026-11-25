"""The games card in the RG DS Plus's second slot (usr/libexec/rgds/games-card) against a fake /sys, /dev and mount."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'os/buildroot/rootfs/usr/libexec/rgds/games-card'


def tool(folder, name, body):
    path = Path(folder) / name
    path.write_text('#!/bin/sh\n' + body)
    path.chmod(0o755)


class GamesCardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        d = Path(self.temp.name)
        self.sys, self.dev, self.run = d / 'sys', d / 'dev', d / 'run'
        self.point, self.mounts, self.bin = d / 'media/tf2', d / 'mounts', d / 'bin'
        for folder in (self.sys / 'block', self.dev, self.run, self.bin):
            folder.mkdir(parents=True)
        (self.run / 'disk').write_text('/dev/mmcblk1\n')
        self.mounts.write_text('/dev/mmcblk1p4 /data exfat rw 0 0\n/dev/mmcblk1p2 / squashfs ro 0 0\n')
        # a fake device file holds its file system's name; mount succeeds when -t matches it
        tool(
            self.bin, 'mount', f'''
[ "$1" = -o ] && exit 0
type=$2 dev=$5 point=$6
[ "$(cat "$dev" 2>/dev/null)" = "$type" ] || exit 32
echo "$dev $point $type $4 0 0" >> {self.mounts}
'''
        )
        tool(
            self.bin, 'umount', f'''
lazy=; [ "$1" = -l ] && {{ lazy=1; shift; }}
[ -z "$lazy" ] && [ -f {d}/busy ] && exit 1
grep -v " $1 " {self.mounts} > {self.mounts}.new; mv {self.mounts}.new {self.mounts}
echo "$1 ${{lazy:+lazy}}" >> {d}/umounts
'''
        )
        tool(self.bin, 'fsck.exfat', f'echo "$2" >> {d}/fsck; [ -f {d}/repairable ] && echo exfat > "$2"; exit 0\n')
        tool(self.bin, 'fuser', f'echo "$@" >> {d}/fuser; rm -f {d}/busy\n')
        tool(self.bin, 'blkid', 'echo GAMES\n')
        tool(self.bin, 'sync', 'exit 0\n')
        self.dir = d
        self.env = dict(
            os.environ,
            PATH=f'{self.bin}:/usr/bin:/bin',
            RGDS_SYS=str(self.sys),
            RGDS_DEV=str(self.dev),
            RGDS_RUN=str(self.run),
            RGDS_MOUNTS=str(self.mounts),
            RGDS_GAMES_CARD=str(self.point)
        )

    def tearDown(self):
        self.temp.cleanup()

    def disk(self, name, kind='SD', cid='0353445347363447', parts=(('p1', 'exfat', 1000, ''),)):
        block = self.sys / 'block' / name
        (block / 'device').mkdir(parents=True)
        (block / 'device/type').write_text(kind + '\n')
        (block / 'device/cid').write_text(cid + '\n')
        if not parts: (self.dev / name).write_text('exfat')
        for suffix, fs, size, label in parts:
            part = block / (name + suffix)
            part.mkdir()
            (part / 'size').write_text(f'{size}\n')
            (part / 'uevent').write_text(f'DEVNAME={name}{suffix}\nPARTNAME={label}\n')
            (self.dev / (name + suffix)).write_text(fs)

    def pull(self, name):
        for path in sorted(self.sys.glob(f'block/{name}/**/*'), reverse=True):
            path.rmdir() if path.is_dir() else path.unlink()
        (self.sys / 'block' / name).rmdir()
        for node in self.dev.glob(name + '*'):
            node.unlink()

    def run_script(self, *args):
        return subprocess.run(['sh', str(SCRIPT), *args], env=self.env, capture_output=True, text=True, timeout=20)

    def state(self):
        path = self.run / 'games-card.ini'
        return dict(line.split('=', 1) for line in path.read_text().splitlines()) if path.exists() else None

    def mounted(self):
        return [line.split()[0] for line in self.mounts.read_text().splitlines() if line.split()[1] == str(self.point)]

    def test_a_card_in_the_second_slot_is_mounted_read_write(self):
        self.disk('mmcblk1', parts=(('p4', 'exfat', 9000, 'rgds-data'),))  # the card RG DS OS booted from
        self.disk('mmcblk0', kind='MMC', parts=(('p1', 'exfat', 99000, ''),))  # the eMMC
        self.disk('mmcblk2')
        self.assertEqual(self.run_script('boot').returncode, 0)
        self.assertEqual(self.mounted(), [str(self.dev / 'mmcblk2p1')])
        line = next(l for l in self.mounts.read_text().splitlines() if str(self.point) in l)
        self.assertIn('iocharset=utf8', line)
        self.assertNotIn('ro,', line)
        state = self.state()
        self.assertEqual(state['point'], str(self.point))
        self.assertEqual(state['fs'], 'exfat')
        self.assertEqual(state['id'], '0353445347363447')
        self.assertEqual(state['label'], 'GAMES')

    def test_fat32_cards_mount_too(self):
        self.disk('mmcblk2', parts=(('p1', 'vfat', 1000, ''),))
        self.run_script('boot')
        self.assertEqual(self.state()['fs'], 'vfat')
        self.assertIn('shortname=mixed', self.mounts.read_text())

    def test_never_the_boot_card_the_emmc_or_a_disk_in_use(self):
        self.disk('mmcblk1')
        self.disk('mmcblk0', kind='MMC')
        self.assertNotEqual(self.run_script('boot').returncode, 0)
        self.assertEqual(self.mounted(), [])
        # an SD card with a partition mounted elsewhere (Bring over has it, or a person by hand)
        self.disk('mmcblk2')
        with self.mounts.open('a') as mounts:
            mounts.write(f'{self.dev}/mmcblk2p1 /run/rgds/import/mmcblk2p1 exfat ro 0 0\n')
        self.assertNotEqual(self.run_script('boot').returncode, 0)
        self.assertEqual(self.mounted(), [])

    def test_nothing_is_mounted_when_the_boot_card_is_unknown(self):
        (self.run / 'disk').write_text('/dev/none\n')
        self.disk('mmcblk2')
        self.assertNotEqual(self.run_script('boot').returncode, 0)
        self.assertEqual(self.mounted(), [])

    def test_the_largest_file_system_wins_and_root_slots_are_skipped(self):
        self.disk(
            'mmcblk2',
            parts=(
                ('p1', 'vfat', 100, 'rgds-boot'), ('p2', 'squashfs', 500, 'rgds-root-a'),
                ('p3', 'ext4', 8000, 'rootfs'), ('p4', 'exfat', 6000, 'rgds-data')
            )
        )
        self.run_script('boot')
        self.assertEqual(self.mounted(), [str(self.dev / 'mmcblk2p4')])

    def test_a_card_without_partitions(self):
        self.disk('mmcblk2', parts=())
        self.run_script('boot')
        self.assertEqual(self.mounted(), [str(self.dev / 'mmcblk2')])

    def test_watch_mounts_a_card_put_in_and_unmounts_it_when_pulled(self):
        self.env['RGDS_GAMES_CARD_ONCE'] = '1'
        self.env['RGDS_GAMES_CARD_POLL'] = '0'
        self.run_script('watch')
        self.assertIsNone(self.state())
        self.disk('mmcblk2')
        self.run_script('watch')
        self.assertEqual(self.mounted(), [str(self.dev / 'mmcblk2p1')])
        self.run_script('watch')  # nothing changed: stays
        self.assertIsNotNone(self.state())
        self.pull('mmcblk2')
        (self.dir / 'busy').write_text('')  # a game had a file open: the mount goes lazily
        self.run_script('watch')
        self.assertIsNone(self.state())
        self.assertEqual(self.mounted(), [])
        self.assertIn('lazy', (self.dir / 'umounts').read_text())

    def test_a_card_swapped_during_sleep_is_told_apart(self):
        self.disk('mmcblk2', cid='aaaa')
        self.run_script('boot')
        self.pull('mmcblk2')
        self.disk('mmcblk2', cid='bbbb')
        self.run_script('check')  # the old card's mount goes
        self.assertIsNone(self.state())
        self.run_script('check')  # and the new one comes in
        self.assertEqual(self.state()['id'], 'bbbb')

    def test_a_dirty_exfat_card_is_repaired_by_watch_not_at_boot(self):
        self.disk('mmcblk2', parts=(('p1', 'dirty', 1000, ''),))
        (self.dir / 'repairable').write_text('')
        self.run_script('boot')
        self.assertFalse((self.dir / 'fsck').exists())
        self.run_script('check')
        self.assertEqual(self.state()['fs'], 'exfat')

    def test_a_card_that_never_mounts_is_tried_once_until_it_is_pulled(self):
        self.disk('mmcblk2', parts=(('p1', 'ntfs', 1000, ''),))
        self.run_script('check')
        self.run_script('check')
        self.assertEqual(len((self.dir / 'fsck').read_text().splitlines()), 1)
        self.pull('mmcblk2')
        self.run_script('check')
        self.assertFalse((self.run / 'games-card.failed').exists())

    def test_eject_unmounts_and_leaves_the_card_alone_until_it_is_pulled(self):
        self.disk('mmcblk2')
        self.run_script('boot')
        (self.dir / 'busy').write_text('')
        result = self.run_script('eject')
        self.assertEqual(result.returncode, 2)  # a game has it open: nothing is pulled from under it
        self.assertIsNotNone(self.state())
        (self.dir / 'busy').unlink()
        self.assertEqual(self.run_script('eject').returncode, 0)
        self.assertIsNone(self.state())
        self.run_script('check')
        self.assertEqual(self.mounted(), [])
        self.pull('mmcblk2')
        self.run_script('check')
        self.disk('mmcblk2')
        self.run_script('check')
        self.assertIsNotNone(self.state())

    def test_stop_unmounts_for_power_off_and_watch_then_keeps_off(self):
        self.disk('mmcblk2')
        self.run_script('boot')
        (self.dir / 'busy').write_text('')
        self.assertEqual(self.run_script('stop').returncode, 0)
        self.assertIn('-k -TERM', (self.dir / 'fuser').read_text())
        self.assertEqual(self.mounted(), [])
        self.assertIsNone(self.state())
        self.run_script('check')
        self.assertEqual(self.mounted(), [])


if __name__ == '__main__':
    unittest.main()
