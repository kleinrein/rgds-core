"""Settings restart and power off through init, and keep the startup choice in boot.ini."""
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
import power


class DeviceLifecycleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "rgds"
        self.service = Services(self.root / "app", self.root / "userdata")
        self.service.data.mkdir(parents=True)

    def test_reboot_asks_init_and_poweroff_also_arms_resume(self):
        with patch("power.execute_system_action") as execute, patch("quick_resume.arm") as arm:
            self.service.reboot()
            arm.assert_not_called()
            self.service.poweroff()
        self.assertEqual([call.args[0] for call in execute.call_args_list], ["reboot", "poweroff"])
        arm.assert_called_once_with(self.service.data)

    def test_failed_poweroff_disarms_resume_and_reports_the_error(self):
        armed = self.service.data / "resume/armed.json"

        def arm(data):
            armed.parent.mkdir(parents=True)
            armed.write_text("{}")

        with patch("quick_resume.arm",
                   side_effect=arm), patch("power.execute_system_action", side_effect=OSError("refused")):
            with self.assertRaises(OSError):
                self.service.poweroff()
        self.assertFalse(armed.exists())

    def test_running_engine_blocks_the_system_action(self):
        with patch("emulators.core_running", return_value=True), patch("subprocess.run") as run:
            with self.assertRaisesRegex(ValueError, "engine is still active"):
                power.execute_system_action("reboot", self.service.data)
        run.assert_not_called()

    def test_action_runs_the_init_command_and_records_the_outcome(self):
        completed = subprocess.CompletedProcess([], 0)
        with patch("emulators.core_running",
                   return_value=False), patch("linux_platform.system_command", return_value=[
                       "/sbin/reboot"
                   ]), patch("power.subprocess.run", return_value=completed) as run:
            power.execute_system_action("reboot", self.service.data)
        self.assertEqual(run.call_args.args[0], ["/sbin/reboot"])
        self.assertIn("request-accepted", (self.service.data / "logs/system-exit.ini").read_text())

    def test_unknown_action_is_refused(self):
        with self.assertRaises(ValueError):
            power.execute_system_action("halt", self.service.data)

    def test_preview_startup_and_shutdown_are_simulated_without_machine_changes(self):
        self.service.preview = True
        with patch("power.execute_system_action") as execute:
            self.service.startup(dict(mode="rgds", warning=False))
            self.service.reboot()
        execute.assert_not_called()
        self.assertEqual(self.service.fields["boot_mode"], "rgds")
        self.assertEqual(self.service.fields["boot_warning"], "0")
        self.assertIn("simulated", self.service.fields["message"])

    def test_anbernic_startup_is_refused_outside_the_preview(self):
        with self.assertRaisesRegex(ValueError, "unavailable in RGDS OS"):
            self.service.startup(dict(mode="stock"))

    def test_startup_choice_is_saved_and_requires_home(self):
        with patch("emulators.core_running", return_value=False):
            self.service.startup(dict(mode="rgds", warning=True))
        self.assertEqual(self.service.fields["boot_warning"], "1")
        self.assertIn("warning=1", (self.service.data / "boot.ini").read_text())
        with patch("emulators.core_running", return_value=True):
            with self.assertRaisesRegex(ValueError, "Return home"):
                self.service.startup(dict(mode="rgds", warning=False))

    def test_invalid_startup_payload_is_refused(self):
        for payload in ([], None, {"mode": "unknown"}, {"mode": "rgds", "warning": "yes"}):
            with self.subTest(payload=payload):
                with self.assertRaises(ValueError):
                    self.service.startup(payload)
        self.assertFalse((self.service.data / "boot.ini").exists())

    def test_wifi_command_uses_a_clean_environment_without_old_preloads(self):
        completed = subprocess.CompletedProcess([], 0, stdout="enabled", stderr="")
        with patch.dict(
            os.environ, RGDS_ROOT="/private", LD_PRELOAD="old.so", LD_LIBRARY_PATH="/old/lib", PATH="/old/bin"
        ), patch("device_services.subprocess.run", return_value=completed) as run:
            self.assertEqual(self.service.command(["radio", "wifi"]), "enabled")
        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["PATH"], "/usr/sbin:/usr/bin:/sbin:/bin")
        self.assertFalse(any(key.startswith("RGDS_") for key in environment))
        self.assertNotIn("LD_PRELOAD", environment)
        self.assertNotIn("LD_LIBRARY_PATH", environment)


if __name__ == "__main__": unittest.main()
