"""Stock APPS startup coverage with temporary cards and harmless Python sessions."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PackagingEntryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rgds-card ")
        self.card = Path(self.temporary.name) / "card with spaces; $literal"
        self.root = self.card / "rgds"
        self.apps = self.card / "Roms/APPS"
        self.apps.mkdir(parents=True)
        (self.root / "state").mkdir(parents=True)
        self.session = self.root / "releases/working/runtime/session.py"
        self.session.parent.mkdir(parents=True)
        self.session.write_text(
            "import json, pathlib, sys\n"
            "root = pathlib.Path(sys.argv[sys.argv.index('--root') + 1])\n"
            "(root / 'userdata/arguments.json').write_text(json.dumps(sys.argv[1:]))\n"
            "print('fixture session started')\n"
        )
        (self.root / "state/active").write_text("working\n")
        shutil.copy2(ROOT / "packaging/main.sh", self.root / "main.sh")
        self.entry = self.apps / "RG DS Core.sh"
        self.control = Path(self.temporary.name) / "boot-control"
        self.control.mkdir()
        self.write_entry("RG DS Core.sh")

    def write_entry(self, name):
        # Redirect only the system config path; exercise the actual shell entry.
        source = (ROOT / "packaging" / name).read_text()
        self.entry.write_text(source.replace('/userdata/rgds-boot', str(self.control)))

    def tearDown(self):
        self.temporary.cleanup()

    def run_entry(self, *arguments, environment=None):
        return subprocess.run(
            ["/bin/sh", str(self.entry), *arguments],
            cwd=self.temporary.name,
            env=environment,
            text=True,
            capture_output=True,
            timeout=10,
        )

    def test_entry_uses_its_card_and_preserves_arguments(self):
        result = self.run_entry("--dev", "--rom", "game name; $literal.nds")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = json.loads((self.root / "userdata/arguments.json").read_text())
        self.assertEqual(Path(arguments[1]).resolve(), self.root.resolve())
        self.assertEqual([arguments[0], *arguments[2:]], ["--root", "--dev", "--rom", "game name; $literal.nds"])
        self.assertIn("fixture session started", (self.root / "userdata/launch.log").read_text())

    def test_configured_installation_is_independent_of_automatic_startup(self):
        configured = Path(self.temporary.name) / "internal install" / "rgds"
        shutil.copytree(self.root, configured)
        for entry in ("RG DS Core.sh", "RGDS Probe.sh"):
            self.write_entry(entry)
            for mode, enabled in (("rgds", True), ("stock", False), ("rgds", False)):
                with self.subTest(entry=entry, mode=mode, enabled=enabled):
                    (configured / "userdata/arguments.json").unlink(missing_ok=True)
                    (self.root / "userdata/arguments.json").unlink(missing_ok=True)
                    (self.control / "config.json").write_text(json.dumps(
                        dict(schema=1, root=str(configured), mode=mode)))
                    marker = self.control / "enabled"
                    if enabled:
                        marker.touch()
                    else:
                        marker.unlink(missing_ok=True)
                    result = self.run_entry("--dev")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    arguments = json.loads((configured / "userdata/arguments.json").read_text())
                    self.assertEqual(Path(arguments[1]).resolve(), configured.resolve())
                    self.assertEqual(arguments[2:], ["--probe"] if entry == "RGDS Probe.sh" else ["--dev"])
                    self.assertFalse((self.root / "userdata/arguments.json").exists())

    def test_invalid_or_removed_configuration_falls_back_to_own_card(self):
        for entry in ("RG DS Core.sh", "RGDS Probe.sh"):
            self.write_entry(entry)
            for config in ("{broken", "[]", json.dumps(dict(mode="stock", root="relative")),
                           json.dumps(dict(mode="stock", root=str(self.control / "missing")))):
                with self.subTest(entry=entry, config=config):
                    (self.control / "config.json").write_text(config)
                    result = self.run_entry()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertTrue((self.root / "userdata/arguments.json").is_file())
                    self.assertNotIn("Traceback", result.stderr)
                    (self.root / "userdata/arguments.json").unlink()

    def test_recovery_marker_prevents_startup_and_writes(self):
        data = self.root / "userdata"
        data.mkdir()
        (data / "boot_stock").touch()
        result = self.run_entry()
        self.assertEqual(result.returncode, 0)
        self.assertIn("disabled", result.stdout)
        self.assertFalse((data / "launch.log").exists())
        self.assertFalse((data / "arguments.json").exists())

    def test_missing_active_pointer_is_reported_in_persistent_log(self):
        (self.root / "state/active").unlink()
        result = self.run_entry()
        self.assertEqual(result.returncode, 1)
        self.assertIn("no readable active build", result.stderr)
        self.assertIn("no readable active build", (self.root / "userdata/launch.log").read_text())

    def test_invalid_active_pointer_never_starts_a_session(self):
        (self.root / "state/active").write_text("../../outside\n")
        result = self.run_entry()
        self.assertEqual(result.returncode, 1)
        self.assertIn("build ID is invalid", result.stderr)
        self.assertFalse((self.root / "userdata/arguments.json").exists())

    def test_incomplete_release_is_reported(self):
        self.session.unlink()
        result = self.run_entry()
        self.assertEqual(result.returncode, 1)
        self.assertIn("active release is incomplete", result.stderr)
        self.assertIn("do not copy only the APPS entry", (self.root / "userdata/launch.log").read_text())

    def test_early_python_failure_is_saved_and_propagated(self):
        self.session.write_text("raise RuntimeError('fixture import failure')\n")
        result = self.run_entry()
        self.assertEqual(result.returncode, 1)
        log = (self.root / "userdata/launch.log").read_text()
        self.assertIn("Traceback", log)
        self.assertIn("fixture import failure", log)

    def test_unsupported_python_is_reported_before_session(self):
        binaries = self.card / "bin"
        binaries.mkdir()
        python = binaries / "python3"
        python.write_text("#!/bin/sh\nexit 1\n")
        python.chmod(0o755)
        environment = os.environ.copy()
        environment["PATH"] = str(binaries) + os.pathsep + environment.get("PATH", "")
        result = self.run_entry(environment=environment)
        self.assertEqual(result.returncode, 1)
        self.assertIn("needs Python 3.8+", result.stderr)
        self.assertFalse((self.root / "userdata/arguments.json").exists())


if __name__ == "__main__":
    unittest.main()
