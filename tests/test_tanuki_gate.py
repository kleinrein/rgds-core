"""3DS games show only on a handheld that has the 3DS engine (emulators.tanuki_engine): the OS image carries it since
0.9.1, and a development build on the data partition goes first. An image built with own_os_build.py --no-3ds has
none, and then neither the launcher nor the card shows a 3DS folder whose games could not start."""
import os, re, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROOTFS = ROOT / 'os/buildroot/rootfs'
sys.path.insert(0, str(ROOT / 'runtime'))
import emulators

ENGINES = [str(engine) for engine in emulators.TANUKI_ENGINES]


class TanukiGateTest(unittest.TestCase):
    def gate(self, script):
        lines = [line for line in (ROOTFS / script).read_text().splitlines() if all(e in line for e in ENGINES)]
        self.assertEqual(len(lines), 1, script)
        return lines[0]

    def run_gate(self, line, tmp, present, show):
        # each engine folder moved under tmp, so the test can say which of them has a run.sh
        for n, engine in enumerate(ENGINES):
            line = line.replace(engine, f'{tmp}/e{n}')
            if n in present:
                Path(tmp, f'e{n}').mkdir(exist_ok=True)
                Path(tmp, f'e{n}/run.sh').write_text('')
        return subprocess.run(['sh', '-c', line + '\n' + show], capture_output=True, text=True).stdout

    def test_the_launcher_gets_the_3ds_folder_where_either_engine_is(self):
        session = (ROOTFS / 'usr/libexec/rgds/session').read_text()
        launch = session[session.index('$PIN $APP/bin/rgds-launcher'):]
        self.assertNotIn('3ds=', launch[:launch.index('>> $LOG')])
        self.assertIn(' $CTR ', launch)
        line = self.gate('usr/libexec/rgds/session')
        show = 'for a in $CTR; do printf "[%s]" "$a"; done'
        for present, expected in (((), ''), ((0,), 'yes'), ((1,), 'yes'), ((0, 1), 'yes')):
            with tempfile.TemporaryDirectory() as tmp:
                out = self.run_gate(line, tmp, present, show)
                self.assertEqual(out, '[--system-dir][3ds=/data/roms/3ds]' if expected else '', present)

    def test_the_3ds_folder_is_made_where_either_engine_is(self):
        rcs = (ROOTFS / 'etc/init.d/rcS').read_text()
        made = re.search(r'mkdir -p /data/rgds/userdata[^\n]*(?:\\\n[^\n]*)*', rcs).group(0)
        self.assertNotIn('/data/roms/3ds', made)
        line = self.gate('etc/init.d/rcS')
        for present in ((), (0,), (1,)):
            with tempfile.TemporaryDirectory() as tmp:
                out = self.run_gate(
                    line.replace('/data/roms/3ds', f'{tmp}/roms3ds'), tmp, present, f'[ -d {tmp}/roms3ds ] && echo made'
                )
                self.assertEqual(out.strip(), 'made' if present else '', present)

    def test_the_data_partition_engine_goes_before_the_image_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            dev, image = Path(tmp, 'data'), Path(tmp, 'image')
            self.assertIsNone(emulators.tanuki_engine((dev, image)))
            image.mkdir()
            (image / 'run.sh').write_text('')
            self.assertEqual(emulators.tanuki_engine((dev, image)), image)
            dev.mkdir()
            self.assertEqual(emulators.tanuki_engine((dev, image)), image)  # an empty folder is no engine
            (dev / 'run.sh').write_text('')
            self.assertEqual(emulators.tanuki_engine((dev, image)), dev)

    def test_the_image_package_installs_the_staged_engine(self):
        mk = (ROOT / 'os/buildroot/package/rgds-core/rgds-core.mk').read_text()
        self.assertIn('cp -a $(@D)/engines $(TARGET_DIR)/usr/share/rgds/engines', mk)
        self.assertIn("stage_dir / 'engines/tanuki3ds'", (ROOT / 'tools/own_os_build.py').read_text())
        self.assertEqual(ENGINES[1], '/usr/share/rgds/engines/tanuki3ds')

    def test_the_image_build_stages_exactly_the_recorded_engine_or_stops(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import hashlib, json, own_os_build
        with tempfile.TemporaryDirectory() as tmp:
            source, stage = Path(tmp, 'engine'), Path(tmp, 'stage')
            real = own_os_build.TANUKI
            own_os_build.TANUKI = source
            try:
                with self.assertRaisesRegex(SystemExit, 'No 3DS engine staged'):
                    own_os_build.stage_tanuki(stage, False)
                own_os_build.stage_tanuki(stage, True)  # --no-3ds: nothing, on purpose
                self.assertFalse((stage / 'engines').exists())
                files = {'run.sh': b'exec ctremu', 'ctremu': b'elf', 'lib2/libEGL_mesa.so.0': b'egl'}
                for name, body in files.items():
                    (source / name).parent.mkdir(parents=True, exist_ok=True)
                    (source / name).write_bytes(body)
                (source / 'ctremu.pt27').write_bytes(b'test build')  # not listed, not shipped
                record = {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}
                (source / 'engine.json').write_text(json.dumps({'files': record}))
                own_os_build.stage_tanuki(stage, False)
                shipped = sorted(
                    p.relative_to(stage / 'engines/tanuki3ds').as_posix() for p in stage.rglob('*') if p.is_file()
                )
                self.assertEqual(shipped, sorted([*files, 'engine.json']))
                self.assertTrue(os.access(stage / 'engines/tanuki3ds/ctremu', os.X_OK))
                (source / 'ctremu').write_bytes(b'another build')
                with self.assertRaisesRegex(SystemExit, 'does not match'):
                    own_os_build.stage_tanuki(Path(tmp, 'stage2'), False)
            finally:
                own_os_build.TANUKI = real


if __name__ == '__main__':
    unittest.main()
