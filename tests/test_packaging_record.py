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
            "build/device/rgds-launcher", "build/device/librgdsbridge.so", "assets/emulator-profiles/drastic.cfg",
            "assets/emulator-profiles/dsperate.ini", "runtime/inventory.sh", "build/engines/dsperate/dsperate",
            "tools/build_engines.py", "tools/elf_abi.py", "THIRD_PARTY.md", "VERSION", "packaging/main.sh",
            "packaging/update_guard.py", "packaging/update-trust.json", "README.md", "docs/DEVICE_SETUP.md",
            "docs/DEVELOPMENT.md", "docs/SCREENSCRAPER.md", "docs/PERFORMANCE.md", "docs/BOOT.md"
        )
        for name in (*names, *package.DOCUMENTS):
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
        self.engine_record = dict(
            source=dict(commit="pinned-source", patches=[]),
            files=dict(dsperate=package.sha(self.root / "build/engines/dsperate/dsperate")),
            build_driver_sha256=package.sha(self.root / "tools/build_engines.py")
        )
        (self.root / "build/device/build.json").write_text(json.dumps(self.rgds_record))
        (self.root / "build/engines/dsperate/build.json").write_text(json.dumps(self.engine_record))
        dist = self.root / "dist"
        dist.mkdir()
        (dist / "latest.txt").write_text("previous.zip\n")
        (dist / "previous.zip").write_bytes(b"last working archive")
        melon = self.root / 'build/melonds-experiment'
        melon.mkdir(parents=True)
        for name in ('melonds', 'source.tar.gz', 'COPYING'):
            (melon / name).write_bytes(b'fixture ' + name.encode())
        self.melon_record = dict(
            engine=dict(sha256=package.sha(melon / 'melonds')),
            source=dict(archive_sha256=package.sha(melon / 'source.tar.gz'))
        )
        (melon / 'build.json').write_text(json.dumps(self.melon_record))
        play = self.root / 'build/play/device/payload'
        (play / 'profiles/gba').mkdir(parents=True)
        for name in ('rgds-play', 'source.tar.gz', 'COPYING', 'COPYING.mgba', 'profiles/gba/AMKE.ini'):
            (play / name).write_bytes(b'fixture ' + name.encode())
        self.play_record = dict(
            files={p.relative_to(play).as_posix(): package.sha(p)
                   for p in play.rglob('*') if p.is_file()}
        )
        (play / 'build.json').write_text(json.dumps(self.play_record))
        backend = self.root / 'build/superdrastic'
        backend.mkdir(parents=True)
        for name in ('libsuperdrastic.so', 'source.tar.gz', 'LICENSE', 'THIRD_PARTY.md', 'VERSION'):
            (backend / name).write_bytes(b'fixture ' + name.encode())
        self.super_record = dict(files={p.name: package.sha(p) for p in backend.iterdir()})
        (backend / 'build.json').write_text(json.dumps(self.super_record))
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
        patch.object(package, "validate_engine_build", return_value=self.engine_record).start()
        patch.object(package, "validate_experimental", return_value=self.experimental_record).start()
        patch.object(package, 'validate_vulkan', return_value=self.vulkan_record).start()
        patch.object(package, "validate_melonds", return_value=self.melon_record).start()
        patch.object(package, "validate_play", return_value=self.play_record).start()
        patch.object(package, "validate_superdrastic", return_value=self.super_record).start()
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

    def test_engine_rebuild_between_binary_and_record_copy_cannot_publish(self):
        with patch.object(
            package,
            "put",
            side_effect=self.rewritten_record("build/engines/dsperate/dsperate", "build/engines/dsperate/build.json")
        ):
            with self.assertRaisesRegex(ValueError, "record changed while packaging"):
                package._package()
        self.assert_previous_archive_preserved()

    def test_melonds_rebuild_during_copy_cannot_publish(self):
        with patch.object(
            package,
            "put",
            side_effect=self.rewritten_record(
                "build/melonds-experiment/melonds", "build/melonds-experiment/build.json"
            )
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

    def test_superdrastic_rebuild_during_copy_cannot_publish(self):
        with patch.object(
            package,
            'put',
            side_effect=self.rewritten_record('build/superdrastic/libsuperdrastic.so', 'build/superdrastic/build.json')
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

    def test_edited_driver_cannot_be_presented_as_the_original_build_driver(self):
        (self.root / "tools/build_engines.py").write_bytes(b"new driver after build")
        with self.assertRaisesRegex(ValueError, "build driver changed since compilation"):
            package._package()
        self.assert_previous_archive_preserved()

    def test_edited_portability_patch_cannot_be_presented_as_original(self):
        portability = self.root / "tools/engine_patches/portability.patch"
        portability.parent.mkdir()
        portability.write_bytes(b"original portability patch")
        self.engine_record["source"]["patches"] = [dict(name=portability.name, sha256=package.sha(portability))]
        (self.root / "build/engines/dsperate/build.json").write_text(json.dumps(self.engine_record))
        portability.write_bytes(b"edited portability patch")
        with self.assertRaisesRegex(ValueError, "portability patch changed since compilation"):
            package._package()
        self.assert_previous_archive_preserved()

    def test_stable_inputs_publish_records_matching_the_checked_binary_bytes(self):
        archive = package._package()
        with zipfile.ZipFile(archive) as release:
            path = next(name for name in release.namelist() if name.endswith("/build/rgds.json"))
            for document in package.DOCUMENTS:
                self.assertIn('rgds/docs/' + document, release.namelist())
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
            engine_path = next(name for name in release.namelist() if name.endswith("/emulators/dsperate/build.json"))
            self.assertEqual(json.loads(release.read(engine_path)), self.engine_record)


if __name__ == "__main__": unittest.main()
