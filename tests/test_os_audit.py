"""OS evidence cannot imply hardware acceptance or mutate an existing baseline."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import os_audit
import os_probe


class OsAuditTests(unittest.TestCase):
    def test_probe_reads_fixture_without_changing_files_or_claiming_acceptance(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            facts = {
                "proc/device-tree/model": "Rockchip RK3568 Board\0",
                "proc/device-tree/compatible": "rockchip,rk3568\0",
                "proc/meminfo": "MemTotal:        2990000 kB\n",
                "sys/class/drm/card0-DSI-1/status": "connected\n",
                "sys/power/state": "freeze mem\n",
                "etc/os-release": 'ID=buildroot\nPASSWORD=private\n'
            }
            for name, value in facts.items():
                path = base / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            before = {str(p): p.read_bytes() for p in base.rglob("*") if p.is_file()}
            result = os_probe.collect(base=base)
            self.assertEqual(result["hardware"]["memory_kib"], 2990000)
            self.assertEqual(result["hardware"]["compatible"], ["rockchip,rk3568"])
            self.assertEqual(result["system"]["os_release"], {"ID": "buildroot"})
            self.assertTrue(result["read_only"])
            self.assertTrue(all(value == "not tested" for value in result["acceptance"].values()))
            self.assertEqual(before, {str(p): p.read_bytes() for p in base.rglob("*") if p.is_file()})

    def test_mount_password_options_are_not_captured(self):
        report = os_probe.mounts(
            "//server/share /mnt/share cifs rw,password=private,user=alice 0 0\n"
            "/dev/mmcblk1p4 /mnt/mmc vfat ro,relatime 0 0\n"
        )
        self.assertTrue(report[0]["writable"])
        self.assertFalse(report[1]["writable"])
        self.assertNotIn("private", json.dumps(report))
        self.assertNotIn("alice", json.dumps(report))

    def test_invalid_active_pointer_never_traverses_release_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "state").mkdir()
            (root / "state/active").write_text("../../other\n")
            result = os_probe.installation(root)
            self.assertIn("error", result)
            self.assertNotIn("manifest", result)
            self.assertNotIn("session_present", result)

    def test_file_hash_refuses_special_nodes_and_oversized_files(self):
        with tempfile.TemporaryDirectory() as folder:
            huge = Path(folder) / "huge"
            with huge.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024 + 1)
            self.assertIn("error", os_probe.file_record(huge))
            self.assertIn("error", os_probe.file_record(Path(folder)))

    def test_ssh_uses_verified_key_and_python_stdin_without_upload(self):
        expected = {"schema": 1, "read_only": True, "system": {"machine": "aarch64"}}
        with patch.object(
            os_audit.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout=json.dumps(expected))
        ) as run:
            self.assertEqual(os_audit.probe("root@device", [Path("/userdata/rgds's test")]), expected)
        command = run.call_args.args[0]
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertIn("BatchMode=yes", command)
        self.assertTrue(command[-1].startswith("python3 -B - --install-root "))
        self.assertEqual(run.call_args.kwargs["input"], (ROOT / "tools/os_probe.py").read_text())
        self.assertEqual(run.call_args.kwargs["timeout"], 60)

    def test_invalid_target_and_root_never_contact_device(self):
        with patch.object(os_audit.subprocess, "run") as run:
            for target, roots in (
                ("-oProxyCommand=bad", []), ("root@device", [Path("relative")]),
                ("root@device", [Path("/userdata/../etc")])
            ):
                with self.assertRaises(ValueError):
                    os_audit.probe(target, roots)
            run.assert_not_called()

    def test_wrong_architecture_is_not_accepted(self):
        with patch.object(
            os_audit.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                [], 0, stdout=json.dumps({
                    "schema": 1,
                    "read_only": True,
                    "system": {
                        "machine": "x86_64"
                    }
                })
            )
        ):
            with self.assertRaises(ValueError):
                os_audit.probe("root@device", [])

    def test_malformed_report_is_rejected_as_evidence(self):
        for evidence in ([], {"schema": 1, "read_only": True, "system": []}):
            with patch.object(
                os_audit.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout=json.dumps(evidence))
            ):
                with self.assertRaises(ValueError):
                    os_audit.probe("root@device", [])

    def test_failed_ssh_saves_local_evidence_but_returns_failure(self):
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(os_audit, "local_components", return_value={"test": {"needed": ["libc.so.6"]}}), \
                patch.object(os_audit, "probe", side_effect=subprocess.TimeoutExpired("ssh", 60)), \
                patch("sys.stdout", new_callable=io.StringIO):
            output = Path(folder) / "baseline"
            self.assertEqual(os_audit.capture(output, "root@device"), 1)
            data = (output / "audit.json").read_bytes()
            report = json.loads(data)
            self.assertEqual(report["device_capture_status"], "failed")
            self.assertIsNone(report["device"])
            self.assertIn("TimeoutExpired", report["device_error"])
            self.assertEqual((output / "audit.sha256").read_text().split()[0], hashlib.sha256(data).hexdigest())
            with self.assertRaises(FileExistsError):
                os_audit.capture(output)
            self.assertEqual((output / "audit.json").read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
