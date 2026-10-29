"""The shared engine cross-build pieces: atomic publication, a clean cross environment and toolchain wrappers."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import engine_toolchain as toolchain


class EngineToolchainTests(unittest.TestCase):
    def test_publication_failure_restores_complete_last_successful_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, pending = root / "rgds-experimental", root / ".pending"
            output.mkdir()
            pending.mkdir()
            original = {"rgds-experimental": b"last working ELF", "build.json": b"matching old metadata"}
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
                    toolchain.publish_build(pending, output)
            self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()}, original)
            self.assertFalse(output.with_name(output.name + ".previous").exists())
            self.assertTrue(pending.is_dir())

    def test_cross_environment_drops_host_search_paths(self):
        host = {name: "/host" for name in toolchain.HOST_VARIABLES}
        with patch.dict(os.environ, dict(host, PATH="/usr/bin"), clear=True):
            env = toolchain.cross_environment("zig-test")
        self.assertFalse(set(toolchain.HOST_VARIABLES) & set(env))
        self.assertEqual(env["PATH"], "/usr/bin")
        self.assertEqual(env["ZIG_LOCAL_CACHE_DIR"], str(ROOT / ".cache/zig-test"))

    def test_toolchain_targets_glibc_2_17_and_hides_build_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = toolchain.toolchain(root / "chain", Path("/opt/zig"), root / "engine", root / "sdl")
            self.assertIn("-target aarch64-linux-gnu.2.17", (root / "chain/cxx").read_text())
            self.assertNotIn("-target", (root / "chain/ar").read_text())
            text = path.read_text()
            self.assertIn(f"-ffile-prefix-map={root / 'engine'}=/src/engine", text)
            self.assertIn(f"-ffile-prefix-map={ROOT}=/rgds", text)

    def test_existing_link_sdk_is_reused_without_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdl-sdk"
            (sdk / "lib/cmake/SDL2").mkdir(parents=True)
            (sdk / "include/SDL2").mkdir(parents=True)
            (sdk / "include/SDL2/SDL.h").write_text("/* fixture */\n")
            with patch.object(toolchain, "SDL_SDK", sdk), \
                 patch.object(toolchain, "find_tool", side_effect=AssertionError("rebuilt")):
                self.assertEqual(toolchain.sdl_sdk(), sdk)


if __name__ == "__main__": unittest.main()
