"""Persistent record atomicity and stable identity for unchanged records."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from library import read_fields, write_fields


class RecordWriteTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.path = self.root / "preferences.ini"

    def test_identical_preferences_retain_inode_and_timestamp(self):
        self.assertTrue(write_fields(self.path, dict(volume=15)))
        identity = self.path.stat()
        self.assertFalse(write_fields(self.path, dict(volume=15)))
        self.assertEqual(self.path.stat().st_ino, identity.st_ino)
        self.assertEqual(self.path.stat().st_mtime_ns, identity.st_mtime_ns)

    def test_failed_durable_write_preserves_previous_record(self):
        write_fields(self.path, dict(volume=15))
        with patch("library.os.fsync", side_effect=OSError("flush failed")):
            with self.assertRaises(OSError):
                write_fields(self.path, dict(volume=80))
        self.assertEqual(read_fields(self.path), dict(volume="15"))
        self.assertEqual(list(self.root.glob(".pending-*")), [])

    def test_transient_status_replacement_is_complete_and_needs_no_disk_flush(self):
        write_fields(self.path, dict(state="paused"))
        with patch("library.os.fsync", side_effect=OSError("no durable disk")):
            self.assertTrue(write_fields(self.path, dict(state="busy", message="Saving…"), durable=False))
        self.assertEqual(read_fields(self.path), dict(state="busy", message="Saving…"))
        self.assertEqual(list(self.root.glob(".pending-*")), [])

    def test_fields_cannot_create_injected_records(self):
        write_fields(self.path, dict(message="first\npassword=bad\rnext"))
        self.assertEqual(read_fields(self.path), dict(message="first password=bad next"))


if __name__ == "__main__": unittest.main()
