"""Settings hand off reboot to the guardian and never manage stock services."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from device_services import Services
import stock_handoff


class DeviceLifecycleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "rgds"
        self.service = Services(self.root / "app", self.root / "userdata")

    def test_reboot_and_shutdown_only_request_current_session_exit(self):
        with patch("stock_handoff.request_exit") as request, patch("stock_handoff.commit_exit") as commit, patch("device_services.subprocess.run") as run:
            self.service.reboot();self.service.poweroff()
        self.assertEqual([call.args[1] for call in request.call_args_list], ["reboot", "poweroff"])
        self.assertEqual(commit.call_count,2)
        run.assert_not_called()
        self.assertEqual(self.service.fields["state"], "ready")

    def test_failed_exit_request_does_not_report_ready_or_run_system_action(self):
        with patch("stock_handoff.request_exit", side_effect=ValueError("Game is running")), patch("device_services.subprocess.run") as run:
            with self.assertRaisesRegex(ValueError, "Game is running"): self.service.reboot()
        run.assert_not_called();self.assertNotEqual(self.service.fields.get("state"), "ready")

    def pending_owner(self):
        owner=dict(pid=123,start="999");stock_handoff.begin_session(self.service.data,owner)
        return owner

    def test_failed_or_cancelled_publication_cannot_dispatch_a_terminal_action(self):
        for error in (OSError("status write failed"),KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                owner=self.pending_owner()
                with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
                     patch.object(self.service,"publish",side_effect=error),patch("device_services.subprocess.run") as run:
                    with self.assertRaises(type(error)):self.service.reboot()
                self.assertIsNone(stock_handoff.consume_exit(self.service.data,owner));run.assert_not_called()

    def test_failed_publication_and_failed_rollback_still_leave_no_eligible_request(self):
        owner=self.pending_owner()
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
             patch.object(self.service,"publish",side_effect=OSError("status unavailable")), \
             patch("stock_handoff.cancel_exit",side_effect=OSError("readonly rollback")):
            with self.assertRaises(OSError):self.service.poweroff()
        self.assertTrue(stock_handoff.request_path(self.service.data).exists())
        self.assertIsNone(stock_handoff.consume_exit(self.service.data,owner))

    def test_ready_is_published_before_the_current_request_becomes_eligible(self):
        owner=self.pending_owner();order=[];real_publish=self.service.publish;real_commit=stock_handoff.commit_exit
        def publish(**fields):
            self.assertEqual(stock_handoff._read_exit(stock_handoff.request_path(self.service.data))["state"],"pending")
            order.append("publish");real_publish(**fields)
        def commit(data,receipt):order.append("commit");real_commit(data,receipt)
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
             patch.object(self.service,"publish",side_effect=publish),patch("stock_handoff.commit_exit",side_effect=commit):
            self.service.reboot()
        self.assertEqual(order,["publish","commit"])
        self.assertEqual(stock_handoff.consume_exit(self.service.data,owner),"reboot")

    def test_failure_or_cancel_immediately_after_commit_rename_rolls_back_exact_receipt(self):
        real_commit=stock_handoff.commit_exit
        for error in (OSError("failure after rename"),KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                owner=self.pending_owner()
                def fail_after_replacement(data,receipt):
                    real_commit(data,receipt)
                    self.assertEqual(stock_handoff._read_exit(stock_handoff.request_path(data))["state"],"committed")
                    raise error
                with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
                     patch("stock_handoff.commit_exit",side_effect=fail_after_replacement):
                    with self.assertRaises(type(error)):self.service.reboot()
                self.assertIsNone(stock_handoff.consume_exit(self.service.data,owner))

    def test_failed_publication_never_cancels_a_different_workers_committed_request(self):
        owner=self.pending_owner()
        def publish_failure(**fields):
            successor=stock_handoff.request_exit(self.service.data,"poweroff")
            stock_handoff.commit_exit(self.service.data,successor)
            raise OSError("first worker publication failed")
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
             patch.object(self.service,"publish",side_effect=publish_failure):
            with self.assertRaises(OSError):self.service.reboot()
        self.assertEqual(stock_handoff.consume_exit(self.service.data,owner),"poweroff")

    def test_post_commit_failure_with_readonly_rollback_cannot_report_success(self):
        self.pending_owner();real_commit=stock_handoff.commit_exit
        def commit_then_fail(data,receipt):
            real_commit(data,receipt)
            raise OSError("worker failed after replacement")
        with patch("emulators.process_identity",return_value="999"),patch("emulators.core_running",return_value=False), \
             patch("stock_handoff.commit_exit",side_effect=commit_then_fail), \
             patch("stock_handoff.cancel_exit",side_effect=OSError("readonly rollback")), \
             patch("device_services.subprocess.run") as run:
            with self.assertRaisesRegex(OSError,"after replacement"):self.service.reboot()
        self.assertEqual(stock_handoff._read_exit(stock_handoff.request_path(self.service.data))["state"],"committed")
        run.assert_not_called()
        # The separate session fixtures require private launcher acknowledgement
        # 76, so this failed worker cannot arm an ordinary ReturnStock exit0.

    def test_preview_startup_and_shutdown_are_simulated_without_machine_changes(self):
        self.service.preview = True
        with patch("stock_handoff.request_exit") as request, patch("boot_control.configure") as configure:
            self.service.startup(dict(mode="rgds", warning=False));self.service.reboot()
        request.assert_not_called();configure.assert_not_called()
        self.assertEqual(self.service.fields["boot_mode"], "rgds")
        self.assertEqual(self.service.fields["boot_warning"], "0")
        self.assertIn("simulated", self.service.fields["message"])

    def test_startup_option_targets_this_installation_and_requires_home(self):
        state = dict(installed=1, mode="rgds", warning=0, root=str(self.root))
        with patch("emulators.core_running", return_value=False), patch("boot_control.configure") as configure, patch("boot_control.inspect", return_value=state):
            self.service.startup(dict(mode="rgds", warning=False))
        configure.assert_called_once_with(self.root, "rgds", warning=False)
        with patch("emulators.core_running", return_value=True), patch("boot_control.configure") as configure:
            with self.assertRaisesRegex(ValueError, "Return home"): self.service.startup(dict(mode="stock"))
        configure.assert_not_called()

    def test_invalid_startup_payload_never_reaches_installer(self):
        for payload in ([], None, {"mode":"unknown"}, {"mode":"rgds", "warning":"yes"}):
            with self.subTest(payload=payload), patch("boot_control.configure") as configure:
                with self.assertRaises(ValueError): self.service.startup(payload)
                configure.assert_not_called()

    def test_wifi_command_uses_stock_binary_environment_without_old_preloads(self):
        completed = subprocess.CompletedProcess([], 0, stdout="enabled", stderr="")
        with patch.dict(os.environ, SIMPLEOS_ROOT="/old", RGDS_ROOT="/private", LD_PRELOAD="old.so", LD_LIBRARY_PATH="/old/lib", PATH="/old/bin"), patch("device_services.subprocess.run", return_value=completed) as run:
            self.assertEqual(self.service.command(["radio", "wifi"]), "enabled")
        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["PATH"], "/usr/sbin:/usr/bin:/sbin:/bin")
        self.assertFalse(any(key.startswith(("SIMPLEOS_", "RGDS_")) for key in environment))
        self.assertNotIn("LD_PRELOAD", environment);self.assertNotIn("LD_LIBRARY_PATH", environment)


if __name__ == "__main__": unittest.main()
