"""os/buildroot/rootfs/usr/libexec/rgds/trial never puts a boot menu without both slots in place: on 2026-11-24 a
commit on an RG DS Plus left a 0-byte extlinux.conf and the handheld stopped at U-Boot with black panels."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'os/buildroot/rootfs/usr/libexec/rgds/trial'
MENU = '''DEFAULT b
FALLBACK a
TIMEOUT 0
LABEL a
  LINUX /a/Image.lz4
  FDTDIR /a/
LABEL b
  LINUX /b/Image.lz4
  FDTDIR /b/
'''


class Trial(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.boot = root / 'boot'
        (self.boot / 'extlinux').mkdir(parents=True)
        (root / 'verified').touch()
        self.env = dict(
            os.environ,
            RGDS_TRIAL_BOOT=str(self.boot),
            RGDS_TRIAL_RESULT=str(root / 'state/update-result.json'),
            RGDS_TRIAL_SLOT='b',
            RGDS_TRIAL_VERIFIED=str(root / 'verified')
        )

    def tearDown(self):
        self.tmp.cleanup()

    def run_trial(self, *args):
        return subprocess.run(['sh', str(SCRIPT), *args], env=self.env, capture_output=True, text=True, timeout=10)

    def menu(self):
        return (self.boot / 'extlinux/extlinux.conf').read_text()

    def test_commit_drops_the_fallback_and_keeps_both_slots(self):
        (self.boot / 'extlinux/extlinux.conf').write_text(MENU)
        (self.boot / 'rgds-trial.json').write_text('{"slot": "b", "previous": "a", "build": "x", "version": "1"}\n')
        self.run_trial('commit')
        self.assertNotIn('FALLBACK', self.menu())
        self.assertIn('LABEL a\n', self.menu())
        self.assertIn('LABEL b\n', self.menu())

    def test_an_empty_menu_is_never_written(self):
        # The filter reading an empty file must not produce one: the old file stays as it is.
        (self.boot / 'extlinux/extlinux.conf').write_text('')
        (self.boot / 'rgds-trial.json').write_text('{"slot": "b", "previous": "a", "build": "x", "version": "1"}\n')
        result = self.run_trial('commit')
        self.assertIn('lacks a slot', result.stderr)
        self.assertFalse((self.boot / 'extlinux/extlinux.conf.new').exists())

    def test_arm_keeps_both_slots(self):
        (self.boot / 'extlinux/extlinux.conf').write_text(MENU.replace('FALLBACK a\n', ''))
        self.run_trial('arm', 'a', 'b', 'build', 'push')
        self.assertTrue(self.menu().startswith('DEFAULT a\nFALLBACK b\n'))
        self.assertIn('LABEL b\n', self.menu())


if __name__ == '__main__':
    unittest.main()
