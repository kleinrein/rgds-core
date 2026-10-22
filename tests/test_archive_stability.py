"""Archive mutation during launch cannot publish a mixed cache identity."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from emulators import resolve_rom


class ArchiveStabilityTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.archive = self.root / "game.zip"
        self.data = self.root / "userdata"
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("game.nds", b"valid fixture")
        self.open_zip = zipfile.ZipFile

    def mutate_after_checksum(self, source, *args, **kwargs):
        with self.archive.open("ab") as changed:
            changed.write(b"changed during launch")
        return self.open_zip(source, *args, **kwargs)

    def test_change_between_hash_and_extraction_never_publishes_partial_rom(self):
        with patch("emulators.zipfile.ZipFile", side_effect=self.mutate_after_checksum):
            with self.assertRaisesRegex(ValueError, "archive changed"):
                resolve_rom(self.archive, self.data)
        self.assertEqual(list(self.data.rglob("*.nds")), [])
        self.assertEqual(list(self.data.rglob("*.partial")), [])
        resolved = resolve_rom(self.archive, self.data)
        self.assertEqual(resolved.read_bytes(), b"valid fixture")
        self.assertEqual(resolved.parent.name, hashlib.sha256(self.archive.read_bytes()).hexdigest())

    def test_change_during_warm_cache_lookup_keeps_old_cache_but_requires_retry(self):
        previous = resolve_rom(self.archive, self.data)
        with patch("emulators.zipfile.ZipFile", side_effect=self.mutate_after_checksum):
            with self.assertRaisesRegex(ValueError, "archive changed"):
                resolve_rom(self.archive, self.data)
        self.assertEqual(previous.read_bytes(), b"valid fixture")
        current = resolve_rom(self.archive, self.data)
        self.assertNotEqual(current, previous)
        self.assertEqual(current.read_bytes(), previous.read_bytes())


if __name__ == "__main__": unittest.main()
