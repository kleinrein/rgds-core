"""RG DS OS shell helpers (os/buildroot/rootfs/usr/libexec/rgds) run against fake tools and files."""
import os
from pathlib import Path
import subprocess
import tempfile
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
        # each suspend takes the next line: "rtc" sleeps until the alarm, "user N" is woken by a person after N s;
        # a line may also set the battery ("rtc 3" = the alarm, then 3 %)
        tool(
            d / 'bin', 'suspend', f"""line=$(head -n 1 {self.wakes}); sed -i.bak 1d {self.wakes}
now=$(cat {d}/rtc/since_epoch)
set -- $line
case "$1" in
rtc) alarm=$(cat {d}/rtc/wakealarm); echo $((now + ${{alarm#+}})) > {d}/rtc/since_epoch
     [ -n "$2" ] && echo $2 > {d}/battery/capacity
     echo 255 > {d}/backlight/a/brightness ;;
user) echo $((now + $2)) > {d}/rtc/since_epoch ;;
*) exit 1 ;;
esac
"""
        )
        self.lid = d / 'lid'
        self.lid.write_text('1\n')
        tool(d / 'bin', 'lid', f'cat {self.lid}\n')
        self.env = dict(
            os.environ,
            RGDS_RUN=str(d / 'run'),
            RGDS_RTC=str(d / 'rtc'),
            RGDS_BATTERY_DIR=str(d / 'battery'),
            RGDS_CHARGER_DIR=str(d / 'charger'),
            RGDS_BACKLIGHTS=str(d / 'backlight'),
            RGDS_SLEEP_LED=str(d / 'led'),
            RGDS_SLEEP_LOG=str(d / 'sleep.log'),
            RGDS_SUSPEND_CMD=str(d / 'bin/suspend'),
            RGDS_LID_CMD=str(d / 'bin/lid'),
            RGDS_SLEEP_CHECK='3600',
            RGDS_SLEEP_LIMIT='10800'
        )

    def tearDown(self):
        self.temp.cleanup()

    def sleep(self, *wakes):
        self.wakes.write_text(''.join(w + '\n' for w in wakes))
        result = subprocess.run(
            ['sh', str(LIBEXEC / 'power'), 'suspend'], env=self.env, capture_output=True, text=True, timeout=10
        )
        state = dict(line.split('=', 1) for line in (self.dir / 'run/suspend-state').read_text().splitlines())
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


if __name__ == '__main__':
    unittest.main()
