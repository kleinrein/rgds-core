"""Exercise volume resets and hardware keys without opening host audio devices."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from audio import EVENT, Mixer, Volume, VolumeWatch, clean_environment
from library import read_fields
from system_controls import Hardware


class FakeMixer:
    def __init__(self, percent=100):
        self.percent = percent
        self.writes = []

    def query(self):
        return self.percent

    def set(self, level):
        self.writes.append(level)
        self.percent = 0 if level == 0 else 30 + round(70 * (level - 1) / 99)


class AudioTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        self.mixer = FakeMixer()
        self.volume = Volume(self.data, self.mixer)

    def test_unknown_full_mixer_is_adopted_quietly_and_saved(self):
        self.assertLessEqual(self.volume.apply(), 25)
        self.assertLess(self.mixer.percent, 50)
        self.assertEqual(self.volume.saved(), self.mixer.writes[-1])

    def test_unknown_mixer_below_the_speaker_floor_stays_muted(self):
        self.mixer.percent = 20
        self.assertEqual(self.volume.apply(), 0)
        self.assertEqual(self.mixer.percent, 0)

    def test_saved_volume_repaired_after_audio_open_without_double_steps(self):
        self.volume.apply(15)
        self.mixer.percent = 100  # Emulator opens PCM and resets the DAC.
        self.assertEqual(self.volume.apply(), 15)
        self.assertEqual(self.mixer.writes, [15, 15])
        self.volume.apply()
        self.assertEqual(self.mixer.writes, [15, 15], "Identical mixer writes hitch gameplay")
        self.assertEqual(Volume(self.data, self.mixer).saved(), 15)

    def test_mute_survives_restart_and_a_sink_reset(self):
        self.volume.apply(0)
        self.mixer.percent = 100
        self.assertEqual(Volume(self.data, self.mixer).apply(), 0)
        self.assertEqual(self.mixer.percent, 0)

    def test_sleep_restores_volume_before_returning_for_menu_and_game_callers(self):
        hardware = Hardware(self.data)
        for independent in (False, True):
            for lid in (False, True):
                for level in (0, 15, 65, 100):
                    with self.subTest(independent=independent, lid=lid, level=level):
                        self.volume.apply(level)
                        expected = self.mixer.percent
                        saved = self.volume.path.read_bytes()

                        def suspend(*args, **kwargs):
                            self.mixer.percent = 100  # Firmware resume reset.
                        with patch('system_controls.Volume', return_value=self.volume), \
                             patch.object(hardware, 'sleep_supported', return_value=True), \
                             patch.object(hardware, 'suspend_helper', return_value='/usr/sbin/pm-suspend'), \
                             patch.object(hardware, 'lid_closed', return_value=True), \
                             patch.object(hardware, 'lid'), \
                             patch.object(hardware, 'wait_closed', return_value=True), \
                             patch('linux_platform.independent', return_value=independent), \
                             patch('linux_platform.system_command', return_value=['systemctl', 'suspend']), \
                             patch('system_controls.suspend_linux', side_effect=suspend) as owned, \
                             patch('system_controls.subprocess.run', side_effect=suspend) as stock, \
                             patch('system_controls.print'):
                            for _ in range(10):
                                (hardware.sleep_lid if lid else hardware.sleep)()
                                self.assertEqual(self.mixer.percent, expected)
                                self.assertEqual(self.volume.path.read_bytes(), saved)
                            self.assertEqual(owned.call_count, 10 if independent else 0)
                            self.assertEqual(stock.call_count, 0 if independent else 10)

    def test_sleep_adopts_volume_before_reset_and_repairs_failed_resume_hooks(self):
        hardware = Hardware(self.data)
        self.mixer.percent = 37
        failure = subprocess.CalledProcessError(1, 'pm-suspend')

        def suspend(*args, **kwargs):
            self.assertIsNotNone(self.volume.saved())
            self.mixer.percent = 100
            raise failure
        with patch('system_controls.Volume', return_value=self.volume), \
             patch.object(hardware, 'sleep_supported', return_value=True), \
             patch.object(hardware, 'suspend_helper', return_value='/usr/sbin/pm-suspend'), \
             patch('linux_platform.independent', return_value=False), \
             patch('system_controls.subprocess.run', side_effect=suspend):
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                hardware.sleep()
        self.assertIs(caught.exception, failure)
        self.assertLessEqual(self.volume.saved(), 25)
        self.assertLess(self.mixer.percent, 50)
        self.assertEqual(self.mixer.writes[-1], self.volume.saved())

    def test_sleep_with_unavailable_audio_preserves_saved_volume_and_helper_error(self):
        hardware = Hardware(self.data)
        self.volume.apply(0)
        failure = subprocess.CalledProcessError(1, 'pm-suspend')
        with patch('system_controls.Volume', return_value=self.volume), \
             patch.object(self.mixer, 'query', side_effect=OSError('Codec unavailable')), \
             patch.object(hardware, 'sleep_supported', return_value=True), \
             patch.object(hardware, 'suspend_helper', return_value='/usr/sbin/pm-suspend'), \
             patch('linux_platform.independent', return_value=False), \
             patch('system_controls.subprocess.run', side_effect=[None, failure]) as suspend, \
             patch('system_controls.print'):
            hardware.sleep()
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                hardware.sleep()
        self.assertIs(caught.exception, failure)
        self.assertEqual(suspend.call_count, 2)
        self.assertEqual(self.volume.saved(), 0)

    def test_boundaries_and_failed_write_do_not_leave_inconsistent_volume(self):
        self.volume.apply(10)
        self.assertEqual(self.volume.apply(delta=-100), 0)
        self.assertEqual(self.volume.apply(delta=1000), 100)
        for invalid in (-1, 101):
            with self.assertRaises(ValueError):
                self.volume.apply(invalid)
        with patch("audio.write_fields", side_effect=OSError("Read-only card")):
            with self.assertRaises(OSError):
                self.volume.apply(5)
        self.assertEqual(self.volume.saved(), 100)
        self.assertEqual(self.mixer.writes[-1], 100)

    def test_held_physical_keys_repeat_speed_up_clamp_and_stop_on_release(self):
        self.volume.apply(10)
        watcher = VolumeWatch(self.data, self.volume)
        event = lambda code, value: EVENT.pack(0, 0, 1, code, value)
        with patch.object(Path, "write_text"):
            watcher.handle(event(115, 1), 1)
            self.assertEqual(self.volume.saved(), 15)
            watcher.repeat(1.2)  # Still inside the initial pause.
            self.assertEqual(self.volume.saved(), 15)
            watcher.repeat(1.36)
            watcher.repeat(1.47)
            self.assertEqual(self.volume.saved(), 25)
            watcher.handle(event(115, 0), 1.5)
            watcher.repeat(3)
            self.assertEqual(self.volume.saved(), 25)
            watcher.handle(event(114, 1), 5)
            for index in range(40):
                watcher.repeat(5.4 + index * .1)
            watcher.handle(event(114, 0), 10)
        self.assertEqual(self.volume.saved(), 0)
        self.assertEqual(watcher.held, {})

    def test_lost_release_does_not_run_away(self):
        self.volume.apply(50)
        watcher = VolumeWatch(self.data, self.volume)
        with patch.object(Path, "write_text"):
            watcher.handle(EVENT.pack(0, 0, 1, 115, 1), 1)
            watcher.repeat(17)
        self.assertEqual(watcher.held, {})

    def test_native_volume_hud_refreshes_at_limits_and_clears_previous_session(self):
        self.volume.apply(100)
        watcher = VolumeWatch(self.data, self.volume)
        event = EVENT.pack(0, 0, 1, 115, 1)
        with patch.object(Path, 'write_text'):
            watcher.handle(event, 1)
            first = read_fields(self.data / 'session/volume-hud.ini')
            watcher.handle(event, 1.2)
            second = read_fields(self.data / 'session/volume-hud.ini')
        self.assertEqual(first['volume'], '100')
        self.assertEqual(second['volume'], '100')
        self.assertNotEqual(first['token'], second['token'])
        with patch('audio.volume_inputs', return_value=[]):
            watcher.start()
            watcher.close()
        self.assertFalse((self.data / 'session/volume-hud.ini').exists())

    def test_mixer_uses_DAC_and_cleans_emulator_preloads(self):
        result = subprocess.CompletedProcess([], 0, stdout="Playback 22 [39%] [-10dB]\nPlayback 22 [39%]")
        with patch.dict("os.environ", {"LD_PRELOAD": "hook.so", "LD_LIBRARY_PATH": "vendor"}), \
             patch("audio.subprocess.run", return_value=result) as run:
            self.assertEqual(Mixer().query(), 39)
            Mixer().set(0)
            self.assertNotIn("LD_PRELOAD", clean_environment())
            self.assertNotIn("LD_LIBRARY_PATH", run.call_args.kwargs["env"])
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "DAC", "0%"])

    def test_unavailable_mixer_fails_before_a_game_can_start(self):
        with patch.object(self.mixer, "query", side_effect=FileNotFoundError("amixer")), \
             patch("audio.volume_inputs") as inputs:
            with self.assertRaises(FileNotFoundError):
                VolumeWatch(self.data, self.volume).start()
            inputs.assert_not_called()

    def test_monitor_stops_and_cannot_restore_an_old_loud_level(self):
        self.volume.apply(5)
        watcher = VolumeWatch(self.data, self.volume)
        with patch("audio.volume_inputs", return_value=[]):
            watcher.start()
            watcher.close()
        self.assertFalse(watcher.thread.is_alive())
        self.assertEqual(self.volume.saved(), 5)
        self.assertEqual(self.mixer.writes, [5])


if __name__ == "__main__":
    unittest.main()
