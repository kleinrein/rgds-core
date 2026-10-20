"""The installer's boot-artwork step never raises and never touches a non-handheld."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import boot_identity


class BootIdentityTests(unittest.TestCase):
    def test_apply_off_the_handheld_is_skipped_with_a_reason(self):
        with patch('platform.machine', return_value='x86_64'):
            result = boot_identity.apply(ROOT)
        self.assertEqual((result['splash'], result['logo']), ('skipped', 'skipped'))
        self.assertTrue(result['messages'])

    def test_apply_on_a_fixture_root_without_stock_weston_keeps_stock_artwork(self):
        with tempfile.TemporaryDirectory() as directory:
            result = boot_identity.apply(ROOT, directory)
        self.assertEqual((result['splash'], result['logo']), ('skipped', 'skipped'))
        self.assertIn('Weston', result['messages'][0])

    def test_apply_is_skipped_on_the_plus(self):
        with patch('platform.machine', return_value='aarch64'), patch('sys.platform', 'linux'), \
                patch('device_profile.detect', return_value=dict(model='rg-ds-plus', source='panel-modes', panels=[])):
            result = boot_identity.apply(ROOT)
        self.assertEqual(result['splash'], 'skipped')
        self.assertIn('Plus', result['messages'][0])

    def test_remove_with_nothing_installed_is_a_no_op(self):
        with tempfile.TemporaryDirectory() as directory:
            result = boot_identity.remove(ROOT, directory)
        self.assertEqual((result['splash'], result['logo']), ('skipped', 'skipped'))
        self.assertEqual(result['messages'], [])

    def test_failures_are_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'etc/init.d').mkdir(parents=True)
            (root / 'etc/init.d/S49weston').write_text('#!/bin/sh\n')
            result = boot_identity.apply(ROOT, root)
        self.assertEqual(result['logo'], 'failed')
        self.assertTrue(any(note.startswith('Boot logo:') for note in result['messages']))


if __name__ == '__main__':
    unittest.main()
