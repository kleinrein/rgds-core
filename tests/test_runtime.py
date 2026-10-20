import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import activate
import emulators
import session

spec = importlib.util.spec_from_file_location("device_tools", ROOT / "tools/device.py")
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)

class Fixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.app = self.root / "app"
        self.data = self.root / "data"
        self.app.mkdir()
    def tearDown(self):
        self.temporary.cleanup()
    def binary(self, core):
        path = self.app / "emulators" / core / core
        path.parent.mkdir(parents=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
        return path
    def release(self, name):
        app = self.root / "releases" / name
        (app / "bin").mkdir(parents=True)
        binary = app / "bin/rgds-launcher"
        # ELF64, little-endian, AArch64: validation fixture rather than executable code.
        binary.write_bytes(b"\x7fELF\x02\x01" + b"\0" * 12 + b"\xb7\x00" + b"fixture")
        manifest = {"project": "rgds", "schema": 1, "target": "aarch64-linux-gnu", "build": name,
                    "files": {"bin/rgds-launcher": hashlib.sha256(binary.read_bytes()).hexdigest()}}
        (app / "manifest.json").write_text(json.dumps(manifest))
        return app
    def archive(self, entries):
        path = self.root / "game.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in entries:
                archive.writestr(name, data)
        return path

class EmulatorTests(Fixture):
    def test_code_refresh_streams_and_detects_same_size_same_mtime_changes(self):
        source = self.root/'component.so'; target = self.root/'installed.so'
        source.write_bytes(b'a' * (1024*1024) + b'original')
        emulators.copy_component(source, target)
        stamp = target.stat()
        with patch.object(Path, 'read_bytes', side_effect=AssertionError('Whole-file allocation')):
            emulators.copy_component(source, target)
            self.assertEqual(target.stat().st_ino, stamp.st_ino)
            with source.open('r+b') as stream:
                stream.seek(-8, 2); stream.write(b'changed!')
            os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            emulators.copy_component(source, target)
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertFalse(target.with_name(target.name+'.partial').exists())

    def test_failed_code_refresh_keeps_previous_component_and_cleans_partial(self):
        source=self.root/'component.so';target=self.root/'installed.so'
        source.write_bytes(b'new code');target.write_bytes(b'old code')
        def interrupted(source, destination):
            destination.write_bytes(b'partial');raise OSError('Card full')
        with patch.object(emulators.shutil, 'copyfile', side_effect=interrupted):
            with self.assertRaises(OSError):emulators.copy_component(source,target)
        self.assertEqual(target.read_bytes(),b'old code')
        self.assertFalse(target.with_name(target.name+'.partial').exists())

    def test_fresh_restart_skips_autoload_without_deleting_checkpoint(self):
        from fields import write_fields
        rom=self.root/'game.nds';rom.write_bytes(b'0123456789abTEST'+bytes(512))
        hook=self.app/'emulators/hooks/librgdsbridge.so';hook.parent.mkdir(parents=True);hook.write_bytes(b'bridge')
        for core in emulators.CORES:
            if core == 'nnddss': continue   # retired: never launched
            self.binary(core)
            automatic=self.data/'library'/emulators.game_key(rom)/('auto-'+core+'.dss')
            automatic.parent.mkdir(parents=True,exist_ok=True);automatic.write_bytes(b'keep checkpoint')
            with patch.dict(os.environ,{'RGDS_FORCE_AUTOLOAD':'1'}):
                plan=emulators.launch_plan(core,self.app,self.data,rom,fresh_start=True)
            self.assertNotIn('--load-state',plan['command']);self.assertNotEqual(plan['env'].get('RGDS_AUTOLOAD'),'1');self.assertNotEqual(plan['env'].get('RGDS_NATIVE_AUTOLOAD'),'1')
            self.assertEqual(automatic.read_bytes(),b'keep checkpoint')
    def test_fresh_restart_is_one_shot_and_keeps_current_engine(self):
        with patch.object(emulators,'_run_once',side_effect=[77,75,0]) as launch:
            self.assertEqual(emulators.run('melonds',self.app,self.data,self.root/'game.nds'),0)
        calls=launch.call_args_list
        self.assertEqual([c.kwargs['fresh_start'] for c in calls],[False,True,False])
        self.assertEqual(calls[1].args[0],'melonds')

    def test_ds_input_choices_reach_supported_engine_profiles(self):
        from fields import write_fields
        from library import game_key
        rom=self.root/'game.nds'
        write_fields(self.data/'library'/game_key(rom)/'ds-input.ini',dict(language=3,microphone=0))
        for core in ('dsperate','melonds'):
            self.binary(core)
            with patch.dict(os.environ,{},clear=True):plan=emulators.launch_plan(core,self.app,self.data,rom)
            self.assertEqual(plan['env']['RGDS_DS_LANGUAGE'],'3');self.assertEqual(plan['env']['RGDS_MICROPHONE'],'0')
        config=(self.data/'emulators/dsperate/config.ini').read_text()
        self.assertIn('language = 3',config);self.assertIn('mic = false',config);self.assertIn('mic_dev = plughw:0,0',config)

    def test_dsperate_uses_embedded_bios_even_with_stale_native_paths(self):
        self.binary("dsperate")
        config = self.data / 'emulators/dsperate/config.ini'
        config.parent.mkdir(parents=True)
        config.write_text('[paths]\nbios7=/mnt/vendor/old.bin\nfirmware=/oem/old.bin\n')
        with patch.object(emulators, 'stock_file', side_effect=AssertionError('stock lookup')), \
             patch.dict(os.environ, {}, clear=True):
            plan = emulators.launch_plan('dsperate', self.app, self.data, self.root/'game.nds')
        for switch in ('--bios7', '--bios9', '--firmware'):
            self.assertEqual(plan['command'][plan['command'].index(switch)+1], '')
        self.assertNotIn('/mnt/', plan['env']['LD_LIBRARY_PATH'])
        self.assertNotIn('/oem/', plan['env']['LD_LIBRARY_PATH'])

    def test_mixer_failure_prevents_emulator_start_and_cleans_activity_marker(self):
        self.binary("drastic")
        rom = self.root / "game.nds";rom.write_bytes(b"game")
        with patch.object(emulators, "VolumeWatch") as watch, \
             patch.object(emulators.subprocess, "Popen") as spawn:
            watch.return_value.start.side_effect = ValueError("Mixer unavailable")
            with self.assertRaisesRegex(ValueError, "Mixer unavailable"):
                emulators.run("drastic", self.app, self.data, rom)
            spawn.assert_not_called()
            watch.return_value.close.assert_called_once()
        self.assertFalse((self.data / "session/core.json").exists())
        diagnostic=(self.data/'logs/drastic.log').read_text()
        self.assertIn('Runtime error: Mixer unavailable',diagnostic)

    def test_automatic_state_import_preserves_the_previous_manual_slot(self):
        self.binary("drastic")
        hook = self.app / "emulators/hooks/librgdsbridge.so"
        hook.parent.mkdir(parents=True);hook.write_bytes(b"fixture hook")
        rom = self.root / "game.nds"
        slot = self.data / "emulators/drastic/savestates/game_1.dss"
        slot.parent.mkdir(parents=True);slot.write_bytes(b"manual state")
        automatic = self.data / "library" / emulators.game_key(rom) / "auto-drastic.dss"
        automatic.parent.mkdir(parents=True);automatic.write_bytes(b"automatic state")
        plan = emulators.launch_plan("drastic", self.app, self.data, rom)
        backups = list((automatic.parent / "manual-state-backups/drastic").glob("*.dss"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b"manual state")
        self.assertEqual(slot.read_bytes(), b"automatic state")
        self.assertEqual(plan["env"]["RGDS_AUTOLOAD"], "1")

    def test_all_three_engines_have_distinct_launch_arguments(self):
        rom = self.root / "A game; $(echo nope).nds"
        for core in emulators.CORES:
            binary = self.binary(core)
            if core == 'nnddss':   # retired: never launched
                with self.assertRaisesRegex(ValueError, 'retired'):
                    emulators.launch_plan(core, self.app, self.data, rom, prepare=False)
                continue
            plan = emulators.launch_plan(core, self.app, self.data, rom, prepare=False)
            self.assertEqual(plan["command"][0], str(binary.resolve()))
            self.assertIn(str(rom), plan["command"])
            if core == "dsperate":
                self.assertIn("--dual-window", plan["command"])
        self.assertFalse(self.data.exists(), "A dry-run must not create or alter save folders")

    def test_launch_never_inherits_a_previous_core_hook(self):
        self.binary("drastic")
        with patch.dict(os.environ, {"LD_PRELOAD": "/old/libsimplehook.so", "SIMPLEOS_NNDDSS": "1"}):
            plan = emulators.launch_plan("drastic", self.app, self.data, self.root / "game.nds", prepare=False)
        self.assertNotIn("LD_PRELOAD", plan["env"])
        self.assertNotIn("SIMPLEOS_NNDDSS", plan["env"])

    def test_archive_extracts_one_game_and_preserves_its_filename(self):
        archive = self.archive([("folder/A game.nds", b"game"), ("readme.txt", b"note")])
        path = emulators.resolve_rom(archive, self.data)
        self.assertEqual(path.name, "A game.nds")
        self.assertEqual(path.read_bytes(), b"game")
        self.assertEqual(emulators.resolve_rom(archive, self.data), path)

    def test_archive_rejects_traversal_even_in_non_game_files(self):
        archive = self.archive([("game.nds", b"game"), ("../../outside", b"bad")])
        with self.assertRaisesRegex(ValueError, "unsafe"):
            emulators.resolve_rom(archive, self.data)
        self.assertFalse((self.root / "outside").exists())

    def test_archive_rejects_symlinks(self):
        path = self.root / "game.zip"
        with zipfile.ZipFile(path, "w") as archive:
            info = zipfile.ZipInfo("game.nds")
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, "../../outside")
        with self.assertRaisesRegex(ValueError, "symlink"):
            emulators.resolve_rom(path, self.data)

    def test_archive_with_multiple_games_needs_a_choice(self):
        archive = self.archive([("a.nds", b"a"), ("b.nds", b"b")])
        with self.assertRaisesRegex(ValueError, "exactly one"):
            emulators.resolve_rom(archive, self.data)

    def test_actual_subprocess_receives_rom_path_as_one_literal_argument(self):
        binary = self.binary("drastic")
        binary.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$HOME/result.txt"\nprintf "%s" "$RGDS_VOLUME_KEYS" > "$HOME/volume-keys.txt"\n')
        rom = self.root / "A game; $(echo nope).nds"
        rom.write_bytes(b"game")
        with patch.object(emulators, "VolumeWatch") as volume:
            volume.return_value.descriptors = []
            result = emulators.run("drastic", self.app.resolve(), self.data.resolve(), rom)
        self.assertEqual(result, 0)
        self.assertEqual((self.data / "emulators/drastic/result.txt").read_text(), str(rom.resolve()) + "\n")
        self.assertEqual((self.data / "emulators/drastic/volume-keys.txt").read_text(), "0")
        self.assertFalse((self.data / "session/core.json").exists())

    def test_activity_checks_start_time_and_refuses_unreadable_records(self):
        marker = self.data / "session/core.json"
        marker.parent.mkdir(parents=True)
        marker.write_text(json.dumps({"pid": 321, "start": "123"}))
        with patch.object(emulators, "process_identity", return_value="123"):
            self.assertTrue(emulators.core_running(self.data))
        with patch.object(emulators, "process_identity", return_value="456"):
            self.assertFalse(emulators.core_running(self.data))
        marker.write_text("broken")
        with self.assertRaisesRegex(ValueError, "Unreadable"):
            emulators.core_running(self.data)

    def test_engine_startup_error_is_captured_before_returning_home(self):
        binary=self.binary('drastic')
        binary.write_text('#!/bin/sh\nprintf "missing runtime dependency\\n" >&2\nexit 17\n')
        rom=self.root/'game.nds';rom.write_bytes(b'game')
        with patch.object(emulators,'VolumeWatch') as volume:
            volume.return_value.descriptors=[]
            self.assertEqual(emulators.run('drastic',self.app,self.data,rom),17)
        output=(self.data/'logs/drastic.log').read_text()
        self.assertIn('missing runtime dependency',output)
        self.assertIn('Exit code: 17',output)
        self.assertFalse((self.data/'session/core.json').exists())

class ActivationTests(Fixture):
    def test_activation_and_rollback_keep_user_data(self):
        self.release("first")
        self.release("second")
        save = self.root / "userdata/game.sav"
        save.parent.mkdir()
        save.write_bytes(b"important")
        activate.activate(self.root, "first", stop=False)
        activate.activate(self.root, "second", stop=False)
        self.assertEqual(activate.read_pointer(self.root / "state/previous"), "first")
        self.assertEqual(activate.rollback(self.root, stop=False), "first")
        self.assertEqual(activate.read_pointer(self.root / "state/active"), "first")
        self.assertEqual(save.read_bytes(), b"important")

    def test_corrupt_candidate_leaves_active_pointer_unchanged(self):
        self.release("working")
        bad = self.release("bad")
        activate.activate(self.root, "working", stop=False)
        (bad / "bin/rgds-launcher").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            activate.activate(self.root, "bad", stop=False)
        self.assertEqual(activate.read_pointer(self.root / "state/active"), "working")

    def test_extra_files_and_directory_escape_are_rejected(self):
        app = self.release("candidate")
        (app / "extra.sh").write_text("unexpected")
        with self.assertRaisesRegex(ValueError, "Unlisted"):
            activate.verify_release(app)
        with self.assertRaisesRegex(ValueError, "build ID"):
            activate.activate(self.root, "../elsewhere", stop=False)

    def test_no_rollback_without_a_previous_build(self):
        with self.assertRaisesRegex(ValueError, "No previous"):
            activate.rollback(self.root, stop=False)

    def test_deploy_refuses_a_running_game_before_signalling_session(self):
        with patch.object(activate, "core_running", return_value=True), patch.object(activate.os, "kill") as kill:
            with self.assertRaisesRegex(ValueError, "game is running"):
                activate.stop_session(self.data)
            kill.assert_not_called()

class SessionTests(Fixture):
    def test_frontend_releases_audio_and_returns_through_its_stock_supervisor(self):
        record = {"pid": 123, "start": "old", "restart": True,
                  "arguments": ["/stock/menu"], "cwd": "/stock", "environment": {"HOME": "/root"}}
        with patch.object(session, "frontend_alive", side_effect=[True, False]), \
             patch.object(session.os, "kill") as kill:
            session.stop_frontends([record])
            kill.assert_called_once_with(123, session.signal.SIGTERM)
        with patch.object(session, "restore_stock") as restore_stock, \
             patch.object(session.subprocess, "Popen") as spawn:
            session.restore([record], lease="owned flag")
            restore_stock.assert_called_once_with("owned flag")
            spawn.assert_not_called()

    def test_new_session_cannot_replace_a_pending_recovery_lock(self):
        lock = self.root / "session.lock"
        lock.mkdir()
        record = json.dumps({"pid": 123, "start": "123"})
        (lock / "owner.json").write_text(record)
        with patch.object(session, "LOCK", lock), patch.object(session, "process_identity", return_value=None):
            with self.assertRaisesRegex(ValueError, "recovery"):
                session.lock()
        self.assertEqual((lock / "owner.json").read_text(), record)

    def test_only_running_known_stock_frontends_are_selected(self):
        proc = self.root / "proc"
        proc.mkdir()
        for pid, name, state in ((10, "dmenu.bin", "S"), (20, "muos3.bin", "T"), (30, "weston", "S")):
            entry = proc / str(pid)
            entry.mkdir()
            (entry / "comm").write_text(name)
            (entry / "stat").write_text(str(pid) + " (" + name + ") " + state + " 0" * 18 + " 999")
        self.assertEqual(session.discover_frontends(proc), [{"pid": 10, "start": "999"}])

    def test_restore_does_not_signal_a_reused_pid(self):
        with patch.object(session, "process_identity", return_value="new"), patch.object(session.os, "kill") as kill:
            session.restore([{"pid": 123, "start": "old"}])
            kill.assert_not_called()

    def test_restore_does_not_signal_when_process_identity_is_unknown(self):
        with patch.object(session, "process_identity", return_value=None), patch.object(session.os, "kill") as kill:
            session.restore([{"pid": 123, "start": None}])
            kill.assert_not_called()

class TransportTests(unittest.TestCase):
    def test_device_address_cannot_be_shell_or_ssh_options(self):
        for invalid in ("", "-oProxyCommand=whoami", "host;whoami", "host\nwhoami", "$(whoami)"):
            with self.assertRaises(ValueError):
                device.target(invalid)
        self.assertEqual(device.target("192.168.1.42"), "root@192.168.1.42")

    def test_remote_installer_is_valid_python(self):
        compile(device.REMOTE_INSTALL, "remote_installer", "exec")

    def test_fresh_installer_and_runtime_commands_do_not_mutate_release(self):
        latest = ROOT / "dist/latest.txt"
        if not latest.exists():
            self.skipTest("Build the private package for the deployment integration check")
        archive = ROOT / "dist" / latest.read_text().strip()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "card/rgds"
            (root / "inbox").mkdir(parents=True)
            uploaded = root / "inbox/package.zip"
            shutil.copy2(archive, uploaded)
            # A new interpreter must import the staged modules for the first time.
            # Isolate the global session marker so this test cannot signal a real session.
            prefix = """import platform, pathlib
platform.machine = lambda: 'aarch64'
real_is_file = pathlib.Path.is_file
pathlib.Path.is_file = lambda p: False if str(p) == '/tmp/rgds-session.lock/owner.json' else real_is_file(p)
"""
            env = os.environ.copy()
            for name in ("PYTHONDONTWRITEBYTECODE", "PYTHONPYCACHEPREFIX", "PYTHONPATH"):
                env.pop(name, None)
            result = subprocess.run([sys.executable, "-c", prefix + device.REMOTE_INSTALL, str(root), str(uploaded)],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            build = activate.read_pointer(root / "state/active")
            app = root / "releases" / build
            # Also test direct entry points without -B/environment protection.
            for command in ([sys.executable, str(app / "runtime/session.py"), "--help"],
                            [sys.executable, str(app / "runtime/activate.py"), "verify", "--root", str(root)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(list(app.rglob("__pycache__")), [])
            self.assertEqual(list(app.rglob("*.pyc")), [])
            activate.verify_release(app)

    def test_uploaded_package_is_verified_and_installed_without_changing_saves(self):
        latest = ROOT / "dist/latest.txt"
        if not latest.exists():
            self.skipTest("Build the private package for the deployment integration check")
        archive = ROOT / "dist" / latest.read_text().strip()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "card/rgds"
            (root / "inbox").mkdir(parents=True)
            save = root / "userdata/game.sav"
            save.parent.mkdir()
            save.write_bytes(b"keep me")
            uploaded = root / "inbox/package.zip"
            shutil.copy2(archive, uploaded)
            with patch.object(sys, "argv", ["installer", str(root), str(uploaded)]), patch("platform.machine", return_value="aarch64"), patch.object(activate, "stop_session"):
                exec(compile(device.REMOTE_INSTALL, "remote_installer", "exec"), {"__name__": "__main__"})
            build = activate.read_pointer(root / "state/active")
            activate.verify_release(root / "releases" / build)
            self.assertEqual(save.read_bytes(), b"keep me")
            self.assertTrue((root.parent / "Roms/APPS/Uninstall RG DS Core.sh").is_file())
            self.assertFalse(uploaded.exists())

    def test_tampered_upload_is_rejected_before_activation(self):
        latest = ROOT / "dist/latest.txt"
        if not latest.exists():
            self.skipTest("Build the private package for the deployment integration check")
        source = ROOT / "dist" / latest.read_text().strip()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "card/rgds"
            (root / "state").mkdir(parents=True)
            (root / "state/active").write_text("working\n")
            archive = root / "tampered.zip"
            with zipfile.ZipFile(source) as original, zipfile.ZipFile(archive, "w") as modified:
                for info in original.infolist():
                    content = original.read(info)
                    if info.filename.endswith("themes/default/light.ini"):
                        content += b"altered\n"
                    modified.writestr(info, content)
            with patch.object(sys, "argv", ["installer", str(root), str(archive)]), patch("platform.machine", return_value="aarch64"):
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    exec(compile(device.REMOTE_INSTALL, "remote_installer", "exec"), {"__name__": "__main__"})
            self.assertEqual((root / "state/active").read_text(), "working\n")

if __name__ == "__main__":
    unittest.main()
