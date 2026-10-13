"""Validate hardware writes against an isolated sysfs tree, never host hardware."""
import json
import os
import time
from pathlib import Path
import sys
import tempfile
import subprocess
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
        self.sensor = self.root / "lid-state"
        self.set_lid(0)
        env = patch.dict(os.environ, {"RGDS_LID_STATE": str(self.sensor)})
        env.start();self.addCleanup(env.stop)
        helper = patch.object(Hardware, "suspend_helper", return_value="/usr/sbin/pm-suspend")
        helper.start();self.addCleanup(helper.stop)
        # Sleep audio is exercised with a stateful mixer in test_audio; these
        # isolated sysfs tests must never invoke the host's audio tools.
        volume = patch("system_controls.Volume")
        volume.start();self.addCleanup(volume.stop)
        grace = patch.object(Hardware, 'wait_closed', side_effect=lambda _deadline:self.hardware.lid_closed())
        grace.start();self.addCleanup(grace.stop)

    def set_lid(self, value):
        self.sensor.write_text(f"{time.monotonic()} {value}\n")

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
                                 "brightness": 50, "battery": 73, "charging": 1,"external_power":1,
                                 "volume_supported": 0, "volume": 0,"poweroff_supported":0})

    def test_full_and_unplugged_are_not_animated_as_charging(self):
        self.put('class/power_supply/battery/type','Battery')
        self.put('class/power_supply/battery/capacity','100')
        self.put('class/power_supply/battery/status','Full')
        status=self.hardware.status()
        self.assertEqual((status['battery'],status['charging'],status['external_power']),(100,0,1))
        self.put('class/power_supply/usb/type','USB')
        self.put('class/power_supply/usb/online','0')
        self.assertEqual(self.hardware.status()['external_power'],0)
        self.put('class/power_supply/usb/online','1')
        self.put('class/power_supply/battery/status','Not charging')
        status=self.hardware.status()
        self.assertEqual((status['charging'],status['external_power']),(0,1))

    def test_charge_counter_fallback_preserves_status_without_capacity(self):
        self.put('class/power_supply/battery/type','Battery')
        self.put('class/power_supply/battery/status','Charging')
        self.put('class/power_supply/battery/charge_now','1500')
        self.put('class/power_supply/battery/charge_full','2000')
        status=self.hardware.status()
        self.assertEqual((status['battery'],status['charging']),(75,1))

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

    def test_sleep_blanks_early_restores_panels_and_keeps_stock_hooks(self):
        backlight = self.backlight()
        state = self.put("power/state", "freeze mem")
        variant = self.put("power/mem_sleep", "s2idle [deep]")
        with patch("system_controls.subprocess.run") as run, patch.dict(
                "os.environ", {"LD_PRELOAD":"foreign.so", "RGDS_CORE":"drastic"}):
            self.hardware.sleep()
        self.assertEqual(run.call_args.args[0], ["/usr/sbin/pm-suspend"])
        self.assertNotIn("LD_PRELOAD", run.call_args.kwargs["env"])
        self.assertNotIn("RGDS_CORE", run.call_args.kwargs["env"])
        self.assertNotIn("timeout", run.call_args.kwargs)
        self.assertNotIn("start_new_session", run.call_args.kwargs)
        self.assertEqual(backlight.read_text().strip(), "100")
        self.assertEqual(state.read_text().strip(), "freeze mem")
        self.assertEqual(variant.read_text(), "s2idle [deep]")
        with patch("system_controls.subprocess.run", side_effect=subprocess.CalledProcessError(1,"pm-suspend")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.hardware.sleep()
        self.assertEqual(backlight.read_text().strip(), "100")
        self.assertEqual(state.read_text().strip(), "freeze mem")
        self.assertFalse(self.hardware.journal.exists())

    def test_missing_stock_suspend_helper_disables_sleep_and_lid(self):
        self.put("power/state", "mem")
        self.put("class/anbernic_misc/hallkey", "1")
        self.put("devices/platform/anbernic_misc/power/wakeup", "disabled")
        with patch.object(Hardware, "suspend_helper", return_value=None), patch("system_controls.subprocess.run") as run:
            self.assertFalse(self.hardware.sleep_supported())
            self.assertFalse(self.hardware.lid_supported())
            with self.assertRaisesRegex(ValueError, "Stock system sleep helper unavailable"):
                self.hardware.sleep()
        run.assert_not_called()

    def test_quick_open_skips_suspend_and_restores_both_panels(self):
        top=self.backlight(level=77);bottom=self.backlight('bottom',level=52)
        self.put('power/state','mem')
        def cue(_data,name):
            expected=('0','0') if name=='lid-close' else ('77','52')
            self.assertEqual((top.read_text().strip(),bottom.read_text().strip()),expected)
        with patch('sleep_sounds.play',side_effect=cue) as play,patch('system_controls.subprocess.run') as run,patch.object(self.hardware,'wait_closed',return_value=False):
            self.assertTrue(self.hardware.sleep(lid=True))
        run.assert_not_called()
        self.assertEqual([call.args[1] for call in play.call_args_list],['lid-close','lid-open'])
        self.assertEqual(self.hardware.sleep_mode,'quick')
        self.assertEqual((top.read_text().strip(),bottom.read_text().strip()),('77','52'))

    def test_deep_sleep_gets_original_brightness_and_failure_recovers(self):
        top=self.backlight(level=77);self.put('power/state','mem')
        def cue(*args):self.assertEqual(top.read_text().strip(),'0')
        def suspend(*args,**kwargs):
            self.assertEqual(top.read_text().strip(),'77')
            raise OSError('failed')
        with patch('sleep_sounds.play',side_effect=cue),patch('system_controls.subprocess.run',side_effect=suspend),patch.object(self.hardware,'wait_closed',return_value=True):
            with self.assertRaises(OSError):self.hardware.sleep(lid=True)
        self.assertEqual(top.read_text().strip(),'77')

    def test_lost_sensor_cancels_without_suspend_or_wake_cue(self):
        top=self.backlight(level=77);self.put('power/state','mem')
        with patch('sleep_sounds.play') as play,patch('system_controls.subprocess.run') as run,patch.object(self.hardware,'wait_closed',return_value=None):
            self.assertFalse(self.hardware.sleep(lid=True))
        run.assert_not_called();self.assertEqual(top.read_text().strip(),'77')
        self.assertEqual([call.args[1] for call in play.call_args_list],['lid-close'])

    def test_native_menu_owns_only_the_successful_wake_cue(self):
        self.put('power/state','mem')
        with patch('sleep_sounds.play') as play,patch('system_controls.subprocess.run'):
            self.assertTrue(self.hardware.sleep(wake_sound=False))
        self.assertEqual([call.args[1] for call in play.call_args_list],['lid-close'])

    def test_native_cached_cues_do_not_open_a_second_audio_sink(self):
        self.put('power/state','mem');self.put('devices/platform/anbernic_misc/power/wakeup','disabled');self.set_lid(1)
        for deep in (False,True):
            with self.subTest(deep=deep), patch('sleep_sounds.play') as play, patch('system_controls.subprocess.run') as run, patch.object(self.hardware,'wait_closed',return_value=deep):
                self.assertTrue(self.hardware.sleep_lid(wake_sound=False,close_sound=False))
                play.assert_not_called()
                self.assertEqual(run.call_count,int(deep))
                self.assertEqual(self.hardware.sleep_mode,'deep' if deep else 'quick')

    def test_lid_cues_bracket_successful_suspend_and_restored_volume(self):
        self.put('power/state','mem');self.put('devices/platform/anbernic_misc/power/wakeup','disabled');self.set_lid(1)
        events=[]
        with patch('system_controls.Volume') as volume, patch('sleep_sounds.play',side_effect=lambda data,cue:events.append(cue)), patch('system_controls.subprocess.run',side_effect=lambda *args,**kwargs:events.append('suspend')):
            volume.return_value.apply.side_effect=lambda:events.append('volume')
            self.assertTrue(self.hardware.sleep_lid())
        self.assertEqual(events,['volume','lid-close','suspend','volume','lid-open'])

    def test_failed_or_early_cancelled_sleep_is_silent_but_quick_open_chimes(self):
        self.put('power/state','mem');self.put('devices/platform/anbernic_misc/power/wakeup','disabled');self.set_lid(1)
        with patch('sleep_sounds.play') as play, patch('system_controls.subprocess.run',side_effect=subprocess.CalledProcessError(1,'pm-suspend')):
            with self.assertRaises(subprocess.CalledProcessError):self.hardware.sleep_lid()
            self.assertEqual([call.args[1] for call in play.call_args_list],['lid-close'])
        with patch('sleep_sounds.play') as play,patch('system_controls.subprocess.run') as run:
            self.set_lid(0);self.assertFalse(self.hardware.sleep_lid())
            play.assert_not_called();run.assert_not_called()
            self.set_lid(1)
            play.side_effect=lambda *args:self.set_lid(0)  # Reopened during chime.
            self.assertTrue(self.hardware.sleep_lid())
            self.assertEqual([call.args[1] for call in play.call_args_list],['lid-close','lid-open']);run.assert_not_called()

    def test_lid_sleep_reads_stock_polarity_and_journals_wake_before_stock_hooks(self):
        hall = self.put('class/anbernic_misc/hallkey', '3')
        wake = self.put('devices/platform/anbernic_misc/power/wakeup', 'disabled')
        self.put('power/state', 'mem')
        for value, expected in [('1', True), ('0', False),
                                ('2', None), ('-1', None), ('3 extra', None), ('', None)]:
            self.set_lid(value)
            self.assertIs(self.hardware.lid_closed(), expected)
        self.set_lid(1)
        def suspend(*args, **kwargs):
            self.assertEqual(args[0], ['/usr/sbin/pm-suspend'])
            self.assertEqual(wake.read_text().strip(), 'enabled')
            self.assertTrue(self.hardware.journal.exists())
        with patch('system_controls.subprocess.run', side_effect=suspend) as run:
            self.assertTrue(self.hardware.sleep_lid())
        run.assert_called_once()
        log = self.root/'userdata/logs/lid-sleep.ini'
        self.assertIn('state=helper-returned', log.read_text())
        self.hardware.restore()
        self.assertEqual(wake.read_text().strip(), 'disabled')

    def test_lid_open_unknown_and_reopened_during_wake_setup_cancel_sleep(self):
        hall = self.put('class/anbernic_misc/hallkey', '3')
        self.put('devices/platform/anbernic_misc/power/wakeup', 'disabled')
        self.put('power/state', 'mem')
        with patch('system_controls.subprocess.run') as run:
            for value in ('0', '-1', '2', 'bad'):
                self.set_lid(value)
                self.assertFalse(self.hardware.sleep_lid())
            self.set_lid(1)
            with patch.object(self.hardware, 'lid_closed', side_effect=[True, False]):
                self.assertFalse(self.hardware.sleep_lid())
        run.assert_not_called()

    def test_failed_lid_helper_records_failure_without_raw_suspend(self):
        self.set_lid(1)
        self.put('devices/platform/anbernic_misc/power/wakeup', 'disabled')
        state = self.put('power/state', 'mem')
        panel = self.backlight()
        with patch('system_controls.subprocess.run', side_effect=subprocess.CalledProcessError(1, 'pm-suspend')) as run:
            with self.assertRaises(subprocess.CalledProcessError): self.hardware.sleep_lid()
        run.assert_called_once()
        self.assertEqual(state.read_text(), 'mem'); self.assertEqual(panel.read_text().strip(), '100')
        self.assertIn('state=failed', (self.root/'userdata/logs/lid-sleep.ini').read_text())

    def test_competing_process_cannot_overwrite_original_recovery_values(self):
        panel = self.backlight()
        script = '''import sys
from pathlib import Path
from system_controls import Hardware
Path(sys.argv[3]).touch()
Hardware(Path(sys.argv[1]), Path(sys.argv[2])).brightness(80)
'''
        ready = self.root / "ready"
        import time
        with self.hardware.transaction():
            self.hardware.brightness(20)
            process = subprocess.Popen([sys.executable, "-c", script,
                str(self.root/"userdata"), str(self.sysfs), str(ready)],
                cwd=ROOT/"runtime", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic()+3
                while not ready.exists() and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue(ready.exists())
                with self.assertRaises(subprocess.TimeoutExpired):process.wait(timeout=.1)
                self.assertEqual(panel.read_text().strip(), "51")
            except BaseException:
                process.kill();process.communicate();raise
        output, error = process.communicate(timeout=3)
        self.assertEqual(process.returncode, 0, error.decode())
        self.assertEqual(panel.read_text().strip(), "204")
        self.hardware.restore()
        self.assertEqual(panel.read_text().strip(), "100")

    def test_live_sensor_enables_lid_sleep_independently_of_hall_configuration(self):
        self.put("power/state", "mem")
        hall=self.put("class/anbernic_misc/hallkey", "3\n")
        wake=self.put("devices/platform/anbernic_misc/power/wakeup", "disabled")
        self.assertEqual(self.hardware.status()["lid_supported"], 1)
        self.hardware.lid(True)
        self.assertEqual(wake.read_text().strip(), "enabled")
        hall.write_text("0\n")
        self.assertTrue(self.hardware.lid_supported())
        self.hardware.lid(False)
        self.assertEqual(wake.read_text().strip(), "disabled")
        for invalid in ("2", "-1", "garbage", "3 extra"):
            hall.write_text(invalid)
            self.assertTrue(self.hardware.lid_supported())
        self.sensor.unlink()
        self.assertFalse(self.hardware.lid_supported())

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

    def test_previous_boot_journal_does_not_overwrite_fresh_vendor_defaults(self):
        panel = self.backlight()
        with patch.object(Hardware, "boot_id", return_value="previous-boot"):
            self.hardware.brightness(100)
        panel.write_text("73")  # New boot has initialized the hardware itself.
        with patch.object(Hardware, "boot_id", return_value="current-boot"):
            self.hardware.restore()
        self.assertEqual(panel.read_text(), "73")
        self.assertFalse(self.hardware.journal.exists())

    def test_legacy_journal_remains_recoverable(self):
        panel = self.backlight()
        self.hardware.journal.parent.mkdir(parents=True)
        self.hardware.journal.write_text(json.dumps({str(panel.relative_to(self.sysfs)):"42"}))
        self.hardware.restore()
        self.assertEqual(panel.read_text().strip(), "42")

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

class LidGracePeriodTests(unittest.TestCase):
    def test_closed_lid_waits_five_seconds_but_open_or_unknown_exits_promptly(self):
        for transition, state in ((3, False), (2, None), (10, False)):
            with self.subTest(transition=transition):
                now=[0.0];hardware=Hardware(Path('/unused-test-data'))
                def sleep(seconds):now[0]+=seconds
                with patch('system_controls.time.monotonic',side_effect=lambda:now[0]),patch('system_controls.time.sleep',side_effect=sleep),patch.object(hardware,'lid_closed',side_effect=lambda:True if now[0]<transition else state):
                    result=hardware.wait_closed(5)
                self.assertIs(result,True if transition>5 else state)
                self.assertGreaterEqual(now[0],min(transition,5))
                self.assertLessEqual(now[0],min(transition,5)+.021)


if __name__ == "__main__":
    unittest.main()
