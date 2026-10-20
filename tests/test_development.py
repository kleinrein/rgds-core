"""Development-loop failures must preserve the running build and active games."""
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import dev
import device


class WatchTests(unittest.TestCase):
    def test_atomic_save_with_same_timestamp_is_seen_and_debounced(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "theme.ini"
            path.write_text("old")
            stamp = path.stat().st_mtime_ns
            before = dev.snapshot([path])
            watcher = dev.Changes(before, 0)
            replacement = path.with_suffix(".tmp")
            replacement.write_text("new")
            os.utime(replacement, ns=(stamp, stamp))
            replacement.replace(path)
            after = dev.snapshot([path])
            self.assertNotEqual(before, after)
            self.assertFalse(watcher.poll(after, 1))
            self.assertFalse(watcher.poll(after, 1.2))
            self.assertTrue(watcher.poll(after, 1.4))
            self.assertFalse(watcher.poll(after, 2))

    def test_newest_build_waits_for_game_and_restarts_only_after_deployment(self):
        queue = dev.DeviceQueue("root@device", Path("/mnt/mmc/rgds"), Path("theme.ini"))
        busy = {"game": True, "launcher": True, "session": True, "lock": True, "recovery": False}
        idle = dict(busy, game=False)
        operations = []
        with patch.object(device, "status", side_effect=[busy, idle]), \
             patch.object(device, "deploy", side_effect=lambda _d, _r, archive, **_kw: operations.append(("deploy", archive.name))), \
             patch.object(device, "sync_theme", side_effect=lambda *_a, **_kw: operations.append(("theme", None))), \
             patch.object(device, "start_dev_session", side_effect=lambda *_a: operations.append(("start", None))), \
             patch("sys.stdout", new_callable=io.StringIO):
            queue.archive = Path("old.zip")
            queue.theme_pending = True
            queue.flush()
            self.assertEqual(operations, [])
            queue.archive = Path("newest.zip")
            queue.flush()
        self.assertEqual(operations, [("deploy", "newest.zip"), ("theme", None), ("start", None)])
        self.assertIsNone(queue.archive)

    def test_game_launch_race_keeps_candidate_queued_without_restart(self):
        queue = dev.DeviceQueue("root@device", Path("/mnt/mmc/rgds"), Path("theme.ini"))
        queue.archive = Path("candidate.zip")
        idle = {"game": False, "launcher": True, "session": True, "lock": True, "recovery": False}
        with patch.object(device, "status", return_value=idle), \
             patch.object(device, "deploy", side_effect=subprocess.CalledProcessError(1, "ssh")), \
             patch.object(device, "start_dev_session") as start, \
             patch("sys.stdout", new_callable=io.StringIO):
            with self.assertRaises(subprocess.CalledProcessError):
                queue.flush()
            start.assert_not_called()
            self.assertEqual(queue.archive, Path("candidate.zip"))

    def test_invalid_theme_does_not_get_uploaded(self):
        with tempfile.TemporaryDirectory() as folder:
            theme = Path(folder) / "theme.ini"
            theme.write_text("schema=1\naccent=oops\n")
            with patch.object(device, "ssh") as ssh:
                with self.assertRaises(ValueError):
                    device.sync_theme("root@device", Path("/mnt/mmc/rgds"), theme)
                ssh.assert_not_called()

    def test_rejected_theme_is_reported_without_claiming_upload(self):
        queue = dev.DeviceQueue("root@device", Path("/mnt/mmc/rgds"), Path("theme.ini"))
        queue.theme_pending = True
        idle = {"game": False, "launcher": True, "session": True, "lock": True, "recovery": False}
        with patch.object(device, "status", return_value=idle), \
             patch.object(device, "sync_theme", side_effect=ValueError("Invalid colour")), \
             patch.object(device, "start_dev_session") as start, \
             patch("sys.stdout", new_callable=io.StringIO) as output:
            queue.flush()
            start.assert_not_called()
            self.assertIn("keeping the device's current theme", output.getvalue())
            self.assertNotIn("uploaded", output.getvalue())

    def test_failed_device_start_is_reported_once_for_missing_session_log(self):
        with patch.object(device, "run_session"), patch.object(device.time, "sleep"), \
             patch.object(device, "status", return_value={"launcher": False}), \
             patch.object(device, "ssh", return_value=subprocess.CompletedProcess("ssh", 0, stdout="session log missing")) as ssh:
            with self.assertRaisesRegex(RuntimeError, "did not stay running"):
                device.start_dev_session("root@device", Path("/mnt/mmc/rgds"))
            self.assertTrue(ssh.call_args.args[1].endswith("|| :"))

    def test_remote_theme_upload_is_atomic_and_outside_release(self):
        compile(device.REMOTE_STATUS, "remote-status", "exec")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "rgds"
            release = root / "releases/working/theme.ini"
            release.parent.mkdir(parents=True)
            release.write_text("immutable")
            content = b"schema=1\naccent=123456\n"
            subprocess.run([sys.executable, "-c", device.REMOTE_THEME, str(root)], input=content, check=True)
            self.assertEqual((root / "userdata/dev/theme.ini").read_bytes(), content)
            self.assertEqual(release.read_text(), "immutable")
            self.assertFalse((root / "userdata/dev/theme.ini.upload").exists())

    def test_failed_compiler_keeps_last_successful_binary(self):
        # Run the real build tool against an isolated tree and a failing compiler.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "tools").mkdir()
            (root / "tools/build.py").write_bytes((ROOT / "tools/build.py").read_bytes())
            output = root / "build/host/rgds-launcher"
            output.parent.mkdir(parents=True)
            output.write_bytes(b"working binary")
            compiler = root / "broken-compiler"
            compiler.write_text("#!/bin/sh\nexit 1\n")
            compiler.chmod(0o755)
            config = root / "sdl2-config"
            config.write_text("#!/bin/sh\nprintf '%s\\n' '-I/fake'\n")
            config.chmod(0o755)
            env = dict(os.environ, CXX=str(compiler), PATH=str(root) + os.pathsep + os.environ["PATH"])
            result = subprocess.run([sys.executable, str(root / "tools/build.py"), "host"], env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"working binary")
            self.assertEqual(list(output.parent.glob(".launcher-*")), [])


@unittest.skipUnless((ROOT / "build/host/rgds-launcher").exists(), "Build host launcher first")
class LiveThemeTests(unittest.TestCase):
    def test_dark_palette_reloads_while_light_edits_preserve_saved_dark_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            light, dark = root / "light.ini", root / "dark.ini"
            light.write_text("schema=1\nmode=light\nbackground=ffffff\n")
            dark.write_text("schema=1\nmode=dark\nbackground=112233\n")
            prefs = root / "preferences.ini"
            prefs.write_text("schema=1\nmode=dark\nfont=rounded\n")
            (root / "ui.ini").write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")   # the theme colour is read off a plain background
            bmp = root / "screen.bmp"
            command = [str(ROOT / "build/host/rgds-launcher"), "--preview", "--library", "--watch-assets", "--themes", str(root),
                       "--theme", str(light), "--preferences", str(prefs), "--screenshot", str(bmp), "--seconds", "5"]
            child = subprocess.Popen(command, env=dict(os.environ, SDL_VIDEODRIVER="dummy"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                self.wait_color(bmp, (0x11, 0x22, 0x33), child)
                light.write_text("schema=1\nmode=light\nbackground=aabbcc\n")
                time.sleep(0.5)
                self.assertEqual(self.color(bmp), (0x11, 0x22, 0x33))
                dark.write_text("schema=1\nmode=dark\nbackground=445566\n")
                self.wait_color(bmp, (0x44, 0x55, 0x66), child)
                child.terminate()
                output, _ = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, output)
                self.assertEqual(prefs.read_text(), "schema=1\nmode=dark\nfont=rounded\n")
            finally:
                if child.poll() is None:
                    child.kill();child.communicate()

    def test_running_sdl_preview_reloads_and_keeps_valid_theme_and_selection(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            theme, state, bmp = (root / name for name in ("live.ini", "state.txt", "screen.bmp"))
            theme.write_text("schema=1\nbackground=112233\n")
            state.write_text('1 2 1 ""\n')
            (root / "ui.ini").write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")   # the theme colour is read off a plain background
            command = [str(ROOT / "build/host/rgds-launcher"), "--preview", "--library", "--watch-assets", "--theme", str(theme),
                       "--state", str(state), "--preferences", str(root / "preferences.ini"), "--screenshot", str(bmp), "--seconds", "6"]
            env = dict(os.environ, SDL_VIDEODRIVER="dummy")
            child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                self.wait_color(bmp, (0x11, 0x22, 0x33), child)
                # Rename saves are detected without restarting the SDL process.
                pending = theme.with_suffix(".tmp")
                pending.write_text("schema=1\nbackground=445566\n")
                pending.replace(theme)
                self.wait_color(bmp, (0x44, 0x55, 0x66), child)
                theme.write_text("schema=1\nbackground=invalid\n")
                time.sleep(0.7)
                self.assertIsNone(child.poll())
                self.assertEqual(self.color(bmp), (0x44, 0x55, 0x66))
                theme.write_text("schema=1\nbackground=778899\n")
                self.wait_color(bmp, (0x77, 0x88, 0x99), child)
                child.terminate()
                output, _ = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, output)
                self.assertIn("Theme reloaded:", output)
                self.assertIn("THEME", output.upper())
                self.assertEqual(state.read_text(), '1 2 1 ""\n')
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate()

    @staticmethod
    def color(path):
        data = path.read_bytes()
        offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        bits = struct.unpack_from("<H", data, 28)[0]
        stride = ((width * bits + 31) // 32) * 4
        row = abs(height) - 1 - 101 if height > 0 else 101
        b, g, r = data[offset + row * stride + 4:offset + row * stride + 7]
        return r, g, b

    def wait_color(self, path, expected, child):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            self.assertIsNone(child.poll(), "Preview exited before theme rendered")
            try:
                if self.color(path) == expected:
                    return
            except (OSError, ValueError, struct.error):
                pass  # Screenshot may currently be being written.
            time.sleep(0.05)
        self.fail("Preview did not render theme colour " + repr(expected))


if __name__ == "__main__":
    unittest.main()

class ConfiguredRootTests(unittest.TestCase):
    def test_remote_selection_keeps_configured_root_in_stock_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            control = Path(temporary) / 'control'
            control.mkdir()
            root = Path(temporary) / 'internal install/rgds'
            root.mkdir(parents=True)
            (root / 'main.sh').touch()

            def remote(device_name, command, **kwargs):
                script = kwargs['input'].replace('/userdata/rgds-boot', str(control))
                return subprocess.run([sys.executable, '-B', '-'], input=script,
                                      text=True, capture_output=True, check=True)

            for mode in ('stock', 'rgds'):
                with self.subTest(mode=mode):
                    (control / 'config.json').write_text(json.dumps(dict(root=str(root), mode=mode)))
                    with patch.object(device, 'ssh', side_effect=remote), patch('sys.stdout', new_callable=io.StringIO):
                        self.assertEqual(device.configured_root('root@device'), root)
            for content in ('[]', '{broken', json.dumps(dict(root='relative', mode='stock'))):
                (control / 'config.json').write_text(content)
                with patch.object(device, 'ssh', side_effect=remote), patch('sys.stdout', new_callable=io.StringIO):
                    self.assertEqual(device.configured_root('root@device'), Path('/mnt/mmc/rgds'))

    def test_default_follows_boot_root_and_explicit_mount_does_not_probe(self):
        with patch.object(device,'ssh',return_value=subprocess.CompletedProcess([],0,'/userdata/rgds-device-test/rgds\n','')) as ssh, patch('sys.stdout',new_callable=io.StringIO):
            self.assertEqual(device.configured_root('root@device'),Path('/userdata/rgds-device-test/rgds'))
            ssh.assert_called_once()
            ssh.reset_mock()
            self.assertEqual(device.configured_root('root@device',Path('/mnt/SDCARD')),Path('/mnt/SDCARD/rgds'))
            ssh.assert_not_called()

    def test_invalid_remote_path_cannot_become_a_deploy_target(self):
        for value in ('relative/path','/mnt/../etc','/safe\n/other'):
            with patch.object(device,'ssh',return_value=subprocess.CompletedProcess([],0,value,'')):
                with self.assertRaises(ValueError):device.configured_root('root@device')
