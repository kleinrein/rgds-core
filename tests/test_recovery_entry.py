"""Recovery disables owned startup even when per-installation writes fail."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class RecoveryEntryTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='rgds-recovery ');self.addCleanup(temporary.cleanup)
        self.base=Path(temporary.name);self.card=self.base/'card';self.apps=self.card/'Roms/APPS';self.apps.mkdir(parents=True)
        self.root=self.card/'rgds';self.root.mkdir()
        self.control=self.base/'system/userdata/rgds-boot';self.control.mkdir(parents=True)
        (self.control/'enabled').write_text('1\n');(self.control/'config.json').write_text('original config\n')
        source=(ROOT/'packaging/RGDS Recovery.sh').read_text()
        replacements={
            '/userdata/rgds-boot/enabled':self.control/'enabled', '/userdata/rgds-boot':self.control,
            '/userdata/rgds-device-test/rgds':self.base/'missing-internal', '/mnt/mmc/rgds':self.base/'missing-card-one',
            '/mnt/SDCARD/rgds':self.base/'missing-card-two',
        }
        # Substitute only fixed system paths so this shell fixture cannot touch
        # the actual system, including on a machine with /userdata mounted.
        pattern='|'.join(re.escape(path) for path in sorted(replacements,key=len,reverse=True))
        source=re.sub(pattern,lambda match:shlex.quote(str(replacements[match.group()])),source)
        self.entry=self.apps/'RGDS Recovery.sh';self.entry.write_text(source)

    def run_entry(self,environment=None):
        return subprocess.run(['/bin/sh',str(self.entry)],env=environment,text=True,capture_output=True,timeout=5)

    def test_global_marker_and_same_card_apps_launching_are_disabled(self):
        result=self.run_entry();self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.control/'enabled').exists());self.assertTrue((self.root/'userdata/boot_stock').is_file())
        self.assertEqual((self.control/'config.json').read_text(),'original config\n')
        self.assertIn('next boot uses Anbernic',result.stdout)

    def test_unwritable_card_does_not_prevent_global_stock_recovery(self):
        helpers=self.base/'helpers';helpers.mkdir();touch=helpers/'touch';touch.write_text('#!/bin/sh\nexit 1\n');touch.chmod(0o755)
        environment=dict(os.environ,PATH=str(helpers)+os.pathsep+os.environ.get('PATH',''))
        result=self.run_entry(environment);self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.control/'enabled').exists());self.assertFalse((self.root/'userdata/boot_stock').exists())
        self.assertIn('Could not write the recovery file',result.stderr)


if __name__=='__main__':unittest.main()
