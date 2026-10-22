"""The GPU-session clock floor against an isolated sysfs tree, never host hardware."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from game_clocks import GameClocks
from system_controls import Hardware


class GameClockTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sysfs = self.root / "sys"
        self.data = self.root / "userdata"
        self.data.mkdir()

    def put(self, name, value):
        path = self.sysfs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(str(value) + "\n")
        return path

    def clock(self, name, prefix, minimum, maximum, available):
        self.put(f"{name}/{prefix}min_freq", minimum)
        self.put(f"{name}/{prefix}max_freq", maximum)
        self.put(f"{name}/{prefix}governor", "simple_ondemand" if not prefix else "schedutil")
        self.put(f"{name}/{prefix}available_frequencies", available)
        self.put(
            f"{name}/{prefix}available_governors",
            "simple_ondemand performance" if not prefix else "schedutil performance"
        )
        return self.sysfs / name

    def read(self, path):
        return path.read_text().strip()

    def test_hold_raises_gpu_and_memory_floors_and_release_puts_them_back(self):
        cpu = self.clock("devices/system/cpu/cpufreq/policy0", "scaling_", 408000, 1992000, "408000 1008000 1992000")
        gpu = self.clock(
            "class/devfreq/fde60000.gpu", "", 200000000, 800000000, "200000000 300000000 400000000 600000000 800000000"
        )
        dmc = self.clock("class/devfreq/dmc", "", 324000000, 1056000000, "324000000 528000000 1056000000")
        clocks = GameClocks(self.data, self.sysfs)
        self.assertEqual(sorted(clocks.hold()), ["dmc", "fde60000.gpu"])
        self.assertEqual(self.read(gpu / "min_freq"), "800000000")
        self.assertEqual(self.read(dmc / "min_freq"), "1056000000")
        self.assertEqual(self.read(gpu / "max_freq"), "800000000")
        self.assertEqual(self.read(gpu / "governor"), "simple_ondemand")
        self.assertEqual(self.read(cpu / "scaling_min_freq"), "408000")  # CPU policy untouched
        # The hardware journal knows the originals, so a crashed session restores them.
        self.assertEqual(Hardware(self.data, self.sysfs).saved()["class/devfreq/fde60000.gpu/min_freq"], "200000000")
        clocks.release()
        self.assertEqual(self.read(gpu / "min_freq"), "200000000")
        self.assertEqual(self.read(dmc / "min_freq"), "324000000")
        clocks.release()  # a second release is a no-op
        self.assertEqual(self.read(gpu / "min_freq"), "200000000")

    def test_floor_never_exceeds_a_power_profile_cap(self):
        gpu = self.clock(
            "class/devfreq/fde60000.gpu", "", 200000000, 600000000, "200000000 300000000 400000000 600000000 800000000"
        )
        clocks = GameClocks(self.data, self.sysfs)
        self.assertEqual(clocks.hold(), ["fde60000.gpu"])
        self.assertEqual(self.read(gpu / "min_freq"), "600000000")
        clocks.release()
        self.assertEqual(self.read(gpu / "min_freq"), "200000000")

    def test_already_pinned_clocks_are_left_alone(self):
        gpu = self.clock("class/devfreq/fde60000.gpu", "", 800000000, 800000000, "200000000 800000000")
        clocks = GameClocks(self.data, self.sysfs)
        self.assertEqual(clocks.hold(), [])
        self.assertFalse((self.data / "session/system-hardware.json").exists())
        clocks.release()
        self.assertEqual(self.read(gpu / "min_freq"), "800000000")

    def test_wanted_only_for_gpu_rendered_scaled_games(self):
        gpu_game = {"DS_GPU3D": "1", "RGDS_ACTIVE_SCALE": "2"}
        self.assertTrue(GameClocks.wanted(gpu_game, self.data))
        self.assertFalse(GameClocks.wanted({"DS_GPU3D": "0", "RGDS_ACTIVE_SCALE": "2"}, self.data))
        self.assertFalse(GameClocks.wanted({"DS_GPU3D": "1", "RGDS_ACTIVE_SCALE": "1"}, self.data))
        self.assertFalse(GameClocks.wanted({"DS_GPU3D": "1"}, self.data))
        self.assertFalse(GameClocks.wanted(dict(gpu_game, RGDS_GPU_FLOOR="0"), self.data))
        (self.data / "performance.ini").write_text("schema=1\ngpu_floor=0\n")
        self.assertFalse(GameClocks.wanted(gpu_game, self.data))


if __name__ == "__main__":
    unittest.main()
