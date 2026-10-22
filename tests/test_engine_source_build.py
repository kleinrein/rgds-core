"""Source build provenance rejects stale or altered artifacts without execution."""
import hashlib
import io
import json
from pathlib import Path
import sys
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
from tools import build_engines as build
from test_elf_abi import fixture


class SourceBuildTests(unittest.TestCase):
    def test_publication_failure_restores_complete_last_successful_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, pending = root / "dsperate", root / ".pending"
            output.mkdir()
            pending.mkdir()
            original = {"dsperate": b"last working ELF", "build.json": b"matching old metadata"}
            for name, content in original.items():
                (output / name).write_bytes(content)
                (pending / name).write_bytes(b"new " + content)
            rename = Path.rename

            def fail_publication(path, target):
                if path == pending and target == output:
                    raise OSError("injected final directory rename failure")
                return rename(path, target)

            with patch.object(Path, "rename", fail_publication):
                with self.assertRaisesRegex(OSError, "final directory rename failure"):
                    build.publish_build(pending, output)
            self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()}, original)
            self.assertFalse(output.with_name(output.name + ".previous").exists())
            self.assertTrue(pending.is_dir())

    def test_wrong_commit_and_dirty_source_are_rejected_before_compiling(self):
        with patch.object(build, "git", return_value="other"):
            with self.assertRaisesRegex(ValueError, "pinned commit"):
                build.source_inputs(ROOT / "unused-source")
        with patch.object(build, "git", side_effect=[build.COMMIT, build.TREE, " M source.cpp"]):
            with self.assertRaisesRegex(ValueError, "source has changes"):
                build.source_inputs(ROOT / "unused-source")

    def test_modified_patch_is_rejected_before_source_checkout_changes(self):
        with patch.object(build, "digest", return_value="other"):
            with self.assertRaisesRegex(ValueError, "patch checksum"):
                build.prepare_build_source(ROOT / "unused-source", ROOT / "unused-build", {})

    def archive(self, path, entries):
        with tarfile.open(path, "w:gz") as archive:
            for name, data in entries:
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))

    def test_archive_source_bytes_are_checked_even_when_archive_hash_is_updated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.tar.gz"
            driver = b"build driver"
            source = b"correct source"
            expected = hashlib.sha256(
                json.dumps({
                    "main.cpp": hashlib.sha256(source).hexdigest()
                }, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest()
            record = {"build_driver_sha256": hashlib.sha256(driver).hexdigest(), "source_file_count": 1}
            entries = [("DSperate/main.cpp", source), ("DSperate/tools/build_engines.py", driver)]
            self.archive(path, entries)
            with patch.object(build, "PATCHED_INPUTS_SHA256", expected):
                build.validate_source_archive(path, record)
                self.archive(path, [(entries[0][0], b"modified source"), entries[1]])
                with self.assertRaisesRegex(ValueError, "source fingerprint"):
                    build.validate_source_archive(path, record)

    def test_archive_rejects_paths_and_duplicate_members(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.tar.gz"
            for entries in ([('../outside', b'bad')], [('DSperate/a', b'first'), ('DSperate/a', b'second')]):
                self.archive(path, entries)
                with self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
                    build.validate_source_archive(path, {})

    def record(self, directory, elf):
        for name, content in {
            "dsperate": elf,
            "LICENSE": b"GPL",
            "NOTICE.txt": b"Notice",
            "source.tar.gz": b"source"
        }.items():
            (directory / name).write_bytes(content)
        files = {
            name: build.digest(directory / name)
            for name in ("dsperate", "LICENSE", "NOTICE.txt", "source.tar.gz")
        }
        record = {
            "schema": 1,
            "kind": build.KIND,
            "files": files,
            "source": {
                "commit": build.COMMIT,
                "tree": build.TREE,
                "inputs_sha256": build.SOURCE_INPUTS_SHA256,
                "patched_inputs_sha256": build.PATCHED_INPUTS_SHA256,
                "patches": [{
                    "name": build.PATCH.name,
                    "sha256": build.PATCH_SHA256
                }],
                "archive_sha256": files["source.tar.gz"]
            },
            "binary": build.inspect_elf(directory / "dsperate", "source build")
        }
        (directory / "build.json").write_text(json.dumps(record))
        return record

    def test_packager_rejects_wrong_architecture_and_newer_glibc(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for machine, version in ((62, "GLIBC_2.17"), (183, "GLIBC_2.34")):
                elf, _ = fixture((("libc.so.6", (version,)),), machine=machine)
                self.record(root, elf)
                with self.assertRaisesRegex(ValueError, "ELF metadata mismatch"):
                    build.validate_build(root)

    def test_gcc_pgo_records_may_need_glibc_2_38_but_never_newer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version, accepted in (("GLIBC_2.38", True), ("GLIBC_2.39", False)):
                elf, _ = fixture((("libc.so.6", (version,)),))
                record = self.record(root, elf)
                record["compiler"] = "gcc-pgo"
                (root / "build.json").write_text(json.dumps(record))
                if accepted:
                    with patch.object(build, "validate_source_archive"):
                        build.validate_build(root)
                else:
                    with self.assertRaisesRegex(ValueError, "ELF metadata mismatch"):
                        build.validate_build(root)
            record["compiler"] = "unknown"
            (root / "build.json").write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "ELF metadata mismatch"):
                build.validate_build(root)

    def test_packager_rejects_replaced_binary_and_extra_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf, _ = fixture((("libc.so.6", ("GLIBC_2.17",)),))
            self.record(root, elf)
            (root / "dsperate").write_bytes(b"different binary")
            with self.assertRaisesRegex(ValueError, "artifact hash mismatch"):
                build.validate_build(root)
            self.record(root, elf)
            (root / "libSDL2.so").write_bytes(b"dummy link SDK must not ship")
            with self.assertRaisesRegex(ValueError, "file inventory"):
                build.validate_build(root)

    def test_patched_slirp_stream_byte_order_on_host(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        source = ROOT / "build/engine-cross/source/src/net/slirp"
        if not compiler or not source.is_dir():
            self.skipTest("Host compiler and prepared DSperate source required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture_source = root / "stream-test.c"
            fixture_source.write_text(
                r'''
#include <assert.h>
#include <string.h>
#include "stream.h"
static unsigned char data[4];
static ssize_t read_bytes(void *out, size_t size, void *opaque) {
    (void)opaque; memcpy(out, data, size); return (ssize_t)size;
}
static ssize_t write_bytes(const void *in, size_t size, void *opaque) {
    (void)opaque; memcpy(data, in, size); return (ssize_t)size;
}
int main(void) {
    SlirpIStream in = { .read_cb = read_bytes };
    SlirpOStream out = { .write_cb = write_bytes };
    data[0] = 0x12; data[1] = 0x34;
    assert(slirp_istream_read_u16(&in) == 0x1234);
    assert(slirp_ostream_write_u16(&out, 0x5678));
    assert(data[0] == 0x56 && data[1] == 0x78);
    assert(slirp_ostream_write_u32(&out, 0x12345678));
    assert(data[0] == 0x12 && data[1] == 0x34 && data[2] == 0x56 && data[3] == 0x78);
    assert(slirp_istream_read_u32(&in) == 0x12345678);
    assert(slirp_ostream_write_i16(&out, -7));
    assert(data[0] == 0xff && data[1] == 0xf9);
    assert(slirp_istream_read_i16(&in) == -7);
    assert(slirp_ostream_write_i32(&out, -7));
    assert(data[0] == 0xff && data[1] == 0xff && data[2] == 0xff && data[3] == 0xf9);
    assert(slirp_istream_read_i32(&in) == -7);
    return 0;
}
'''
            )
            binary = root / "stream-test"
            subprocess.run(
                [
                    compiler, "-std=c99", "-I" + str(source), "-I" + str(source / "glib"),
                    str(source / "stream.c"),
                    str(fixture_source), "-o",
                    str(binary)
                ],
                check=True
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
