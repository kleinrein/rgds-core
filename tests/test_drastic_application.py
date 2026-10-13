"""Private DraStic import excludes OS dependencies, BIOS dumps and user progress."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
sys.path.insert(0,str(ROOT/'tests'))
import provision_drastic
from test_elf_abi import fixture


class DrasticApplicationTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name);self.source=self.root/'source';self.source.mkdir()
        elf,_=fixture()
        (self.source/'drastic').write_bytes(elf)
        (self.source/'libs').mkdir();(self.source/'libs/libdrastouch.so').write_bytes(elf)

    def test_only_application_whitelist_is_imported(self):
        for name in ('libs/libSDL2.so','system/nds_bios_arm9.bin','backup/game.sav','config/drastic.cfg','run.sh'):
            p=self.source/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'excluded')
        (self.source/'game_database.xml').write_text('<database/>')
        destination=self.root/'application'
        record=provision_drastic.provision(self.source,destination)
        self.assertEqual(set(record['files']),{'drastic','libs/libdrastouch.so','game_database.xml'})
        self.assertTrue(record['private_only'])
        self.assertEqual(provision_drastic.validate(destination),record)
        self.assertFalse((destination/'system').exists());self.assertFalse((destination/'backup').exists())

    def test_missing_touch_adapter_fails_before_destination_is_created(self):
        (self.source/'libs/libdrastouch.so').unlink();destination=self.root/'application'
        with self.assertRaisesRegex(ValueError,'libdrastouch'):
            provision_drastic.provision(self.source,destination)
        self.assertFalse(destination.exists())

    def test_wrong_architecture_is_rejected(self):
        elf,_=fixture(machine=62);(self.source/'drastic').write_bytes(elf)
        with self.assertRaisesRegex(ValueError,'AArch64'):
            provision_drastic.provision(self.source,self.root/'application')

    def test_changed_and_unlisted_components_fail_validation(self):
        destination=self.root/'application';provision_drastic.provision(self.source,destination)
        (destination/'run.sh').write_text('foreign')
        with self.assertRaisesRegex(ValueError,'inventory'):
            provision_drastic.validate(destination)
        (destination/'run.sh').unlink();(destination/'drastic').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'hash'):
            provision_drastic.validate(destination)

    def test_symlinked_source_is_not_followed(self):
        binary=self.source/'drastic';binary.rename(self.root/'external');binary.symlink_to(self.root/'external')
        with self.assertRaisesRegex(ValueError,'regular'):
            provision_drastic.provision(self.source,self.root/'application')


if __name__=='__main__':unittest.main()
