"""The experimental core cannot silently fall back to another source or CPU."""
import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from tools import build_melonds as build
from tools import probe_melonds as probe


class MelonDSBuildTests(unittest.TestCase):
    def test_wrong_source_commit_and_dirty_checkout_rejected(self):
        for replies, message in (
            (["wrong"], "pinned commit"), ([build.COMMIT, build.TREE, " M NDS.cpp"], "unmodified")
        ):
            with patch.object(build.subprocess, "check_output", side_effect=replies):
                with self.assertRaisesRegex(ValueError, message):
                    build.source_inputs(Path("unused"))

    def test_modified_source_bytes_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            source = Path(name)
            (source / "NDS.cpp").write_bytes(b"unverified code")
            with patch.object(
                build.subprocess, "check_output", side_effect=[build.COMMIT, build.TREE, "", b"NDS.cpp\0"]
            ):
                with self.assertRaisesRegex(ValueError, "source bytes"):
                    build.source_inputs(source)

    def test_archived_source_needs_no_git_but_rejects_modified_or_extra_files(self):
        with tempfile.TemporaryDirectory() as name:
            source = Path(name)
            (source / 'core.cpp').write_bytes(b'pinned source')
            files = {'core.cpp': hashlib.sha256(b'pinned source').hexdigest()}
            checksum = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            with patch.object(build, 'INPUTS_SHA256', checksum), patch.object(
                build.subprocess, 'check_output', side_effect=AssertionError('must not need git')
            ):
                self.assertEqual(build.source_inputs(source, archived=True), files)
                (source / 'extra.cpp').write_bytes(b'extra')
                with self.assertRaisesRegex(ValueError, 'source bytes'):
                    build.source_inputs(source, archived=True)
                (source / 'extra.cpp').unlink()
                (source / 'core.cpp').write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'source bytes'):
                    build.source_inputs(source, archived=True)

    def test_missing_jit_or_lto_rejected_before_probe_execution(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            for missing in ("ENABLE_JIT", "ENABLE_LTO_RELEASE"):
                (directory / "CMakeCache.txt").write_text(
                    "\n".join(
                        f"{key}:BOOL={value if key != missing else 'OFF'}"
                        for key, value in (("ENABLE_JIT", "ON"), ("ENABLE_LTO_RELEASE", "ON"), ("BUILD_QT_SDL", "OFF"))
                    ) + "\n"
                )
                with self.assertRaisesRegex(ValueError, missing):
                    build.validate_build(directory)
            (directory /
             "CMakeCache.txt").write_text("ENABLE_JIT:BOOL=ON\nENABLE_LTO_RELEASE:BOOL=ON\nBUILD_QT_SDL:BOOL=OFF\n")
            (directory / "compile_commands.json").write_text(json.dumps([{"file": "NDS.cpp", "command": "c++ -flto"}]))
            with self.assertRaisesRegex(ValueError, "ARM64 JIT"):
                build.validate_build(directory)

    def test_probe_requires_linux_arm64_and_preserves_evidence(self):
        with patch.object(probe.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(ValueError, "Linux ARM64"):
                probe.probe(Path("unused"), Path("unused-report"))
        with tempfile.TemporaryDirectory() as name:
            report = Path(name) / "report.json"
            report.write_text("previous evidence")
            with patch.object(probe.platform, "system",
                              return_value="Linux"), patch.object(probe.platform, "machine", return_value="aarch64"):
                with self.assertRaisesRegex(ValueError, "fresh report"):
                    probe.probe(Path("unused"), report)
            self.assertEqual(report.read_text(), "previous evidence")


if __name__ == "__main__":
    unittest.main()
