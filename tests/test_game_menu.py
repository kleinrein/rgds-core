"""Pause/resume, failed saves and exit ownership with no real device processes."""
from pathlib import Path
import contextlib
import io
import json
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from game_menu import GameMenu, menu_environment
from library import read_fields, write_fields


class Process:
    def __init__(self):
        self.result = None
        self.pid = 456
        self.signals = []

    def poll(self):
        return self.result

    def send_signal(self, value):
        self.signals.append(value)

    def terminate(self):
        self.result = 0

    def kill(self):
        self.result = -9

    def wait(self, timeout=None):
        return self.result


class Inputs:
    def poll(self):
        return False

    def close(self):
        pass


class MenuEnvironmentTests(unittest.TestCase):
    def test_unique_touch_is_cached_without_mutating_engine_environment(self):
        source = {'WAYLAND_DISPLAY': 'wayland-0'}
        with patch(
            'input_devices.discover', return_value={
                'touch': '/dev/input/event1',
                'touch_ambiguous': False
            }
        ) as discover:
            result = menu_environment(source)
        discover.assert_called_once_with()
        self.assertEqual(result, dict(source, RGDS_TOUCH_DEV='/dev/input/event1'))
        self.assertNotIn('RGDS_TOUCH_DEV', source)

    def test_explicit_hardware_profile_is_not_replaced_or_rediscovered(self):
        source = {'RGDS_TOUCH_DEV': '/dev/input/event7'}
        with patch('input_devices.discover') as discover:
            self.assertEqual(menu_environment(source), source)
        discover.assert_not_called()

    def test_missing_or_ambiguous_touch_keeps_native_fallback(self):
        for inputs in (
            {
                'touch': None,
                'touch_ambiguous': False
            }, {
                'touch': None,
                'touch_ambiguous': True
            }, {
                'touch': '/dev/input/event1',
                'touch_ambiguous': True
            }
        ):
            with self.subTest(inputs=inputs), patch('input_devices.discover', return_value=inputs):
                self.assertNotIn('RGDS_TOUCH_DEV', menu_environment({}))
        with patch('input_devices.discover', side_effect=OSError('sysfs unavailable')):
            self.assertNotIn('RGDS_TOUCH_DEV', menu_environment({}))


class GameMenuTests(unittest.TestCase):
    def test_graphics_status_uses_launched_renderer_not_newly_saved_preferences(self):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            menu = GameMenu(
                Process(), data / 'app', data / 'data', 'rgds-experimental', data / 'game.nds', {
                    'RGDS_ACTIVE_SCALE': '3',
                    'RGDS_3D_SMOOTH': '1'
                }, Inputs()
            )
            try:
                menu.publish('paused')
                self.assertEqual(read_fields(menu.status)['active_scale'], '3')
                self.assertEqual(read_fields(menu.status)['active_smooth'], '1')
            finally:
                menu.close()

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.clock = 100
        self.time = patch("game_menu.time.monotonic", side_effect=lambda: self.clock)
        self.time.start()
        self.addCleanup(self.time.stop)
        self.child, self.ui = Process(), Process()
        self.menu = GameMenu(
            self.child, self.root / "app", self.root / "data", "drastic", self.root / "game.nds", {
                "LD_PRELOAD": "old.so",
                "WAYLAND_DISPLAY": "wayland-0"
            }, Inputs()
        )
        self.addCleanup(self.menu.close)
        with patch("game_menu.subprocess.Popen", return_value=self.ui) as spawn:
            self.menu.open()
            self.menu.ready.write_text(str(self.ui.pid))
            self.assertIn("--game-menu", spawn.call_args.args[0])
            self.assertNotIn("LD_PRELOAD", spawn.call_args.kwargs["env"])
            self.assertEqual(spawn.call_args.kwargs["env"]["RGDS_VOLUME_EXTERNAL"], "1")

    def request(self, action, token="1"):
        write_fields(self.menu.request, dict(token=token, action=action))
        self.menu.poll()

    def result(self, state, token=None, checkpoint=True):
        result_token = token or self.menu.pending["token"]
        if state == 'saved' and checkpoint and result_token == self.menu.pending["token"]:
            saved = self.menu.checkpoint()
            saved.parent.mkdir(parents=True, exist_ok=True)
            saved.write_bytes(b'complete checkpoint')
        write_fields(Path(str(self.menu.control) + ".result"), dict(token=result_token, state=state))
        self.clock += 1
        self.menu.poll()

    def test_reset_state_saves_before_requesting_fresh_restart(self):
        self.request('reset-state')
        self.assertTrue(self.menu.restart)
        self.assertTrue(self.menu.reset_state)
        self.assertEqual(self.menu.pending['action'], 'save-quit')
        self.result('save-failed')
        self.assertFalse(self.menu.restart)
        self.assertFalse(self.menu.reset_state)
        self.assertIsNone(self.child.poll())
        self.request('resume', '2')
        self.assertFalse(self.menu.restart)

    def test_slot_load_captures_current_progress_before_loading(self):
        vault = self.menu.vault()
        vault.canonical.write_bytes(b'old slot')
        vault.store(0)
        self.request('slot-load:0')
        self.assertEqual(self.menu.pending['action'], 'save')
        self.result('saved')
        self.assertEqual(self.menu.pending['action'], 'load')
        self.assertEqual(vault.canonical.read_bytes(), b'old slot')
        self.assertEqual(vault.checked(vault.catalog()['undo_load']).read_bytes(), b'complete checkpoint')
        self.result('loaded')
        self.assertIsNone(self.menu.pending)

    def test_slot_overwrite_requires_confirmation_and_failed_save_preserves_slot(self):
        vault = self.menu.vault()
        vault.canonical.write_bytes(b'old slot')
        vault.store(0)
        self.request('slot-save:0')
        self.assertIsNone(self.menu.pending)
        self.assertIn('Confirm', read_fields(self.menu.status)['message'])
        self.request('slot-save:0:yes', '2')
        self.result('save-failed')
        self.assertEqual(vault.checked(vault.catalog()['slots']['0']).read_bytes(), b'old slot')

    def test_resume_clears_pause_and_discards_queued_input(self):
        self.assertEqual(self.child.signals, [signal.SIGSTOP])
        self.assertTrue(self.menu.block.exists())
        self.request("resume")
        self.assertEqual(self.child.signals[-1], signal.SIGCONT)
        self.assertFalse(self.menu.block.exists())
        self.assertTrue(Path(str(self.menu.control) + ".resume").exists())

    def test_engine_resumes_before_ui_teardown_with_input_blocked_until_done(self):
        resume_file = Path(str(self.menu.control) + '.resume')
        self.menu.environment['RGDS_LAUNCH_PROFILE'] = '1'
        steps = []

        def terminate():
            self.assertFalse(self.menu.paused)
            self.assertEqual(self.child.signals[-1], signal.SIGCONT)
            self.assertTrue(self.menu.block.exists())
            self.assertFalse(resume_file.exists())
            steps.append('terminate')
            self.clock += .1

        def wait(timeout=None):
            self.assertEqual(timeout, 2)
            self.assertTrue(self.menu.block.exists())
            self.assertTrue(self.menu.ready.exists())
            self.assertFalse(resume_file.exists())
            steps.append('wait')
            self.clock += .2
            self.ui.result = 0
            return 0

        def write_checked(path, *args, **kwargs):
            if path == resume_file:
                self.assertTrue(self.menu.block.exists())
                self.assertFalse(self.menu.ready.exists())
                steps.append('resume-marker')
            return write_fields(path, *args, **kwargs)

        output = io.StringIO()
        with patch.object(self.ui, 'terminate',
                          side_effect=terminate), patch.object(self.ui, 'wait', side_effect=wait), patch(
                              'game_menu.write_fields', side_effect=write_checked
                          ), contextlib.redirect_stdout(output):
            self.menu.close_menu()
        self.assertEqual(steps, ['terminate', 'wait', 'resume-marker'])
        self.assertFalse(self.menu.block.exists())
        self.assertFalse(self.menu.ready.exists())
        self.assertTrue(resume_file.exists())
        record = json.loads(output.getvalue())
        self.assertEqual(record['type'], 'rgds-menu-close-profile')
        self.assertFalse(record['killed'])
        self.assertEqual(record['exit_code'], 0)
        self.assertLessEqual(record['resumed_monotonic'], record['terminated_monotonic'])
        self.assertAlmostEqual(record['ended_monotonic'] - record['started_monotonic'], .3)

    def test_ui_teardown_timeout_remains_bounded_and_reports_forced_exit(self):
        def wait(timeout=None):
            self.assertFalse(self.menu.paused)
            self.assertTrue(self.menu.block.exists())
            if timeout is not None: raise subprocess.TimeoutExpired('fixture menu', timeout)
            return self.ui.result

        output = io.StringIO()
        with patch.object(self.ui, 'terminate'), patch.object(self.ui, 'wait',
                                                              side_effect=wait), contextlib.redirect_stderr(output):
            self.menu.close_menu()
        self.assertEqual(self.ui.result, -9)
        self.assertIn('did not exit within 2 seconds', output.getvalue())
        self.assertFalse(self.menu.block.exists())
        self.assertTrue(Path(str(self.menu.control) + '.resume').exists())

    def test_failed_menu_creation_resumes_before_unblocking_and_discards_queued_input(self):
        self.menu.close_menu()
        resume_file = Path(str(self.menu.control) + '.resume')
        resume_file.unlink()
        original = self.child.send_signal

        def signal_checked(value):
            if value == signal.SIGCONT: self.assertTrue(self.menu.block.exists())
            original(value)

        with patch.object(self.child, 'send_signal', side_effect=signal_checked
                          ), patch('game_menu.subprocess.Popen', side_effect=OSError('fixture spawn failure')):
            with self.assertRaisesRegex(OSError, 'fixture spawn failure'):
                self.menu.open()
        self.assertFalse(self.menu.paused)
        self.assertIsNone(self.menu.menu)
        self.assertFalse(self.menu.block.exists())
        self.assertTrue(resume_file.exists())

    def test_helper_exit_before_presentation_is_reported_and_restores_game(self):
        self.menu.ready.unlink()
        self.ui.result = 0
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.menu.poll()
        self.assertIn('exited with code 0; presented=0', output.getvalue())
        self.assertIsNone(self.menu.menu)
        self.assertFalse(self.menu.paused)

    def test_paused_menu_uses_own_windows_and_has_a_first_frame_deadline(self):
        self.menu.close_menu()
        self.ui.result = None
        overlay = self.root / 'overlay.frame'
        overlay.write_bytes(struct.pack('7I', 0x52474453, 1, 640, 480, 1, 1, 1))
        self.menu.overlay_path = overlay
        self.menu.environment['RGDS_OVERLAY_FILE'] = str(overlay)
        with patch('game_menu.subprocess.Popen', return_value=self.ui) as spawn:
            self.menu.open()
        env = spawn.call_args.kwargs['env']
        self.assertNotIn('RGDS_OVERLAY_FILE', env)
        self.assertEqual(env['RGDS_MENU_READY_FILE'], str(self.menu.ready))
        self.assertTrue(self.menu.paused)
        # A stale readiness marker from another PID cannot hide a failed UI.
        self.menu.ready.write_text('123')
        self.clock += 6
        self.menu.poll()
        self.assertIsNone(self.menu.menu)
        self.assertFalse(self.menu.paused)
        self.assertFalse(self.menu.block.exists())
        self.assertIn('did not render', read_fields(self.menu.status)['message'])

    def test_presented_paused_menu_survives_without_engine_overlay_ack(self):
        self.menu.overlay_path = self.root / 'no-shared-frame'
        self.clock += 6
        self.menu.poll()
        self.assertTrue(self.menu.direct_ready)
        self.assertIs(self.menu.menu, self.ui)
        self.assertTrue(self.menu.paused)

    def test_independent_menu_preserves_hardware_mode_and_display_names(self):
        self.menu.close_menu()
        self.menu.environment.update(
            RGDS_SESSION_PLATFORM='independent',
            RGDS_TOP_DISPLAY='DSI-2',
            RGDS_BOTTOM_DISPLAY='DSI-1',
            RGDS_LID_DEV='/dev/input/event5',
            RGDS_LID_WAKEUP='/sys/devices/lid/power/wakeup',
            SDL_RENDER_DRIVER='opengles2',
            SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR='0'
        )
        with patch('game_menu.subprocess.Popen', return_value=self.ui) as spawn:
            self.menu.open()
        env = spawn.call_args.kwargs['env']
        command = spawn.call_args.args[0]
        self.assertEqual(env['RGDS_SESSION_PLATFORM'], 'independent')
        self.assertEqual(env['RGDS_LID_WAKEUP'], '/sys/devices/lid/power/wakeup')
        self.assertEqual(env['SDL_RENDER_DRIVER'], 'opengles2')
        self.assertEqual(env['SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR'], '0')
        self.assertEqual(command[command.index('--top-display') + 1], 'DSI-2')
        self.assertEqual(command[command.index('--bottom-display') + 1], 'DSI-1')

    def test_menu_receives_session_launch_profiling_opt_in(self):
        self.menu.close_menu()
        self.menu.environment['RGDS_LAUNCH_PROFILE'] = '1'
        # The menu uses the supplied session environment even if its guardian
        # did not inherit the diagnostic flag in its own process environment.
        with patch.dict('game_menu.os.environ', {}, clear=True), \
             patch('game_menu.subprocess.Popen',return_value=self.ui) as spawn:
            self.menu.open()
        self.assertEqual(spawn.call_args.kwargs['env']['RGDS_LAUNCH_PROFILE'], '1')

    def test_reopening_menu_reuses_validated_touch_candidate(self):
        self.menu.close_menu()
        with patch(
            'input_devices.discover', return_value={
                'touch': '/dev/input/event1',
                'touch_ambiguous': False
            }
        ) as discover:
            self.menu.environment = menu_environment({})
            with patch('game_menu.subprocess.Popen', return_value=self.ui) as spawn:
                self.menu.open()
                self.menu.close_menu()
                self.menu.open()
        discover.assert_called_once_with()
        self.assertEqual(spawn.call_count, 2)
        self.assertTrue(
            all(call.kwargs['env']['RGDS_TOUCH_DEV'] == '/dev/input/event1' for call in spawn.call_args_list)
        )

    def test_mapped_speed_does_not_open_a_menu_or_hold_checkpoint_command_pending(self):
        self.menu.close_menu()

        class SpeedInput:
            def poll(self):
                return 'fast-forward-off'

            def close(self):
                pass

        self.menu.inputs = SpeedInput()
        self.menu.poll()
        self.assertIsNone(self.menu.menu)
        self.assertIsNone(self.menu.pending)
        request = read_fields(Path(str(self.menu.control) + '.speed-request'))
        self.assertEqual(request['action'], 'fast-forward-off')

    def test_saved_acknowledgement_pauses_again_and_stale_ack_is_ignored(self):
        self.request("save")
        self.assertEqual(self.child.signals[-1], signal.SIGCONT)
        self.result("saved", token="another-command")
        self.assertIsNotNone(self.menu.pending)
        self.assertFalse(self.menu.has_checkpoint())
        self.result("saved")
        self.assertIsNone(self.menu.pending)
        self.assertEqual(self.child.signals[-1], signal.SIGSTOP)
        self.assertEqual(read_fields(self.menu.status)["message"], "Checkpoint saved.")

    def test_saved_quit_ack_without_checkpoint_keeps_the_engine_and_menu_alive(self):
        self.request("quit")
        self.result("saved", checkpoint=False)
        self.assertFalse(self.menu.has_checkpoint())
        self.assertIsNone(self.menu.pending)
        self.assertIsNone(self.child.result)
        self.assertIs(self.menu.menu, self.ui)
        self.assertTrue(self.menu.block.exists())
        self.assertEqual(read_fields(self.menu.status)["state"], "error")
        self.assertIn("Could not save", read_fields(self.menu.status)["message"])

    def test_return_home_waits_for_actual_engine_exit_after_saving(self):
        self.request("quit")
        self.result("saved")
        self.assertEqual(read_fields(self.menu.status)["state"], "busy")
        self.assertIsNotNone(self.menu.menu)
        self.child.result = 0
        self.menu.poll()
        self.assertEqual(read_fields(self.menu.status)["state"], "home")
        self.assertIsNone(self.menu.menu)
        self.assertFalse(self.menu.block.exists())

    def test_save_timeout_preserves_previous_checkpoint_and_allows_retry(self):
        checkpoint = self.menu.checkpoint()
        checkpoint.parent.mkdir(parents=True, exist_ok=True)
        checkpoint.write_bytes(b"old")
        self.request("quit")
        self.result("save-timeout")
        self.assertEqual(checkpoint.read_bytes(), b"old")
        self.assertEqual(read_fields(self.menu.status)["state"], "error")
        self.assertIsNone(self.child.result)
        self.request("quit", token="retry")
        self.assertIsNotNone(self.menu.pending)

    def test_a_hung_engine_gets_a_visible_error_and_an_explicit_exit_route(self):
        self.request("quit")
        self.clock += 13
        self.menu.poll()
        self.assertEqual(read_fields(self.menu.status)["state"], "error")
        self.assertIsNone(self.child.result)
        self.request("force-quit", token="exit")
        self.assertEqual(self.child.result, 0)

    def test_load_requires_this_engines_checkpoint_and_reports_dispatch_honestly(self):
        self.request("load")
        self.assertIsNone(self.menu.pending)
        self.assertIn("No checkpoint", read_fields(self.menu.status)["message"])
        checkpoint = self.menu.checkpoint()
        checkpoint.parent.mkdir(parents=True, exist_ok=True)
        checkpoint.write_bytes(b"state")
        self.request("load", token="load")
        self.result("load-dispatched")
        self.assertIn("Load requested", read_fields(self.menu.status)["message"])

    def test_menu_time_is_excluded_and_ui_failure_resumes_owned_engine(self):
        self.clock += 30
        self.assertEqual(self.menu.played_seconds(), 0)
        self.ui.result = 1
        self.menu.poll()
        self.assertFalse(self.menu.paused)
        self.clock += 10
        self.assertEqual(self.menu.played_seconds(), 10)

    def test_live_overlay_blocks_input_without_stopping_the_engine(self):
        child = Process()
        menu = GameMenu(
            child, self.root / 'app', self.root / 'live', 'drastic', self.root / 'game.nds', {'RGDS_MENU_LIVE': '1'},
            Inputs()
        )
        self.addCleanup(menu.close)
        with patch('game_menu.subprocess.Popen', return_value=Process()) as spawn:
            menu.open()
        self.assertEqual(child.signals, [])
        self.assertTrue(menu.block.exists())
        self.assertEqual(spawn.call_args.kwargs['env']['RGDS_MENU_LIVE'], '1')
        self.assertEqual(read_fields(menu.status)['live'], '1')

    def test_fps_setting_persists_only_after_bridge_acknowledges(self):
        for core in ('drastic', 'dsperate', 'nnddss'):
            self.menu.core = core
            self.request('fps:1', token='fps-' + core)
            self.assertEqual(self.menu.pending['action'], 'fps:1')
            self.result('fps-on')
            self.assertEqual(read_fields(self.menu.status)['fps'], '1')
            self.assertEqual(read_fields(self.menu.data / 'performance.ini')['fps'], '1')
            self.assertEqual(read_fields(self.menu.status)['state'], 'paused')

    def test_fps_preference_write_failure_does_not_exit_the_game(self):
        self.request('fps:1', token='fps-unsaved')
        original = write_fields

        def fail_preference(path, fields, **kwargs):
            if Path(path).name == 'performance.ini': raise OSError('Read-only settings')
            return original(path, fields, **kwargs)

        with patch('game_menu.write_fields', side_effect=fail_preference):
            self.result('fps-on')
        self.assertIsNone(self.menu.pending)
        self.assertIsNone(self.child.poll())
        self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
        self.assertIn('Settings storage', read_fields(self.menu.status)['message'])

    def test_threading_result_preserves_fps_and_only_persists_successful_threading(self):
        self.menu.core = 'melonds'
        self.request('fps:1', token='enable-fps')
        self.result('fps-on')
        preference = self.menu.data / 'performance.ini'
        before = preference.read_bytes(), preference.stat().st_ino
        for state in ('threaded-off', 'threaded-on', 'threaded-failed'):
            previous = self.menu.threaded
            self.request('threaded:0', token=state)
            self.result(state)
            self.assertEqual((preference.read_bytes(), preference.stat().st_ino), before)
            self.assertTrue(self.menu.fps)
            self.assertEqual(self.menu.threaded, previous if state == 'threaded-failed' else state == 'threaded-on')
            self.assertTrue(self.menu.paused)
            self.assertIn('3D threading', read_fields(self.menu.status)['message'])

    def test_live_graphics_persists_only_after_success_and_keeps_process(self):
        from library import game_key
        self.menu.core = 'rgds-experimental'
        self.menu.launch_hires = True
        path = self.menu.data / 'library' / game_key(self.menu.rom) / 'game.ini'
        write_fields(path, dict(rgds_scale=1, rgds_smooth=1, frameskip=0, favourite=1))
        before = path.read_bytes()
        self.request('graphics:5:0:2', token='graphics-success')
        self.assertEqual(self.menu.pending['action'], 'graphics:5:0:2')
        self.assertEqual(path.read_bytes(), before)
        self.result('graphics-applied')
        self.assertEqual(read_fields(path)['rgds_scale'], '5')
        self.assertEqual(read_fields(path)['favourite'], '1')
        self.assertEqual(read_fields(self.menu.status)['active_scale'], '5')
        self.assertFalse(self.menu.restart)
        self.assertIsNone(self.child.poll())
        before = path.read_bytes()
        self.request('graphics:3:1:0', token='graphics-failure')
        self.result('graphics-failed')
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(read_fields(self.menu.status)['active_scale'], '5')
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')

    def test_live_2d_filter_persists_only_after_success_and_keeps_process(self):
        from library import game_key
        self.menu.core = 'rgds-experimental'
        self.menu.launch_hires = True
        path = self.menu.data / 'library' / game_key(self.menu.rom) / 'game.ini'
        write_fields(path, dict(rgds_scale=3, rgds_smooth=1, frameskip=0, favourite=1))
        before = path.read_bytes()
        self.request('filter2d:1', token='filter-success')
        self.assertEqual(self.menu.pending['action'], 'filter2d:1')
        self.assertEqual(path.read_bytes(), before)
        self.result('filter2d-applied')
        self.assertEqual(read_fields(path)['rgds_2d'], '1')
        self.assertEqual(read_fields(path)['rgds_scale'], '3')
        self.assertEqual(read_fields(path)['favourite'], '1')
        self.assertEqual(read_fields(self.menu.status)['active_filter2d'], '1')
        self.assertFalse(self.menu.restart)
        self.assertIsNone(self.child.poll())
        before = path.read_bytes()
        self.request('filter2d:2', token='filter-failure')
        self.result('filter2d-failed')
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(read_fields(self.menu.status)['active_filter2d'], '1')
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')

    def test_cheat_toggles_pass_through_to_dscore_and_report_outcomes(self):
        stale = Path(str(self.menu.control) + '.cheats')
        self.assertFalse(stale.exists())  # a previous game's list is cleared at start
        self.menu.core = 'rgds-experimental'
        self.request('cheat:2:1', token='cheat-on')
        self.assertEqual(self.menu.pending['action'], 'cheat:2:1')
        self.assertEqual(read_fields(self.menu.status)['message'], 'Applying cheat…')
        self.result('cheat-on')
        self.assertIsNone(self.menu.pending)
        self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
        self.assertEqual(read_fields(self.menu.status)['message'], 'Cheat enabled.')
        self.assertIsNone(self.child.poll())
        self.request('cheat:2:0', token='cheat-off')
        self.result('cheat-off')
        self.assertEqual(read_fields(self.menu.status)['message'], 'Cheat disabled.')
        self.request('cheat:5:1', token='cheat-failed')
        self.result('cheat-failed')
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')
        self.assertIsNone(self.menu.pending)

    def test_cheat_requests_need_dscore_and_a_valid_shape(self):
        self.menu.send('cheat:0:1')
        self.assertIsNone(self.menu.pending)
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')
        self.menu.core = 'rgds-experimental'
        for command in ('cheat:', 'cheat:0', 'cheat:0:2', 'cheat:x:1', 'cheat:0:1:0', 'cheat:-1:1'):
            self.menu.send(command)
            self.assertIsNone(self.menu.pending, command)
        self.menu.send('cheat:12:0')
        self.assertEqual(self.menu.pending['action'], 'cheat:12:0')

    def test_live_2d_filter_rejects_malformed_and_other_engine_requests(self):
        for command in ('filter2d:1', 'filter2d:3', 'filter2d:-1', 'filter2d:', 'filter2d:1:0'):
            self.menu.send(command)
            self.assertIsNone(self.menu.pending)
        self.menu.core = 'rgds-experimental'
        for command in ('filter2d:4', 'filter2d:x', 'filter2d:1:0'):
            self.menu.send(command)
            self.assertIsNone(self.menu.pending)
        self.menu.send('filter2d:2')
        self.assertEqual(self.menu.pending['action'], 'filter2d:2')

    def test_crossing_native_and_high_resolution_saves_choice_and_restarts(self):
        from library import game_key
        self.menu.core = 'rgds-experimental'
        self.menu.launch_hires = False
        path = self.menu.data / 'library' / game_key(self.menu.rom) / 'game.ini'
        write_fields(path, dict(rgds_scale=1, rgds_smooth=1, frameskip=0, favourite=1))
        self.request('graphics:3:0:1', token='to-hires')
        self.assertEqual(read_fields(path)['rgds_scale'], '3')
        self.assertEqual(read_fields(path)['frameskip'], '1')
        self.assertEqual(read_fields(path)['favourite'], '1')
        self.assertTrue(self.menu.restart)
        self.assertEqual(self.menu.pending['action'], 'save-quit')
        # Within the same renderer the change still applies live.
        self.menu.pending = None
        self.menu.restart = False
        self.menu.launch_hires = True
        self.request('graphics:5:1:0', token='still-hires')
        self.assertEqual(self.menu.pending['action'], 'graphics:5:1:0')
        self.assertFalse(self.menu.restart)

    def test_live_graphics_rejects_malformed_and_other_engine_requests(self):
        for command in ('graphics:5:1:0', 'graphics:9:1:0', 'graphics:3:1', 'graphics:3:1:0:2'):
            self.menu.send(command)
            self.assertIsNone(self.menu.pending)
        self.menu.core = 'rgds-experimental'
        self.menu.send('graphics:3:2:0')
        self.assertIsNone(self.menu.pending)

    def test_speed_acknowledgement_and_restart_intent_use_the_same_owned_protocol(self):
        self.menu.core = 'dsperate'
        self.request('fast-forward')
        self.result('fast-forward-on')
        self.assertEqual(read_fields(self.menu.status)['fast_forward'], '1')
        self.request('restart', token='restart')
        self.assertTrue(self.menu.restart)
        self.result('saved')
        self.assertEqual(read_fields(self.menu.status)['state'], 'busy')
        self.child.result = 0
        self.menu.poll()
        self.assertEqual(read_fields(self.menu.status)['state'], 'home')

    def test_native_speed_commands_wait_for_bridge_result_for_each_engine(self):
        for core in ('drastic', 'nnddss'):
            with self.subTest(core=core):
                self.menu.core = core
                self.menu.send('fast-forward')
                self.assertIsNotNone(self.menu.pending)
                self.assertFalse(self.menu.fast_forward)
                self.result('fast-forward-on')
                self.assertTrue(self.menu.fast_forward)
                self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
                self.menu.fast_forward = False

    def test_unavailable_bridge_speed_result_finishes_without_a_timeout_or_save_error(self):
        self.menu.core = 'dsperate'
        self.request('fast-forward')
        self.result('fast-forward-unavailable')
        self.assertIsNone(self.menu.pending)
        self.assertFalse(self.menu.fast_forward)
        self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
        self.assertIn('Fast-forward is unavailable', read_fields(self.menu.status)['message'])

    def test_shader_without_live_scale_api_returns_an_honest_restart_notice(self):
        self.menu.core = 'dsperate'
        self.request('shader:1')
        self.result('shader-restart-required')
        self.assertIsNone(self.menu.pending)
        self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
        self.assertIn('Restart this game', read_fields(self.menu.status)['message'])

    def test_live_shader_reply_keeps_engine_and_menu_running_without_restart(self):
        self.menu.live = True
        self.menu.core = 'dsperate'
        self.request('shader:1', token='shader-1')
        self.assertEqual(read_fields(self.menu.status)['request_token'], 'shader-1')
        before = list(self.child.signals)
        self.result('shader-applied')
        self.assertIsNone(self.menu.pending)
        self.assertEqual(read_fields(self.menu.status)['state'], 'paused')
        self.assertIn('Shader applied', read_fields(self.menu.status)['message'])
        self.assertIs(self.menu.menu, self.ui)
        self.assertEqual(self.child.signals, before)
        self.assertFalse(self.menu.restart)

    def test_save_success_requires_a_nonempty_checkpoint_on_disk(self):
        self.request('save')
        self.result('saved', checkpoint=False)
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')
        self.assertEqual(read_fields(self.menu.status)['checkpoint'], '0')
        self.assertIn('Could not save', read_fields(self.menu.status)['message'])
        self.menu.checkpoint().parent.mkdir(parents=True, exist_ok=True)
        self.menu.checkpoint().write_bytes(b'')
        self.request('load', token='empty-load')
        self.assertIsNone(self.menu.pending)

    def test_pending_save_cannot_be_replaced_by_a_second_action(self):
        self.request('save', token='save-1')
        control = self.menu.control.read_text()
        self.request('quit', token='extra-tap')
        self.assertEqual(self.menu.control.read_text(), control)
        self.assertEqual(self.menu.pending['action'], 'save')
        self.result('saved')
        self.assertIsNone(self.child.result)

    def test_restart_requires_a_checkpoint_even_when_automatic_saving_is_off(self):
        write_fields(self.menu.data / 'preferences.ini', dict(auto_save=0))
        self.request('restart')
        self.assertEqual(self.menu.pending['action'], 'save-quit')
        self.assertTrue(self.menu.control.read_text().endswith(' save-quit\n'))
        self.assertIn('before restarting', read_fields(self.menu.status)['message'])
        self.result('save-timeout')
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')
        self.assertIsNone(self.child.result)
        self.request('quit', token='retry')
        self.assertEqual(self.menu.pending['action'], 'save-quit')
        self.result('saved')
        self.assertIsNotNone(self.menu.pending)
        self.child.result = 0
        self.menu.poll()
        self.assertTrue(self.menu.restart)

    def test_resuming_after_a_failed_restart_cancels_restart_intent(self):
        self.request('restart')
        self.result('save-failed')
        self.request('resume', token='resume')
        self.assertFalse(self.menu.restart)
        self.assertIsNone(self.menu.menu)

    def test_missing_render_ack_restores_controls_instead_of_trapping_the_game(self):
        child = Process()
        menu = GameMenu(
            child, self.root / 'app', self.root / 'failed-overlay', 'drastic', self.root / 'game.nds', {
                'RGDS_MENU_LIVE': '1',
                'RGDS_OVERLAY_FILE': str(self.root / 'missing.frame')
            }, Inputs()
        )
        self.addCleanup(menu.close)
        with patch('game_menu.subprocess.Popen', return_value=Process()):
            menu.open()
        self.clock += 4
        menu.poll()
        self.assertIsNone(menu.menu)
        self.assertFalse(menu.block.exists())
        self.assertEqual(child.signals, [])
        self.assertIn('controls restored', read_fields(menu.status)['message'])

    def test_duplicate_input_paths_do_not_immediately_close_a_new_menu(self):
        self.menu.trigger.touch()
        self.clock += .02
        self.menu.poll()
        self.assertIs(self.menu.menu, self.ui)
        self.assertFalse(self.menu.trigger.exists())
        self.clock += .3
        self.menu.trigger.touch()
        self.menu.poll()
        self.assertIs(self.menu.menu, self.ui)
        self.assertTrue(self.menu.block.exists())
        self.assertEqual(read_fields(self.menu.status)['view'], 'switcher')

    def test_reopened_overlay_cannot_show_an_old_frame_and_close_hides_after_termination(self):
        frame = self.root / 'persistent.frame'
        frame.write_bytes(struct.pack('16I', 0x52474453, 640, 480, 2, 1, 0, 2, *([0] * 9)))
        self.menu.close_menu()
        self.menu.overlay_path = frame
        with patch('game_menu.subprocess.Popen', return_value=Process()):
            self.menu.open()
        header = struct.unpack('16I', frame.read_bytes())
        self.assertEqual(header[4], 0)
        self.assertEqual(header[6], 0)
        with frame.open('r+b') as stream:
            stream.seek(16)
            stream.write(struct.pack('I', 1))
        self.menu.close_menu()
        self.assertEqual(struct.unpack('16I', frame.read_bytes())[4], 0)


if __name__ == "__main__": unittest.main()
