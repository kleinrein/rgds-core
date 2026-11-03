from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime"))

import linux_platform
from linux_platform import all_cpus


class AllCpusTests(unittest.TestCase):
    def run_with_mask(self, rgds_os, wanted=True, fail=False):
        mask = [{0, 1, 2}]

        def set_mask(pid, cpus):
            if fail: raise OSError(22, "Invalid argument")
            mask[0] = set(cpus)

        inside = []
        with patch.object(linux_platform, "rgds_os", return_value=rgds_os), \
             patch.object(linux_platform.os, "sched_getaffinity", create=True, side_effect=lambda pid: set(mask[0])), \
             patch.object(linux_platform.os, "sched_setaffinity", create=True, side_effect=set_mask), \
             patch.object(linux_platform.os, "cpu_count", return_value=4):
            with all_cpus(wanted):
                inside.append(set(mask[0]))
        return inside[0], mask[0]

    def test_a_game_starts_on_every_cpu_and_the_launcher_goes_back_to_its_own(self):
        # DSCore read "host: 3 cores" from the session's mask, pinned nothing and left CPU 3 idle.
        self.assertEqual(self.run_with_mask(True), ({0, 1, 2, 3}, {0, 1, 2}))

    def test_not_wanted_or_stock_leaves_the_mask(self):
        self.assertEqual(self.run_with_mask(True, wanted=False), ({0, 1, 2}, {0, 1, 2}))
        self.assertEqual(self.run_with_mask(False), ({0, 1, 2}, {0, 1, 2}))

    def test_a_refused_mask_is_not_an_error(self):
        self.assertEqual(self.run_with_mask(True, fail=True), ({0, 1, 2}, {0, 1, 2}))

    def test_the_launch_uses_it_around_the_engine_only(self):
        # RG DS Play's pinned main loop keeps its other threads on the session's CPUs; the menu helper starts after.
        source = (Path(__file__).resolve().parents[1] / "runtime/emulators.py").read_text()
        guard = source.index("with all_cpus('RGDS_PLAY_CPU' not in plan['env']):")
        self.assertLess(guard, source.index("child = subprocess.Popen(", guard))
        self.assertLess(source.index("child = subprocess.Popen(", guard), source.index("menu = GameMenu(child", guard))


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
