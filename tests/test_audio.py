"""Exercise volume resets and hardware keys without opening host audio devices."""
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from audio import (
    EVENT, MIC_GAIN, Amplifier, Mixer, Volume, VolumeWatch, capture_device, clean_environment, microphone_levels,
    mixer_target
)
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

    def speakers(self, level):
        self.speakers_level = level


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
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "DAC", "playback", "0%"])

    def test_mixer_falls_back_to_mainline_master(self):
        missing = subprocess.CompletedProcess([], 1, stdout="")
        master = subprocess.CompletedProcess([], 0, stdout="Playback 109 [43%] [-54.12dB]")
        with patch("audio.subprocess.run", side_effect=[missing, master, master]) as run:
            mixer = Mixer()
            self.assertEqual(mixer.query(), 43)
            mixer.set(0)
            self.assertEqual(run.call_args_list[1].args[0], ["amixer", "-c", "0", "sget", "Master"])
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "Master", "playback", "0%"])

    def test_mixer_follows_the_card_that_plays(self):
        # RG DS OS's RG DS Plus: the RK817 is not card 0, and the jack moves playback to it
        result = subprocess.CompletedProcess([], 0, stdout="Playback 109 [43%] [-54.12dB]")
        with patch("audio.subprocess.run", return_value=result) as run:
            Mixer(target=lambda: (1, "rk817")).set(50)
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "1", "sset", "DAC", "playback", "65%"])
        with tempfile.TemporaryDirectory() as folder:
            route = Path(folder) / "audio-mixer"
            self.assertEqual(mixer_target(route), (0, "rk817"))
            route.write_text("2 aw88166\n")
            self.assertEqual(mixer_target(route), (2, "aw88166"))
            route.write_text("garbage")
            self.assertEqual(mixer_target(route), (0, "rk817"))

    def test_mute_powers_the_amplifiers_down_and_only_the_change_is_written(self):
        def amixer(command, **kwargs):
            return subprocess.CompletedProcess(command, 0, stdout="  : values=off\n")

        with patch("audio.subprocess.run", side_effect=amixer) as run:
            mixer = Mixer(target=lambda: (0, "rk817"))
            mixer.speakers(0)
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "Internal Speakers", "off"])
            run.reset_mock()
            mixer.speakers(0)  # the watcher's repair every 2 s spawns nothing
            run.assert_not_called()
            mixer.speakers(5)
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "Internal Speakers", "on"])
            run.reset_mock()
            mixer.speakers(50)
            run.assert_not_called()

    def test_unmuting_leaves_the_speakers_off_with_headphones_in(self):
        def amixer(command, **kwargs):
            # The jack is a CARD control: amixer finds nothing under the default MIXER interface.
            found = "iface=CARD,name=Headphones Jack" in command or "sset" in command
            return subprocess.CompletedProcess(command, 0 if found else 1, stdout="  : values=on\n" if found else "")

        with patch("audio.subprocess.run", side_effect=amixer) as run:
            Mixer(target=lambda: (0, "rk817")).speakers(50)
            self.assertEqual(run.call_args.args[0], ["amixer", "-q", "-c", "0", "sset", "Internal Speakers", "off"])

    def test_the_plus_amplifiers_have_no_speaker_pin(self):
        with patch("audio.subprocess.run") as run:
            Mixer(target=lambda: (1, "aw88166")).speakers(0)
            run.assert_not_called()

    def test_a_missing_amixer_does_not_stop_the_volume(self):
        with patch("audio.subprocess.run", side_effect=FileNotFoundError("amixer")), redirect_stderr(StringIO()):
            Mixer(target=lambda: (0, "rk817")).speakers(0)

    def test_volume_passes_every_level_to_the_speakers(self):
        self.volume.apply(0)
        self.assertEqual(self.mixer.speakers_level, 0)
        self.volume.apply(40)
        self.assertEqual(self.mixer.speakers_level, 40)

    def test_plus_amplifiers_take_an_attenuation(self):
        def amixer(command, **kwargs):
            if command[3] == "sget" and command[4] == "Left PCM":
                return subprocess.CompletedProcess(command, 0, stdout="  Mono: Playback 267 [26%] [-25.03dB]\n")
            return subprocess.CompletedProcess(command, 0 if "sset" in command else 1, stdout="")

        with patch("audio.subprocess.run", side_effect=amixer) as run:
            mixer = Mixer(target=lambda: (0, "aw88166"))
            # 267 is level 50: reported on the RK817's scale, so Volume compares it as it always has
            self.assertEqual(mixer.query(), 30 + round(70 * 49 / 99))
            mixer.set(100)
            self.assertEqual([c.args[0][5:] for c in run.call_args_list[-2:]], [["Left PCM", "0"], ["Right PCM", "0"]])
            mixer.set(0)
            self.assertEqual(run.call_args.args[0][5:], ["Right PCM", "1023"])
        self.assertEqual([Amplifier.from_raw(Amplifier.raw(level)) for level in (0, 1, 37, 100)], [0, 1, 37, 100])

    def test_the_microphone_is_named_on_rg_ds_os(self):
        with tempfile.TemporaryDirectory() as folder:
            conf = Path(folder) / "audio-capture.conf"
            self.assertEqual(capture_device(conf), "plughw:0,0")
            conf.write_text("pcm.rgds_in { type plug slave.pcm \"hw:1,0\" }\n")
            self.assertEqual(capture_device(conf), "rgds_mic")

    def test_the_microphones_capture_level_is_not_read_as_the_volume(self):
        # amixer's line for the RK817's Master on RG DS OS, with the microphone open
        line = "  Front Left: Playback 202 [79%] [-19.75dB] Capture 255 [100%] [0.00dB]\n"
        with patch("audio.subprocess.run", return_value=subprocess.CompletedProcess([], 0, stdout=line)):
            self.assertEqual(Mixer(target=lambda: (0, "rk817")).query(), 79)

    def test_microphone_levels_go_to_the_codec_card(self):
        run = Mock()
        with tempfile.TemporaryDirectory() as folder:
            conf = Path(folder) / "audio-capture.conf"
            microphone_levels(conf, run)  # stock: no capture file, the kernel's own controls
            run.assert_not_called()
            conf.write_text("pcm.rgds_in {\n    type plug\n    slave { pcm \"hw:1,0\" format S16_LE }\n}\n")
            microphone_levels(conf, run)
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ["amixer", "-q", "-c", "1", "cset", "name=Master Capture Volume", "255,255"],
            ["amixer", "-q", "-c", "1", "cset", "name=Mic Capture Gain", f"{MIC_GAIN},{MIC_GAIN}"],
        ])

    def test_microphone_levels_never_stop_a_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            conf = Path(folder) / "audio-capture.conf"
            conf.write_text("pcm.rgds_in { type plug slave.pcm \"hw:0,0\" }\n")
            microphone_levels(conf, Mock(side_effect=FileNotFoundError("amixer")))
            conf.write_text("garbage")
            microphone_levels(conf, Mock(side_effect=AssertionError("no card, no call")))

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
