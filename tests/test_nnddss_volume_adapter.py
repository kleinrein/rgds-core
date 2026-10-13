"""External helper protocol obeys the shared RGDS saved-volume policy."""
import contextlib
import io
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import nnddss_volume


class LoaderVolumeTests(unittest.TestCase):
    def run_command(self, arguments, keys="0"):
        volume = MagicMock();volume.mixer.query.return_value = 20
        output = io.StringIO()
        with patch.dict(os.environ, RGDS_VOLUME_HELPER=str(ROOT / "runtime/audio.py"), RGDS_VOLUME_DATA="/fixture/userdata", RGDS_VOLUME_KEYS=keys), patch("audio.Volume", return_value=volume), contextlib.redirect_stdout(output):
            result = nnddss_volume.main(arguments)
        return result, volume, output.getvalue()

    def test_native_initialization_reapplies_saved_level_instead_of_native_dac_value(self):
        result, volume, output = self.run_command(["--set-once", "255"])
        self.assertEqual(result, 0);volume.apply.assert_called_once_with()

    def test_physical_keys_are_not_applied_twice_by_the_loader(self):
        for command in ("--up", "--down"):
            result, volume, output = self.run_command([command], keys="1")
            self.assertEqual(result, 0);volume.apply.assert_called_once_with(delta=0)
            self.assertEqual(output, "51\n")

    def test_loader_keys_work_without_physical_watcher(self):
        for command, delta in (("--up", 5), ("--down", -5)):
            result, volume, _ = self.run_command([command])
            self.assertEqual(result, 0);volume.apply.assert_called_once_with(delta=delta)

    def test_get_and_watch_do_not_create_another_background_volume_process(self):
        result, volume, output = self.run_command(["--get"])
        self.assertEqual(result, 0);self.assertEqual(output, "51\n");volume.apply.assert_not_called()
        result, volume, _ = self.run_command(["--watch"])
        self.assertEqual(result, 0);volume.apply.assert_not_called()

    def test_invalid_protocol_and_missing_policy_do_not_apply_volume(self):
        for raw in ("-1", "256", "wrong"):
            with self.assertRaises(ValueError): self.run_command(["--set-once", raw])
        self.assertEqual(self.run_command(["--unknown"])[0], 2)
        with patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(ValueError, "through RGDS"):
            nnddss_volume.main(["--get"])


if __name__ == "__main__": unittest.main()
