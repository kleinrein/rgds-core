"""Direct display: DSCore on KMS with Weston stopped, and Weston back on every exit."""
import configparser
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from test_runtime import Fixture
import direct_display
import emulators
from emulator_profiles import game_options
from library import game_key, write_fields

ENGINE = 'rgds-experimental'


class DirectDisplayPlanTests(Fixture):
    def prepare(self):
        self.binary(ENGINE)
        bridge = self.app/'emulators/hooks/librgdsbridge.so'
        bridge.parent.mkdir(parents=True); bridge.write_bytes(b'bridge')
        rom = self.root/'Kart.nds'; rom.write_bytes(b'0123456789abAMCE' + bytes(512))
        return rom

    def plan(self, rom):
        with patch.dict(os.environ, WAYLAND_DISPLAY='wayland-0', SDL_VIDEODRIVER='wayland', SDL_RENDER_VSYNC='1'), \
                patch('direct_display.available', return_value=True):
            return emulators.launch_plan(ENGINE, self.app, self.data, rom)

    def test_game_choice_wins_over_the_global_preference_and_default_is_off(self):
        rom = self.prepare()
        game = self.data/'library'/game_key(rom)/'game.ini'
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 0)
        write_fields(self.data/'preferences.ini', dict(schema=1, rgds_direct=1))
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 1)
        write_fields(game, dict(rgds_direct=0))
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 0)
        write_fields(game, dict(rgds_direct=-1))
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 1)

    def test_display_files_survive_the_launcher_rewriting_game_ini(self):
        rom = self.prepare()
        folder = self.data/'library'/game_key(rom)
        write_fields(folder/'display.ini', dict(direct=1))
        # The launcher's GameOptions writer keeps only the keys it knows.
        write_fields(folder/'game.ini', dict(schema=1, rgds_scale=2))
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 1)
        write_fields(folder/'display.ini', dict(direct=0))
        write_fields(self.data/'display.ini', dict(direct=1))
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 0)
        (folder/'display.ini').unlink()
        self.assertEqual(game_options(self.data, rom)['rgds_direct'], 1)

    def test_opted_in_engine_presents_on_kms_with_a_live_menu(self):
        rom = self.prepare()
        plain = self.plan(rom)
        self.assertNotIn('display', plain)
        self.assertEqual(plain['env']['SDL_VIDEODRIVER'], 'wayland')
        self.assertEqual(plain['env']['RGDS_MENU_LIVE'], '0')
        self.assertEqual(plain['env']['SDL_RENDER_VSYNC'], '0')
        config = configparser.ConfigParser(); config.read(self.data/'emulators'/ENGINE/'config.ini')
        self.assertEqual(config['video']['sink'], 'renderer')
        write_fields(self.data/'library'/game_key(rom)/'game.ini', dict(rgds_direct=1))
        plan = self.plan(rom)
        self.assertEqual(plan['display'], 'kms')
        # Direct scanout for the engine's frames: dma-buf buffers page-flipped
        # with no GL job in the frame's path (the bridge's present hook
        # blends the menu). DS_RGDS_SINK keeps the renderer for A/B runs.
        config = configparser.ConfigParser(); config.read(self.data/'emulators'/ENGINE/'config.ini')
        self.assertEqual(config['video']['sink'], 'auto')
        self.assertEqual(config['video']['gpu_present'], 'false')
        with patch.dict(os.environ, DS_RGDS_SINK='renderer'):
            self.plan(rom)
        config = configparser.ConfigParser(); config.read(self.data/'emulators'/ENGINE/'config.ini')
        self.assertEqual(config['video']['sink'], 'renderer')
        self.plan(rom)
        self.assertEqual(plan['env']['SDL_VIDEODRIVER'], 'KMSDRM')
        self.assertNotIn('WAYLAND_DISPLAY', plan['env'])
        self.assertEqual(plan['env']['RGDS_MENU_LIVE'], '1')
        # DSCore's own KMSDRM window order, with the windows' contents swapped
        # so the DS top screen lands on DSI-1, the top panel.
        self.assertNotIn('DS_DUAL_SCREENS', plan['env'])
        self.assertEqual(plan['command'][-2:], ['--screen', 'bottom'])
        self.assertNotIn('--screen', plain['command'])
        self.assertEqual(plan['env']['SDL_RENDER_VSYNC'], '0')
        # A failed stop puts the engine back on Weston with the paused menu.
        direct_display.use_wayland(plan['env'], plan['wayland'])
        self.assertEqual({key: plan['env'][key] for key in ('WAYLAND_DISPLAY', 'SDL_VIDEODRIVER', 'RGDS_MENU_LIVE')},
                         dict(WAYLAND_DISPLAY='wayland-0', SDL_VIDEODRIVER='wayland', RGDS_MENU_LIVE='0'))
        self.assertEqual(plan['env']['SDL_RENDER_VSYNC'], '0')   # DSCore paces itself on Weston too
        self.assertEqual(plan['wayland_command'], plain['command'])

    def test_other_engines_and_independent_linux_keep_weston(self):
        rom = self.prepare()
        write_fields(self.data/'library'/game_key(rom)/'game.ini', dict(rgds_direct=1))
        with patch.dict(os.environ, WAYLAND_DISPLAY='wayland-0'), patch('direct_display.available', return_value=False):
            self.assertNotIn('display', emulators.launch_plan(ENGINE, self.app, self.data, rom))
        self.binary('melonds')
        with patch.dict(os.environ, WAYLAND_DISPLAY='wayland-0'), patch('direct_display.available', return_value=True):
            self.assertNotIn('display', emulators.launch_plan('melonds', self.app, self.data, rom))


class GpuFaultTests(Fixture):
    def test_engine_log_counts_terminated_gpu_jobs_across_chunks(self):
        from engine_log import EngineLog
        log = EngineLog(self.data, ENGINE, [], self.root)
        log.append(b'video: ok\n[KFAULT] atom=26 code=0x4 core_req=0x16\n')
        log.append(b'[KWAITFAIL] atom=27 errored\n[KFAULT] atom=27 code=0x4 core_req=0x1\n')
        self.assertEqual(log.gpu_faults, 2)


class DirectDisplayLifecycleTests(Fixture):
    def setUp(self):
        super().setUp()
        self.marker = self.root/'weston-stopped.json'
        self.events = []
        self.running = True
        patches = [patch.object(direct_display, 'MARKER', self.marker),
                   patch.object(direct_display, 'LOCK', self.root/'weston.lock'),
                   patch.object(direct_display, 'weston_running', side_effect=lambda: self.running),
                   patch.object(direct_display, 'ready', side_effect=lambda: self.running),
                   patch.object(direct_display.subprocess, 'run', side_effect=self.init_run),
                   patch.object(direct_display.subprocess, 'Popen', side_effect=self.init_popen)]
        for item in patches:
            item.start(); self.addCleanup(item.stop)

    def init_run(self, command, **options):
        self.events.append(command[-1])
        self.assertTrue(self.marker.exists(), 'the marker must exist before Weston stops')
        self.running = False

    def init_popen(self, command, **options):
        self.events.append(command[-1])
        self.assertTrue(options.get('start_new_session'), 'Weston must not join the launcher process group')
        self.running = True
        class Script:
            def wait(self, timeout=None): return 0
        return Script()

    def test_stop_once_across_engine_restarts_then_restore_once(self):
        display = direct_display.DirectDisplay(self.data)
        self.assertEqual(display.prepare('kms'), 'kms')
        self.assertEqual(display.prepare('kms'), 'kms')
        self.assertEqual(self.events, ['stop'])
        display.restore()
        self.assertEqual(self.events, ['stop', 'start'])
        self.assertFalse(self.marker.exists())
        timings = json.loads((self.data/'session/direct-display.json').read_text())
        self.assertIn('weston_stop_seconds', timings); self.assertIn('weston_start_seconds', timings)
        display.restore()
        self.assertEqual(self.events, ['stop', 'start'])

    def test_switch_to_a_wayland_engine_restores_weston_first(self):
        display = direct_display.DirectDisplay(self.data)
        display.prepare('kms')
        self.assertEqual(display.prepare('wayland'), 'wayland')
        self.assertTrue(self.running)
        self.assertEqual(self.events, ['stop', 'start'])

    def test_weston_that_will_not_stop_keeps_the_game_on_wayland(self):
        display = direct_display.DirectDisplay(self.data)
        def stuck(command, **options):
            self.events.append(command[-1])
        with patch.object(direct_display.subprocess, 'run', side_effect=stuck), \
                patch.object(direct_display, '_processes', return_value=[]), \
                patch.object(direct_display.time, 'monotonic', side_effect=[0, 0, 5, 5, 7] + [7] * 50):
            self.assertEqual(display.prepare('kms'), 'wayland')
        self.assertFalse(display.stopped)
        # Weston never stopped; the marker is cleared by the restore.
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.events, ['stop'])

    def test_recovery_waits_for_a_live_owner_and_restarts_after_a_crash(self):
        self.running = False
        self.marker.write_text(json.dumps(dict(pid=os.getpid(), start='42')) + '\n')
        with patch.object(direct_display, 'process_identity', return_value='42'):
            self.assertIsNone(direct_display.recover())
        self.assertEqual(self.events, [])
        with patch.object(direct_display, 'process_identity', return_value=None):
            self.assertIsNotNone(direct_display.recover())
        self.assertEqual(self.events, ['start'])
        self.assertFalse(self.marker.exists())
        self.assertIsNone(direct_display.recover())

    def test_runner_restores_weston_when_the_game_fails(self):
        restored = []
        class Display:
            def __init__(self, data): pass
            def restore(self): restored.append(True)
        with patch('direct_display.DirectDisplay', Display), \
                patch.object(emulators, '_run_once', side_effect=OSError('engine failed')):
            with self.assertRaises(OSError):
                emulators.run(ENGINE, self.app, self.data, self.root/'game.nds')
        self.assertEqual(restored, [True])
        with patch('direct_display.DirectDisplay', Display), patch.object(emulators, '_run_once', return_value=0):
            self.assertEqual(emulators.run(ENGINE, self.app, self.data, self.root/'game.nds'), 0)
        self.assertEqual(restored, [True, True])


if __name__ == '__main__':
    unittest.main()
