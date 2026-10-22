"""Install/update/rollback and rejected package paths, without device or network access."""
import hashlib
import json
from pathlib import Path
import shutil
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import activate

INSTALL = (ROOT / 'packaging/install_package.py').read_text()


class SDInstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'card/rgds'

    def package(self, version, extra=None, metadata=None):
        files = {'bin/rgds-launcher': b'\x7fELF\x02\x01' + b'\0' * 12 + b'\xb7\x00fixture'}
        files.update({'runtime/' + p.name: p.read_bytes() for p in (ROOT / 'runtime').glob('*.py')})
        prefix = 'rgds/releases/' + version + '/'
        entries = {prefix + k: v for k, v in files.items()}
        manifest = dict(
            schema=1,
            project='rgds',
            target='aarch64-linux-gnu',
            build=version,
            files={
                k: hashlib.sha256(v).hexdigest()
                for k, v in files.items()
            }
        )
        manifest.update(metadata or {})
        entries[prefix + 'manifest.json'] = json.dumps(manifest).encode()
        entries.update(
            {
                'rgds/state/active': version.encode(),
                'rgds/main.sh': b'#!/bin/sh\n',
                'Roms/APPS/RG DS Core.sh': b'#!/bin/sh\n'
            }
        )
        if extra: entries.update(extra)
        archive = self.base / (version + '.zip')
        with zipfile.ZipFile(archive, 'w') as z:
            for name, data in entries.items():
                z.writestr(name, data)
        return archive

    def install(self, archive, *options):
        with patch.object(sys, 'argv', ['installer', str(self.root),
                                        str(archive), *options]), patch('platform.machine', return_value='aarch64'):
            exec(compile(INSTALL, 'install_package.py', 'exec'), {'__name__': '__main__'})

    def test_internal_install_refreshes_existing_card_launch_entries(self):
        install_apps = runpy.run_path(str(ROOT / 'packaging/install_package.py'))['install_apps']
        source = self.base / 'package/Roms/APPS'
        source.mkdir(parents=True)
        card = self.base / 'card/Roms/APPS'
        card.mkdir(parents=True)
        other_card = self.base / 'absent-card/Roms/APPS'
        internal = self.base / 'internal/Roms/APPS'
        for name in ('RG DS Core.sh', 'Uninstall RG DS Core.sh'):
            (source / name).write_text('new ' + name)
            (source / name).chmod(0o755)
            (card / name).write_text('old ' + name)
        for name in ('Try RGDS.sh', 'RGDS Probe.sh', 'RGDS Inventory.sh', 'RGDS Recovery.sh'):
            (card / name).write_text('old ' + name)
        (internal / 'RGDS Probe.sh').parent.mkdir(parents=True)
        (internal / 'RGDS Probe.sh').write_text('old probe')
        install_apps(source, internal, (card, other_card))
        for name in ('RG DS Core.sh', 'Uninstall RG DS Core.sh'):
            self.assertEqual((card / name).read_text(), 'new ' + name)
            self.assertEqual((internal / name).read_text(), 'new ' + name)
        self.assertEqual(sorted(p.name for p in card.iterdir()), ['RG DS Core.sh', 'Uninstall RG DS Core.sh'])
        self.assertEqual(sorted(p.name for p in internal.iterdir()), ['RG DS Core.sh', 'Uninstall RG DS Core.sh'])
        self.assertFalse(other_card.exists())

    def test_fresh_upgrade_and_rollback_keep_user_data(self):
        save = self.root / 'userdata/library/game/save.sav'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'progress')
        with patch.object(activate, 'stop_session'):
            self.install(self.package('v1'))
            self.install(self.package('v2'))
            self.assertEqual(activate.read_pointer(self.root / 'state/active'), 'v2')
            self.assertEqual(activate.read_pointer(self.root / 'state/previous'), 'v1')
            self.assertEqual(activate.rollback(self.root), 'v1')
        self.assertEqual(save.read_bytes(), b'progress')
        for version in ['v1', 'v2']:
            activate.verify_release(self.root / 'releases' / version)

    def test_service_update_refuses_to_replace_a_changed_release_before_stopping_it(self):
        path = self.root / 'state/active'
        path.parent.mkdir(parents=True)
        path.write_text('newer\n')
        archive = self.package('sharing-update', metadata=dict(service_update=dict(base_build='older')))
        with patch.object(activate, 'stop_session') as stop:
            with self.assertRaisesRegex(ValueError, 'installed release changed'):
                self.install(archive)
        stop.assert_not_called()
        self.assertEqual(path.read_text(), 'newer\n')

    def test_fresh_install_defaults_to_core_and_upgrade_preserves_stock_choice(self):
        import boot_control
        supervisor = self.base / 'supervisor'
        supervisor.touch()
        with patch.object(activate, 'stop_session'), patch.object(boot_control, 'SUPERVISOR', supervisor), patch.object(
            boot_control, 'read_json', return_value={}
        ), patch.object(boot_control, 'configure') as configure:
            self.install(self.package('v1'))
            configure.assert_called_once_with(self.root, 'rgds', warning=True)
        self.assertTrue((self.root / 'userdata/setup-required').exists())
        with patch.object(activate, 'stop_session'), patch.object(boot_control, 'SUPERVISOR', supervisor), patch.object(
            boot_control, 'read_json', return_value=dict(root=str(self.root), mode='stock', warning=False)
        ), patch.object(boot_control, 'configure') as configure:
            self.install(self.package('v2'))
            configure.assert_called_once_with(self.root, 'stock', warning=False)

    def test_check_is_read_only_and_checksum_covers_bootstrap(self):
        archive = self.package('v1')
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        with self.assertRaises(SystemExit) as done:
            self.install(archive, '--check', '--sha256', digest)
        self.assertEqual(done.exception.code, 0)
        self.assertFalse(self.root.exists())
        self.assertTrue(archive.exists())
        archive = self.package('v1', {'rgds/main.sh': b'changed bootstrap'})
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.install(archive, '--sha256', digest)
        self.assertFalse(self.root.exists())

    def test_traversal_and_duplicate_entries_are_rejected(self):
        archive = self.package('v1', {'rgds/../../escape': b'bad'})
        with self.assertRaisesRegex(ValueError, 'Unsafe archive'):
            self.install(archive)
        self.assertFalse(self.root.exists())
        archive = self.package('v1')
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            with zipfile.ZipFile(archive, 'a') as z:
                z.writestr('rgds/state/active', 'v1')
        with self.assertRaisesRegex(ValueError, 'Duplicate archive'):
            self.install(archive)

    def test_full_card_is_rejected_before_staging(self):
        archive = self.package('v1')
        with patch('shutil.disk_usage', return_value=shutil._ntuple_diskusage(100, 99, 1)):
            with self.assertRaisesRegex(ValueError, 'Not enough space'):
                self.install(archive)
        self.assertFalse(self.root.exists())

    def test_expanded_package_limit_is_enforced_before_reading_payloads(self):
        archive = self.package('v1')
        oversized = zipfile.ZipInfo('oversized')
        oversized.file_size = 512 * 1024 * 1024 + 1
        with patch.object(zipfile.ZipFile, 'infolist', return_value=[oversized]):
            with self.assertRaisesRegex(ValueError, 'Package exceeds size limit'):
                self.install(archive)
        self.assertFalse(self.root.exists())

    def test_active_game_and_failed_activation_keep_previous_release(self):
        with patch.object(activate, 'stop_session'):
            self.install(self.package('v1'))
        with patch.object(activate, 'stop_session', side_effect=ValueError('A game is running')):
            with self.assertRaisesRegex(ValueError, 'game is running'):
                self.install(self.package('v2'))
        self.assertEqual(activate.read_pointer(self.root / 'state/active'), 'v1')
        real_write = activate.write_pointer

        def fail_active(path, value):
            if path.name == 'active': raise OSError('interrupted activation')
            return real_write(path, value)

        with patch.object(activate, 'stop_session'), patch.object(activate, 'write_pointer', side_effect=fail_active):
            with self.assertRaisesRegex(OSError, 'interrupted'):
                self.install(self.package('v2'))
        self.assertEqual(activate.read_pointer(self.root / 'state/active'), 'v1')
        with patch.object(activate, 'stop_session'):
            self.install(self.package('v2'))
        self.assertEqual(activate.read_pointer(self.root / 'state/active'), 'v2')


if __name__ == '__main__': unittest.main()
