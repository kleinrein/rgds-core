"""Optional backend boundaries: settings, input protocol, saves and display return."""
import ctypes
import errno
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import emulators
import superdrastic as sd
from fields import read_fields, write_fields
from library import game_key


class SuperDrasticTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve(); self.app = self.root / 'app'; self.data = self.root / 'data'
        self.rom = self.root / 'test.nds'; self.rom.write_bytes(b'game')
        self.binary = self.app / 'emulators/drastic/drastic'; self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b'fixture binary'); self.binary.chmod(0o755)
        lib = self.app / 'emulators/superdrastic/libsuperdrastic.so'; lib.parent.mkdir(parents=True); lib.write_bytes(b'fixture')
        self.sdl = lib.parent / 'sdl/libSDL2-2.0.so.0'; self.sdl.parent.mkdir(); self.sdl.write_bytes(b'fixture SDL')
        self.auto = self.data / 'library' / game_key(self.rom) / 'auto-drastic.dss'
        self.game = self.auto.parent / 'game.ini'

    def plan(self):
        with patch.object(sd, 'DRASTIC_SHA256', hashlib.sha256(self.binary.read_bytes()).hexdigest()), patch('linux_platform.independent', return_value=False), patch.object(sd, 'check_sdl'):
            return sd.launch_plan(self.app, self.data, self.binary, self.rom, emulators.base_environment())

    def supervisor(self):
        plan = self.plan(); child = Mock(); child.poll.return_value = None
        hardware = Mock(); hardware.status.return_value = dict(volume=30, brightness=60, battery=79, charging=1)
        hardware.lid_closed.return_value = False
        inputs = Mock(); inputs.poll.return_value = False
        supervisor = sd.NativeSupervisor(child, self.app, self.data, 'drastic', self.rom, plan['env'], inputs, hardware)
        return supervisor, child, hardware, inputs

    def shared_supervisor(self):
        plan = self.plan(); child = Mock(); child.poll.return_value = None
        inputs = Mock(spec=['poll','close']); inputs.poll.return_value = False
        hardware = Mock(); hardware.lid_closed.return_value = False
        supervisor = sd.Supervisor(child, self.app, self.data, 'drastic', self.rom, plan['env'], inputs, hardware)
        supervisor.lid = Mock(); supervisor.lid.poll.return_value = False
        Path(plan['env']['DSFLIP_STATE']).write_text('ready')
        return supervisor, child

    def test_shared_menu_is_default_and_retains_rgds_helper_environment(self):
        supervisor, child = self.shared_supervisor()
        self.assertEqual(supervisor.env['DSFLIP_MENU'], '0')
        self.assertTrue(supervisor.live)
        supervisor.open()
        self.assertTrue(supervisor.block.exists())
        self.assertIsNone(supervisor.menu)
        (supervisor.folder/'menu-background.bmp').write_bytes(b'frame')
        helper = Mock(); helper.poll.return_value = None
        with patch('game_menu.subprocess.Popen', return_value=helper) as launch:
            supervisor.poll_snapshot()
        self.assertIn('--game-menu', launch.call_args.args[0])
        self.assertEqual(launch.call_args.kwargs['env']['RGDS_SD_SHARED_MENU'], '1')
        self.assertNotIn('LD_PRELOAD', launch.call_args.kwargs['env'])
        child.send_signal.assert_not_called()  # The DRM presenter must keep running.

    def test_menu_open_stops_speed_enabled_outside_physical_input(self):
        supervisor, child = self.shared_supervisor()
        write_fields(supervisor.speed_state, dict(active=1))
        supervisor.open()
        request = read_fields(Path(str(supervisor.control)+'.speed-request'))
        self.assertEqual(request['action'], 'fast-forward-off')
        self.assertTrue(supervisor.block.exists())
        child.send_signal.assert_not_called()

    def test_shared_save_is_verified_before_gallery_consumes_it(self):
        supervisor, _ = self.shared_supervisor()
        supervisor.pending = dict(token='test-1', action='save', began=time.monotonic())
        result = Path(str(supervisor.control)+'.result')
        write_fields(result, dict(token='test-1', state='native-saved'))
        supervisor.path('RESUME').write_bytes(b'complete new checkpoint')
        with patch('game_menu.GameMenu.poll') as parent:
            supervisor.poll()
        self.assertEqual(read_fields(result)['state'], 'saved')
        self.assertEqual(supervisor.checkpoint().read_bytes(), b'complete new checkpoint')
        parent.assert_called_once()
        supervisor.path('RESUME').unlink()
        write_fields(result, dict(token='test-1', state='native-saved'))
        with patch('game_menu.GameMenu.poll'): supervisor.poll()
        self.assertEqual(read_fields(result)['state'], 'save-failed')
        self.assertEqual(supervisor.checkpoint().read_bytes(), b'complete new checkpoint')

    def test_inheritance_invalid_values_and_independent_core_default(self):
        self.assertEqual(sd.backend_choice(self.data, self.rom), 'standard')
        write_fields(self.data / 'ui.ini', dict(drastic_backend=1))
        self.assertEqual(sd.backend_choice(self.data, self.rom), 'superdrastic')
        write_fields(self.game, dict(core=0, drastic_backend=0))
        self.assertEqual(sd.backend_choice(self.data, self.rom), 'standard')
        write_fields(self.game, dict(core=0, drastic_backend=-1))
        self.assertEqual(sd.backend_choice(self.data, self.rom), 'superdrastic')
        write_fields(self.data / 'ui.ini', dict(drastic_backend=900))
        self.assertEqual(sd.backend_choice(self.data, self.rom), 'standard')
        self.assertEqual(emulators.game_options(self.data, self.rom)['core'], 0)
        self.assertFalse((self.data / 'preferences.ini').exists())

    def test_standard_screen_choice_does_not_leak_into_superdrastic_native_override(self):
        write_fields(self.game,dict(drastic_screen_swap=1))
        native=self.data/'emulators/drastic/config/test.cfg'
        write_fields(native,dict(screen_swap=1,custom_option='keep'))
        self.plan()
        fields={k.strip():v.strip() for k,v in read_fields(native).items()}
        self.assertEqual(fields['screen_swap'],'0')
        self.assertEqual(fields['controls_a[CONTROL_INDEX_MENU]'],'65535')
        self.assertEqual(fields['custom_option'],'keep')
        self.assertEqual(read_fields(self.game)['drastic_screen_swap'],'1')
        self.assertIn('screen_swap = 0\n',native.read_text())
        with patch.dict(os.environ,{},clear=True):
            emulators.launch_plan('drastic',self.app,self.data,self.rom)
        self.assertEqual(read_fields(native)['screen_swap'],'1')

    def test_no_inherited_hook_settings_leak_to_another_engine(self):
        with patch.dict(os.environ, {'LD_PRELOAD':'unsafe.so', 'DSFLIP_RA_PASSWORD':'private', 'DSFLIP_STOP_CMD':'bad', 'DSHOOK_SHADER':'bad', 'RAST':'ours', 'RGDS_SD_EXIT':'old'}):
            env = emulators.base_environment()
        for key in ('LD_PRELOAD', 'DSFLIP_RA_PASSWORD', 'DSFLIP_STOP_CMD', 'DSHOOK_SHADER', 'RAST', 'RGDS_SD_EXIT'):
            self.assertNotIn(key, env)

    def test_wrong_drastic_binary_fails_before_profile_or_save_changes(self):
        with self.assertRaisesRegex(ValueError, 'verified RG DS'):
            sd.launch_plan(self.app, self.data, self.binary, self.rom, {})
        self.assertFalse(self.data.exists())

    def test_native_config_uses_physical_controls_spaced_keys_and_single_hook(self):
        write_fields(self.data / 'controls.ini', dict(preset='left-handed'))
        plan = self.plan(); env = plan['env']
        config = (Path(plan['cwd']) / 'config/drastic.cfg').read_text()
        self.assertIn('controls_a[CONTROL_INDEX_A] = 1090\n', config)
        self.assertIn('controls_b[CONTROL_INDEX_UP] = 1027\n', config)
        self.assertIn('controls_a[CONTROL_INDEX_MENU] = 65535\n', config)
        self.assertIn('controls_a[CONTROL_INDEX_FAST_FORWARD] = 1043\n', config)
        self.assertNotIn(':', env['LD_PRELOAD'])
        self.assertNotIn('librgdsbridge', env['LD_PRELOAD'])
        self.assertEqual(env['DSFLIP_CPUGOV'], '0')
        self.assertEqual(env['DSFLIP_RAST'], '0')
        self.assertEqual(env['SDL_VIDEODRIVER'], 'dummy')
        self.assertEqual(env['RGDS_SDL_LIBRARY'], str(self.sdl))
        self.assertEqual(env['LD_LIBRARY_PATH'].split(':')[0], str(self.sdl.parent))

    def test_missing_sdl_fails_before_profile_or_save_changes(self):
        self.sdl.unlink()
        with self.assertRaisesRegex(ValueError, 'private SDL runtime is missing'):
            self.plan()
        self.assertFalse(self.data.exists())

    def test_gengis_is_opt_in_and_requires_hires_for_each_game(self):
        for scale in (2, 3):
            write_fields(self.game, dict(hires_3d=1, gengis_scale=scale))
            plan = self.plan()
            self.assertEqual(plan['env']['DSFLIP_RAST'], '1')
            self.assertEqual(plan['env']['DSFLIP_RAST_SCALE'], str(scale))
            self.assertEqual(plan['env']['DSFLIP_RAST_TEXFILTER'], '0')
            self.assertIn('hires_3d = 1\n', (Path(plan['cwd'])/'config/drastic.cfg').read_text())
            # The same game keeps standard DraStic's 2x option when switching back.
            with patch.dict(os.environ, {}, clear=True):
                standard = emulators.launch_plan('drastic', self.app, self.data, self.rom)
            self.assertNotIn('DSFLIP_RAST', standard['env'])
        for hires, scale in ((0, 3), (1, 0), (1, 1), (1, 4), (1, 'bad')):
            write_fields(self.game, dict(hires_3d=hires, gengis_scale=scale))
            self.assertEqual(self.plan()['env']['DSFLIP_RAST'], '0')

    def test_sdl_probe_rejects_missing_dummy_without_initializing_video(self):
        library = Mock()
        library.SDL_GetNumVideoDrivers.return_value = 2
        library.SDL_GetVideoDriver.side_effect = [b'wayland', b'KMSDRM']
        with patch.object(sd.ctypes, 'CDLL', return_value=library), self.assertRaisesRegex(ValueError, 'dummy driver'):
            sd.check_sdl(self.sdl)
        library.SDL_Init.assert_not_called()

    def test_sdl_probe_accepts_private_drivers_without_initializing_video(self):
        library = Mock()
        library.SDL_GetNumVideoDrivers.return_value = 1
        library.SDL_GetVideoDriver.return_value = b'dummy'
        library.SDL_GetNumAudioDrivers.return_value = 1
        library.SDL_GetAudioDriver.return_value = b'alsa'
        with patch.object(sd.ctypes, 'CDLL', return_value=library):sd.check_sdl(self.sdl)
        library.SDL_Init.assert_not_called()

    def test_autoload_is_a_copy_and_autosave_off_remains_independent(self):
        self.auto.parent.mkdir(parents=True); self.auto.write_bytes(b'old checkpoint')
        write_fields(self.data / 'preferences.ini', dict(auto_load=1, auto_save=0))
        plan = self.plan(); env = plan['env']
        self.assertEqual(env['DSFLIP_RESUME_LOAD'], '1')
        self.assertEqual(env['RGDS_SD_SAVE_ON_EXIT'], '0')
        self.assertEqual(Path(env['DSFLIP_RESUME_FILE']).read_bytes(), self.auto.read_bytes())
        Path(env['DSFLIP_RESUME_FILE']).unlink()
        self.assertEqual(self.auto.read_bytes(), b'old checkpoint')

    def test_newer_in_game_save_prevents_old_checkpoint_autoload(self):
        self.auto.parent.mkdir(parents=True); self.auto.write_bytes(b'old'); os.utime(self.auto, (1,1))
        backup = self.data / 'emulators/drastic/backup/test.dsv'; backup.parent.mkdir(parents=True); backup.write_bytes(b'new game save')
        self.assertEqual(self.plan()['env']['DSFLIP_RESUME_LOAD'], '0')
        self.assertEqual(self.auto.read_bytes(), b'old')

    def test_native_receipt_is_required_and_previous_checkpoint_is_backed_up(self):
        supervisor, child, _, _ = self.supervisor()
        self.auto.parent.mkdir(parents=True, exist_ok=True); self.auto.write_bytes(b'old')
        supervisor.path('RESUME').write_bytes(b'new')
        self.assertEqual(supervisor.finish(-9), -9)
        self.assertEqual(self.auto.read_bytes(), b'old')
        write_fields(supervisor.path('EXIT'), dict(state='saved'))
        self.assertEqual(supervisor.finish(-9), 0)
        self.assertEqual(self.auto.read_bytes(), b'new')
        self.assertEqual(next((self.auto.parent/'checkpoint-backups').iterdir()).read_bytes(), b'old')

    def test_save_failure_preserves_both_previous_and_partial_evidence(self):
        supervisor, _, _, _ = self.supervisor()
        self.auto.parent.mkdir(parents=True, exist_ok=True); self.auto.write_bytes(b'old')
        supervisor.path('RESUME').write_bytes(b'partial')
        write_fields(supervisor.path('EXIT'), dict(state='save-failed'))
        with self.assertRaisesRegex(ValueError, 'previous checkpoint'): supervisor.finish(-9)
        self.assertEqual(self.auto.read_bytes(), b'old')
        self.assertEqual(supervisor.path('RESUME').read_bytes(), b'partial')

    def test_failed_checkpoint_publish_keeps_original(self):
        source = self.root/'source'; target = self.root/'target'
        source.write_bytes(b'new'); target.write_bytes(b'old')
        with patch.object(sd.os, 'replace', side_effect=OSError('read only')):
            with self.assertRaises(OSError): sd.copy_state(source, target)
        self.assertEqual(target.read_bytes(), b'old')
        self.assertEqual(list(self.root.glob('.checkpoint-*')), [])

    def test_checkpoint_flushes_file_and_directory_but_allows_unsupported_metadata_flush(self):
        source=self.root/'source';target=self.root/'target';source.write_bytes(b'new')
        native_fsync=os.fsync
        with patch.object(sd.os,'fsync',wraps=native_fsync) as flushed:
            sd.copy_state(source,target)
        self.assertEqual(flushed.call_count,2)
        with patch.object(sd.os,'fsync',side_effect=[None,OSError(errno.EINVAL,'directory fsync unsupported')]) as flushed:
            sd.copy_state(source,target)
        self.assertEqual(flushed.call_count,2)
        self.assertEqual(target.read_bytes(),b'new')

    def test_requested_autosave_cannot_be_silently_lost_on_zero_exit(self):
        supervisor,child,_,_=self.supervisor();supervisor.ready=True
        self.auto.parent.mkdir(parents=True,exist_ok=True);self.auto.write_bytes(b'old')
        self.assertEqual(supervisor.finish(0),0)  # Unrequested ordinary clean exit.
        supervisor.request('home');child.send_signal.assert_called_once_with(signal.SIGUSR1)
        with self.assertRaisesRegex(ValueError,'without confirming'):
            supervisor.finish(0)
        self.assertEqual(self.auto.read_bytes(),b'old')
        supervisor.env['RGDS_SD_SAVE_ON_EXIT']='0'
        self.assertEqual(supervisor.finish(0),0)

    def test_readiness_is_required_before_home_signal(self):
        supervisor, child, _, inputs = self.supervisor(); inputs.poll.return_value = 'home'
        supervisor.poll(); child.send_signal.assert_not_called()
        Path(supervisor.env['DSFLIP_STATE']).write_text('ready\n')
        supervisor.poll(); supervisor.poll()
        child.send_signal.assert_called_once_with(signal.SIGUSR1)

    def test_passthrough_and_readiness_timeout_are_errors(self):
        supervisor, _, _, _ = self.supervisor()
        Path(supervisor.env['DSFLIP_STATE']).write_text('passthrough: display busy')
        with self.assertRaisesRegex(ValueError, 'display busy'): supervisor.poll()
        Path(supervisor.env['DSFLIP_STATE']).unlink(); supervisor.began -= 13
        with self.assertRaisesRegex(ValueError, 'readiness timeout'): supervisor.poll()

    def test_menu_pause_excluded_from_playtime_and_commands_acknowledged(self):
        supervisor, _, _, inputs = self.supervisor()
        Path(supervisor.env['DSFLIP_STATE']).write_text('ready'); supervisor.poll()
        inputs.poll.return_value = 'menu'; supervisor.poll(); inputs.poll.return_value = False
        token, action = supervisor.path('COMMAND').read_text().split(); self.assertEqual(action, 'speed-off')
        write_fields(supervisor.path('RESULT'), dict(token=token, state='speed-off'))
        supervisor.poll(); self.assertEqual(supervisor.path('COMMAND').read_text().split()[1], 'menu')
        write_fields(supervisor.path('MENU_STATUS'), dict(open=1)); supervisor.poll()
        before = supervisor.played; supervisor.last_tick -= 5; supervisor.poll()
        self.assertEqual(supervisor.played, before)

    def test_native_hardware_requests_preserve_other_preferences(self):
        supervisor, _, hardware, _ = self.supervisor()
        write_fields(self.data/'system.ini', dict(schema=1, brightness=60, lid=1, power=2))
        write_fields(supervisor.path('SETTINGS'), dict(sequence=1, volume=45, brightness=-1))
        supervisor.poll_hardware(); hardware.volume.assert_called_once_with(45)
        write_fields(supervisor.path('SETTINGS'), dict(sequence=2, volume=45, brightness=80))
        supervisor.poll_hardware(); hardware.volume.assert_called_once_with(45); hardware.brightness.assert_called_once_with(80)
        self.assertEqual(read_fields(self.data/'system.ini'), dict(schema='1', brightness='80', lid='1', power='2'))
        self.assertEqual(read_fields(supervisor.path('SYSTEM'))['battery'], '79')

    def test_lid_failure_resumes_game_and_does_not_kill_it(self):
        supervisor, child, hardware, inputs = self.supervisor(); supervisor.ready = True
        supervisor.lid = Mock(); supervisor.lid.poll.return_value = True
        hardware.sleep_lid.side_effect = OSError('cannot suspend')
        supervisor.poll()
        self.assertEqual([call.args[0] for call in child.send_signal.call_args_list], [signal.SIGSTOP, signal.SIGCONT])
        inputs.discard_after_sleep.assert_called_once()
        child.kill.assert_not_called()

    def test_vt_lease_restores_original_after_mid_acquire_failure_and_is_idempotent(self):
        lease = sd.VTLease(); switches = []
        def ioctl(fd, request, value):
            if request == 0x5603: value[0] = 2
            elif request == 0x5600: value[0] = 9
            elif request == 0x4b3b: value[0] = 0
        def switch(number):
            switches.append(number)
            if number == 9: raise OSError('switch failed')
        with patch.object(sd.os, 'open', side_effect=[20,21]), patch.object(sd.os, 'close') as close, patch.object(sd.fcntl, 'ioctl', side_effect=ioctl), patch.object(lease, 'switch', side_effect=switch):
            with self.assertRaisesRegex(OSError, 'switch failed'): lease.acquire()
            lease.close()
        self.assertEqual(switches, [9,2]); self.assertEqual(close.call_count, 2)

    def test_busy_inactive_vt_cleanup_is_not_a_failed_display_return(self):
        lease = sd.VTLease(); lease.fd=20; lease.tty=21
        lease.original=1; lease.allocated=2; lease.switched=True
        def ioctl(fd, request, value):
            if request == 0x5608: raise OSError(errno.EBUSY, 'inactive VT still open')
        with patch.object(lease, 'switch') as switch, patch.object(sd.fcntl, 'ioctl', side_effect=ioctl), patch.object(sd.os, 'close') as close:
            lease.close(); lease.close()
        switch.assert_called_once_with(1)
        self.assertEqual(close.call_count, 2)
        self.assertFalse(lease.switched)

    def test_runner_reaps_failed_native_menu_before_restoring_display(self):
        launched = []; restored = []
        real_popen = subprocess.Popen
        def start(*args, **kwargs):
            child = real_popen(*args, **kwargs); launched.append(child); return child
        def restore(): restored.append(launched[0].poll())
        plan = dict(command=[sys.executable, '-c', 'import time;time.sleep(20)'], cwd=str(self.root), env=emulators.base_environment(), backend='superdrastic')
        with patch.object(emulators, 'launch_plan', return_value=plan), patch.object(emulators, 'VolumeWatch'), patch.object(emulators, 'MenuInput') as inputs, patch.object(sd, 'VTLease') as lease, patch.object(sd, 'Supervisor', side_effect=ValueError('native menu init failed')), patch.object(emulators.subprocess, 'Popen', side_effect=start), patch.object(emulators.signal, 'signal'):
            inputs.return_value.descriptors = [1]; lease.return_value.close.side_effect = restore
            with self.assertRaisesRegex(ValueError, 'native menu init failed'):
                emulators._run_once('drastic', self.app, self.data, self.rom)
            lease.return_value.acquire.assert_called_once(); inputs.return_value.close.assert_called_once()
        self.assertEqual(len(restored), 1); self.assertIsNotNone(restored[0])
        self.assertFalse((self.data/'session/core.json').exists())

    def test_runner_spawn_failure_restores_display_and_missing_controls_never_switches(self):
        for missing_inputs in (False, True):
            plan = dict(command=['/missing/engine'], cwd=str(self.root), env={}, backend='superdrastic')
            with patch.object(emulators, 'launch_plan', return_value=plan), patch.object(emulators, 'VolumeWatch'), patch.object(emulators, 'MenuInput') as inputs, patch.object(sd, 'VTLease') as lease, patch.object(emulators.subprocess, 'Popen', side_effect=OSError('spawn failed')) as spawn, patch.object(emulators.signal, 'signal'):
                inputs.return_value.descriptors = [] if missing_inputs else [1]
                with self.assertRaises((ValueError, OSError)): emulators._run_once('drastic', self.app, self.data, self.rom)
                if missing_inputs: lease.assert_not_called(); spawn.assert_not_called()
                else: lease.return_value.acquire.assert_called_once(); lease.return_value.close.assert_called_once()
                inputs.return_value.close.assert_called_once()
            self.assertFalse((self.data/'session/core.json').exists())

    def test_switching_to_standard_skips_resume_older_than_native_save(self):
        hook = self.app/'emulators/hooks/librgdsbridge.so'; hook.parent.mkdir(); hook.write_bytes(b'bridge')
        self.auto.parent.mkdir(parents=True); self.auto.write_bytes(b'old'); os.utime(self.auto, (1,1))
        native = self.data/'emulators/drastic/backup/test.dsv'; native.parent.mkdir(parents=True); native.write_bytes(b'new')
        plan = emulators.launch_plan('drastic', self.app, self.data, self.rom)
        self.assertEqual(plan['env']['RGDS_AUTOLOAD'], '0')
        self.assertFalse((self.data/'emulators/drastic/savestates/test_1.dss').exists())
        self.assertEqual(native.read_bytes(), b'new')

    def test_failed_display_return_is_not_reported_as_success(self):
        plan = dict(command=[sys.executable, '-c', 'pass'], cwd=str(self.root), env=emulators.base_environment(), backend='superdrastic')
        with patch.object(emulators, 'launch_plan', return_value=plan), patch.object(emulators, 'VolumeWatch'), patch.object(emulators, 'MenuInput') as inputs, patch.object(sd, 'VTLease') as lease, patch.object(sd, 'Supervisor') as menu, patch.object(emulators.signal, 'signal'):
            inputs.return_value.descriptors = [1]; lease.return_value.close.side_effect = OSError('VT refused return')
            menu.return_value.played_seconds.return_value = 0; menu.return_value.finish.return_value = 0; menu.return_value.restart = False
            with self.assertRaisesRegex(OSError, 'restore the original display'):
                emulators._run_once('drastic', self.app, self.data, self.rom)
        self.assertIn('VT refused return', (self.data/'logs/drastic.log').read_text())
        self.assertFalse((self.data/'session/core.json').exists())


class NativeProtocolTests(unittest.TestCase):
    def test_native_command_scheduler_retries_busy_controls_and_deduplicates(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); stub = root/'stub.c'
            stub.write_text('int rgds_shared_enabled(void){return 0;} int rgds_shared_poll(void*e){return 0;} int idle=1, presses=0, toggles=0; int rgds_sd_idle(void){return idle;} int rgds_sd_joystick(void){return 0;} void resume_press(int b){if(b==19)presses++;} void rgds_sd_menu_toggle(void){toggles++;}\n')
            lib = root/'adapter.so'
            subprocess.run(['cc', '-shared', '-fPIC', str(ROOT/'src/experimental/superdrastic/rgds.c'), str(stub), '-o', str(lib)], check=True, capture_output=True)
            native = ctypes.CDLL(str(lib)); idle=ctypes.c_int.in_dll(native,'idle'); presses=ctypes.c_int.in_dll(native,'presses'); toggles=ctypes.c_int.in_dll(native,'toggles')
            command = root/'command'; result=root/'result'
            with patch.dict(os.environ, RGDS_SD_COMMAND=str(command), RGDS_SD_RESULT=str(result)):
                command.write_text('1 speed-on\n'); idle.value=0; native.rgds_sd_poll(None)
                self.assertFalse(result.exists()); self.assertEqual(presses.value, 0)
                idle.value=1; time.sleep(.025); native.rgds_sd_poll(None)
                self.assertEqual(read_fields(result), dict(token='1',state='speed-on')); self.assertEqual(presses.value,1)
                time.sleep(.025); native.rgds_sd_poll(None); self.assertEqual(presses.value,1)
                command.write_text('2 menu\n'); idle.value=0; time.sleep(.025); native.rgds_sd_poll(None); self.assertEqual(toggles.value,0)
                idle.value=1; time.sleep(.025); native.rgds_sd_poll(None); self.assertEqual(toggles.value,1)
                native.rgds_sd_menu_state(1); command.write_text('3 speed-on\n'); time.sleep(.025); native.rgds_sd_poll(None)
                self.assertEqual(read_fields(result)['state'], 'paused')
            status = root/'system'; status.write_text('battery=78\ncharging=1\n')
            with patch.dict(os.environ, RGDS_SD_SYSTEM=str(status)):
                charging = ctypes.c_int(0)
                self.assertEqual(native.rgds_sd_battery(ctypes.byref(charging)), 78)
                self.assertEqual(charging.value, 1)
                status.write_text('charging=0\n')
                self.assertEqual(native.rgds_sd_battery(ctypes.byref(charging)), -1)


if __name__ == '__main__': unittest.main()
