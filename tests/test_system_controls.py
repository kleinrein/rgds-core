"""Validate hardware writes against an isolated sysfs tree, never host hardware."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from system_controls import Hardware


class SystemControlTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sysfs = self.root / "sys"
        self.hardware = Hardware(self.root / "userdata", self.sysfs)

    def put(self, name, value):
        path = self.sysfs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(str(value))
        return path

    def backlight(self, name="top", maximum=255, level=100):
        self.put(f"class/backlight/{name}/max_brightness", maximum)
        return self.put(f"class/backlight/{name}/brightness", level)

    def clocks(self, name="devices/system/cpu/cpufreq/policy0", prefix="scaling_"):
        values = {prefix + "min_freq": "400", prefix + "max_freq": "1200",
                  prefix + "governor": "schedutil", prefix + "available_frequencies": "1600 400 800 1200",
                  ("scaling_available_governors" if prefix else "available_governors"): "schedutil performance"}
        # CPU's available-frequency spelling differs from devfreq.
        for key, value in values.items():
            self.put(name + "/" + key, value)
        return self.sysfs / name

    def test_status_detects_capabilities_and_real_battery(self):
        self.assertFalse(self.hardware.status()["brightness_supported"])
        self.assertEqual(self.hardware.status()["battery"], -1)
        self.backlight(level=128)
        self.clocks()
        self.put("class/power_supply/battery/type", "Battery")
        self.put("class/power_supply/battery/capacity", "73")
        self.put("class/power_supply/battery/status", "Charging")
        self.put("power/state", "freeze mem")
        self.put("class/anbernic_misc/hallkey", "1")
        self.put("devices/platform/anbernic_misc/power/wakeup", "disabled")
        status = self.hardware.status()
        self.assertEqual(status, {"brightness_supported": 1, "power_supported": 1,
                                 "sleep_supported": 1, "lid_supported": 1,
                                 "brightness": 50, "battery": 73, "charging": 1})

    def test_two_backlights_scale_without_blanking_and_restore_originals(self):
        top = self.backlight()
        bottom = self.backlight("bottom", 20, 13)
        self.hardware.brightness(5)
        self.assertEqual(top.read_text().strip(), "13")
        self.assertEqual(bottom.read_text().strip(), "1")
        self.hardware.brightness(100)
        self.assertEqual(top.read_text().strip(), "255")
        self.assertEqual(bottom.read_text().strip(), "20")
        self.hardware.restore()
        self.assertEqual(top.read_text().strip(), "100")
        self.assertEqual(bottom.read_text().strip(), "13")
        self.assertFalse(self.hardware.journal.exists())

    def test_failed_second_panel_write_rolls_back_first(self):
        first = self.backlight("a")
        second = self.backlight("b")
        original_write = Path.write_text
        attempts = []
        def fail_once(path, value, *args, **kwargs):
            if path == second and not attempts:
                attempts.append(path)
                raise PermissionError("Panel unavailable")
            return original_write(path, value, *args, **kwargs)
        with patch.object(Path, "write_text", fail_once):
            with self.assertRaises(PermissionError):
                self.hardware.brightness(90)
        self.assertEqual(first.read_text().strip(), "100")
        self.assertEqual(second.read_text().strip(), "100")
        self.hardware.restore()

    def test_profiles_use_advertised_frequencies_and_legal_restore_order(self):
        cpu = self.clocks()
        gpu = self.clocks("class/devfreq/fde60000.gpu", "")
        original_write = Path.write_text
        def constrained_write(path, value, *args, **kwargs):
            if path.name in ("scaling_min_freq", "min_freq"):
                maximum = path.with_name(path.name.replace("min", "max"))
                self.assertLessEqual(int(value), int(maximum.read_text()))
            elif path.name in ("scaling_max_freq", "max_freq"):
                minimum = path.with_name(path.name.replace("max", "min"))
                self.assertGreaterEqual(int(value), int(minimum.read_text()))
            return original_write(path, value, *args, **kwargs)
        with patch.object(Path, "write_text", constrained_write):
            self.hardware.power(3)
            self.assertEqual((cpu / "scaling_min_freq").read_text().strip(), "1600")
            self.hardware.power(1)
            self.assertEqual((cpu / "scaling_max_freq").read_text().strip(), "800")
            self.hardware.power(2)
            self.assertEqual((gpu / "max_freq").read_text().strip(), "1200")
            self.hardware.power(0)
            self.assertEqual((cpu / "scaling_governor").read_text().strip(), "schedutil")
            self.hardware.power(3)
            self.hardware.restore()
        self.assertEqual((cpu / "scaling_min_freq").read_text().strip(), "400")
        self.assertEqual((gpu / "max_freq").read_text().strip(), "1200")

    def test_invalid_or_unavailable_controls_never_create_journal(self):
        for method, value in ((self.hardware.brightness, 0), (self.hardware.brightness, 101),
                              (self.hardware.power, 9), (self.hardware.power, 1), (self.hardware.lid, True)):
            with self.assertRaises(ValueError):
                method(value)
        with self.assertRaises(ValueError):
            self.hardware.sleep()
        self.assertFalse(self.hardware.journal.exists())

    def test_sleep_restores_panels_on_resume_and_failure_and_preserves_variant(self):
        backlight = self.backlight()
        state = self.put("power/state", "freeze mem")
        variant = self.put("power/mem_sleep", "s2idle [deep]")
        with patch("system_controls.os.sync"):
            self.hardware.sleep()
        self.assertEqual(backlight.read_text().strip(), "100")
        self.assertEqual(state.read_text().strip(), "mem")
        self.assertEqual(variant.read_text(), "s2idle [deep]")
        state.write_text("freeze mem")
        original_write = Path.write_text
        def rejected_sleep(path, value, *args, **kwargs):
            if path == state:
                raise OSError("Suspend rejected")
            return original_write(path, value, *args, **kwargs)
        with patch.object(Path, "write_text", rejected_sleep), patch("system_controls.os.sync"):
            with self.assertRaises(OSError):
                self.hardware.sleep()
        self.assertEqual(backlight.read_text().strip(), "100")

    def test_lid_wake_and_recovery_after_a_new_controller_is_created(self):
        self.put("power/state", "mem")
        self.put("class/anbernic_misc/hallkey", "1")
        wake = self.put("devices/platform/anbernic_misc/power/wakeup", "disabled")
        self.hardware.lid(True)
        self.assertEqual(wake.read_text().strip(), "enabled")
        Hardware(self.root / "userdata", self.sysfs).restore()
        self.assertEqual(wake.read_text().strip(), "disabled")

    def test_recovery_rejects_paths_outside_sysfs(self):
        self.hardware.save_journal({"../../outside/brightness": "1"})
        with self.assertRaises(ValueError):
            self.hardware.restore()
        self.assertTrue(self.hardware.journal.exists())

    def test_recovery_attempts_other_controls_when_one_panel_fails(self):
        first = self.backlight("a")
        second = self.backlight("b")
        self.hardware.brightness(100)
        original_write = Path.write_text
        def broken_panel(path, value, *args, **kwargs):
            if path == first:
                raise OSError("Panel unavailable")
            return original_write(path, value, *args, **kwargs)
        with patch.object(Path, "write_text", broken_panel):
            with self.assertRaisesRegex(OSError, "recovery incomplete"):
                self.hardware.restore()
        self.assertEqual(second.read_text().strip(), "100")
        self.assertTrue(self.hardware.journal.exists())
        self.hardware.restore()
        self.assertEqual(first.read_text().strip(), "100")


if __name__ == "__main__":
    unittest.main()
