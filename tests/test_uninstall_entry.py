"""APPS -> Uninstall RG DS Core: stock startup and artwork back, app files gone, saves kept."""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import boot_control as control  # noqa: E402


class UninstallFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='rgds-uninstall ')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.card = self.base / 'card'
        self.apps = self.card / 'Roms/APPS'
        self.apps.mkdir(parents=True)
        self.root = self.card / 'rgds'
        self.system = self.base / 'system'
        supervisor = control.location(self.system, control.SUPERVISOR)
        supervisor.parent.mkdir(parents=True)
        supervisor.write_text('#!/bin/sh\n# vendor controller loads /mnt/mod/ctrl/autostart\n')
        self.hook = control.location(self.system, control.HOOK)
        self.hook.parent.mkdir(parents=True)
        self.hook.write_text('#!/bin/sh\n# stock autostart\n')
        self.ctl = control.location(self.system, control.CONTROL)
        (self.root / 'state').mkdir(parents=True)
        (self.root / 'state/active').write_text('test-build\n')
        self.release = self.root / 'releases/test-build'
        (self.release / 'runtime').mkdir(parents=True)
        for source in (ROOT / 'runtime').glob('*.py'):
            shutil.copy(source, self.release / 'runtime' / source.name)
        (self.release / 'manifest.json').write_text('{}')
        for name in ('main.sh', 'inventory.sh', 'update_guard.py'):
            (self.root / name).write_text('#!/bin/sh\n')
        (self.root / 'docs').mkdir()
        (self.root / 'docs/README.md').write_text('docs\n')
        (self.root / 'inbox').mkdir()
        self.save = self.root / 'userdata/library/game/save.sav'
        self.save.parent.mkdir(parents=True)
        self.save.write_bytes(b'progress')
        (self.root / 'userdata/ui.ini').write_text('schema=1\n')
        self.rom = self.root / 'games/Puzzle.nds'
        self.rom.parent.mkdir()
        self.rom.write_bytes(b'rom')
        # An owned startup hook as the installer leaves it, with the stock original kept.
        self.ctl.mkdir(parents=True, mode=0o700)
        (self.ctl / 'original-autostart').write_bytes(self.hook.read_bytes())
        text = control.hook_text(self.root, control.CONTROL).encode()
        self.hook.write_bytes(text)
        import hashlib
        (self.ctl / 'installation.json').write_text(
            json.dumps(
                dict(schema=1, had_hook=True, previous_mode=0o755, hook_sha256=hashlib.sha256(text).hexdigest())
            )
        )
        (self.ctl /
         'config.json').write_text(json.dumps(dict(schema=1, root=str(self.root), mode='rgds', warning=True)))
        (self.ctl / 'enabled').write_text('1\n')
        for name in (
            'RG DS Core.sh', 'RGDS Probe.sh', 'RGDS Inventory.sh', 'RGDS Recovery.sh', 'Install RG DS Core.sh'
        ):
            (self.apps / name).write_text('#!/bin/sh\n')
        source = (ROOT / 'packaging/Uninstall RG DS Core.sh').read_text()
        replacements = {
            '/userdata/rgds-boot/enabled': self.ctl / 'enabled',
            '/mnt/mmc/Roms/APPS': self.base / 'missing-card-one/Roms/APPS',
            '/mnt/SDCARD/Roms/APPS': self.base / 'missing-card-two/Roms/APPS'
        }
        pattern = '|'.join(re.escape(path) for path in sorted(replacements, key=len, reverse=True))
        source = re.sub(pattern, lambda match: shlex.quote(str(replacements[match.group()])), source)
        self.entry = self.apps / 'Uninstall RG DS Core.sh'
        self.entry.write_text(source)

    def run_entry(self, environment=None):
        environment = dict(os.environ, RGDS_SYSTEM_ROOT=str(self.system), **(environment or {}))
        return subprocess.run(['/bin/sh', str(self.entry)], env=environment, text=True, capture_output=True, timeout=30)


class UninstallTests(UninstallFixture):
    def test_python_uninstaller_restores_stock_and_keeps_user_data(self):
        result = subprocess.run(
            [
                sys.executable, '-B',
                str(self.release / 'runtime/uninstall.py'), '--root',
                str(self.root), '--system-root',
                str(self.system)
            ],
            text=True,
            capture_output=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout[:result.stdout.rindex('}') + 1])
        self.assertEqual(report['startup'], 'restored')
        self.assertEqual(self.hook.read_text(), '#!/bin/sh\n# stock autostart\n')
        self.assertFalse(self.ctl.exists(), 'the owned control folder goes with the hook')
        for name in ('releases', 'state', 'inbox', 'docs', 'main.sh', 'inventory.sh', 'update_guard.py'):
            self.assertFalse((self.root / name).exists(), name)
        self.assertEqual(self.save.read_bytes(), b'progress')
        self.assertTrue((self.root / 'userdata/ui.ini').is_file())
        self.assertEqual(self.rom.read_bytes(), b'rom')
        self.assertEqual(sorted(report['kept']), ['games', 'userdata'])
        self.assertIn('Saves and settings stay', result.stdout)

    def test_a_changed_hook_is_left_alone_but_startup_is_still_disabled(self):
        self.hook.write_text('#!/bin/sh\n# someone else\n')
        result = subprocess.run(
            [
                sys.executable, '-B',
                str(self.release / 'runtime/uninstall.py'), '--root',
                str(self.root), '--system-root',
                str(self.system)
            ],
            text=True,
            capture_output=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout[:result.stdout.rindex('}') + 1])
        self.assertEqual(report['startup'], 'failed')
        self.assertEqual(self.hook.read_text(), '#!/bin/sh\n# someone else\n')
        self.assertFalse((self.ctl / 'enabled').exists())
        self.assertTrue((self.ctl / 'installation.json').is_file())
        self.assertFalse((self.root / 'releases').exists())
        self.assertEqual(self.save.read_bytes(), b'progress')

    def test_menu_entry_runs_the_uninstaller_and_removes_every_rgds_entry(self):
        result = self.run_entry()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RG DS Core removed', result.stdout)
        self.assertIn('entries have been removed', result.stdout)
        self.assertEqual(self.hook.read_text(), '#!/bin/sh\n# stock autostart\n')
        self.assertFalse((self.root / 'releases').exists())
        self.assertEqual(self.save.read_bytes(), b'progress')
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), [])
        self.assertTrue((self.card / 'rgds-uninstall.log').is_file())

    def test_menu_entry_without_a_release_removes_files_and_the_marker(self):
        shutil.rmtree(self.release)
        result = self.run_entry()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('removing the application files only', result.stderr)
        self.assertFalse((self.ctl / 'enabled').exists())
        self.assertEqual(self.hook.read_bytes(), control.hook_text(self.root, control.CONTROL).encode())
        for name in ('releases', 'state', 'inbox', 'docs', 'main.sh'):
            self.assertFalse((self.root / name).exists(), name)
        self.assertEqual(self.save.read_bytes(), b'progress')
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()), [])

    def test_a_boot_logo_backup_that_could_not_be_restored_is_kept(self):
        # The logo store lives under the control folder; without the boot device
        # the restore fails and the folder must survive for a later recovery.
        store = self.ctl / 'branding'
        store.mkdir()
        (store /
         'manifest.json').write_text(json.dumps(dict(schema=1, state='installed', boot_device='/dev/missing-boot')))
        (store / 'original.bin').write_bytes(b'stock logo')
        (self.system / 'etc/init.d').mkdir(parents=True)
        (self.system / 'etc/init.d/S49weston').write_text('#!/bin/sh\n')
        result = subprocess.run(
            [
                sys.executable, '-B',
                str(self.release / 'runtime/uninstall.py'), '--root',
                str(self.root), '--system-root',
                str(self.system)
            ],
            text=True,
            capture_output=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout[:result.stdout.rindex('}') + 1])
        self.assertEqual(report['startup'], 'restored')
        self.assertEqual(report['artwork'], 'failed')
        self.assertEqual((store / 'original.bin').read_bytes(), b'stock logo')
        self.assertEqual(self.hook.read_text(), '#!/bin/sh\n# stock autostart\n')
        self.assertFalse((self.root / 'releases').exists())

    def test_installer_accepts_the_menu_entry(self):
        source = (ROOT / 'packaging/install_package.py').read_text()
        self.assertIn("'Uninstall RG DS Core.sh'", source)


if __name__ == '__main__': unittest.main()
