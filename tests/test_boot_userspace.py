import json
import unittest
from unittest.mock import patch
import test_boot_branding as fixtures
import boot_userspace
import boot_branding

class UserspaceBrandingTests(unittest.TestCase):
    setUp = fixtures.BootBrandingTests.setUp
    # Reuse only the realistic fixture, not the partition-writing test cases.
    def test_userspace_apply_repeat_restore_preserves_boot_image(self):
        before=self.device.read_bytes()
        first=boot_userspace.install(self.assets,self.root)
        self.assertEqual(first['files'],7)
        self.assertFalse(first['boot_partition_written'])
        self.assertEqual(first,boot_userspace.install(self.assets,self.root))
        boot_userspace.install(self.assets,self.root,restore=True)
        self.assertEqual(self.device.read_bytes(),before)
        for path,(content,mode) in self.users.items():
            self.assertEqual(path.read_bytes(),content)
            self.assertEqual(path.stat().st_mode & 0o777,mode)

    def test_userspace_external_change_rejects_before_writing_any_file(self):
        boot_userspace.install(self.assets,self.root)
        changed=list(self.users)[-1];changed.write_bytes(b'owner change')
        before={p:p.read_bytes() for p in self.users}
        with self.assertRaisesRegex(ValueError,'outside RGDS'):boot_userspace.install(self.assets,self.root,restore=True)
        self.assertEqual(before,{p:p.read_bytes() for p in self.users})

    def test_userspace_failure_restores_changed_files(self):
        original=boot_branding.replace_user;calls=0
        def replace(*args,**kwargs):
            nonlocal calls
            calls+=1
            if calls==2:raise OSError('simulated write failure')
            return original(*args,**kwargs)
        with patch.object(boot_branding,'replace_user',side_effect=replace):
            with self.assertRaises(OSError):boot_userspace.install(self.assets,self.root)
        for path,(content,_) in self.users.items():self.assertEqual(path.read_bytes(),content)

    def test_read_only_inspection_does_not_claim_an_early_logo_or_visual_boot(self):
        before={p:p.read_bytes() for p in self.users}
        self.assertEqual(boot_userspace.inspect(self.root)['files'],[])
        self.assertFalse((self.root/'userdata/rgds-boot/userspace-branding').exists())
        boot_userspace.install(self.assets,self.root)
        status=boot_userspace.inspect(self.root)
        self.assertFalse(status['early_boot_logo_changed'])
        self.assertFalse(status['visual_boot_verified'])
        self.assertEqual({f['state'] for f in status['files']},{'installed'})
        list(self.users)[0].write_bytes(b'firmware replaced the splash')
        status=boot_userspace.inspect(self.root)
        self.assertEqual(sum(f['state']=='changed' for f in status['files']),1)
