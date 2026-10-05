"""Readers must not mistake incomplete ownership data for an idle device."""
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
sys.path.insert(0, str(ROOT / "tools"))
import records
import device


class RecordTests(unittest.TestCase):
    def test_transient_empty_record_is_retried(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "core.json"
            path.write_text("")
            def writer():
                time.sleep(0.03)
                records.write_record(path, {"pid": 123, "start": "456"})
            thread = threading.Thread(target=writer)
            thread.start()
            self.assertEqual(records.read_record(path), {"pid": 123, "start": "456"})
            thread.join()

    def test_atomic_writes_leave_no_partial_record_or_temporary_files(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "launcher.json"
            records.write_record(path, {"pid": 123, "start": "456"})
            records.write_record(path, {"pid": 789, "start": "1000"})
            self.assertEqual(records.read_record(path), {"pid": 789, "start": "1000"})
            self.assertEqual([p.name for p in path.parent.iterdir()], ["launcher.json"])

    def test_persistent_empty_record_is_not_reported_idle(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / "userdata/session/core.json"
            path.parent.mkdir(parents=True)
            path.write_text("")
            with self.assertRaisesRegex(ValueError, "Unreadable activity"):
                records.read_record(path, attempts=1)
            # Execute the exact remote reader against fixture paths, with a fake
            # platform and lock path. No process records or files are modified.
            prefix = """import platform,sys,pathlib
sys.platform = 'linux'
platform.machine = lambda: 'aarch64'
real_Path = pathlib.Path
pathlib.Path = lambda *p: real_Path(sys.argv[1])/'no-lock' if p == ('/tmp/rgds-session.lock',) else real_Path(*p)
"""
            import subprocess
            result = subprocess.run([sys.executable, "-c", prefix+device.REMOTE_STATUS, str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            value = json.loads(result.stdout)
            self.assertTrue(value["game"])
            self.assertEqual(value["record_errors"], [str(path)])


if __name__ == "__main__": unittest.main()
