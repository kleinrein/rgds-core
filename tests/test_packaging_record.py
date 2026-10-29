"""A concurrently rewritten build record cannot enter a checked release ZIP."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import package


class PackagingRecordTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        names = (
            "build/device/rgds-launcher", "build/device/librgdsbridge.so", "assets/emulator-profiles/dsperate.ini",
            "runtime/inventory.sh", "THIRD_PARTY.md", "VERSION", "packaging/update_guard.py",
            "packaging/update-trust.json", "README.md", "docs/DEVELOPMENT.md", "docs/SCREENSCRAPER.md",
            "docs/PERFORMANCE.md"
        )
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture " + name.encode())
        (self.root / "VERSION").write_text("0.0.1-dev\n")
        (self.root / "build/device/rgds-launcher").write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00")
        self.rgds_record = dict(
            schema=1,
            artifacts={
                name: dict(sha256=package.sha(self.root / "build/device" / name))
                for name in ("rgds-launcher", "librgdsbridge.so")
            }
        )
        (self.root / "build/device/build.json").write_text(json.dumps(self.rgds_record))
        dist = self.root / "dist"
        dist.mkdir()
        (dist / "latest.txt").write_text("previous.zip\n")
        (dist / "previous.zip").write_bytes(b"last working archive")
        play = self.root / 'build/play/device/payload'
        (play / 'profiles/gba').mkdir(parents=True)
        for name in ('rgds-play', 'source.tar.gz', 'COPYING', 'COPYING.mgba', 'profiles/gba/AMKE.ini'):
            (play / name).write_bytes(b'fixture ' + name.encode())
        self.play_record = dict(
            files={p.relative_to(play).as_posix(): package.sha(p)
                   for p in play.rglob('*') if p.is_file()}
        )
        (play / 'build.json').write_text(json.dumps(self.play_record))
        experiment = self.root / 'build/engines/rgds-experimental'
        experiment.mkdir(parents=True)
        for name in ('rgds-experimental', 'source.tar.gz', 'LICENSE', 'NOTICE.txt'):
            (experiment / name).write_bytes(b'fixture ' + name.encode())
        self.experimental_record = dict(files={p.name: package.sha(p) for p in experiment.iterdir()})
        (experiment / 'build.json').write_text(json.dumps(self.experimental_record))
        vulkan = self.root / 'build/engines/rgds-vulkan'
        vulkan.mkdir(parents=True)
        self.vulkan_record = dict(files={})
        (vulkan / 'vulkan-build.json').write_text(json.dumps(self.vulkan_record))
        samba = self.root / 'build/samba'
        samba.mkdir(parents=True)
        for name in (
            'bin/smbd', 'bin/smbpasswd', 'bin/smbclient', 'bin/samba-dcerpcd', 'bin/rpcd_classic', 'bin/rpcd_winreg',
            'bin/rpcd_lsad', 'source.tar.gz', 'COPYING'
        ):
            path = samba / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture ' + name.encode())
        self.samba_record = dict(
            files={p.relative_to(samba).as_posix(): package.sha(p)
                   for p in samba.rglob('*') if p.is_file()}
        )
        (samba / 'build.json').write_text(json.dumps(self.samba_record))
        self.original_put = package.put
        self.addCleanup(patch.stopall)
        patch.object(package, "ROOT", self.root).start()
        patch.object(package, "validate_rgds_build", return_value=self.rgds_record).start()
        patch.object(package, "validate_experimental", return_value=self.experimental_record).start()
        patch.object(package, 'validate_vulkan', return_value=self.vulkan_record).start()
        patch.object(package, "validate_play", return_value=self.play_record).start()
        patch.object(package, 'validate_samba', return_value=self.samba_record).start()
        patch.object(package, "validate_bundle", return_value=dict(files={}, components={})).start()
        patch.object(
            package,
            "validate_shader_previews",
            return_value=dict(scenes=[dict(presets=[dict(id=n) for n in range(21)]) for _ in range(3)])
        ).start()
        # ABI checking is independent and has its own real ELF parser tests.
        patch.object(package, "component_abi", return_value={}).start()

    def assert_previous_archive_preserved(self):
        self.assertEqual((self.root / "dist/latest.txt").read_text(), "previous.zip\n")
        self.assertEqual((self.root / "dist/previous.zip").read_bytes(), b"last working archive")
        self.assertEqual({p.name for p in (self.root / "dist").iterdir()}, {"previous.zip", "latest.txt"})

    def rewritten_record(self, trigger, record):
        def copy(source, destination):
            self.original_put(source, destination)
            if source == self.root / trigger:
                path = self.root / record
                changed = json.loads(path.read_text())
                changed["concurrent_build"] = True
                path.write_text(json.dumps(changed))

        return copy

    def test_rgds_rebuild_between_binary_and_record_copy_cannot_publish(self):
        with patch.object(
            package,
            "put",
            side_effect=self.rewritten_record("build/device/librgdsbridge.so", "build/device/build.json")
        ):
            with self.assertRaisesRegex(ValueError, "record changed while packaging"):
                package._package()
        self.assert_previous_archive_preserved()

    def test_experimental_rebuild_during_copy_cannot_publish(self):
        with patch.object(
            package,
            'put',
            side_effect=self.rewritten_record(
                'build/engines/rgds-experimental/rgds-experimental', 'build/engines/rgds-experimental/build.json'
            )
        ):
            with self.assertRaisesRegex(ValueError, 'record changed while packaging'):
                package._package()
        self.assert_previous_archive_preserved()

    def test_samba_rebuild_during_copy_cannot_publish(self):
        with patch.object(
            package, 'put', side_effect=self.rewritten_record('build/samba/bin/smbd', 'build/samba/build.json')
        ):
            with self.assertRaisesRegex(ValueError, 'record changed while packaging'):
                package._package()
        self.assert_previous_archive_preserved()

    def test_stable_inputs_publish_records_matching_the_checked_binary_bytes(self):
        archive = package._package()
        with zipfile.ZipFile(archive) as release:
            path = next(name for name in release.namelist() if name.endswith("/build/rgds.json"))
            # the release alone: no stock card bootstraps (main.sh, Roms/APPS) or documents beside it
            self.assertTrue(all(name.startswith('rgds/releases/') for name in release.namelist()))
            experiment = next(
                name for name in release.namelist() if name.endswith('/emulators/rgds-experimental/rgds-experimental')
            )
            self.assertTrue((release.getinfo(experiment).external_attr >> 16) & 0o111)
            for name in ('smbd', 'smbpasswd', 'smbclient', 'samba-dcerpcd', 'rpcd_classic', 'rpcd_winreg', 'rpcd_lsad'):
                smb = next(n for n in release.namelist() if n.endswith('/services/samba/bin/' + name))
                self.assertTrue((release.getinfo(smb).external_attr >> 16) & 0o111)
            smb_record = next(n for n in release.namelist() if n.endswith('/services/samba/build.json'))
            self.assertEqual(json.loads(release.read(smb_record)), self.samba_record)
            record = json.loads(release.read(path))
            launcher = path.removesuffix("build/rgds.json") + "bin/rgds-launcher"
            self.assertEqual(
                record["artifacts"]["rgds-launcher"]["sha256"],
                hashlib.sha256(release.read(launcher)).hexdigest()
            )
            # DSCore is the only DS engine: no retired engine is packaged
            self.assertFalse([n for n in release.namelist() if '/emulators/dsperate/' in n or '/emulators/melonds/' in n])


if __name__ == "__main__": unittest.main()
