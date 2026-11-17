"""3DS games show only on a handheld that has the 3DS engine: it lives on the data partition, not in the OS image, so
one that has only had OTA updates has none (emulators.TANUKI_ENGINE)."""
import re, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROOTFS = ROOT / 'os/buildroot/rootfs'
ENGINE = '/data/rgds/engines/tanuki3ds/run.sh'


class TanukiGateTest(unittest.TestCase):

    def gate(self, script):
        lines = [line for line in (ROOTFS / script).read_text().splitlines() if ENGINE in line]
        self.assertEqual(len(lines), 1, script)
        return lines[0]

    def test_the_launcher_gets_the_3ds_folder_only_with_the_engine(self):
        session = (ROOTFS / 'usr/libexec/rgds/session').read_text()
        launch = session[session.index('$PIN $APP/bin/rgds-launcher'):]
        self.assertNotIn('3ds=', launch[:launch.index('>> $LOG')])
        self.assertIn(' $CTR ', launch)
        line = self.gate('usr/libexec/rgds/session')
        with tempfile.TemporaryDirectory() as tmp:
            engine = Path(tmp) / 'run.sh'
            run = line.replace(ENGINE, str(engine)) + '\nfor a in $CTR; do printf "[%s]" "$a"; done'
            self.assertEqual(subprocess.run(['sh', '-c', run], capture_output=True, text=True).stdout, '')
            engine.write_text('')
            self.assertEqual(
                subprocess.run(['sh', '-c', run], capture_output=True, text=True).stdout,
                '[--system-dir][3ds=/data/roms/3ds]'
            )

    def test_no_empty_3ds_folder_on_a_card_without_the_engine(self):
        rcs = (ROOTFS / 'etc/init.d/rcS').read_text()
        made = re.search(r'mkdir -p /data/rgds/userdata[^\n]*(?:\\\n[^\n]*)*', rcs).group(0)
        self.assertNotIn('/data/roms/3ds', made)
        self.assertIn('&& mkdir -p /data/roms/3ds', self.gate('etc/init.d/rcS'))


if __name__ == '__main__':
    unittest.main()
