"""RG DS OS shell helpers (os/buildroot/rootfs/usr/libexec/rgds) run against fake tools and files."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIBEXEC = ROOT / 'os/buildroot/rootfs/usr/libexec/rgds'


def tool(folder, name, body):
    path = Path(folder) / name
    path.write_text('#!/bin/sh\n' + body)
    path.chmod(0o755)
    return path


class BtAudioTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)
        self.bin = self.dir / 'bin'
        self.bin.mkdir()
        self.pcms = self.dir / 'pcms'
        self.pcms.write_text('')
        self.ctl = tool(self.bin, 'bluealsactl', f'[ "$1" = list-pcms ] && cat {self.pcms}\n')
        self.amixer_log = self.dir / 'amixer.log'
        tool(
            self.bin, 'amixer', f'echo "$@" >> {self.amixer_log}\n'
            'case "$*" in *scontrols*) echo "Simple mixer control \'Buds A2DP\',0";; esac\n'
        )
        self.speaker = self.dir / 'speaker.conf'
        self.speaker.write_text('pcm.rgds_out "speaker"\n')
        self.out = self.dir / 'audio-out.conf'
        self.out.write_text(self.speaker.read_text())
        self.sink = self.dir / 'bt-sink'
        self.volume = self.dir / 'volume.ini'
        self.volume.write_text('schema=1\nvolume=40\n')
        self.env = dict(
            os.environ,
            PATH=f'{self.bin}:/usr/bin:/bin',
            RGDS_AUDIO_OUT=str(self.out),
            RGDS_BT_SINK=str(self.sink),
            RGDS_AUDIO_SPEAKER=str(self.speaker),
            RGDS_VOLUME_FILE=str(self.volume),
            RGDS_BLUEALSA_CTL=str(self.ctl)
        )

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, *args):
        return subprocess.run(
            ['sh', str(LIBEXEC / 'bt-audio'), *args], env=self.env, capture_output=True, text=True, timeout=10
        )

    def test_headset_takes_playback_and_gives_it_back(self):
        self.pcms.write_text(
            '/org/bluealsa/hci0/dev_AC_C1_22_33_44_5f/a2dpsrc/sink\n'
            '/org/bluealsa/hci0/dev_AC_C1_22_33_44_5f/hfpag/sink\n'
        )
        self.assertEqual(self.run_script('refresh').returncode, 0)
        self.assertEqual(self.sink.read_text().strip(), 'AC:C1:22:33:44:5F')
        conf = self.out.read_text()
        self.assertIn('type bluealsa', conf)
        self.assertIn('device "AC:C1:22:33:44:5F"', conf)
        self.assertIn('profile "a2dp"', conf)
        # the saved level reaches the headset's own control
        self.assertIn('-q -D bluealsa sset Buds A2DP 40%', self.amixer_log.read_text())

        # the same headset again: nothing rewritten
        stamp = self.out.stat().st_mtime_ns
        self.run_script('refresh')
        self.assertEqual(self.out.stat().st_mtime_ns, stamp)

        self.pcms.write_text('')
        self.run_script('refresh')
        self.assertFalse(self.sink.exists())
        self.assertEqual(self.out.read_text(), self.speaker.read_text())

    def test_a_microphone_profile_alone_is_not_a_headset(self):
        self.pcms.write_text('/org/bluealsa/hci0/dev_AC_C1_22_33_44_55/a2dpsnk/source\n')
        self.run_script('refresh')
        self.assertFalse(self.sink.exists())
        self.assertEqual(self.out.read_text(), self.speaker.read_text())

    def test_volume_only_with_a_headset(self):
        self.assertEqual(self.run_script('volume', '55').returncode, 0)
        self.assertFalse(self.amixer_log.exists())
        self.sink.write_text('AC:C1:22:33:44:55\n')
        self.run_script('volume', '55')
        self.assertIn('sset Buds A2DP 55%', self.amixer_log.read_text())
        self.assertEqual(self.run_script('volume', 'loud').returncode, 2)


class PowerSleepTest(unittest.TestCase):
    """usr/libexec/rgds/power suspend: dark RTC wakes keep the handheld asleep until a person, or the battery, ends it."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        d = self.dir = Path(self.temp.name)
        for folder in ('run', 'rtc', 'battery', 'charger', 'backlight/a', 'bin'):
            (d / folder).mkdir(parents=True)
        (d / 'run/suspend-request').write_text('tok\n')
        (d / 'rtc/since_epoch').write_text('1000\n')
        (d / 'rtc/wakealarm').write_text('')
        (d / 'battery/capacity').write_text('80\n')
        (d / 'battery/status').write_text('Discharging\n')
        (d / 'charger/online').write_text('0\n')
        (d / 'backlight/a/brightness').write_text('180\n')
        self.wakes = d / 'wakes'
        # each suspend takes the next line: "rtc" sleeps until the alarm, "user N" is woken by the power key after
        # N s, "open N" by the lid opening, "charger N" by the charger with the lid still shut, "fail" aborts the
        # suspend; a line may also set the battery ("rtc 3" = the alarm, then 3 %)
        self.interrupts = d / 'interrupts'
        self.interrupts.write_text(
            ' 35:          0          0          0          0  rk817  0 Edge      rk805_pwrkey_fall\n'
            ' 40:          0          0          0          0  rk817  5 Edge      RTC alarm\n'
        )
        tool(
            d / 'bin', 'suspend', f"""line=$(head -n 1 {self.wakes}); sed -i.bak 1d {self.wakes}
now=$(cat {d}/rtc/since_epoch)
set -- $line
case "$1" in
rtc) alarm=$(cat {d}/rtc/wakealarm); echo $((now + ${{alarm#+}})) > {d}/rtc/since_epoch
     [ -n "$2" ] && echo $2 > {d}/battery/capacity
     echo 255 > {d}/backlight/a/brightness ;;
user) echo $((now + $2)) > {d}/rtc/since_epoch
      n=$(awk '/pwrkey/ {{ print $2 + 1 }}' {self.interrupts})
      echo " 35:  $n  0  0  0  rk817  0 Edge      rk805_pwrkey_fall" > {self.interrupts} ;;
open) echo $((now + $2)) > {d}/rtc/since_epoch; echo 0 > {d}/lid ;;
charger) echo $((now + $2)) > {d}/rtc/since_epoch; echo 255 > {d}/backlight/a/brightness ;;
*) exit 1 ;;
esac
"""
        )
        self.lid = d / 'lid'
        self.lid.write_text('1\n')
        tool(d / 'bin', 'lid', f'cat {self.lid}\n')
        # the radios: fake net and bluetoothctl log their calls; wlan0 and hci0 exist unless a test removes them
        self.radio_log = d / 'radios.log'
        tool(d / 'bin', 'net', f'echo "net $*" >> {self.radio_log}\n')
        tool(d / 'bin', 'bluetoothctl', f'echo "bluetoothctl $*" >> {self.radio_log}\n')
        tool(d / 'bin', 'timeout', 'shift; exec "$@"\n')  # BusyBox has it, macOS does not
        (d / 'wlan0').mkdir()
        (d / 'hci0').mkdir()
        # the USB gadget (usb sleep|wake) and the kernel log a failed suspend is explained from
        self.usb_log = d / 'usb.log'
        tool(d / 'bin', 'usb', f'echo "usb $*" >> {self.usb_log}\n')
        tool(
            d / 'bin', 'dmesg', 'echo "[ 2046.893325] dwc3 fcc00000.usb: PM: failed to suspend: error -11"\n'
            'echo "[ 2046.893452] PM: Some devices failed to suspend, or early wake event detected"\n'
        )
        self.env = dict(
            os.environ,
            PATH=f"{d / 'bin'}:{os.environ.get('PATH', '')}",
            RGDS_NET_CMD=str(d / 'bin/net'),
            RGDS_BTCTL=str(d / 'bin/bluetoothctl'),
            RGDS_WLAN_DIR=str(d / 'wlan0'),
            RGDS_HCI_DIR=str(d / 'hci0'),
            RGDS_USB_CMD=str(d / 'bin/usb'),
            RGDS_DMESG_CMD=str(d / 'bin/dmesg'),
            RGDS_RUN=str(d / 'run'),
            RGDS_RTC=str(d / 'rtc'),
            RGDS_BATTERY_DIR=str(d / 'battery'),
            RGDS_CHARGER_DIR=str(d / 'charger'),
            RGDS_BACKLIGHTS=str(d / 'backlight'),
            RGDS_SLEEP_LED=str(d / 'led'),
            RGDS_SLEEP_LOG=str(d / 'sleep.log'),
            RGDS_SUSPEND_CMD=str(d / 'bin/suspend'),
            RGDS_LID_CMD=str(d / 'bin/lid'),
            RGDS_INTERRUPTS=str(self.interrupts),
            RGDS_SLEEP_CHECK='3600',
            RGDS_SLEEP_LIMIT='10800',
            RGDS_DATA=str(d / 'data'),
            RGDS_LOGS=str(d / 'logs'),
            RGDS_MEM_SLEEP=str(d / 'mem_sleep'),
            RGDS_PMIC_DUMP='echo f0: c0 8c a0 40 20'
        )
        for folder in ('data', 'logs'):
            (d / folder).mkdir()
        (d / 'mem_sleep').write_text('s2idle [deep]\n')

    def tearDown(self):
        self.temp.cleanup()

    def boot_check(self):
        return subprocess.run(
            ['sh', str(LIBEXEC / 'power'), 'boot-check'], env=self.env, capture_output=True, text=True, timeout=10
        ).returncode

    def test_a_sleep_saves_the_pmic_and_leaves_no_marker(self):
        code, state = self.sleep('user 30')
        self.assertEqual(code, 0)
        self.assertEqual((self.dir / 'mem_sleep').read_text().strip(), 'deep')
        self.assertIn('f0: c0 8c a0 40 20', (self.dir / 'logs/pmic-before-sleep.txt').read_text())
        self.assertFalse((self.dir / 'data/sleep-pending').exists())

    def test_a_handheld_marked_shallow_sleeps_shallow(self):
        (self.dir / 'data/sleep-shallow').touch()
        self.sleep('user 30')
        self.assertEqual((self.dir / 'mem_sleep').read_text().strip(), 's2idle')

    def test_a_deep_sleep_that_ended_in_a_cold_boot_turns_sleep_shallow(self):
        # went to sleep at 80 % at t=1000; this boot is ten minutes later
        (self.dir / 'data/sleep-pending').write_text('1000 80 deep\n')
        (self.dir / 'rtc/since_epoch').write_text('1600\n')
        self.assertEqual(self.boot_check(), 0)
        self.assertTrue((self.dir / 'data/sleep-shallow').exists())
        self.assertFalse((self.dir / 'data/sleep-pending').exists())
        self.assertIn('shallow from now on', (self.dir / 'sleep.log').read_text())
        self.assertIn('f0:', (self.dir / 'logs/pmic-boot.txt').read_text())

    def test_a_battery_run_flat_in_a_long_sleep_keeps_deep_sleep(self):
        (self.dir / 'data/sleep-pending').write_text('1000 20 deep\n')
        (self.dir / 'rtc/since_epoch').write_text(str(1000 + 4 * 86400) + '\n')
        self.boot_check()
        self.assertFalse((self.dir / 'data/sleep-shallow').exists())
        self.assertIn('ended in a cold boot', (self.dir / 'sleep.log').read_text())

    def test_a_shallow_sleep_that_ended_in_a_cold_boot_is_only_logged(self):
        (self.dir / 'data/sleep-pending').write_text('1000 80 s2idle\n')
        (self.dir / 'rtc/since_epoch').write_text('1600\n')
        self.boot_check()
        self.assertFalse((self.dir / 'data/sleep-shallow').exists())
        self.assertIn('s2idle sleep ended in a cold boot', (self.dir / 'sleep.log').read_text())

    def test_an_ordinary_boot_changes_nothing(self):
        self.boot_check()
        self.assertFalse((self.dir / 'data/sleep-shallow').exists())
        self.assertFalse((self.dir / 'sleep.log').exists())

    def sleep(self, *wakes):
        self.wakes.write_text(''.join(w + '\n' for w in wakes))
        result = subprocess.run(
            ['sh', str(LIBEXEC / 'power'), 'suspend'], env=self.env, capture_output=True, text=True, timeout=10
        )
        state = dict(line.split('=', 1) for line in (self.dir / 'run/suspend-state').read_text().splitlines())
        # the radios come back behind the wake (run/radios-up.pid): wait for them before reading their log
        pending = self.dir / 'run/radios-up.pid'
        if pending.exists():
            pid, deadline = int(pending.read_text()), time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
                time.sleep(.02)
        return result.returncode, state

    def test_a_person_wakes_it_at_once(self):
        code, state = self.sleep('user 30')
        self.assertEqual((code, state['phase'], state['wake'], state['token']), (0, 'post', 'user', 'tok'))
        self.assertFalse((self.dir / 'run/poweroff-requested').exists())
        self.assertEqual((self.dir / 'rtc/wakealarm').read_text().strip(), '0')

    def test_rtc_checks_sleep_again_dark_until_opened(self):
        code, state = self.sleep('rtc', 'rtc 60', 'user 5')
        self.assertEqual(state['wake'], 'user')
        self.assertFalse((self.dir / 'run/poweroff-requested').exists())
        # the panels were dark between checks and have their own level back
        self.assertEqual((self.dir / 'backlight/a/brightness').read_text().strip(), '180')
        self.assertEqual(self.wakes.read_text(), '')
        self.assertEqual(self.dir.joinpath('sleep.log').read_text().count('dark wake'), 2)

    def test_low_battery_or_a_long_sleep_ends_in_power_off(self):
        code, state = self.sleep('rtc', 'rtc 4', 'user 5')
        self.assertEqual(state['wake'], 'poweroff')
        self.assertTrue((self.dir / 'run/poweroff-requested').exists())
        (self.dir / 'run/poweroff-requested').unlink()
        (self.dir / 'battery/capacity').write_text('80\n')
        code, state = self.sleep('rtc', 'rtc', 'rtc', 'user 5')
        self.assertEqual(state['wake'], 'poweroff')  # 3 h is the test's limit
        self.assertEqual(self.wakes.read_text(), 'user 5\n')

    def test_on_the_charger_it_keeps_sleeping(self):
        (self.dir / 'charger/online').write_text('1\n')
        code, state = self.sleep('rtc 2', 'rtc', 'rtc', 'rtc', 'user 5')
        self.assertEqual(state['wake'], 'user')

    def test_an_alarm_with_the_lid_open_is_a_person(self):
        self.lid.write_text('0\n')
        code, state = self.sleep('rtc', 'user 5')
        self.assertEqual(state['wake'], 'user')
        self.assertEqual(self.wakes.read_text(), 'user 5\n')

    def test_the_charger_with_the_lid_closed_sleeps_again(self):
        # 2026-11-06: unplugged with the lid shut, the handheld woke and ran at Home until the battery was flat
        code, state = self.sleep('charger 30', 'charger 60', 'open 5')
        self.assertEqual((code, state['wake']), (0, 'user'))
        self.assertEqual(self.wakes.read_text(), '')
        self.assertEqual(self.dir.joinpath('sleep.log').read_text().count('woken with the lid closed'), 2)
        self.assertEqual((self.dir / 'backlight/a/brightness').read_text().strip(), '180')

    def test_the_power_key_wakes_a_closed_handheld(self):
        code, state = self.sleep('charger 30', 'user 5', 'open 5')
        self.assertEqual(state['wake'], 'user')
        self.assertEqual(self.wakes.read_text(), 'open 5\n')

    def test_without_interrupt_counts_every_wake_is_a_person(self):
        self.interrupts.unlink()
        code, state = self.sleep('charger 30', 'open 5')
        self.assertEqual(self.wakes.read_text(), 'open 5\n')

    def test_an_aborted_suspend_with_the_lid_closed_tries_again(self):
        tool(self.dir / 'bin', 'sleep', ':\n')  # the 2 s pause between tries
        code, state = self.sleep('fail', 'user 5', 'open 5')
        self.assertEqual((code, state['result']), (0, '0'))
        self.assertEqual(self.wakes.read_text(), 'open 5\n')
        code, state = self.sleep('fail', 'fail', 'fail', 'open 5')
        self.assertNotEqual(code, 0)
        self.assertEqual(self.wakes.read_text(), 'open 5\n')
        self.lid.write_text('0\n')
        code, state = self.sleep('fail', 'open 5')
        self.assertNotEqual(code, 0)

    def test_radios_are_powered_down_for_the_sleep_and_back_on_the_wake(self):
        # the dark wakes in between leave them down: one down, one up, however many sleeps
        code, state = self.sleep('rtc', 'rtc', 'user 5')
        self.assertEqual(state['wake'], 'user')
        self.assertEqual(
            self.radio_log.read_text().splitlines(),
            ['net wifi-sleep', 'bluetoothctl power off', 'bluetoothctl power on', 'net wifi']
        )

    def test_the_wake_is_reported_before_the_radios_are_back(self):
        # RG DS Core waits for phase=post: a slow Wi-Fi start must not hold the controls
        tool(self.dir / 'bin', 'net', f'[ "$1" = wifi ] && sleep 3; echo "net $*" >> {self.radio_log}\n')
        self.wakes.write_text('user 5\n')
        subprocess.run(['sh', str(LIBEXEC / 'power'), 'suspend'], env=self.env, timeout=10)
        self.assertIn('phase=post', (self.dir / 'run/suspend-state').read_text())
        self.assertNotIn('net wifi', self.radio_log.read_text().splitlines())
        # a sleep that comes meanwhile waits for them: up, then down again, never the two at once
        code, state = self.sleep('user 5')
        self.assertEqual(
            self.radio_log.read_text().splitlines(), [
                'net wifi-sleep', 'bluetoothctl power off', 'bluetoothctl power on', 'net wifi', 'net wifi-sleep',
                'bluetoothctl power off', 'bluetoothctl power on', 'net wifi'
            ]
        )

    def test_the_usb_gadget_is_unbound_for_the_sleep(self):
        # 2026-11-26: bound on a wall charger, it failed every suspend on an RG DS Plus (dwc3 -EAGAIN)
        code, state = self.sleep('rtc', 'charger 30', 'user 5')
        self.assertEqual(state['wake'], 'user')
        self.assertEqual(self.usb_log.read_text().splitlines(), ['usb sleep', 'usb wake'])

    def test_a_failed_sleep_names_the_device_that_stopped_it(self):
        tool(self.dir / 'bin', 'sleep', ':\n')
        code, state = self.sleep('fail', 'user 5')
        self.assertEqual(state['wake'], 'user')
        log = self.dir.joinpath('sleep.log').read_text()
        self.assertIn('the sleep failed (1): dwc3 fcc00000.usb: PM: failed to suspend: error -11', log)
        self.assertNotIn('Some devices failed', log)

    def test_radios_that_were_off_stay_off(self):
        (self.dir / 'wlan0').rmdir()
        (self.dir / 'hci0').rmdir()
        code, state = self.sleep('user 5')
        self.assertEqual(state['wake'], 'user')
        self.assertFalse(self.radio_log.exists())


class UsbSleepTest(unittest.TestCase):
    """usr/libexec/rgds/usb sleep|wake: the gadget off its controller for a suspend and back on after."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        d = self.dir = Path(self.temp.name)
        (d / 'gadget').mkdir()
        (d / 'run').mkdir()
        (d / 'bin').mkdir()
        self.udc = d / 'gadget/UDC'
        self.ip_log = d / 'ip.log'
        tool(d / 'bin', 'ip', f'echo "ip $*" >> {self.ip_log}\n')
        self.env = dict(
            os.environ,
            PATH=f"{d / 'bin'}:{os.environ.get('PATH', '')}",
            RGDS_GADGET=str(d / 'gadget'),
            RGDS_RUN=str(d / 'run')
        )

    def tearDown(self):
        self.temp.cleanup()

    def usb(self, command):
        return subprocess.run(['sh', str(LIBEXEC / 'usb'), command], env=self.env, timeout=10).returncode

    def test_a_bound_gadget_is_unbound_and_bound_again(self):
        self.udc.write_text('fcc00000.usb\n')
        self.assertEqual(self.usb('sleep'), 0)
        self.assertEqual(self.udc.read_text().strip(), '')
        self.assertEqual(self.usb('wake'), 0)
        self.assertEqual(self.udc.read_text().strip(), 'fcc00000.usb')
        self.assertIn('ip link set usb0 up', self.ip_log.read_text())
        self.assertFalse((self.dir / 'run/usb-asleep').exists())

    def test_a_gadget_that_was_off_stays_off(self):
        # USB data transfer switched off: no UDC bound (or no gadget at all)
        self.udc.write_text('\n')
        self.usb('sleep')
        self.usb('wake')
        self.assertEqual(self.udc.read_text().strip(), '')
        self.assertFalse(self.ip_log.exists())
        self.udc.unlink()
        self.assertEqual(self.usb('sleep'), 0)
        self.assertEqual(self.usb('wake'), 0)
        self.assertFalse(self.udc.exists())


class BatteryWatchTest(unittest.TestCase):
    def test_an_empty_battery_asks_rg_ds_core_first_then_powers_off(self):
        with tempfile.TemporaryDirectory() as folder:
            d = Path(folder)
            for name in ('battery', 'charger', 'run', 'bin'):
                (d / name).mkdir()
            (d / 'battery/capacity').write_text('2\n')
            (d / 'battery/status').write_text('Discharging\n')
            (d / 'charger/online').write_text('0\n')
            # what init would do; the request must still be there when nobody took it, and gone afterwards
            tool(d / 'bin', 'poweroff', f'ls {d}/run > {d}/at-poweroff; echo off >> {d}/at-poweroff\n')
            env = dict(
                os.environ,
                RGDS_BATTERY_DIR=str(d / 'battery'),
                RGDS_CHARGER_DIR=str(d / 'charger'),
                RGDS_RUN=str(d / 'run'),
                RGDS_BATTERY_LOG=str(d / 'battery.log'),
                RGDS_POWEROFF=str(d / 'bin/poweroff'),
                RGDS_BATTERY_INTERVAL='0.05',
                RGDS_POWEROFF_WAIT='1'
            )
            process = subprocess.Popen(['sh', str(LIBEXEC / 'battery-watch')], env=env, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 5
                while not (d / 'run/poweroff-requested').exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue((d / 'run/poweroff-requested').exists())
                self.assertFalse((d / 'at-poweroff').exists())
                self.assertEqual(process.wait(timeout=5), 0)
            finally:
                process.kill()
            self.assertEqual((d / 'at-poweroff').read_text(), 'off\n')
            self.assertIn('powered off', (d / 'battery.log').read_text())

    def test_history_rows_around_a_sleep_and_the_charge_put_in(self):
        """Battery health's history: a row at boot, the reading before a sleep and after it, one when the charger
        comes, and what the charger put in carried over a restart."""
        with tempfile.TemporaryDirectory() as folder:
            d = Path(folder)
            for name in ('battery', 'charger', 'run', 'bin'):
                (d / name).mkdir()
            battery = d / 'battery'

            def gauge(now, charge, status='Discharging', online='0'):
                (battery / 'charge_now').write_text(f'{charge}\n')
                (battery / 'status').write_text(status + '\n')
                (d / 'charger/online').write_text(online + '\n')
                (d / 'now').write_text(f'{now}\n')

            for name, value in (
                ('capacity', 60), ('current_avg', -480000), ('voltage_avg', 3800000), ('charge_full', 4000000)
            ):
                (battery / name).write_text(f'{value}\n')
            gauge(1000, 2400000)
            tool(d / 'bin', 'date', f'[ "$1" = +%s ] && exec cat {d}/now; exec /bin/date "$@"\n')
            history = d / 'logs/battery-history.tsv'
            env = dict(
                os.environ,
                PATH=f"{d / 'bin'}:{os.environ.get('PATH', '')}",
                RGDS_BATTERY_DIR=str(battery),
                RGDS_CHARGER_DIR=str(d / 'charger'),
                RGDS_RUN=str(d / 'run'),
                RGDS_BATTERY_LOG=str(d / 'battery.log'),
                RGDS_BATTERY_HISTORY=str(history),
                RGDS_BATTERY_INTERVAL='0.05'
            )

            def rows():
                return [line.split('\t') for line in history.read_text().splitlines()] if history.exists() else []

            def until(count):
                deadline = time.monotonic() + 5
                while len(rows()) < count and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertEqual(len(rows()), count)

            def settle():
                time.sleep(.4)  # several readings

            process = subprocess.Popen(['sh', str(LIBEXEC / 'battery-watch')], env=env, stderr=subprocess.DEVNULL)
            try:
                until(1)
                gauge(1010, 2390000)
                settle()
                self.assertEqual(len(rows()), 1)  # held: under five minutes since the last row
                gauge(5000, 2380000)  # a wall-clock jump: the handheld slept
                until(3)
                gauge(5010, 2380000, 'Charging', '1')
                until(4)
                # five minutes on the charger, read every minute: one row at the end, 30 mA a minute put in
                for step in range(1, 6):
                    gauge(5010 + 60 * step, 2380000 + 30000 * step, 'Charging', '1')
                    settle()
                until(5)
            finally:
                process.kill()
                process.wait()
            got = rows()
            self.assertEqual(
                [(r[0], r[2], r[5], r[6], r[7]) for r in got], [
                    ('1000', '2400000', 'D', '0', 'b'), ('1010', '2390000', 'D', '0', 'a'),
                    ('5000', '2380000', 'D', '0', 's'), ('5010', '2380000', 'C', '1', 'a'),
                    ('5310', '2530000', 'C', '1', 'a')
                ]
            )
            self.assertEqual(got[0][1:5] + got[0][9:], ['60', '2400000', '-480000', '3800000', '4000000'])
            self.assertEqual(got[-1][8], '150000')
            # a restart carries the count on
            gauge(9000, 2530000, 'Charging', '1')
            process = subprocess.Popen(['sh', str(LIBEXEC / 'battery-watch')], env=env, stderr=subprocess.DEVNULL)
            try:
                until(6)
            finally:
                process.kill()
                process.wait()
            self.assertEqual((rows()[-1][7], rows()[-1][8]), ('b', '150000'))


if __name__ == '__main__':
    unittest.main()


class AudioRouteTest(unittest.TestCase):
    """usr/libexec/rgds/audio: one shared stream on the RG DS's single card; on the RG DS Plus the speakers' card or
    the codec's, by the jack."""
    RG_DS = ' 0 [rk817ext       ]: simple-card - rk817_ext\n                      rk817_ext\n'
    PLUS = (
        ' 0 [rk817hp        ]: simple-card - rk817_hp\n                      rk817_hp\n'
        ' 1 [aw88166        ]: audio-graph-card2 - aw88166\n                      aw88166\n'
    )

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.bin = self.dir / 'bin'
        self.bin.mkdir()
        self.run_dir = self.dir / 'run'
        self.run_dir.mkdir()
        self.cards = self.dir / 'cards'
        self.amixer_log = self.dir / 'amixer.log'
        tool(self.bin, 'amixer', f'echo "$@" >> {self.amixer_log}\n')
        self.env = dict(
            os.environ,
            PATH=f'{self.bin}:/usr/bin:/bin',
            RGDS_AUDIO_RUN=str(self.run_dir),
            RGDS_ASOUND_CARDS=str(self.cards),
            RGDS_AMIXER=str(self.bin / 'amixer')
        )

    def audio(self, *args):
        result = subprocess.run(
            ['sh', str(LIBEXEC / 'audio'), *args], env=self.env, capture_output=True, text=True, timeout=10
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def read(self, name):
        return (self.run_dir / name).read_text()

    def test_rg_ds_plays_on_its_one_card_as_before(self):
        self.cards.write_text(self.RG_DS)
        self.audio('setup')
        self.assertIn('pcm "hw:0,0"', self.read('audio-out.conf'))
        self.assertIn('ipc_key 1380402259', self.read('audio-out.conf'))
        self.assertEqual(self.read('audio-mixer'), '0 rk817\n')
        self.assertEqual(self.read('audio-route'), 'speaker\n')
        self.assertIn('slave.pcm "hw:0,0"', self.read('audio-capture.conf'))
        self.assertFalse(self.amixer_log.exists())  # the RG DS's mixer is the headphones watcher's alone
        self.assertEqual(self.audio('cards').stdout.strip(), 'codec=0 speaker=')

    def test_plus_speakers_then_the_jack(self):
        self.cards.write_text(self.PLUS)
        self.audio('setup')
        self.assertIn('pcm "hw:1,0"', self.read('audio-out.conf'))
        self.assertEqual(self.read('audio-mixer'), '1 aw88166\n')
        self.assertIn('pcm "hw:0,0"', self.read('audio-capture.conf'))
        self.assertIn('rate 48000', self.read('audio-capture.conf'))
        self.assertIn('-c 0 sset Playback Mux HP', self.amixer_log.read_text())
        self.audio('route', 'headphones')
        out = self.read('audio-out.conf')
        self.assertIn('pcm "hw:0,0"', out)
        self.assertIn('ipc_key 1380402260', out)  # a stream of its own: the speakers' may still be open
        self.assertEqual(self.read('audio-mixer'), '0 rk817\n')
        self.assertEqual(self.read('audio-route'), 'headphones\n')
        self.assertIn('-c 1 sset Left PCM 1023', self.amixer_log.read_text())
        self.audio('route', 'speaker')
        self.assertIn('pcm "hw:1,0"', self.read('audio-out.conf'))
        # setup again (the watcher, once the amplifier card is up) keeps the route it finds
        self.audio('route', 'headphones')
        self.audio('setup')
        self.assertEqual(self.read('audio-route'), 'headphones\n')

    def test_a_bluetooth_headset_keeps_playback(self):
        self.cards.write_text(self.PLUS)
        (self.run_dir / 'bt-sink').write_text('AC:C1:22:33:44:5F\n')
        (self.run_dir / 'audio-out.conf').write_text('pcm.rgds_out "headset"\n')
        self.audio('route', 'headphones')
        self.assertEqual(self.read('audio-out.conf'), 'pcm.rgds_out "headset"\n')
        self.assertIn('hw:0,0', self.read('audio-local.conf'))  # what bt-audio gives playback back to

    def test_no_card_list_still_names_card_0(self):
        self.audio('setup')
        self.assertIn('pcm "hw:0,0"', self.read('audio-out.conf'))
