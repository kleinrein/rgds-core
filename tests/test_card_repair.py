"""Read-only games card detection, attempt limits and the detached check script."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import card_repair


class CardRepairTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.mounts = self.root / 'mounts'
        self.mounts.write_text('/dev/root / ext4 rw,relatime 0 0\n'
                               '/dev/mmcblk1p10 /userdata ext4 rw,relatime 0 0\n'
                               '/dev/mmcblk1p11 /mnt/mmc vfat ro,noatime,errors=remount-ro 0 0\n'
                               '/dev/sda1 /media/My\\040Card exfat rw 0 0\n')

    def test_mount_lookup_prefers_the_longest_matching_mount(self):
        with mock.patch('os.path.realpath', side_effect=lambda p: str(p)):
            self.assertEqual(card_repair.mount_of('/mnt/mmc/rgds/userdata', self.mounts)[:3], ('/dev/mmcblk1p11', '/mnt/mmc', 'vfat'))
            self.assertEqual(card_repair.mount_of('/userdata/rgds-boot', self.mounts)[1], '/userdata')
            self.assertEqual(card_repair.mount_of('/media/My Card/x', self.mounts)[1], '/media/My Card')
            self.assertEqual(card_repair.mount_of('/mnt/mmcfoo', self.mounts)[1], '/')

    def test_writable_card_is_left_alone(self):
        with mock.patch.object(card_repair, 'read_only', return_value=False):
            code, _ = card_repair.start('/mnt/mmc/rgds/userdata', journal=self.root / 'j.json', spawn=False)
        self.assertEqual(code, card_repair.NOT_NEEDED)
        self.assertFalse((self.root / 'j.json').exists())

    def test_two_attempts_then_gives_up_and_window_expires(self):
        journal = self.root / 'j.json'
        found = ('/dev/mmcblk1p11', '/mnt/mmc', 'vfat', ['ro'])
        with mock.patch.object(card_repair, 'read_only', return_value=True), mock.patch.object(card_repair, 'mount_of', return_value=found):
            self.assertEqual(card_repair.start('/d', journal=journal, now=1000, spawn=False)[0], card_repair.STARTED)
            self.assertEqual(card_repair.start('/d', journal=journal, now=1100, spawn=False)[0], card_repair.STARTED)
            self.assertEqual(card_repair.start('/d', journal=journal, now=1200, spawn=False)[0], card_repair.GIVE_UP)
            self.assertEqual(card_repair.start('/d', journal=journal, now=1100 + card_repair.WINDOW_SECONDS, spawn=False)[0], card_repair.STARTED)

    def test_unknown_filesystems_are_not_checked(self):
        found = ('/dev/mmcblk1p10', '/userdata', 'ext4', ['ro'])
        with mock.patch.object(card_repair, 'read_only', return_value=True), mock.patch.object(card_repair, 'mount_of', return_value=found):
            self.assertEqual(card_repair.start('/d', journal=self.root / 'j.json', spawn=False)[0], card_repair.UNSUPPORTED)

    def test_script_checks_only_while_read_only_then_reboots(self):
        text = card_repair.script('/dev/mmcblk1p11', '/mnt/mmc', card_repair.REPAIRABLE['vfat'])
        self.assertIn('fsck.fat -a -w /dev/mmcblk1p11', text)
        self.assertLess(text.index('(^|,)ro(,|$)'), text.index('fsck.fat'))
        self.assertTrue(text.rstrip().endswith('reboot'))
        self.assertNotIn('mount -o remount,rw', text)


if __name__ == '__main__':
    unittest.main()
