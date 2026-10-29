import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime"))

import os_update
from os_update import OsUpdater, fit_label, replace_label

TEMPLATE = """DEFAULT a
TIMEOUT 0
LABEL a
  LINUX /a/Image.lz4
  APPEND root=PARTLABEL=rgds-root-a new
LABEL b
  LINUX /b/Image.lz4
  APPEND root=PARTLABEL=rgds-root-b new
"""


class ReplaceLabelTests(unittest.TestCase):
    def test_replaces_one_slots_block_and_keeps_the_rest(self):
        conf = "DEFAULT a\nFALLBACK b\nLABEL a\n  LINUX /a/Image\n  APPEND old-a\nLABEL b\n  LINUX /b/Image\n  APPEND old-b\n"
        out = replace_label(conf, "b", TEMPLATE)
        self.assertIn("APPEND old-a", out)
        self.assertIn("APPEND root=PARTLABEL=rgds-root-b new", out)
        self.assertNotIn("old-b", out)
        self.assertTrue(out.startswith("DEFAULT a\nFALLBACK b\n"))

    def test_adds_a_missing_block(self):
        out = replace_label("DEFAULT a\nLABEL a\n  LINUX /a/Image\n", "b", TEMPLATE)
        self.assertIn("LABEL b\n  LINUX /b/Image.lz4", out)


UNIVERSAL = """DEFAULT a
LABEL a
  LINUX /a/Image.lz4
  FDTDIR /a/
  APPEND root=PARTLABEL=rgds-root-a rgds.detected=${rgds_board}
LABEL b
  LINUX /b/Image.lz4
  FDTDIR /b/
  APPEND root=PARTLABEL=rgds-root-b rgds.detected=${rgds_board}
"""


class FitLabelTests(unittest.TestCase):
    RG_DS = b"anbernic,rg-ds\0rockchip,rk3568\0"
    PLUS = b"anbernic,rg-ds-plus\0rockchip,rk3568\0"

    def test_keeps_fdtdir_when_u_boot_picked_this_handheld(self):
        self.assertEqual(fit_label(UNIVERSAL, "ro rgds.detected=rgds rgds.adc=1061000", self.RG_DS), UNIVERSAL)
        self.assertEqual(fit_label(UNIVERSAL, "rgds.detected=rgds-plus rgds.adc=1770000", self.PLUS), UNIVERSAL)

    def test_names_the_tree_under_a_u_boot_that_picks_none(self):
        # an older U-Boot sets no $fdtfile: FDTDIR would load the Quartz64's default, which is not on the card
        out = fit_label(UNIVERSAL, "ro rgds.detected= rgds.adc= panic=10", self.RG_DS)
        self.assertIn("  FDT /a/rk3568-anbernic-rg-ds.dtb\n", out)
        self.assertIn("  FDT /b/rk3568-anbernic-rg-ds.dtb\n", out)
        self.assertNotIn("FDTDIR", out)
        self.assertNotIn("FDTDIR", fit_label(UNIVERSAL, "ro panic=10", self.RG_DS))

    def test_names_the_plus_tree_when_the_voltage_picked_wrong(self):
        out = fit_label(UNIVERSAL, "rgds.detected=rgds rgds.adc=1061000", self.PLUS)
        self.assertIn("  FDT /b/rk3568-anbernic-rg-ds-plus.dtb\n", out)
        self.assertNotIn("FDTDIR", out)

    def test_a_label_that_names_its_tree_is_left_alone(self):
        named = UNIVERSAL.replace("FDTDIR /a/", "FDT /a/rk3568-anbernic-rg-ds-plus.dtb")
        self.assertIn("FDT /a/rk3568-anbernic-rg-ds-plus.dtb", fit_label(named, "", self.RG_DS))


def make_release(folder, build):
    payload = {
        "rootfs.squashfs": os.urandom(3000),
        "Image.lz4": b"kernel" * 100,
        os_update.DTBS[0]: b"dtb",
        os_update.DTBS[1]: b"dtb-plus",
        "extlinux.conf": TEMPLATE.encode()
    }
    import hashlib
    files = {n: hashlib.sha256(b).hexdigest() for n, b in payload.items()}
    archive = folder / "release.zip"
    with zipfile.ZipFile(archive, "w") as z:
        for name, data in payload.items():
            z.writestr(f"{build}/{name}", data)
        z.writestr(
            f"{build}/manifest.json", json.dumps(dict(schema=1, build=build, target=os_update.TARGET, files=files))
        )
    with zipfile.ZipFile(archive) as z:
        unpacked = sum(e.file_size for e in z.infolist())
    return archive, payload, unpacked


class StageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.app = root / "releases/0.7.1-abc-os11111111"
        (self.app / "os").mkdir(parents=True)
        guard = Path(__file__).resolve().parents[1] / "packaging/update_guard.py"
        (self.app / "os/update_guard.py").write_bytes(guard.read_bytes())
        self.data = root / "userdata"
        self.data.mkdir()
        self.boot = root / "boot"
        (self.boot / "extlinux").mkdir(parents=True)
        (self.boot / "extlinux/extlinux.conf").write_text("DEFAULT a\nLABEL a\n  LINUX /a/Image.lz4\n  APPEND old-a\n")
        self.run = root / "run"
        self.run.mkdir()
        (self.run / "slot").write_text("a\n")
        (self.run / "disk").write_text("/dev/mmcblk1\n")
        self.device = root / "mmcblk1p3"
        self.device.write_bytes(b"\0" * 4096)
        self.trial = root / "trial"
        self.trial.write_text(
            "#!/bin/sh\necho \"$@\" >> \"$(dirname \"$0\")/trial.log\"\n[ \"$1\" = status ] && echo none\nexit 0\n"
        )
        self.trial.chmod(0o755)
        fake = types.ModuleType("emulators")
        import contextlib
        fake.activity_lock = lambda data: contextlib.nullcontext()
        fake.core_running = lambda data: False
        sys.modules["emulators"] = fake

    def tearDown(self):
        sys.modules.pop("emulators", None)
        self.tmp.cleanup()

    def updater(self):
        u = OsUpdater(self.app, self.data, boot=self.boot, run_dir=self.run, trial=self.trial)
        u.guard = u.load_guard()
        u.remount = lambda mode: None
        u.slots = lambda: ("a", "b", self.device)
        u.partition_size = lambda device: 4096
        u.restarted = False

        def restart():
            u.restarted = True

        u.restart = restart
        return u

    def test_the_launcher_entry_point_runs(self):
        # device_services.py calls Updater.run(action); an attribute named `run` once shadowed it, so every update
        # action on RG DS OS died with "'PosixPath' object is not callable"
        u = OsUpdater(self.app, self.data, boot=self.boot, run_dir=self.run, trial=self.trial)
        u.run('info')
        self.assertIn('configured=0', (self.data / 'update/status.ini').read_text())

    def test_the_os_feed_is_used_whichever_trust_file_wins(self):
        trust = dict(schema=1, channel='stable', feed='https://example.org/latest/download/stable.json', keys={})
        (self.app / 'os/update-trust.json').write_text(json.dumps(trust))
        u = OsUpdater(self.app, self.data, boot=self.boot, run_dir=self.run, trial=self.trial)
        self.assertEqual(u.config()['feed'], 'https://example.org/latest/download/stable-os.json')
        # a trust file in state/ (a card set up from a stock rgds/ folder) names the application's feed
        (Path(self.tmp.name) / 'state').mkdir()
        (Path(self.tmp.name) / 'state/update-trust.json').write_text(json.dumps(dict(trust, channel='testing')))
        self.assertEqual(
            (u.config()['feed'], u.config()['channel']),
            ('https://example.org/latest/download/stable-os.json', 'testing')
        )
        (Path(self.tmp.name) /
         'state/update-trust.json').write_text(json.dumps(dict(trust, feed=trust['feed'][:-5] + '-os.json')))
        self.assertEqual(u.config()['feed'], 'https://example.org/latest/download/stable-os.json')

    def test_stage_writes_the_other_slot_and_records_it(self):
        build = "0.7.1-abc-os22222222"
        archive, payload, unpacked = make_release(Path(self.tmp.name), build)
        u = self.updater()
        u.stage(archive, dict(build=build, unpacked_size=unpacked, sequence=5, version="0.7.1"))
        self.assertEqual(self.device.read_bytes()[:3000], payload["rootfs.squashfs"])
        self.assertEqual((self.boot / "b/Image.lz4").read_bytes(), payload["Image.lz4"])
        self.assertEqual((self.boot / "b" / os_update.DTBS[0]).read_bytes(), b"dtb")
        self.assertEqual((self.boot / "b" / os_update.DTBS[1]).read_bytes(), b"dtb-plus")
        conf = (self.boot / "extlinux/extlinux.conf").read_text()
        self.assertTrue(conf.startswith("DEFAULT a\n"))
        self.assertIn("APPEND old-a", conf)
        self.assertIn("root=PARTLABEL=rgds-root-b new", conf)
        pending = json.loads((Path(self.tmp.name) / "state/update-pending.json").read_text())
        self.assertEqual(
            (pending["phase"], pending["slot"], pending["running"], pending["previous"]),
            ("staged", "b", "a", self.app.name)
        )

    def test_a_tampered_payload_is_refused(self):
        build = "0.7.1-abc-os22222222"
        archive, payload, unpacked = make_release(Path(self.tmp.name), build)
        with zipfile.ZipFile(archive, "a") as z:
            z.writestr(f"{build}/extra", b"x")
        u = self.updater()
        with self.assertRaises(ValueError):
            u.stage(archive, dict(build=build, unpacked_size=unpacked + 1, sequence=5, version="0.7.1"))
        self.assertEqual(self.device.read_bytes(), b"\0" * 4096)

    def test_apply_arms_the_trial_and_restarts(self):
        u = self.updater()
        u.power_check = lambda: None
        u.setup = lambda: None
        u.guard.save(
            Path(self.tmp.name) / "state/update-pending.json",
            dict(
                phase="staged",
                previous=self.app.name,
                slot="b",
                running="a",
                build="0.7.1-abc-os22222222",
                version="0.7.1"
            )
        )
        u.apply()
        self.assertIn("arm b a 0.7.1-abc-os22222222 0.7.1", (Path(self.tmp.name) / "trial.log").read_text())
        self.assertEqual(json.loads((Path(self.tmp.name) / "state/update-pending.json").read_text())["phase"], "trial")
        self.assertTrue(u.restarted)

    def test_commit_after_a_fallback_tells_the_launcher(self):
        u = self.updater()
        u.guard.save(
            Path(self.tmp.name) / "state/update-pending.json", dict(phase="trial", build="0.7.1-abc-os22222222")
        )
        u.guard.save(
            Path(self.tmp.name) / "state/update-result.json", dict(state="rolled-back", build="0.7.1-abc-os22222222")
        )
        u.commit_trial()
        from fields import read_fields
        status = read_fields(self.data / "update/status.ini")
        self.assertEqual(status["state"], "rolled-back")
        self.assertFalse((Path(self.tmp.name) / "state/update-pending.json").exists())


if __name__ == "__main__":
    unittest.main()
