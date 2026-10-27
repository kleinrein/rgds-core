from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime"))

import linux_platform
from linux_platform import cpu_governor


class CpuGovernorTests(unittest.TestCase):
    def test_sets_the_governor_on_rgds_os_and_returns_the_one_before(self):
        with tempfile.TemporaryDirectory() as tmp:
            node = Path(tmp) / "scaling_governor"
            node.write_text("schedutil\n")
            with patch.object(linux_platform, "rgds_os", return_value=True):
                self.assertEqual(cpu_governor("performance", node), "schedutil")
                self.assertEqual(node.read_text(), "performance")
                # Putting it back: the game's exit restores what the session booted with.
                self.assertEqual(cpu_governor("schedutil", node), "performance")
            self.assertEqual(node.read_text(), "schedutil")

    def test_leaves_stock_firmware_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            node = Path(tmp) / "scaling_governor"
            node.write_text("ondemand\n")
            with patch.object(linux_platform, "rgds_os", return_value=False):
                self.assertIsNone(cpu_governor("performance", node))
            self.assertEqual(node.read_text(), "ondemand\n")

    def test_missing_cpufreq_is_not_an_error(self):
        with patch.object(linux_platform, "rgds_os", return_value=True):
            self.assertIsNone(cpu_governor("performance", Path("/nonexistent/scaling_governor")))


class SystemCommandTests(unittest.TestCase):
    def test_rgds_os_uses_busybox_init_and_its_own_suspend_script(self):
        from linux_platform import system_command, RGDS_OS_POWER
        with patch.object(linux_platform, "rgds_os", return_value=True):
            self.assertEqual(system_command("suspend"), [str(RGDS_OS_POWER), "suspend"])
            self.assertEqual(system_command("reboot"), ["/sbin/reboot"])
            self.assertEqual(system_command("poweroff"), ["/sbin/poweroff"])
            with self.assertRaises(ValueError):
                system_command("halt")


if __name__ == "__main__":
    unittest.main()
