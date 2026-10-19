"""Fast startup still validates owned code and defers only engine hashing."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import activate


class StartupIntegrityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.app = Path(temporary.name) / "test-build"
        header = bytearray(20);header[:6] = b"\x7fELF\x02\x01";header[18:20] = b"\xb7\x00"
        self.payload = {
            "bin/rgds-launcher": bytes(header), "runtime/session.py": b"owned startup",
            "themes/default/light.ini": b"palette", "assets/fonts/rounded-20.rgf": b"font",
            "emulators/hooks/librgdsbridge.so": b"owned integration",
            "emulators/dsperate/dsperate": b"external DSperate",
            "emulators/dsperate/source.tar.gz": b"corresponding engine source",
            "emulators/dsperate/rgds-build/tools/build_engines.py": b"host rebuild tools",
            "emulators/nnddss/nnddss": b"external NNDS",
        }
        for name, content in self.payload.items():
            path = self.app / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_bytes(content)
        self.manifest = dict(schema=1, project="rgds", target="aarch64-linux-gnu", build=self.app.name,
                             files={name:hashlib.sha256(content).hexdigest() for name, content in self.payload.items()})
        self.save()

    def save(self): (self.app / "manifest.json").write_text(json.dumps(self.manifest))

    def test_engine_hash_is_deferred_and_verified_before_selected_engine_launch(self):
        (self.app / "emulators/nnddss/nnddss").write_bytes(b"damaged engine")
        activate.verify_startup(self.app)
        activate.verify_engine(self.app, "dsperate")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            activate.verify_engine(self.app, "nnddss")
        with self.assertRaisesRegex(ValueError, "SHA-256"): activate.verify_release(self.app)

    def test_startup_code_font_and_palette_corruption_never_reaches_ui(self):
        for name in ("runtime/session.py", "assets/fonts/rounded-20.rgf", "themes/default/light.ini"):
            with self.subTest(name=name):
                (self.app / name).write_bytes(b"damaged")
                with self.assertRaisesRegex(ValueError, "SHA-256"): activate.verify_startup(self.app)
                (self.app / name).write_bytes(self.payload[name])

    def test_shared_bridge_is_checked_for_every_engine(self):
        (self.app / "emulators/hooks/librgdsbridge.so").write_bytes(b"damaged bridge")
        for core in ("drastic", "dsperate", "nnddss"):
            with self.subTest(core=core), self.assertRaisesRegex(ValueError, "SHA-256"):
                activate.verify_engine(self.app, core)

    def test_optional_backend_hash_is_only_required_on_its_selected_launch(self):
        for name in ('emulators/superdrastic/libsuperdrastic.so', 'emulators/superdrastic/source.tar.gz', 'emulators/superdrastic/sdl/libSDL2-2.0.so.0', 'emulators/superdrastic/sdl/source.tar.gz'):
            path = self.app / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'checked')
            self.manifest['files'][name] = hashlib.sha256(b'checked').hexdigest()
        self.save()
        (self.app / 'emulators/superdrastic/source.tar.gz').write_bytes(b'changed source')
        (self.app / 'emulators/superdrastic/sdl/source.tar.gz').write_bytes(b'changed SDL source')
        activate.verify_engine(self.app, 'drastic', backend='superdrastic')
        sdl = self.app / 'emulators/superdrastic/sdl/libSDL2-2.0.so.0'
        previous = sdl.read_bytes(); sdl.write_bytes(b'changed runtime')
        with self.assertRaises(ValueError):activate.verify_engine(self.app, 'drastic', backend='superdrastic')
        sdl.write_bytes(previous)
        (self.app / 'emulators/superdrastic/libsuperdrastic.so').write_bytes(b'changed backend')
        activate.verify_startup(self.app)
        activate.verify_engine(self.app, 'dsperate')
        activate.verify_engine(self.app, 'drastic')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            activate.verify_engine(self.app, 'drastic', backend='superdrastic')

    def test_engine_launch_defers_corresponding_source_but_install_verifies_it(self):
        for name in ("emulators/dsperate/source.tar.gz", "emulators/dsperate/rgds-build/tools/build_engines.py"):
            (self.app / name).write_bytes(b"changed source")
        activate.verify_engine(self.app, "dsperate")
        with self.assertRaisesRegex(ValueError, "SHA-256"): activate.verify_release(self.app)

    def test_deferred_hashes_still_require_exact_inventory_and_present_safe_files(self):
        extra = self.app / "emulators/unlisted.py";extra.write_text("foreign helper")
        with self.assertRaisesRegex(ValueError, "Unlisted"): activate.verify_startup(self.app)
        extra.unlink()
        engine = self.app / "emulators/nnddss/nnddss";engine.unlink()
        with self.assertRaisesRegex(ValueError, "Missing or unsafe"): activate.verify_startup(self.app)
        engine.symlink_to(self.app / "runtime/session.py")
        with self.assertRaisesRegex(ValueError, "Missing or unsafe"): activate.verify_startup(self.app)

    def test_malformed_manifest_is_rejected_as_validation_error(self):
        for files in ({"bin/rgds-launcher": None}, {"bin/rgds-launcher": "x"*64}, {"bin/rgds-launcher": "0"*64, "./runtime/session.py": "0"*64}):
            self.manifest["files"] = files;self.save()
            with self.assertRaises(ValueError): activate.verify_startup(self.app)
        (self.app / "manifest.json").write_text("[]")
        with self.assertRaises(ValueError): activate.verify_startup(self.app)

    def test_same_boot_receipt_reuses_hashes_only_for_unchanged_inventory(self):
        with patch("activate.hashlib.sha256", wraps=hashlib.sha256) as hashes:
            receipt = activate.verify_startup(self.app, receipt=True)
            count = hashes.call_count
            activate.verify_startup_receipt(self.app, receipt)
            self.assertEqual(hashes.call_count, count)
            self.assertEqual(count, 4)
        for name in ("runtime/session.py", "manifest.json"):
            with self.subTest(name=name):
                receipt = activate.verify_startup(self.app, receipt=True)
                path = self.app / name;before = path.read_bytes();info = path.stat()
                path.write_bytes(bytes((before[0] ^ 1,)) + before[1:])
                os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
                with self.assertRaisesRegex(ValueError, "changed"):
                    activate.verify_startup_receipt(self.app, receipt)
                path.write_bytes(before)
        # Engine payloads are no longer stat'ed on the boot path; their bytes
        # are checked by the selected engine's launch verification instead.
        receipt = activate.verify_startup(self.app, receipt=True)
        engine = self.app / "emulators/nnddss/nnddss";before = engine.read_bytes()
        engine.write_bytes(bytes((before[0] ^ 1,)) + before[1:])
        activate.verify_startup_receipt(self.app, receipt)
        with self.assertRaisesRegex(ValueError, "SHA-256"):activate.verify_engine(self.app, "nnddss")
        engine.write_bytes(before)

    def installed(self):
        """Move the fixture into an installed <root>/releases/<build> layout."""
        root = self.app.parent / "rgds";(root / "releases").mkdir(parents=True);(root / "userdata").mkdir()
        self.app = self.app.rename(root / "releases" / self.app.name)
        return root

    def test_installed_release_memoizes_unchanged_hashes_across_processes(self):
        root = self.installed()
        activate.verify_startup(self.app);activate.verify_engine(self.app, "dsperate")
        memo = root / "userdata/cache/verified" / (self.app.name + ".json")
        self.assertTrue(memo.is_file())
        with patch("activate.hashlib.sha256", wraps=hashlib.sha256) as hashes:
            activate.verify_startup(self.app);activate.verify_engine(self.app, "dsperate")
        # Only the two manifest digests; no payload was read again.
        self.assertEqual(hashes.call_count, 2)
        # A changed size/time is re-hashed and a mismatch is rejected.
        (self.app / "runtime/session.py").write_bytes(b"damaged startup")
        with self.assertRaisesRegex(ValueError, "SHA-256"):activate.verify_startup(self.app)
        self.assertFalse(memo.exists())
        (self.app / "runtime/session.py").write_bytes(self.payload["runtime/session.py"])
        activate.verify_startup(self.app)
        # A manifest change invalidates every memoized entry.
        self.manifest["files"]["emulators/nnddss/nnddss"] = hashlib.sha256(b"new engine").hexdigest()
        (self.app / "emulators/nnddss/nnddss").write_bytes(b"new engine");self.save()
        with patch("activate.hashlib.sha256", wraps=hashlib.sha256) as hashes:
            activate.verify_startup(self.app)
        self.assertEqual(hashes.call_count, 5)

    def test_audit_rehashes_memoized_files_and_drops_a_silently_corrupted_memo(self):
        root = self.installed()
        activate.verify_startup(self.app)
        self.assertEqual(activate.audit(self.app, clock=lambda: 10**10)["state"], "verified")
        self.assertEqual(activate.audit(self.app, clock=lambda: 10**10 + 60)["state"], "skipped")
        path = self.app / "themes/default/light.ini";info = path.stat()
        path.write_bytes(b"palettf")  # Same size; restore the old modification time.
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        result = activate.audit(self.app, force=True)
        self.assertEqual(result["state"], "failed")
        self.assertFalse((root / "userdata/cache/verified" / (self.app.name + ".json")).exists())
        with self.assertRaisesRegex(ValueError, "SHA-256"):activate.verify_startup(self.app)

    def test_uninstalled_fixture_never_writes_a_memo_outside_its_tree(self):
        activate.verify_startup(self.app)
        self.assertIsNone(activate.verify_cache_path(self.app))
        self.assertFalse((self.app.parent.parent / "userdata").exists() and (self.app.parent.parent / "userdata/cache/verified").exists())

    def test_receipt_rejects_a_different_release_and_new_payload(self):
        receipt = activate.verify_startup(self.app, receipt=True)
        with self.assertRaisesRegex(ValueError, "Invalid"):
            activate.verify_startup_receipt(self.app.with_name("another-build"), receipt)
        (self.app / "emulators/foreign").write_text("unlisted")
        with self.assertRaisesRegex(ValueError, "changed"):
            activate.verify_startup_receipt(self.app, receipt)

    def test_inventory_is_one_walk_without_resolving_each_payload(self):
        with patch.object(Path, "resolve", side_effect=AssertionError("per-file resolve")):
            activate.verify_startup(self.app)
        source = self.app / "emulators/nnddss"
        destination = self.app.parent / "external-engine";source.rename(destination)
        source.symlink_to(destination, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            activate.verify_startup(self.app)


if __name__ == "__main__": unittest.main()
