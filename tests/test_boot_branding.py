"""Only proven logo patches reach a boot partition; originals always survive."""
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from test_boot_image import fit
import boot_branding as branding
import boot_image


def png(colour=b'\x10\x20\x30'):
    def chunk(name, payload):
        return struct.pack('>I', len(payload)) + name + payload + struct.pack('>I', zlib.crc32(name + payload) & 0xffffffff)
    header = struct.pack('>IIBBBBB', 640, 480, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress((b'\0' + colour * 640) * 480)) + chunk(b'IEND', b'')


class BootBrandingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.assets = self.root / 'assets';shutil.copytree(ROOT / 'assets/boot', self.assets)
        self.original = bytes(fit()[0]);self.prepared,self.details = boot_image.patch(self.original, (self.assets / 'logo.bmp').read_bytes())
        self.device = self.root / 'dev/mmcblk0p4';self.device.parent.mkdir();self.device.write_bytes(self.original)
        self.users = {}
        waiting = bytearray((self.assets / 'waiting.bmp').read_bytes());waiting[54:] = bytes([0x28]) * (len(waiting) - 54)
        for index,(absolute, asset) in enumerate(branding.USER_LOGOS):
            path = branding.location(self.root, absolute);path.parent.mkdir(parents=True,exist_ok=True)
            content = png() if asset == 'logo.png' else bytes(waiting)
            mode = 0o640 if index == 0 else 0o644
            path.write_bytes(content);path.chmod(mode);self.users[path] = (content,mode)
        self.init = self.root / 'etc/init.d/S50launch';self.init.parent.mkdir(parents=True);self.init.write_text('untouched vendor init\n')
        self.store = branding.location(self.root, branding.STORE)

    def install(self):
        return branding.apply(self.device,self.prepared,self.original,self.assets,self.root)

    def assert_stock(self):
        self.assertEqual(self.device.read_bytes(),self.original)
        for path,(content,mode) in self.users.items():
            self.assertEqual(path.read_bytes(),content);self.assertEqual(stat.S_IMODE(path.stat().st_mode),mode)
        self.assertEqual(self.init.read_text(),'untouched vendor init\n')

    def test_plan_is_read_only_and_names_exact_known_changes(self):
        result = branding.plan(self.device,self.prepared,self.original,self.assets,self.root)
        self.assertEqual(result['state'],'prepared');self.assertEqual(len(result['files']),7)
        self.assertEqual(result['boot_device'],str(self.device));self.assertEqual(result['size'],len(self.original))
        self.assertFalse(self.store.exists());self.assert_stock()

    def test_installs_only_logo_sectors_after_durable_originals_and_restores_exact_bytes_modes(self):
        native = branding.write_device;writes = []
        native_write = os.write
        def recorded_write(fd, content):
            writes.append((os.lseek(fd,0,os.SEEK_CUR),len(content)));return native_write(fd,content)
        def checked_write(fd,content):
            self.assertEqual((self.store / 'original-boot.bin').read_bytes(),self.original)
            record = json.loads((self.store / 'manifest.json').read_text())
            self.assertEqual(record['state'],'prepared')
            for item in record['files']:
                self.assertEqual(branding.sha256((self.store / item['backup']).read_bytes()),item['original_sha256'])
            with patch.object(branding.os,'write',side_effect=recorded_write):native(fd,content)
        with patch.object(branding,'write_device',side_effect=checked_write):record = self.install()
        allowed = set()
        for change in self.details['changes']:
            allowed.update(range(change['start']//512,(change['start'] + change['size'] - 1)//512 + 1))
        written = {offset//512 for start,size in writes for offset in range(start,start+size,512)}
        self.assertTrue(written);self.assertTrue(written <= allowed)
        self.assertEqual(record['state'],'installed');self.assertEqual(self.device.read_bytes(),self.prepared)
        self.assertEqual(self.device.read_bytes()[-31:],self.original[-31:])
        for path,(original,mode) in self.users.items():
            self.assertNotEqual(path.read_bytes(),original);self.assertEqual(stat.S_IMODE(path.stat().st_mode),mode)
        restored = branding.restore(self.device,self.root)
        self.assertEqual(restored['state'],'restored');self.assert_stock()
        self.assertEqual((self.store / 'original-boot.bin').read_bytes(),self.original)
        self.assertEqual(branding.restore(self.device,self.root)['state'],'restored')

    def test_prepared_extra_kernel_or_tail_changes_are_rejected_without_backups(self):
        for position in (80,len(self.prepared)-1):
            edited = bytearray(self.prepared);edited[position] ^= 1
            with self.subTest(position=position),self.assertRaisesRegex(ValueError,'beyond the approved'):
                branding.apply(self.device,bytes(edited),self.original,self.assets,self.root)
        self.assertFalse(self.store.exists());self.assert_stock()

    def test_snapshot_must_match_complete_current_partition(self):
        with self.assertRaisesRegex(ValueError,'complete boot partition'):
            branding.plan(self.device,self.prepared[:-31],self.original[:-31],self.assets,self.root)
        altered = bytearray(self.original);altered[-1] ^= 1;self.device.write_bytes(altered)
        with self.assertRaisesRegex(ValueError,'no longer matches'):
            branding.plan(self.device,self.prepared,self.original,self.assets,self.root)
        self.assertFalse(self.store.exists())

    def test_relative_outside_fixture_and_real_host_devices_cannot_be_written(self):
        with self.assertRaisesRegex(ValueError,'explicit absolute'):
            branding.plan(Path('dev/mmcblk0p4'),self.prepared,self.original,self.assets,self.root)
        with self.assertRaisesRegex(ValueError,'inside the explicit fixture'):
            branding.open_device(self.device,self.root/'other',write=True)
        with patch.object(branding,'discover_boot_device',return_value=self.device),patch.object(branding.platform,'machine',return_value='x86_64'):
            with self.assertRaisesRegex(ValueError,'Linux AArch64'):branding.open_device(self.device,'/',write=True)
        self.assertFalse(self.store.exists());self.assert_stock()

    def test_optional_missing_userspace_logos_are_skipped_but_symlinked_logos_reject(self):
        absent = next(iter(self.users));absent.unlink()
        self.assertEqual(len(branding.plan(self.device,self.prepared,self.original,self.assets,self.root)['files']),6)
        foreign = self.root/'foreign.bmp';foreign.write_bytes(self.users[absent][0]);absent.symlink_to(foreign)
        with self.assertRaisesRegex(ValueError,'symlinks'):
            branding.plan(self.device,self.prepared,self.original,self.assets,self.root)
        self.assertFalse(self.store.exists());self.assertEqual(foreign.read_bytes(),self.users[absent][0])

    def test_corrupt_userspace_assets_or_unknown_original_shapes_reject(self):
        logo = self.assets / 'logo.png';original = logo.read_bytes();corrupt = bytearray(original);corrupt[-1] ^= 1;logo.write_bytes(corrupt)
        with self.assertRaisesRegex(ValueError,'checksum'):branding.plan(self.device,self.prepared,self.original,self.assets,self.root)
        logo.write_bytes(original)
        waiting = next(iter(self.users));wrong = bytearray(waiting.read_bytes());struct.pack_into('<i',wrong,18,123);waiting.write_bytes(wrong)
        with self.assertRaisesRegex(ValueError,'330x50'):branding.plan(self.device,self.prepared,self.original,self.assets,self.root)
        self.assertFalse(self.store.exists())

    def test_partial_partition_write_failure_restores_original_and_keeps_provenance(self):
        native = branding.write_device;attempts = []
        def fail_first(fd,content):
            attempts.append(content)
            if len(attempts) == 1:
                change = self.details['changes'][0];os.lseek(fd,change['start'],os.SEEK_SET)
                os.write(fd,content[change['start']:change['start']+20]);os.fsync(fd)
                raise OSError('injected flash failure')
            return native(fd,content)
        with patch.object(branding,'write_device',side_effect=fail_first),self.assertRaisesRegex(OSError,'injected flash failure'):
            self.install()
        self.assertEqual(len(attempts),2);self.assert_stock()
        self.assertEqual(json.loads((self.store/'manifest.json').read_text())['state'],'restored')
        self.assertEqual((self.store/'original-boot.bin').read_bytes(),self.original)

    def test_userspace_readback_failure_rolls_back_partition_and_written_logo(self):
        native = branding.replace_user;attempts = []
        def fail_first(*args,**kwargs):
            attempts.append(args);native(*args,**kwargs)
            if len(attempts) == 1:raise OSError('injected logo failure')
        with patch.object(branding,'replace_user',side_effect=fail_first),self.assertRaisesRegex(OSError,'injected logo failure'):
            self.install()
        self.assert_stock();self.assertEqual(json.loads((self.store/'manifest.json').read_text())['state'],'restored')

    def test_restore_rejects_external_partition_or_logo_edits_before_any_write(self):
        self.install();installed = self.device.read_bytes()
        changed = bytearray(installed);changed[-1] ^= 1;self.device.write_bytes(changed)
        with patch.object(branding,'write_device') as writer,self.assertRaisesRegex(ValueError,'changed externally'):
            branding.restore(self.device,self.root)
        writer.assert_not_called();self.assertEqual(self.device.read_bytes(),bytes(changed))
        self.device.write_bytes(installed)
        target = next(iter(self.users));external = b'external logo';target.write_bytes(external)
        with patch.object(branding,'write_device') as writer,self.assertRaisesRegex(ValueError,'changed externally'):
            branding.restore(self.device,self.root)
        writer.assert_not_called();self.assertEqual(target.read_bytes(),external)
        self.assertEqual(self.device.read_bytes(),installed)

    def test_corrupt_original_backup_cannot_be_used_for_restore(self):
        self.install();original = self.device.read_bytes();(self.store/'original-boot.bin').write_bytes(b'bad backup')
        with self.assertRaisesRegex(ValueError,'provenance'):branding.restore(self.device,self.root)
        self.assertEqual(self.device.read_bytes(),original)

    def test_configure_current_is_idempotent_and_never_overwrites_original_backups(self):
        first = branding.configure_current(self.assets,self.device,self.root)
        stamp = (self.store/'original-boot.bin').stat().st_mtime_ns
        with patch.object(branding,'write_device') as writer:second = branding.configure_current(self.assets,self.device,self.root)
        writer.assert_not_called();self.assertEqual(first,second)
        self.assertEqual((self.store/'original-boot.bin').stat().st_mtime_ns,stamp)
        (self.assets/'logo.png').write_bytes(png(b'\x66\x77\x88'))
        branding.configure_current(self.assets,self.device,self.root)
        self.assertEqual((self.store/'original-boot.bin').read_bytes(),self.original)
        branding.verify_installed(self.device,self.root,self.assets)

    def test_restore_failure_rolls_back_to_installed_bytes(self):
        self.install();installed = self.device.read_bytes();native = branding.write_device;attempts = []
        def fail_first(fd,content):
            attempts.append(content);native(fd,content)
            if len(attempts) == 1:raise OSError('injected restore failure')
        with patch.object(branding,'write_device',side_effect=fail_first),self.assertRaisesRegex(OSError,'injected restore failure'):
            branding.restore(self.device,self.root)
        self.assertEqual(self.device.read_bytes(),installed)
        self.assertEqual(branding.verify_installed(self.device,self.root)['state'],'installed')

    def boot_aliases(self, selected='0'):
        attr = branding.location(self.root,branding.BOOT_SOURCE);attr.parent.mkdir(parents=True);attr.write_text(selected+'\n')
        alias = branding.location(self.root,branding.BOOT_ALIASES[0]);alias.parent.mkdir(parents=True);alias.symlink_to(self.device)
        physical = self.root/'sys/devices/platform/mmcblk0/mmcblk0p4';physical.mkdir(parents=True);(physical/'partition').write_text('4\n')
        entry = self.root/'sys/class/block/mmcblk0p4';entry.parent.mkdir(parents=True);entry.symlink_to(physical)
        return attr,physical

    def test_discovery_requires_alias_and_active_medium_proof_and_nonzero_partition(self):
        with self.assertRaises(ValueError):branding.discover_boot_device(self.root)
        attr,physical = self.boot_aliases()
        self.assertEqual(branding.discover_boot_device(self.root),self.device)
        attr.write_text('1\n')
        with self.assertRaisesRegex(ValueError,'active boot medium'):branding.discover_boot_device(self.root)
        attr.write_text('0\n');(physical/'partition').write_text('0\n')
        with self.assertRaisesRegex(ValueError,'sysfs partition'):branding.discover_boot_device(self.root)

    def test_sysfs_stat_size_is_not_used_as_the_attribute_content_length(self):
        self.boot_aliases();native = Path.stat
        def sysfs_size(path,*args,**kwargs):
            result = native(path,*args,**kwargs)
            if path.name in ('boot_device','partition'):
                values = list(result);values[6] = 4096;return os.stat_result(values)
            return result
        with patch.object(Path,'stat',sysfs_size):self.assertEqual(branding.discover_boot_device(self.root),self.device)
        attr = branding.location(self.root,branding.BOOT_SOURCE);attr.write_text('0'*33)
        with self.assertRaisesRegex(ValueError,'supported length'):branding.discover_boot_device(self.root)

    def test_cli_requires_explicit_device_and_plan_remains_read_only(self):
        original = self.root/'snapshot.bin';original.write_bytes(self.original)
        prepared = self.root/'prepared.bin';prepared.write_bytes(self.prepared)
        command = [sys.executable,'-B',str(ROOT/'runtime/boot_branding.py'),'plan','--image',str(prepared),
                   '--original',str(original),'--assets',str(self.assets),'--system-root',str(self.root)]
        missing = subprocess.run(command,capture_output=True,text=True,timeout=5)
        self.assertEqual(missing.returncode,2);self.assertIn('--boot-device',missing.stderr)
        planned = subprocess.run(command+['--boot-device',str(self.device)],capture_output=True,text=True,timeout=5)
        self.assertEqual(planned.returncode,0,planned.stderr);self.assertEqual(json.loads(planned.stdout)['state'],'prepared')
        self.assertFalse(self.store.exists());self.assert_stock()



class RebrandTests(unittest.TestCase):
    setUp = BootBrandingTests.setUp
    install = BootBrandingTests.install
    assert_stock = BootBrandingTests.assert_stock
    def test_rebrand_keeps_vendor_originals_and_can_restore_them(self):
        self.install()
        newer = self.root/'new-assets';newer.mkdir()
        for name in ('logo.bmp','logo.png','waiting.bmp'):
            (newer/name).write_bytes((self.assets/name).read_bytes())
        # A valid logo with one changed RGB pixel is a distinct identity.
        logo=bytearray((newer/'logo.bmp').read_bytes());logo[-1]^=1;(newer/'logo.bmp').write_bytes(logo)
        previous=branding.sha256(self.prepared)
        result=branding.rebrand_current(newer,self.device,self.root)
        self.assertEqual(result['state'],'installed')
        self.assertEqual((self.store/'original-boot.bin').read_bytes(),self.original)
        archived=self.store.with_name(self.store.name+'.previous-'+previous[:16])
        self.assertEqual((archived/'original-boot.bin').read_bytes(),self.original)
        branding.verify_installed(self.device,self.root,newer)
        branding.restore(self.device,self.root);self.assert_stock()
    def test_rebrand_rejects_external_changes_before_restore(self):
        self.install();damaged=bytearray(self.device.read_bytes());damaged[-1]^=1;self.device.write_bytes(damaged)
        with self.assertRaises(ValueError):branding.rebrand_current(self.assets,self.device,self.root)
        self.assertEqual(self.device.read_bytes(),bytes(damaged))


if __name__ == '__main__':unittest.main()
