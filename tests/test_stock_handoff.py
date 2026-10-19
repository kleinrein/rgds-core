"""Stock owns frontend restart; RGDS touches only its own session and stop flag."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import session
import stock_handoff as handoff
from records import write_record


class StockHandoffTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name);self.proc = self.root / "proc";self.proc.mkdir()
        self.script = self.root / "loadapp.sh";self.script.write_text("#!/bin/sh\nexit 0\n");self.script.chmod(0o755)
        self.marker = self.root / "stopAPP.ini";self.owner = dict(pid=123, start="999")
        self.data = self.root / "data"

    def process(self, pid, name, group=10, arguments=None, state="S", start="999"):
        entry = self.proc / str(pid);entry.mkdir()
        (entry / "comm").write_text(name)
        fields = [state, "1", str(group)] + ["0"] * 16 + [start]
        (entry / "stat").write_text(f"{pid} ({name}) " + " ".join(fields))
        if arguments is not None:(entry / "cmdline").write_bytes(b"\0".join(os.fsencode(value) for value in arguments) + b"\0")
        return entry

    def lease(self):return handoff.acquire_stock(self.owner, marker=self.marker, script=self.script)

    def test_only_the_verified_stock_now_playing_binary_is_claimed(self):
        overlay=self.process(12,'subscreen.dge')
        (overlay/'exe').symlink_to('/mnt/vendor/subscreen/subscreen.dge')
        unrelated=self.process(13,'subscreen.dge')
        (unrelated/'exe').symlink_to('/home/other/subscreen.dge')
        self.assertEqual(session.discover_frontends(self.proc),[dict(pid=12,start='999')])

    def test_owned_flag_is_removed_before_supervisor_start(self):
        lease = self.lease()
        def observe(*args, **kwargs):
            self.assertFalse(self.marker.exists())
            self.assertEqual(args[0], [str(self.script)])
            self.assertEqual(kwargs["env"]["RGDS_SKIP_BOOT_ONCE"], "1")
        with patch.object(handoff.subprocess, "Popen", side_effect=observe) as spawn:
            handoff.restore_stock(lease, self.proc)
        spawn.assert_called_once()

    def test_existing_supervisor_owns_restart_and_no_frontend_is_spawned(self):
        lease = self.lease();self.process(10, "loadapp.sh", arguments=["/bin/sh", self.script])
        with patch.object(handoff.subprocess, "Popen") as spawn:
            handoff.restore_stock(lease, self.proc)
        self.assertFalse(self.marker.exists());spawn.assert_not_called()

    def test_relative_stock_script_argument_is_also_recognized(self):
        lease = self.lease();entry = self.process(10, "sh", arguments=["sh", self.script.name])
        (entry / "cwd").symlink_to(self.script.parent, target_is_directory=True)
        with patch.object(handoff.subprocess, "Popen") as spawn:handoff.restore_stock(lease, self.proc)
        spawn.assert_not_called()

    def test_surviving_frontend_prevents_duplicate_stock_loop(self):
        lease = self.lease();self.process(10, "dmenu.bin")
        with patch.object(handoff.subprocess, "Popen") as spawn:
            handoff.restore_stock(lease, self.proc)
        spawn.assert_not_called()

    def test_preexisting_or_replaced_foreign_flags_are_preserved(self):
        self.marker.write_text("foreign")
        with self.assertRaisesRegex(ValueError, "already owned"):self.lease()
        self.assertEqual(self.marker.read_text(), "foreign")
        self.marker.unlink();lease = self.lease();self.marker.write_text("new owner")
        with self.assertRaisesRegex(ValueError, "changed owner"):handoff.release_stock_flag(lease)
        self.assertEqual(self.marker.read_text(), "new owner")

    def test_atomic_foreign_flag_replacement_cannot_launch_a_duplicate_supervisor(self):
        lease = self.lease();foreign = self.root / "foreign.ini";foreign.write_text("other owner")
        foreign.replace(self.marker)
        with patch.object(handoff.subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "changed owner"):handoff.restore_stock(lease, self.proc)
        self.assertEqual(self.marker.read_text(), "other owner");spawn.assert_not_called()

    def test_foreign_touch_of_an_owned_marker_is_not_deleted(self):
        lease = self.lease();os.utime(self.marker, ns=(lease["mtime"]+1,lease["mtime"]+1))
        with self.assertRaisesRegex(ValueError, "changed owner"):handoff.release_stock_flag(lease)
        self.assertTrue(self.marker.exists())

    def test_boot_handoff_is_a_noop_and_does_not_signal_services(self):
        with patch.object(handoff.os, "kill") as kill, patch.object(handoff.subprocess, "Popen") as spawn:
            handoff.restore_stock(None, self.proc)
        kill.assert_not_called();spawn.assert_not_called();self.assertFalse(self.marker.exists())

    def test_terminal_request_is_owned_and_never_dispatches_a_command(self):
        handoff.begin_session(self.data, self.owner)
        with patch("emulators.process_identity", return_value="999"), patch("emulators.core_running", return_value=False), \
             patch.object(handoff.subprocess, "run") as run, patch.object(handoff.os, "kill") as kill:
            receipt=handoff.request_exit(self.data, "reboot")
            handoff.commit_exit(self.data,receipt)
        run.assert_not_called();kill.assert_not_called()
        self.assertEqual(handoff.consume_exit(self.data, self.owner), "reboot")
        self.assertFalse(handoff.request_path(self.data).exists())

    def test_live_or_unreadable_engine_prevents_shutdown_request(self):
        handoff.begin_session(self.data, self.owner)
        for outcome in (True, ValueError("Unreadable core")):
            with patch("emulators.core_running", **({"side_effect": outcome} if isinstance(outcome, Exception) else {"return_value": outcome})):
                with self.assertRaises(ValueError):handoff.request_exit(self.data, "poweroff")
        self.assertFalse(handoff.request_path(self.data).exists())

    def test_pending_terminal_request_is_never_dispatched(self):
        handoff.begin_session(self.data,self.owner)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False):
            handoff.request_exit(self.data,"reboot")
        self.assertIsNone(handoff.consume_exit(self.data,self.owner))

    def test_cancel_and_commit_cannot_replace_a_successor_request(self):
        handoff.begin_session(self.data,self.owner)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False):
            previous=handoff.request_exit(self.data,"reboot")
            successor=handoff.request_exit(self.data,"reboot")
        self.assertFalse(handoff.cancel_exit(self.data,previous))
        with self.assertRaisesRegex(ValueError,"changed"):handoff.commit_exit(self.data,previous)
        handoff.commit_exit(self.data,successor)
        self.assertEqual(handoff.consume_exit(self.data,self.owner),"reboot")

    def test_failed_atomic_commit_leaves_only_an_ineligible_pending_request(self):
        handoff.begin_session(self.data,self.owner)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False):
            receipt=handoff.request_exit(self.data,"reboot")
        with patch.object(handoff.os,"replace",side_effect=OSError("storage became readonly")):
            with self.assertRaises(OSError):handoff.commit_exit(self.data,receipt)
        self.assertIsNone(handoff.consume_exit(self.data,self.owner))

    def test_exact_receipt_can_roll_back_its_committed_state(self):
        handoff.begin_session(self.data,self.owner)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False):
            receipt=handoff.request_exit(self.data,"reboot")
        handoff.commit_exit(self.data,receipt)
        self.assertTrue(handoff.cancel_exit(self.data,receipt))
        self.assertIsNone(handoff.consume_exit(self.data,self.owner))

    def test_cancel_preserves_a_different_committed_token_from_the_same_owner(self):
        handoff.begin_session(self.data,self.owner)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False):
            old=handoff.request_exit(self.data,"reboot")
            current=handoff.request_exit(self.data,"reboot")
        handoff.commit_exit(self.data,current)
        self.assertFalse(handoff.cancel_exit(self.data,old))
        self.assertEqual(handoff.consume_exit(self.data,self.owner),"reboot")

    def test_dead_owner_request_never_survives_into_the_next_session(self):
        handoff.begin_session(self.data, self.owner)
        with patch("emulators.process_identity", return_value="999"), patch("emulators.core_running", return_value=False):
            handoff.request_exit(self.data, "poweroff")
        with patch("emulators.process_identity", return_value=None), patch("emulators.core_running", return_value=False), \
             patch.object(handoff.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "No managed RGDS session"):handoff.request_exit(self.data, "reboot")
            next_owner = dict(pid=444,start="222");handoff.begin_session(self.data, next_owner)
            self.assertIsNone(handoff.consume_exit(self.data, next_owner))
        run.assert_not_called()

    def test_stale_foreign_or_duplicate_terminal_fields_never_trigger_shutdown(self):
        handoff.begin_session(self.data, self.owner)
        for content in ("schema=1\naction=reboot\nowner_pid=444\nowner_start=999\n",
                        "schema=1\naction=reboot\naction=poweroff\nowner_pid=123\nowner_start=999\n"):
            handoff.request_path(self.data).write_text(content)
            with self.assertRaises(ValueError):handoff.consume_exit(self.data, self.owner)

    def test_normal_system_action_has_no_force_flags_or_hook_environment(self):
        with patch("emulators.core_running", return_value=False), \
             patch.object(handoff.shutil, "which", return_value="/sbin/reboot"), \
             patch.object(handoff.subprocess, "run") as run, \
             patch.dict(os.environ, {"LD_PRELOAD": "bad", "SIMPLEOS_ROOT": "old", "RGDS_CORE": "drastic"}):
            handoff.execute_system_action("reboot", self.data)
        self.assertEqual(run.call_args.args[0], ["/sbin/reboot"])
        for key in ("LD_PRELOAD", "SIMPLEOS_ROOT", "RGDS_CORE"):self.assertNotIn(key, run.call_args.kwargs["env"])
        self.assertEqual(run.call_args.kwargs["timeout"], 5)

    def test_poweroff_dispatch_is_recorded_without_substituting_reboot(self):
        from fields import read_fields
        with patch('emulators.core_running',return_value=False), \
             patch.object(handoff.shutil,'which',return_value='/sbin/poweroff'), \
             patch.object(handoff.subprocess,'run') as run:
            handoff.execute_system_action('poweroff',self.data)
        self.assertEqual(run.call_args.args[0],['/sbin/poweroff'])
        receipt=read_fields(self.data/'logs/system-exit.ini')
        self.assertEqual(receipt['action'],'poweroff')
        self.assertEqual(receipt['state'],'request-accepted')
        self.assertIn('boot_id',receipt)

    def test_failed_poweroff_is_recorded_and_never_retried_as_reboot(self):
        from fields import read_fields
        with patch('emulators.core_running',return_value=False), \
             patch.object(handoff.shutil,'which',return_value='/sbin/poweroff'), \
             patch.object(handoff.subprocess,'run',side_effect=OSError('stock helper failed')) as run:
            with self.assertRaises(OSError):handoff.execute_system_action('poweroff',self.data)
        run.assert_called_once()
        self.assertEqual(read_fields(self.data/'logs/system-exit.ini')['state'],'failed')

    def test_final_live_backend_check_prevents_init_dispatch(self):
        with patch("emulators.core_running", return_value=True), patch.object(handoff.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "still active"):handoff.execute_system_action("poweroff", self.data)
        run.assert_not_called()

    def test_group_scan_excludes_system_network_and_display_processes(self):
        self.process(10, "device_services", group=123)
        for pid, name in ((20, "NetworkManager"), (30, "wpa_supplicant"), (40, "weston"), (50, "dbus-daemon")):
            self.process(pid, name, group=123)
        self.assertEqual(session.group_members(self.owner, self.proc), [dict(pid=10, start="999")])

    def test_reused_leader_or_reused_worker_pid_is_never_signalled(self):
        with patch.object(session, "process_identity", return_value="changed"), patch.object(session.os, "killpg") as kill:
            session.stop_owned_group(self.owner, [dict(pid=456, start="999")], self.data, timeout=0)
        kill.assert_not_called()

    def test_orphaned_recorded_worker_is_stopped_with_a_bounded_group_shutdown(self):
        def identity(pid):return "999" if pid==456 else None
        with patch.object(session, "process_identity", side_effect=identity), \
             patch.object(session.os, "getpgid", return_value=123), patch.object(session.os, "kill") as kill, \
             patch.object(session, "protected_process", return_value=False), \
             patch.object(session, "group_members", return_value=[]), \
             patch.object(session, "frontend_alive", return_value=False):
            session.stop_owned_group(self.owner, [dict(pid=456, start="999")], self.data, timeout=0)
        self.assertEqual(kill.call_args_list[0].args, (456, signal.SIGTERM))
        self.assertEqual(kill.call_args_list[1].args, (456, signal.SIGKILL))

    def test_protected_daemon_is_never_signalled_even_in_the_old_group(self):
        with patch.object(session, "process_identity", side_effect=lambda pid:"999" if pid==456 else None), \
             patch.object(session.os, "getpgid", return_value=123), \
             patch.object(session, "protected_process", return_value=True), \
             patch.object(session.os, "kill") as kill, patch.object(session.os, "killpg") as killpg:
            session.stop_owned_group(self.owner, [dict(pid=456,start="999")], self.data, timeout=0)
        kill.assert_not_called();killpg.assert_not_called()

    def test_end_session_preserves_a_new_owners_record(self):
        handoff.begin_session(self.data, self.owner)
        write_record(handoff.owner_path(self.data), dict(pid=444, start="222"))
        handoff.end_session(self.data, self.owner)
        self.assertTrue(handoff.owner_path(self.data).exists())

    def test_boot_existing_frontend_falls_back_before_marker_or_fork(self):
        with patch.object(session.sys, "argv", ["session.py", "--root", str(self.root), "--boot"]), \
             patch.object(session.sys, "platform", "linux"), patch.object(session, "release", return_value=self.root) as release, \
             patch.object(session, "environment", return_value={}), patch.object(session, "lock"), \
             patch.object(session, "unlock") as unlock, patch.object(session, "Hardware"), \
             patch.object(session, "process_identity", return_value="999"), \
             patch.object(session, "discover_frontends", return_value=[dict(pid=10,start="999")]), \
             patch.object(session, "acquire_stock") as acquire, patch.object(session.os, "fork") as fork:
            with self.assertRaisesRegex(ValueError, "existing stock frontend"):session.main()
        release.assert_called_once_with(self.root.resolve(), boot=True)
        acquire.assert_not_called();fork.assert_not_called();unlock.assert_called_once()

    def test_boot_preflight_never_acquires_stop_flag_and_closes_pipe_on_fork_failure(self):
        descriptors = [];real_pipe = os.pipe
        def make_pipe():
            result = real_pipe();descriptors.extend(result);return result
        with patch.object(session.sys, "argv", ["session.py", "--root", str(self.root), "--boot"]), \
             patch.object(session.sys, "platform", "linux"), patch.object(session, "release", return_value=self.root), \
             patch.object(session, "environment", return_value={}), patch.object(session, "lock"), \
             patch.object(session, "unlock") as unlock, patch.object(session, "Hardware"), \
             patch.object(session, "process_identity", return_value="999"), \
             patch.object(session, "discover_frontends", return_value=[]), \
             patch.object(session, "acquire_stock") as acquire, patch.object(session.os, "pipe", side_effect=make_pipe), \
             patch.object(session.os, "fork", side_effect=OSError("fork failed")), \
             patch("lid_sensor.LidSensor") as sensor:
            with self.assertRaisesRegex(OSError, "fork failed"):session.main()
        sensor.return_value.close.assert_called_once()
        acquire.assert_not_called();unlock.assert_called_once()
        for descriptor in descriptors:
            with self.assertRaises(OSError):os.fstat(descriptor)

    def test_hardware_restore_timeout_reaps_only_its_own_helper(self):
        helper = Mock(pid=777)
        helper.communicate.side_effect = [session.subprocess.TimeoutExpired("restore",5), (None,b"")]
        with patch.object(session.subprocess, "Popen", return_value=helper), patch.object(session.os, "killpg") as kill:
            with self.assertRaisesRegex(ValueError, "timed out"):session.recover_hardware(self.root, self.data)
        kill.assert_called_once_with(777, signal.SIGKILL)
        self.assertEqual([call.kwargs["timeout"] for call in helper.communicate.call_args_list], [5,1])

    def test_unreaped_recovery_helper_retains_its_ownership_record(self):
        helper = Mock(pid=777)
        helper.communicate.side_effect = session.subprocess.TimeoutExpired("restore",5)
        lock = self.root / "lock";lock.mkdir()
        with patch.object(session, "LOCK", lock), patch.object(session, "process_identity", return_value="999"), \
             patch.object(session.subprocess, "Popen", return_value=helper), patch.object(session.os, "killpg"):
            with self.assertRaises(session.RecoveryPending):session.recover_hardware(self.root, self.data)
        self.assertTrue((lock / "recovery.json").exists())

    def fake_boot_terminal_exit(self, exit_code, request_state="committed"):
        child = Mock(pid=456);child.wait.return_value=exit_code;child.poll.return_value=exit_code
        order = []
        def spawn(command, **kwargs):
            if command[1:3] == ["-m", "compileall"]:return Mock()   # session.precompile, after the launcher
            self.assertIn("--boot-startup", command)
            self.assertIn("--boot-warning", command)
            self.assertFalse(any("simpleos" in str(value) for value in command))
            owner = handoff.read_record(handoff.owner_path(self.root/"userdata"))
            handoff.request_path(self.root/"userdata").write_text(f"schema=2\naction=reboot\nowner_pid={owner['pid']}\nowner_start={owner['start']}\ntoken={'a'*32}\nstate={request_state}\n")
            return child
        def wait_guardian(*args):
            self.assertFalse(handoff.request_path(self.root/"userdata").exists())
            order.append("guardian");return (202, (session.SYSTEM_DISPATCH_ACK << 8) if exit_code == 76 and request_state == "committed" else 0)
        def action(value, data):
            self.assertEqual(value,"reboot");self.assertFalse(handoff.owner_path(data).exists())
            self.assertFalse((Path(data)/"session/launcher.json").exists());order.append("init")
        with patch.object(session.sys, "argv", ["session.py", "--root", str(self.root), "--boot", "--boot-warning"]), \
             patch.object(session.sys, "platform", "linux"), patch.object(session, "release", return_value=self.root), \
             patch.object(session, "environment", return_value={}), patch.object(session, "lock"), \
             patch.object(session, "Hardware"), patch.object(session, "process_identity", return_value="999"), \
             patch.object(session, "discover_frontends", return_value=[]), patch.object(session, "acquire_stock") as acquire, \
             patch.object(session, "stop_frontends") as stop, patch.object(session.os, "fork", return_value=202), \
             patch.object(session.os, "write", return_value=1), patch.object(session.os, "waitpid", side_effect=wait_guardian), \
             patch.object(session.signal, "signal"), patch.object(session.subprocess, "Popen", side_effect=spawn), \
             patch.object(session, "execute_system_action", side_effect=action):
            result=session.main()
        acquire.assert_not_called();stop.assert_not_called()
        return result,order

    def test_boot_warning_is_forwarded_and_terminal_action_follows_guardian_cleanup(self):
        result,order=self.fake_boot_terminal_exit(76)
        self.assertEqual(result,0);self.assertEqual(order,["guardian"])

    def test_ordinary_return_stock_never_dispatches_a_surviving_committed_request(self):
        result,order=self.fake_boot_terminal_exit(0)
        self.assertEqual(result,0);self.assertEqual(order,["guardian"])

    def test_interrupted_launcher_never_dispatches_a_surviving_committed_request(self):
        result,order=self.fake_boot_terminal_exit(143)
        self.assertEqual(result,143);self.assertEqual(order,["guardian"])

    def test_launcher_acknowledgement_cannot_dispatch_an_uncommitted_request(self):
        with self.assertRaisesRegex(ValueError,"without a committed request"):
            self.fake_boot_terminal_exit(76,request_state="pending")

    def test_real_guardian_fork_dispatches_after_cleanup_without_restarting_stock(self):
        import json
        script=self.root/'bin/rgds-launcher';script.parent.mkdir()
        script.write_text("#!" + sys.executable + "\n" + r"""
import json,sys
from pathlib import Path
data=Path(sys.argv[sys.argv.index('--data')+1]);owner=json.loads((data/'session/handoff-owner.json').read_text())
(data/'session/exit.request').write_text('schema=2\naction=poweroff\nowner_pid='+str(owner['pid'])+'\nowner_start='+owner['start']+'\ntoken='+'a'*32+'\nstate=committed\n')
raise SystemExit(76)
""")
        script.chmod(0o755)
        events=self.root/'events'
        def record(event):
            with events.open('a') as output:output.write(event+'\n')
        def dispatch(action,data):
            self.assertEqual(action,'poweroff')
            self.assertNotEqual(os.getpid(),parent)
            self.assertTrue(self.marker.exists())
            record('init')
        parent=os.getpid();handlers={s:signal.getsignal(s) for s in (signal.SIGTERM,signal.SIGINT)}
        try:
            with patch.object(session.sys,'platform','linux'),patch('file_sharing.Sharing.stop'),patch.object(session,'release',return_value=self.root), \
                 patch.object(session,'environment',return_value=os.environ.copy()),patch.object(session,'lock'), \
                 patch.object(session,'unlock',side_effect=lambda:record('unlock')),patch.object(session,'Hardware'), \
                 patch.object(session,'process_identity',return_value='123'),patch.object(session,'discover_frontends',return_value=[]), \
                 patch.object(session,'acquire_stock',side_effect=lambda owner,**kw:handoff.acquire_stock(owner,marker=self.marker,script=self.script)), \
                 patch.object(session,'stop_frontends'),patch.object(session,'group_members',return_value=[]), \
                 patch.object(session,'stop_owned_group',side_effect=lambda *a:record('workers')), \
                 patch.object(session,'recover_hardware',side_effect=lambda *a:record('hardware')), \
                 patch.object(session,'restore',side_effect=lambda *a:record('restore')), \
                 patch.object(session,'execute_system_action',side_effect=dispatch):
                result=session.main(['--root',str(self.root)])
            self.assertEqual(result,0)
            self.assertEqual(events.read_text().splitlines(),['workers','hardware','init','unlock'])
        finally:
            for number,handler in handlers.items():signal.signal(number,handler)

    def test_guardian_messages_handle_fragmented_and_coalesced_pipe_writes(self):
        messages=session.GuardianMessages()
        messages.feed(b'{"pid":123,')
        self.assertIsNone(messages.owner)
        messages.feed(b'"start":"456"}\n{"action":"power')
        self.assertEqual(messages.owner,dict(pid=123,start="456"))
        self.assertIsNone(messages.action)
        messages.feed(b'off"}\n')
        self.assertEqual(messages.action,"poweroff")
        with self.assertRaises(ValueError):messages.feed(b'{"action":"reboot"}\n')

    def test_guardian_rejects_invalid_owner_and_action(self):
        for value in (b'{"action":"poweroff"}\n', b'{"pid":true,"start":"456"}\n',
                      b'{"pid":123,"start":null}\n', b'x'*4097):
            with self.assertRaises(ValueError):session.GuardianMessages().feed(value)
        messages=session.GuardianMessages()
        messages.feed(b'{"pid":123,"start":"456"}\n')
        with self.assertRaises(ValueError):messages.feed(b'{"action":"halt-force"}\n')

    def test_terminal_handoff_does_not_restart_stock_frontend_or_release_its_flag(self):
        lease=self.lease()
        with patch.object(session,'execute_system_action') as execute, patch.object(session,'restore') as restore:
            result=session.finish_handoff([],lease,'poweroff',self.data)
        self.assertEqual(result,session.SYSTEM_DISPATCH_ACK)
        execute.assert_called_once_with('poweroff',self.data)
        restore.assert_not_called()
        self.assertTrue(self.marker.exists())

    def test_failed_terminal_handoff_restores_stock_and_releases_only_our_flag(self):
        lease=self.lease()
        with patch.object(session,'execute_system_action',side_effect=OSError('refused')), \
             patch.object(session,'restore',side_effect=lambda records,lease:handoff.release_stock_flag(lease)) as restore:
            with self.assertRaises(OSError):session.finish_handoff([],lease,'poweroff',self.data)
        restore.assert_called_once_with([],lease)
        self.assertFalse(self.marker.exists())

    def test_boot_terminal_handoff_acquires_stop_flag_before_dispatch(self):
        def dispatch(action, data):
            self.assertTrue(self.marker.exists())
        with patch.object(session, 'process_identity', return_value='999'), \
             patch.object(session, 'acquire_stock', side_effect=lambda owner: handoff.acquire_stock(owner, marker=self.marker, script=self.script)), \
             patch.object(session, 'execute_system_action', side_effect=dispatch) as execute, \
             patch.object(session, 'restore') as restore:
            self.assertEqual(session.finish_handoff([], None, 'reboot', self.data), session.SYSTEM_DISPATCH_ACK)
        execute.assert_called_once_with('reboot', self.data)
        restore.assert_not_called()
        self.assertTrue(self.marker.exists())

    def test_standalone_terminal_handoff_does_not_acquire_stock_flag(self):
        with patch.object(session, 'acquire_stock') as acquire, \
             patch.object(session, 'execute_system_action') as execute:
            self.assertEqual(session.finish_handoff([], None, 'poweroff', self.data, standalone=True), session.SYSTEM_DISPATCH_ACK)
        acquire.assert_not_called()
        execute.assert_called_once_with('poweroff', self.data, standalone=True)

    def test_ordinary_handoff_still_restores_stock_without_system_action(self):
        lease=self.lease()
        with patch.object(session,'execute_system_action') as execute,patch.object(session,'restore') as restore:
            self.assertEqual(session.finish_handoff([],lease,None,self.data),0)
        restore.assert_called_once_with([],lease);execute.assert_not_called()

    def test_cleanup_path_cannot_resolve_legacy_helpers(self):
        with patch.dict(os.environ, {"PATH":"/old/simpleos/bin:/usr/bin", "LD_LIBRARY_PATH":"/old/simpleos/lib", "SIMPLEOS_ROOT":"/old"}):
            environment=handoff.clean_environment()
        self.assertNotIn("simpleos",environment["PATH"])
        self.assertEqual(environment["PATH"],"/usr/sbin:/usr/bin:/sbin:/bin")
        self.assertNotIn("LD_LIBRARY_PATH",environment);self.assertNotIn("SIMPLEOS_ROOT",environment)


if __name__ == "__main__":unittest.main()
