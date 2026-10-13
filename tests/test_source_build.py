"""A failed second compile or stale source cannot publish a mixed RGDS pair."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build


class SourceBuildTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ("tools/build.py", "tools/bridge.exports", "src/launcher/main.cpp", "src/platform/emulator_bridge.cpp"):
            path = self.root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text("source\n")
        self.directory = self.root / "build/device"; self.directory.mkdir(parents=True)
        for name in ("rgds-launcher", "librgdsbridge.so"):
            (self.directory / name).write_bytes(b"previous-" + name.encode())
        self.original = {p.name: p.read_bytes() for p in self.directory.iterdir()}
        self.addCleanup(patch.stopall)
        patch.object(build, "ROOT", self.root).start()
        patch.object(build.shutil, "which", return_value="compiler").start()
        patch.object(build.subprocess, "check_output", side_effect=lambda cmd, **kw: "-I/fake" if cmd[0] == "sdl2-config" else "0.13.0").start()
        patch("tools.elf_abi.inspect_elf", side_effect=lambda p, **kw: dict(machine="aarch64", glibc_minimum="2.17", sha256=build.digest(p), exports=sorted(build.BRIDGE_EXPORTS))).start()

    def assert_preserved(self):
        self.assertEqual({p.name: p.read_bytes() for p in self.directory.iterdir()}, self.original)

    def test_bridge_failure_preserves_last_launcher_and_bridge(self):
        calls = []
        def compile(command, **kwargs):
            calls.append(command)
            Path(command[-1]).write_bytes(b"new binary")
            if len(calls) == 2: raise subprocess.CalledProcessError(1, command)
        with patch.object(build.subprocess, "run", side_effect=compile):
            with self.assertRaises(subprocess.CalledProcessError): build.build("device")
        self.assertEqual(len(calls), 2); self.assert_preserved()

    def test_source_edit_during_compile_preserves_last_pair(self):
        def compile(command, **kwargs):
            Path(command[-1]).write_bytes(b"new binary")
            (self.root / "src/launcher/main.cpp").write_text("edited source")
        with patch.object(build.subprocess, "run", side_effect=compile):
            with self.assertRaisesRegex(ValueError, "changed during compilation"): build.build("device")
        self.assert_preserved()

    def test_published_pair_has_verified_hashes_and_optimized_isolated_bridge(self):
        def compile(command, **kwargs): Path(command[-1]).write_bytes(command[-3].encode())
        with patch.object(build.subprocess, "run", side_effect=compile): build.build("device")
        record = build.validate_build(self.directory)
        command = record["commands"]["librgdsbridge.so"]
        self.assertIn("-O3", command); self.assertIn("-flto", command); self.assertIn("-mcpu=cortex_a55", command)
        self.assertTrue(any(x.startswith("-Wl,--version-script=") for x in command))
        (self.directory / "librgdsbridge.so").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "changed after compilation"): build.validate_build(self.directory)

    def test_stale_source_and_incomplete_record_are_rejected(self):
        artifacts = {name: {"sha256": build.digest(self.directory / name)} for name in self.original}
        record = dict(schema=1, target="aarch64-linux-gnu.2.17", sources=build.source_snapshot(), artifacts=artifacts)
        (self.directory / "build.json").write_text(json.dumps(record))
        (self.root / "src/platform/emulator_bridge.cpp").write_text("changed")
        with self.assertRaisesRegex(ValueError, "sources changed"): build.validate_build(self.directory)
        record["sources"] = build.source_snapshot(); record["artifacts"].pop("librgdsbridge.so")
        (self.directory / "build.json").write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "Incomplete"): build.validate_build(self.directory)

    def test_cpp_runtime_exports_prevent_publication(self):
        def compile(command, **kwargs): Path(command[-1]).write_bytes(b"candidate")
        with patch.object(build.subprocess, "run", side_effect=compile), patch("tools.elf_abi.inspect_elf", return_value=dict(machine="aarch64", glibc_minimum="2.17", exports=["_ZNSt6vector"] )):
            with self.assertRaisesRegex(ValueError, "private runtime symbols"): build.build("device")
        self.assert_preserved()

    def test_hidden_audio_or_render_hook_prevents_publication(self):
        def compile(command, **kwargs): Path(command[-1]).write_bytes(b"candidate")
        for required in ('snd_pcm_open','pthread_create','pthread_cond_init'):
            with self.subTest(symbol=required), patch.object(build.subprocess, "run", side_effect=compile), patch("tools.elf_abi.inspect_elf", return_value=dict(machine="aarch64", glibc_minimum="2.17", exports=sorted(build.BRIDGE_EXPORTS - {required}))):
                with self.assertRaisesRegex(ValueError, "missing required"): build.build("device")
            self.assert_preserved()

    def test_second_artifact_rename_failure_restores_previous_pair(self):
        def compile(command, **kwargs): Path(command[-1]).write_bytes(b"candidate")
        original = build.os.replace
        def replace(source, target):
            if Path(source).name.startswith(".bridge-") and Path(target).name == "librgdsbridge.so":
                raise OSError("fixture publication failure")
            return original(source, target)
        with patch.object(build.subprocess, "run", side_effect=compile), patch.object(build.os, "replace", side_effect=replace):
            with self.assertRaisesRegex(OSError, "publication failure"): build.build("device")
        self.assert_preserved()


if __name__ == "__main__": unittest.main()
