"""The first frame gates optional network work, and the guardian owns its worker."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import session
import boot
import activate


class DeferredBootWorkTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.data = Path(temporary.name)
        (self.data / "session").mkdir()
        reader, self.writer = socket.socketpair()
        self.addCleanup(self.writer.close)
        self.work = session.BootWork(
            "/owned/releases/test-build",
            self.data,
            reader.detach(),
            Path("/owned/config.json"),
            dict(began=10, verify_seconds=.25),
            began=12
        )
        self.addCleanup(self.work.close)

    def test_no_network_before_first_frame_and_exact_ready_notification_records_timings(self):
        with patch("session.subprocess.Popen") as launch, patch("session.time.monotonic", return_value=14
                                                                ), patch("session.kernel_uptime", return_value=7.25):
            launch.return_value.poll.return_value = None
            self.work.poll()
            launch.assert_not_called()
            self.assertFalse((self.data / "session/boot-startup.json").exists())
            self.writer.send(b"1")
            self.work.displayed()
            self.assertEqual(launch.call_count, 1)
            self.assertEqual(
                launch.call_args.args[0][-4:],
                ["--deferred", "/owned/config.json", "--result",
                 str(self.data / "session/boot-network.json")]
            )
            self.assertTrue(launch.call_args.kwargs["start_new_session"])
            record = json.loads((self.data / "session/boot-startup.json").read_text())
            self.assertEqual(record["hook_to_first_frame_seconds"], 4)
            self.assertEqual(record["session_to_first_frame_seconds"], 2)
            self.assertEqual(record["verify_seconds"], .25)
            self.assertEqual(record["kernel_boot_to_first_frame_seconds"], 7.25)
            self.assertEqual(record["timing_source"], "guardian_first_present_ack")
            self.work.close()
            launch.return_value.terminate.assert_called_once()
            launch.return_value.wait.assert_called_once_with(timeout=.5)

    def test_closed_launcher_never_starts_network_or_reports_first_frame(self):
        self.writer.close()
        with patch("session.subprocess.Popen") as launch:
            self.work.displayed()
            launch.assert_not_called()
        self.assertIsNone(self.work.ready_fd)
        self.assertFalse((self.data / "session/boot-startup.json").exists())

    def test_new_boot_discards_previous_readiness_reports_before_waiting_for_ack(self):
        for name in ("boot-startup.json", "boot-network.json"):
            (self.data / "session" / name).write_text("previous boot")
        reader, writer = socket.socketpair()
        work = session.BootWork("/owned/releases/new-build", self.data, reader.detach())
        try:
            self.assertFalse((self.data / "session/boot-startup.json").exists())
            self.assertFalse((self.data / "session/boot-network.json").exists())
        finally:
            work.close()
            writer.close()

    def test_completed_worker_is_reaped_and_not_signalled_on_session_exit(self):
        with patch("session.subprocess.Popen") as launch:
            self.writer.send(b"1")
            self.work.displayed()
            process = launch.return_value
            process.poll.return_value = 0
            self.work.poll()
            self.work.close()
            process.terminate.assert_not_called()
            process.kill.assert_not_called()
            self.assertIsNone(self.work.process)

    def test_stalled_owned_worker_is_stopped_with_bounded_waits(self):
        with patch("session.subprocess.Popen") as launch:
            self.writer.send(b"1")
            self.work.displayed()
            process = launch.return_value
            process.poll.return_value = None
            process.wait.side_effect = [subprocess.TimeoutExpired("owned", .5), 0]
            self.work.close()
            process.terminate.assert_called_once()
            process.kill.assert_called_once()
            self.assertEqual([call.kwargs["timeout"] for call in process.wait.call_args_list], [.5, .5])

    def test_direct_boot_still_hashes_and_inherited_receipt_never_skips_change_checks(self):
        root = self.data / "rgds"
        (root / "state").mkdir(parents=True)
        (root / "state/active").write_text("test-build\n")
        with patch("activate.verify_startup") as hashes, patch("activate.verify_startup_receipt") as reuse:
            session.release(root, boot=True)
            hashes.assert_called_once()
            reuse.assert_not_called()
            receipt = dict(schema=1, app="verified")
            session.release(root, boot=True, receipt=receipt)
            reuse.assert_called_once_with(root / "releases/test-build", receipt)
            self.assertEqual(hashes.call_count, 1)

    def test_private_in_process_api_uses_explicit_argv_and_rechecks_active_release(self):
        root = self.data / "rgds"
        (root / "state").mkdir(parents=True)
        (root / "state/active").write_text("new-build\n")
        receipt = dict(schema=1, app=str(root / "releases/old-build"), files={})
        with patch("session.sys.platform", "linux"), patch("file_sharing.Sharing.stop"), patch("session.environment") as environment, \
             patch.object(sys, "argv", ["unrelated caller", "--invalid-option"]):
            with self.assertRaisesRegex(ValueError, "Invalid startup verification receipt"):
                session.main(argv=["--root", str(root), "--boot"], startup_receipt=receipt)
        environment.assert_not_called()
        with self.assertRaisesRegex(ValueError, "managed boot"):
            session.main(argv=["--root", str(root)], startup_receipt=receipt)
        with self.assertRaisesRegex(ValueError, "only one"):
            session.main(argv=["--root", str(root), "--boot", "--startup-fd", "3"], startup_receipt=receipt)

    def test_managed_session_delivers_first_frame_socket_and_owns_deferred_worker(self):
        root = self.data / "rgds"
        app = root / "releases/test-build"
        (app / "bin").mkdir(parents=True)
        (app / "runtime").mkdir()
        (root / "userdata/session").mkdir(parents=True)
        launcher = app / "bin/rgds-launcher"
        launcher.write_text(
            "#!" + sys.executable + "\nimport socket, sys, time\n"
            "fd = int(sys.argv[sys.argv.index('--boot-ready-fd') + 1])\n"
            "ready = socket.socket(fileno=fd);ready.send(b'1');ready.close()\n"
            "time.sleep(.3)\n"
        )
        launcher.chmod(0o755)
        (app / "runtime/boot_network.py").write_text(
            "import pathlib, sys\n"
            "pathlib.Path(sys.argv[sys.argv.index('--result') + 1]).write_text('deferred worker ran')\n"
        )
        began = time.monotonic()
        with patch("session.sys.platform", "linux"), patch("file_sharing.Sharing.stop"), patch("session.release", return_value=app), \
             patch("session.environment", return_value=dict(os.environ)), patch("session.lock"), \
             patch("session.unlock"), patch("session.Hardware.restore"), \
             patch("session.process_identity", return_value="1"), patch("session.discover_frontends", return_value=[]), \
             patch("session.begin_session"), patch("session.end_session"), patch("session.group_members", return_value=[]), \
             patch("session.stop_owned_group"), patch("session.recover_hardware"), patch("session.restore"), \
             patch("session.consume_exit", return_value=None), patch("session.signal.signal"), \
             patch.object(sys, "argv", ["session.py", "--root", str(root), "--boot", "--boot-network-config", "/owned/config.json"]):
            self.assertEqual(session.main(), 0)
        record = json.loads((root / "userdata/session/boot-startup.json").read_text())
        self.assertEqual(record["build"], "test-build")
        self.assertLess(record["session_to_first_frame_seconds"], time.monotonic() - began - .1)
        self.assertEqual((root / "userdata/session/boot-network.json").read_text(), "deferred worker ran")

    def test_in_process_boot_keeps_session_pid_and_full_guardian_recovery_lifecycle(self):
        root = (self.data / "rgds").resolve()
        app = root / "releases/test-build"
        (app / "bin").mkdir(parents=True)
        (app / "runtime").mkdir()
        (root / "state").mkdir()
        (root / "state/active").write_text("test-build\n")
        data = root / "userdata"
        (data / "session").mkdir(parents=True)
        launcher = app / "bin/rgds-launcher"
        launcher.write_text(
            "#!" + sys.executable + "\nimport json, pathlib, socket, sys, time\n"
            "pathlib.Path(" + repr(str(data / "command.json")) + ").write_text(json.dumps(sys.argv))\n"
            "fd = int(sys.argv[sys.argv.index('--boot-ready-fd') + 1])\n"
            "ready = socket.socket(fileno=fd);ready.send(b'1');ready.close();time.sleep(.2)\n"
        )
        launcher.chmod(0o755)
        (app / "runtime/boot_network.py").write_text(
            "import pathlib, sys\n"
            "pathlib.Path(sys.argv[sys.argv.index('--result') + 1]).write_text('worker finished')\n"
        )
        source = (ROOT / "runtime/session.py").read_text()
        source += '''
process_identity = lambda pid: "1"
environment = lambda app: dict(os.environ)
discover_frontends = lambda **kwargs: []
lock = lambda: None
unlock = lambda: None
group_members = lambda owned: []
stop_owned_group = lambda *args: None
recover_hardware = lambda *args: None
consume_exit = lambda *args: None
class Hardware:
    def __init__(self, data):pass
    def restore(self):pass
original_begin_session = begin_session
def begin_session(data, owner):
    (data / "owner-observed.json").write_text(json.dumps(owner))
    original_begin_session(data, owner)
'''
        (app / "runtime/session.py").write_text(source)
        control = self.data / "control"
        control.mkdir()
        (control / "enabled").write_text("1")
        (control / "config.json").write_text(json.dumps(dict(schema=1, mode="rgds", warning=False, root=str(root))))
        receipt = dict(schema=1, app=str(app), files=activate.release_inventory(app))
        before = dict(os.environ)
        with patch("session.sys.platform", "linux"), patch("file_sharing.Sharing.stop"), patch("boot.same_runtime", return_value=True), \
             patch("activate.verify_startup", return_value=receipt):
            self.assertEqual(boot.run(control, self.data / "run"), 0)
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(json.loads((data / "owner-observed.json").read_text())["pid"], os.getpid())
        self.assertFalse((data / "session/handoff-owner.json").exists())
        self.assertFalse((data / "session/launcher.json").exists())
        self.assertNotIn("--theme", json.loads((data / "command.json").read_text()))
        self.assertEqual((data / "session/boot-network.json").read_text(), "worker finished")
        self.assertEqual(json.loads((data / "session/boot-startup.json").read_text())["dispatch_mode"], "in_process")
        self.assertEqual(json.loads((self.data / "run/rgds-boot-result.json").read_text())["exit"], 0)


if __name__ == "__main__": unittest.main()
