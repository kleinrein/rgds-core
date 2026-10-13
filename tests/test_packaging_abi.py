"""Package ABI claims come from the actual inspected payloads, not just launcher flags."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.package import component_abi, glibc_floor, payload_source


class PackagingABITests(unittest.TestCase):
    def test_floor_accounts_for_every_bundled_binary(self):
        components = {"bin/rgds-launcher": dict(glibc_minimum="2.17"),
                      "emulators/dsperate/dsperate": dict(glibc_minimum="2.34"),
                      "emulators/nnddss/lib/z.so": dict(glibc_minimum=None)}
        self.assertEqual(glibc_floor(components), "2.34")
        self.assertEqual(glibc_floor({}), "2.17")
        self.assertEqual(glibc_floor({"x": dict(glibc_minimum="2.9")}), "2.17")

    def test_owned_bridge_and_external_cores_have_distinct_provenance(self):
        self.assertEqual(payload_source("emulators/hooks/librgdsbridge.so"), "RGDS source build")
        self.assertIn("Supplied DSperate", payload_source("emulators/dsperate/dsperate"))
        self.assertIn("Supplied NNDDSS", payload_source("emulators/nnddss/lib/runtime.so"))
        record = {"source": {"commit": "pinned-source"}}
        self.assertEqual(payload_source("emulators/dsperate/dsperate", record), "DSperate source build at pinned-source")

    def test_nonelf_assets_are_skipped_and_elf_hash_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory); (app / "image.bmp").write_bytes(b"BM image")
            (app / "bridge.so").write_bytes(b"\x7fELF fixture")
            files = {name: hashlib.sha256((app / name).read_bytes()).hexdigest() for name in ("image.bmp", "bridge.so")}
            metadata = dict(machine="aarch64", glibc_minimum="2.17", sha256=files["bridge.so"])
            with patch("tools.package.inspect_elf", return_value=metadata) as inspect:
                result = component_abi(app, files)
            self.assertEqual(list(result), ["bridge.so"])
            inspect.assert_called_once()
            with patch("tools.package.inspect_elf", return_value=dict(metadata, sha256="changed")):
                with self.assertRaisesRegex(ValueError, "changed during inspection"): component_abi(app, files)

    def test_wrong_architecture_never_makes_a_device_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory); (app / "core").write_bytes(b"\x7fELF fixture")
            with patch("tools.package.inspect_elf", return_value=dict(machine="x86_64")):
                with self.assertRaisesRegex(ValueError, "wrong architecture"): component_abi(app, {"core": "fixture"})


if __name__ == "__main__": unittest.main()
