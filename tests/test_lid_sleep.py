"""Lid cycles and game ownership, using isolated files and fake suspend only."""
import contextlib
import io
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from lid_sleep import LidSleep, PowerKey
from game_menu import GameMenu, MenuInput
from fields import read_fields, write_fields
from test_game_menu import Process, Inputs

# The launcher and supervisor must agree, including unknown samples and startup.
CYCLES = [
    (None, True, False),
    (True, True, False),
    (True, True, True),
    (True, True, False),
    (None, True, False),
    (True, True, False),
    (True, True, False),
    (False, True, False),
    (True, True, False),
    (True, True, False),
    (False, True, False),
    (False, True, False),
    (True, False, False),
    (True, False, False),
    (True, True, True),
    (True, True, False),
    (False, True, False),
    (None, True, False),
    (True, True, False),
    (True, True, False),
    (False, True, False),
    (False, True, False),
    (True, True, False),
    (False, True, False),
    (False, True, False),
    (True, True, False),
    (True, True, True),
]


class LidPollingTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        self.hardware = Mock()
        self.lid = LidSleep(self.data, self.hardware)
        self.now = 100
        clock = patch('lid_sleep.time.monotonic', side_effect=lambda: self.now)
        clock.start()
        self.addCleanup(clock.stop)

    def sample(self, closed, ready=True):
        self.now += .21
        self.hardware.lid_closed.return_value = closed
        return self.lid.poll(ready)

    def test_debounce_startup_deferred_close_and_no_resleep_until_stable_open(self):
        for closed, ready, expected in CYCLES:
            self.assertEqual(self.sample(closed, ready), expected, (closed, ready))
        self.hardware.sleep_lid.assert_not_called()

    def test_close_after_open_while_owner_busy_rearms_from_generation(self):
        self.hardware.lid_generation = 1
        self.assertFalse(self.sample(True))
        self.assertTrue(self.sample(True))
        self.assertFalse(self.sample(True))
        self.hardware.lid_generation = 2  # Open/close occurred during helper wait.
        self.assertFalse(self.sample(True))
        self.assertTrue(self.sample(True))
        self.assertFalse(self.sample(True))

    def test_poll_rate_is_bounded_with_no_helper_or_filesystem_writes(self):
        self.hardware.lid_closed.return_value = False
        for _ in range(100):
            self.lid.poll()
        self.hardware.lid_closed.assert_called_once()
        self.assertEqual(list(self.data.iterdir()), [])

    def test_existing_off_is_preserved_and_enabling_with_closed_lid_works(self):
        write_fields(self.data / 'system.ini', dict(schema=1, lid=0))
        self.assertFalse(self.sample(True))
        self.assertFalse(self.sample(True))
        write_fields(self.data / 'system.ini', dict(schema=1, lid=1))
        self.assertTrue(self.sample(True))
        self.assertFalse(self.sample(True))

    @unittest.skipUnless(shutil.which('clang++'), 'Host compiler required')
    def test_launcher_uses_the_same_edge_contract(self):
        source = self.data / 'lid.cpp'
        binary = self.data / 'lid'
        cases = ',\n'.join(
            '{%d,%d,%d}' % (-1 if state is None else state, ready, result) for state, ready, result in CYCLES
        )
        source.write_text(
            '''#include "platform/lid_sleep.hpp"
#include <cassert>
int main(){rgds::LidSleep lid;int cases[][3]={''' + cases + '''};
 for(auto& row:cases){std::optional<bool> state;
 if(row[0]>=0)state=bool(row[0]);assert(lid.poll(state,row[1])==bool(row[2]));}
 rgds::LidSleep fast;
 assert(!fast.poll(true,true,1));assert(fast.poll(true,true,1));
 assert(!fast.poll(true,true,1));assert(!fast.poll(true,true,2));
 assert(fast.poll(true,true,2));assert(!fast.poll(true,true,2));}
'''
        )
        subprocess.run(
            [
                'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                str(source), '-o',
                str(binary)
            ],
            check=True,
            capture_output=True,
            text=True
        )
        subprocess.run([str(binary)], check=True, timeout=3)


class GameLidTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.now = 100
        clock = patch('lid_sleep.time.monotonic', side_effect=lambda: self.now)
        clock.start()
        self.addCleanup(clock.stop)

    def make(self, core='drastic', live=False):
        child = Process()
        inputs = Inputs()
        inputs.discard_after_sleep = Mock()
        menu = GameMenu(
            child, self.root / 'app', self.root / core, core, self.root / 'game.nds',
            {'RGDS_MENU_LIVE': '1' if live else '0'}, inputs
        )
        menu.lid.hardware = Mock()
        menu.lid.hardware.lid_closed.return_value = False
        self.addCleanup(menu.close)
        return menu

    def sample(self, menu, closed):
        menu.lid.hardware.lid_closed.return_value = closed
        self.now += .21
        menu.poll()

    def test_ten_close_open_cycles_for_every_engine_playing_and_in_both_menu_modes(self):
        for core in ('drastic', 'dsperate', 'nnddss', 'melonds'):
            for mode in ('playing', 'paused-menu', 'live-menu'):
                with self.subTest(core=core, mode=mode):
                    menu = self.make(core, mode == 'live-menu')
                    ui = Process()
                    if mode != 'playing':
                        with patch('game_menu.subprocess.Popen', return_value=ui):
                            menu.open()
                        menu.ready.write_text(str(ui.pid))
                    paused = menu.paused
                    original_signals = len(menu.child.signals)
                    for cycle in range(10):
                        self.sample(menu, False)
                        self.sample(menu, False)

                        def suspended():
                            self.assertTrue(menu.paused)
                            self.assertTrue(menu.block.exists())
                            if mode != 'playing': self.assertEqual(ui.signals[-1], signal.SIGSTOP)
                            menu.trigger.touch()
                            menu.home_trigger.touch()

                        menu.lid.hardware.sleep_lid.side_effect = suspended
                        self.sample(menu, True)
                        self.sample(menu, True)
                        self.assertEqual(menu.lid.hardware.sleep_lid.call_count, cycle + 1)
                        self.assertEqual(menu.paused, paused)
                        self.assertEqual(menu.block.exists(), mode != 'playing')
                        self.assertFalse(menu.trigger.exists())
                        self.assertFalse(menu.home_trigger.exists())
                        self.assertIsNone(menu.pending)
                        self.assertFalse(menu.has_checkpoint())
                        self.assertIn('realtime', read_fields(Path(str(menu.control) + '.resume')))
                        self.sample(menu, True)
                        self.sample(menu, None)
                        self.sample(menu, True)
                        self.sample(menu, True)
                        self.assertEqual(menu.lid.hardware.sleep_lid.call_count, cycle + 1)
                    self.assertEqual(menu.inputs.discard_after_sleep.call_count, 10)
                    if paused:
                        self.assertEqual(len(menu.child.signals), original_signals)
                    else:
                        self.assertEqual(menu.child.signals[original_signals:], [signal.SIGSTOP, signal.SIGCONT] * 10)
                    if mode != 'playing':
                        self.assertEqual(ui.signals, [signal.SIGSTOP, signal.SIGCONT] * 10)
                        self.assertTrue((menu.folder / 'game-menu.wake').exists())
                    menu.close()

    def test_save_finishes_before_sleep_and_exit_request_never_suspends(self):
        menu = self.make()
        with patch('game_menu.subprocess.Popen', return_value=Process()):
            menu.open()
        menu.send('save')
        self.sample(menu, True)
        self.sample(menu, True)
        menu.lid.hardware.sleep_lid.assert_not_called()
        checkpoint = menu.checkpoint()
        checkpoint.parent.mkdir(parents=True, exist_ok=True)
        checkpoint.write_bytes(b'actual checkpoint')
        write_fields(Path(str(menu.control) + '.result'), dict(token=menu.pending['token'], state='saved'))
        self.sample(menu, True)
        self.assertIsNone(menu.pending)
        self.sample(menu, True)
        menu.lid.hardware.sleep_lid.assert_called_once()
        self.assertEqual(checkpoint.read_bytes(), b'actual checkpoint')
        self.sample(menu, False)
        self.sample(menu, False)
        menu.send('save-quit')
        write_fields(Path(str(menu.control) + '.result'), dict(token=menu.pending['token'], state='saved'))
        self.sample(menu, True)
        self.sample(menu, True)
        menu.lid.hardware.sleep_lid.assert_called_once()
        menu.child.result = 0
        self.sample(menu, True)
        menu.lid.hardware.sleep_lid.assert_called_once()

    def test_helper_failure_restores_running_game_and_requires_reopen_before_retry(self):
        menu = self.make()
        menu.lid.hardware.sleep_lid.side_effect = ValueError('Stock helper unavailable')
        with contextlib.redirect_stdout(io.StringIO()):
            self.sample(menu, True)
            self.sample(menu, True)
            self.assertEqual(menu.child.signals, [signal.SIGSTOP, signal.SIGCONT])
            self.assertFalse(menu.paused)
            self.assertFalse(menu.block.exists())
            for _ in range(20):
                self.sample(menu, True)
            menu.lid.hardware.sleep_lid.assert_called_once()
            self.sample(menu, False)
            self.sample(menu, False)
            self.sample(menu, True)
            self.sample(menu, True)
            self.assertEqual(menu.lid.hardware.sleep_lid.call_count, 2)

    def test_failed_resume_record_cannot_leave_engine_or_menu_stopped(self):
        menu = self.make(live=True)
        ui = Process()
        with patch('game_menu.subprocess.Popen', return_value=ui):
            menu.open()
        with patch('game_menu.write_fields',
                   side_effect=OSError('Storage unavailable')), contextlib.redirect_stdout(io.StringIO()):
            menu.sleep_for_lid()
        self.assertFalse(menu.paused)
        self.assertEqual(menu.child.signals[-1], signal.SIGCONT)
        self.assertEqual(ui.signals[-1], signal.SIGCONT)

    def test_sound_sink_is_released_before_stopping_ui_and_restored_even_if_wake_record_fails(self):
        menu = self.make(live=True)
        ui = Process()
        write_fields(menu.data / 'ui.ini', dict(menu_sounds=1))
        with patch('game_menu.subprocess.Popen', return_value=ui):
            menu.open()
        request = menu.folder / 'game-menu.audio-sleep'
        ready = menu.folder / 'game-menu.audio-sleep.ready'

        def ui_ack(duration):
            self.assertTrue(request.exists())
            self.assertNotIn(signal.SIGSTOP, ui.signals)
            ready.touch()
            self.now += duration

        def suspend():
            self.assertTrue(ready.exists())
            self.assertEqual(ui.signals[-1], signal.SIGSTOP)

        menu.lid.hardware.sleep_lid.side_effect = suspend
        with patch('game_menu.time.sleep',
                   side_effect=ui_ack), patch('game_menu.write_fields',
                                              side_effect=OSError('Storage unavailable')), contextlib.redirect_stdout(
                                                  io.StringIO()
                                              ):
            menu.sleep_for_lid()
        self.assertFalse(request.exists())
        self.assertFalse(ready.exists())
        self.assertTrue((menu.folder / 'game-menu.wake').exists())
        self.assertEqual(ui.signals, [signal.SIGSTOP, signal.SIGCONT])
        self.assertFalse(menu.paused)

    def test_unresponsive_menu_audio_ack_does_not_prevent_sleep(self):
        menu = self.make()
        menu.menu = Process()
        write_fields(menu.data / 'ui.ini', dict(menu_sounds=1))

        def wait(duration):
            self.now += duration

        began = self.now
        with patch('game_menu.time.sleep', side_effect=wait):
            menu.sleep_for_lid()
        self.assertLess(self.now - began, .4)
        menu.lid.hardware.sleep_lid.assert_called_once()
        self.assertFalse((menu.folder / 'game-menu.audio-sleep').exists())
        self.assertEqual(menu.menu.signals, [signal.SIGSTOP, signal.SIGCONT])


# (step, seconds since the start, ready, sleep wanted): the launcher and the game runner must agree.
POWER_KEY = [
    ('up', 0.0, True, None),  # let go after holding it to boot: a release with no press
    ('down', 1.0, True, None),
    ('up', 1.1, False, None),  # a save is finishing: kept until it is ready
    ('poll', 1.2, False, None),
    ('poll', 1.3, True, 'sleep'),
    ('poll', 1.4, True, None),
    ('down', 2.0, True, None),
    ('poll', 3.9, True, None),
    ('poll', 4.0, True, 'poweroff'),  # held for two seconds: no need to let go
    ('up', 4.5, True, None),  # and letting go then does not sleep as well
    ('down', 5.0, True, None),
    ('up', 7.0, False, None),  # the loop was busy: a hold all the same, once ready
    ('poll', 7.1, True, 'poweroff'),
    ('down', 8.0, True, None),
    ('up', 8.1, True, 'sleep'),
    ('down', 9.0, True, None),  # pressed again before the sleep began: thrown away
    ('slept', 10.0, True, None),
    ('down', 10.1, True, None),  # the press that woke it, reported late
    ('poll', 13.0, True, None),  # even held: waking must not power off
    ('up', 13.1, True, None),
]
QUIET_POWER_KEY = [
    ('slept', 0.0, True, None),
    ('down', 0.9, True, None),  # still inside the wake's quiet second, released after it
    ('up', 1.1, True, None),
    ('down', 1.2, True, None),
    ('up', 1.3, True, 'sleep'),
]


class PowerKeyTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_press_sleeps_hold_powers_off_and_never_the_press_that_woke_it(self):
        for table in (POWER_KEY, QUIET_POWER_KEY):
            key = PowerKey(self.root / 'absent')
            with patch('lid_sleep.time.monotonic') as clock:
                for step, now, ready, wanted in table:
                    clock.return_value = now
                    if step == 'slept': key.after_sleep()
                    elif step != 'poll': key.feed(int(step == 'down'), now)
                    key.tick(now)
                    self.assertEqual(key.take(ready), wanted, (step, now))

    @unittest.skipUnless(shutil.which('clang++'), 'Host compiler required')
    def test_launcher_uses_the_same_contract(self):
        source = self.root / 'power.cpp'
        binary = self.root / 'power'

        def rows(table):
            return ',\n'.join(
                '{%d,%d,%d,%d}' % (
                    'up down slept poll'.split().index(step), round(now * 1000), ready,
                    [None, 'sleep', 'poweroff'].index(wanted)
                ) for step, now, ready, wanted in table
            )

        source.write_text(
            '''#include "platform/power_key.hpp"
#include <cassert>
template <size_t N> void run(const int (&cases)[N][4]){rgds::PowerKey key;
 for(auto& row:cases){
 if(row[0]==2)key.after_sleep(row[1]);else if(row[0]!=3)key.feed(row[0],row[1]);
 key.tick(row[1]);
 assert(key.take(row[2],row[2])==rgds::PowerPress(row[3]));}}
int main(){const int main_cases[][4]={''' + rows(POWER_KEY) + '''};
 const int quiet_cases[][4]={''' + rows(QUIET_POWER_KEY) + '''};
 run(main_cases);run(quiet_cases);
 rgds::PowerKey split;split.feed(1,0);split.tick(2000);
 assert(split.take(true,false)==rgds::PowerPress::None);
 assert(split.take(false,true)==rgds::PowerPress::Off);}
'''
        )
        subprocess.run(
            [
                'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                str(source), '-o',
                str(binary)
            ],
            check=True,
            capture_output=True,
            text=True
        )
        subprocess.run([str(binary)], check=True, timeout=3)

    def test_events_from_the_device_and_a_game_sleeping_on_them(self):
        child, inputs = Process(), Inputs()
        menu = GameMenu(child, self.root / 'app', self.root / 'dscore', 'dscore', self.root / 'game.nds', {}, inputs)
        menu.lid.hardware = Mock()
        menu.lid.hardware.lid_closed.return_value = False
        self.addCleanup(menu.close)
        read, write = os.pipe()
        self.addCleanup(os.close, write)
        os.set_blocking(read, False)
        menu.power_key.fd = read

        def event(kind, code, value):
            os.write(write, struct.pack('llHHi', 0, 0, kind, code, value))

        event(1, 114, 1)  # another key on the same node
        event(1, 116, 1)
        menu.poll()
        menu.lid.hardware.sleep.assert_not_called()
        event(0, 3, 0)  # SYN_DROPPED: the release may be lost, never guess
        event(1, 116, 0)
        menu.poll()
        menu.lid.hardware.sleep.assert_not_called()
        event(1, 116, 1)
        event(1, 116, 0)
        event(1, 116, 1)  # the second press, made while it goes to sleep, is the one that woke it
        menu.lid.hardware.sleep.side_effect = lambda: self.assertTrue(menu.paused)
        menu.poll()
        menu.lid.hardware.sleep.assert_called_once_with()
        menu.lid.hardware.sleep_lid.assert_not_called()
        self.assertFalse(menu.paused)
        event(1, 116, 0)
        menu.poll()
        menu.lid.hardware.sleep.assert_called_once_with()

    def test_a_held_key_saves_the_game_and_leaves_the_request_for_the_launcher(self):
        menu = GameMenu(
            Process(), self.root / 'app', self.root / 'dscore', 'dscore', self.root / 'game.nds', {}, Inputs()
        )
        menu.lid.hardware = Mock()
        menu.lid.hardware.lid_closed.return_value = False
        self.addCleanup(menu.close)
        request = self.root / 'poweroff-requested'
        menu.power_key.fd = os.pipe()[0]
        os.set_blocking(menu.power_key.fd, False)
        menu.power_key.armed = True
        menu.power_key.pressed_at = time.monotonic() - 3
        with patch('game_menu.POWEROFF_REQUEST', request), \
                patch('game_menu.subprocess.Popen', return_value=Process()):
            menu.poll()
        self.assertTrue(request.exists())
        self.assertEqual(menu.pending['action'] if isinstance(menu.pending, dict) else menu.pending, 'save-quit')
        self.assertFalse(menu.restart)
        menu.lid.hardware.sleep.assert_not_called()


class WakeInputTests(unittest.TestCase):
    def test_queued_and_held_shortcuts_are_discarded_until_fresh_press(self):
        with patch('game_menu.Path.glob', return_value=[]):
            inputs = MenuInput()
        inputs.descriptors = [123]
        inputs.codes[123] = (139,)
        inputs.starts[123] = 315
        inputs.shortcut.pending = 10
        inputs.speed.held.add(123)
        inputs.actions.append('home')

        def ioctl(fd, request, data):
            if request == 0x80604518: data[139 // 8] |= 1 << (139 % 8)
            else: struct.pack_into('i', data, 0, 1)

        with patch('game_menu.os.read', side_effect=BlockingIOError), patch('game_menu.fcntl.ioctl', side_effect=ioctl):
            inputs.discard_after_sleep()
        self.assertFalse(inputs.actions)
        self.assertFalse(inputs.speed.held)
        self.assertIsNone(inputs.shortcut.pending)
        self.assertIn((123, 1, 139), inputs.suppressed_keys)
        self.assertIn((123, 3, 16), inputs.suppressed_keys)
        from audio import EVENT

        def feed(value):
            with patch('game_menu.select.select',
                       return_value=([123], [],
                                     [])), patch('game_menu.os.read', return_value=EVENT.pack(1, 0, 1, 139, value)):
                return inputs.poll()

        self.assertFalse(feed(1))
        self.assertIsNone(inputs.shortcut.pending)
        self.assertFalse(feed(0))
        self.assertNotIn((123, 1, 139), inputs.suppressed_keys)
        # KEY_MENU is the dedicated game-menu key: a fresh press opens it at once.
        self.assertEqual(feed(1), 'game-menu')


class OwnedProcessSleepTests(unittest.TestCase):
    def test_real_owned_process_stops_and_resumes_even_when_suspend_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            beat = root / 'heartbeat'
            child = subprocess.Popen(
                [
                    sys.executable, '-c', 'import pathlib,sys,time\np=pathlib.Path(sys.argv[1])\n'
                    'while True:\n p.write_text(str(time.time_ns()))\n time.sleep(.01)\n',
                    str(beat)
                ]
            )
            menu = GameMenu(child, root / 'app', root / 'data', 'dsperate', root / 'game.nds', {}, Inputs())
            menu.lid.hardware = Mock()
            try:
                deadline = time.time() + 3
                while not beat.exists() and time.time() < deadline:
                    time.sleep(.01)
                self.assertTrue(beat.exists())
                for failed in (False, True):

                    def helper():
                        deadline = time.time() + 3
                        while time.time() < deadline:
                            pid, status = os.waitpid(child.pid, os.WUNTRACED | os.WNOHANG)
                            if pid:
                                self.assertTrue(os.WIFSTOPPED(status))
                                break
                            time.sleep(.01)
                        else:
                            self.fail('Owned engine did not stop')
                        before = beat.read_text()
                        time.sleep(.04)
                        self.assertEqual(beat.read_text(), before)
                        if failed: raise ValueError('Fake suspend failed')

                    menu.lid.hardware.sleep_lid.side_effect = helper
                    with contextlib.redirect_stdout(io.StringIO()):
                        menu.sleep_for_lid()
                    self.assertFalse(menu.paused)
                    self.assertIsNone(child.poll())
                    before = beat.read_text()
                    deadline = time.time() + 3
                    while beat.read_text() == before and time.time() < deadline:
                        time.sleep(.01)
                    self.assertNotEqual(beat.read_text(), before)
            finally:
                menu.close()
                if child.poll() is None: child.terminate()
                child.wait(timeout=3)


if __name__ == '__main__': unittest.main()
