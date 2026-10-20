"""The comparison driver must retain experimental settings and measurement boundaries."""
import ast
import contextlib
import hashlib
import io
import json
import os
import struct
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from tools import benchmark_emulators as benchmark


def remote_helpers():
    tree=ast.parse(benchmark.DEVICE_SCRIPT)
    functions=[node for node in tree.body if isinstance(node,ast.FunctionDef)
               and node.name in ('benchmark_environment','measurement_settings','capture_scene','menu_helpers','menu_state','open_menu','resume_menu','phase_frame_times')]
    namespace=dict(Path=Path,hashlib=hashlib,time=time,os=os,struct=struct)
    exec(compile(ast.Module(body=functions,type_ignores=[]),'<benchmark helpers>','exec'),namespace)
    return namespace


class BenchmarkEmulatorTests(unittest.TestCase):
    def test_explicit_telemetry_override_wins_without_losing_environment(self):
        inherited={'PATH':'/usr/bin','DS_FPS':'7','NNDDSS_CPU_AFFINITY':'1'}
        overrides={'DS_FPS':'0','DS_FRAME_STATS':'0','NNDDSS_CPU_AFFINITY':'0','RGDS_LAUNCH_PROFILE':'1'}
        actual=remote_helpers()['benchmark_environment'](inherited,overrides)
        self.assertEqual(actual,{**inherited,**overrides})
        self.assertEqual(inherited['DS_FPS'],'7')
        self.assertEqual(remote_helpers()['benchmark_environment']({},{}),{'DS_FPS':'1','DS_FRAME_STATS':'1'})

    def test_phase_settings_describe_seed_warmup_and_selected_phase(self):
        settings=remote_helpers()['measurement_settings']
        self.assertEqual(settings({'seconds':30},False),dict(
            mode='wall_time',requested_phase='all',volume_percent=0,phases=['normal','fast_forward','normal_restored'],
            seconds_per_phase=30,sample_interval_seconds=1,initial_warmup_seconds=2,
            phase_settle_seconds=2,menu_cycles=3,repeats=1,save_on_exit=False,clock_probe=False,capture_scene=False))
        actual=settings(dict(seconds=9,phase='fast_forward',menu_cycles=0,repeats=2,
                             checkpoints=True,probe_clock='clock source'),True)
        self.assertEqual(actual['phases'],['fast_forward'])
        self.assertEqual(actual['initial_warmup_seconds'],8)
        self.assertEqual(actual['seconds_per_phase'],9)
        self.assertEqual(actual['menu_cycles'],0)
        self.assertTrue(actual['clock_probe'])

    def test_frame_times_exclude_io_outside_phase_and_crossing_boundaries(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'frames.csv'
            path.write_text('500 600 500000000 1000000000\n'
                            '4 6 1000000000 1004000000\n'
                            '5 7 1100000000 1105000000\n'
                            '10 20 1990000000 2000000000\n'
                            'nan 8 1200000000 1208000000\n'
                            '9 2 1300000000 1309000000\n'
                            'truncated\n')
            phases=[dict(phase='normal',started_monotonic=1,ended_monotonic=2)]
            stats=remote_helpers()['phase_frame_times'](path,phases)['normal']
            self.assertEqual(stats['emu_ms']['frames'],2)
            self.assertEqual(stats['emu_ms']['median'],4.5)
            self.assertEqual(stats['work_ms']['p99'],7)
            self.assertEqual(stats['work_ms']['over_budget'],0)
            self.assertEqual(remote_helpers()['phase_frame_times'](path.with_name('absent'),phases),{})

    def run_cli(self,extra,output,reply=None):
        remote=SimpleNamespace(target=Mock(side_effect=lambda value:value),ssh=Mock())
        remote.ssh.return_value=reply or subprocess.CompletedProcess([],0,'RESULTS /isolated/results.json\n','')
        args=['benchmark_emulators','--device','example','--rom','/roms/test.nds','--output',str(output),*extra]
        with patch.dict(sys.modules,{'device':remote}),patch.object(sys,'argv',args),contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            status=benchmark.main()
        return status,remote

    def test_experimental_engine_and_audible_volume_reach_isolated_run(self):
        with tempfile.TemporaryDirectory() as name:
            status,remote=self.run_cli(['--cores','dsperate','rgds-experimental','--volume','50'],Path(name)/'result.txt')
            self.assertEqual(status,0)
            options=json.loads(shlex.split(remote.ssh.call_args.args[1])[-1])
            self.assertEqual(options['cores'],['dsperate','rgds-experimental'])
            self.assertEqual(options['volume'],50)
            self.assertEqual(remote_helpers()['measurement_settings'](options,False)['volume_percent'],50)

    def test_failed_emulator_launch_fails_cli_even_when_ssh_succeeds(self):
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'result.txt'
            failure=json.dumps(dict(backend='rgds-experimental',exit_code=1,
                                    error='Release validation failed'))+'\nRESULTS /isolated/results.json\n'
            status,_=self.run_cli(['--cores','rgds-experimental'],output,
                                 subprocess.CompletedProcess([],0,failure,''))
            self.assertEqual(status,1)
            self.assertEqual(output.read_text(),failure)

    def test_max_speed_settings_are_explicit_and_reject_unsupported_engines(self):
        with tempfile.TemporaryDirectory() as name:
            status, remote = self.run_cli(['--cores','dsperate','rgds-experimental','--max-speed'],Path(name)/'result.txt')
            self.assertEqual(status, 0)
            options=json.loads(shlex.split(remote.ssh.call_args.args[1])[-1])
            self.assertTrue(options['max_speed'])
            with self.assertRaises(SystemExit):
                self.run_cli(['--cores','superdrastic','--max-speed'],Path(name)/'bad.txt')

    def test_cli_passes_profile_and_affinity_controls_and_preserves_output(self):
        with tempfile.TemporaryDirectory() as name:
            output=Path(name)/'result.txt'
            status,remote=self.run_cli(['--phase','normal','--seconds','7','--menu-cycles','0',
                '--engine-env','DS_FPS=0','--engine-env','DS_FRAME_STATS=0',
                '--engine-env','RGDS_LAUNCH_PROFILE=1','--engine-env','NNDDSS_CPU_AFFINITY=0','--capture-scene',
                '--threaded-3d','0'],output)
            self.assertEqual(status,0)
            options=json.loads(shlex.split(remote.ssh.call_args.args[1])[-1])
            self.assertEqual(options['engine_env'],dict(DS_FPS='0',DS_FRAME_STATS='0',
                RGDS_LAUNCH_PROFILE='1',NNDDSS_CPU_AFFINITY='0'))
            self.assertEqual(options['experiment_settings'],sorted(benchmark.EXPERIMENT_SETTINGS))
            self.assertEqual((options['phase'],options['seconds'],options['menu_cycles']),('normal',7,0))
            self.assertTrue(options['capture_scene'])
            self.assertEqual(options['threaded_3d'],0)
            self.assertEqual(remote.ssh.call_args.kwargs['input'],benchmark.DEVICE_SCRIPT)
            self.assertIn('RESULTS',output.read_text())

    def test_device_threading_configuration_is_read_by_real_runtime_before_launch(self):
        # The runtime imports its siblings by bare name, as it does on the device.
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
        from runtime.emulator_profiles import game_options
        from runtime.fields import write_fields
        from runtime.library import game_key
        script=ast.parse(benchmark.DEVICE_SCRIPT)
        loop=next(node for node in script.body if isinstance(node,ast.For))
        index=next(i for i,node in enumerate(loop.body) if isinstance(node,ast.Assign)
                   and any(isinstance(target,ast.Name) and target.id=='tuning' for target in node.targets))
        statements=loop.body[index-2:index+3]
        self.assertIn('threaded_3d',ast.unparse(statements[-1]))
        launch=next(i for i,node in enumerate(loop.body) if 'subprocess.Popen(' in ast.unparse(node))
        self.assertLess(index+2,launch)
        code=compile(ast.Module(body=statements,type_ignores=[]),'<benchmark game settings>','exec')
        for requested in (0,1):
            with self.subTest(threaded=requested),tempfile.TemporaryDirectory() as name:
                base=Path(name);data=base/'private-profile';rom=base/'copied-ROM.nds'
                exec(code,dict(data=data,rom=rom,options={'threaded_3d':requested},
                               game_key=game_key,write_fields=write_fields))
                self.assertEqual(game_options(data,rom)['threaded'],requested)
                self.assertFalse(rom.exists())
        for requested in (2,3):
            with self.subTest(resolution=requested),tempfile.TemporaryDirectory() as name:
                base=Path(name);data=base/'private-profile';rom=base/'copied-ROM.nds'
                exec(code,dict(data=data,rom=rom,options={'resolution':requested,'rasterizer':'gengis'},
                               game_key=game_key,write_fields=write_fields))
                tuning=game_options(data,rom)
                self.assertEqual(tuning['hires'],1);self.assertEqual(tuning['gengis_scale'],requested)

    def test_capture_records_fresh_both_panel_files_and_hashes(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);request=folder/'emulator.control.capture'
            (folder/'emulator.control.capture-combined.bmp').write_bytes(b'stale')
            def present():
                self.assertTrue(request.read_text().strip().isdigit())
                for panel in ('top','bottom'):(folder/('emulator.control.capture-'+panel+'.bmp')).write_bytes(panel.encode())
                request.unlink();return None
            record=remote_helpers()['capture_scene']('dsperate',folder,Mock(poll=present))
            self.assertTrue(record['ready']);self.assertTrue(record['acknowledged'])
            self.assertEqual(len(record['files']),2)
            self.assertFalse(request.exists())
            # Opening a later menu replaces the bridge's capture files.
            # The benchmark evidence must retain the original measured scene.
            for panel in ('top','bottom'):
                (folder/('emulator.control.capture-'+panel+'.bmp')).write_bytes(b'later menu')
            for item in record['files']:
                content=Path(item['path']).read_bytes()
                self.assertIn(content,(b'top',b'bottom'))
                self.assertEqual(item['bytes'],len(content))
                self.assertEqual(item['sha256'],hashlib.sha256(content).hexdigest())

    def test_capture_stops_with_owned_child_and_cancels_request(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name)
            record=remote_helpers()['capture_scene']('melonds',folder,Mock(poll=Mock(return_value=1)))
            self.assertFalse(record['ready']);self.assertFalse(record['acknowledged'])
            self.assertEqual(record['files'],[])
            self.assertFalse((folder/'emulator.control.capture').exists())

    def test_capture_accepts_combined_surface_but_not_ack_without_pixels(self):
        for pixels in (b'combined-screen', b''):
            with self.subTest(pixels=bool(pixels)),tempfile.TemporaryDirectory() as name:
                folder=Path(name)
                def present():
                    (folder/'emulator.control.capture-combined.bmp').write_bytes(pixels)
                    (folder/'emulator.control.capture').unlink();return None
                record=remote_helpers()['capture_scene']('drastic',folder,Mock(poll=present))
                self.assertTrue(record['acknowledged'])
                self.assertEqual(record['ready'],bool(pixels))

    def test_capture_timeout_is_bounded_and_removes_pending_request(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=remote_helpers()
            namespace['time']=SimpleNamespace(monotonic=Mock(side_effect=[10,10.1,13.01,13.02]),
                                            monotonic_ns=Mock(return_value=10000),sleep=Mock())
            record=namespace['capture_scene']('dsperate',folder,Mock(poll=Mock(return_value=None)))
            self.assertFalse(record['ready']);self.assertFalse(record['acknowledged'])
            self.assertAlmostEqual(record['elapsed_seconds'],3.02)
            self.assertFalse((folder/'emulator.control.capture').exists())

    def test_unsupported_backends_never_request_capture(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name)
            for backend in ('superdrastic',):
                child=Mock()
                record=remote_helpers()['capture_scene'](backend,folder,child)
                self.assertFalse(record['supported']);self.assertFalse(record['ready'])
                child.poll.assert_not_called()
            self.assertEqual(list(folder.iterdir()),[])

    def test_capture_is_before_phase_settle_and_measurement_clock(self):
        script=benchmark.DEVICE_SCRIPT
        warmup=script.index("time.sleep(result['measurement']['initial_warmup_seconds'])")
        capture=script.index("result['scene_capture']=capture_scene")
        settle=script.index("time.sleep(result['measurement']['phase_settle_seconds'])")
        measurement=script.index('phase_started=time.monotonic()')
        self.assertLess(warmup,capture);self.assertLess(capture,settle);self.assertLess(settle,measurement)

    def test_unapproved_environment_override_never_reaches_ssh(self):
        with tempfile.TemporaryDirectory() as name:
            remote=SimpleNamespace(target=Mock(),ssh=Mock())
            args=['benchmark_emulators','--device','example','--rom','/roms/test.nds',
                  '--output',str(Path(name)/'result.txt'),'--engine-env','LD_PRELOAD=unverified']
            with patch.dict(sys.modules,{'device':remote}),patch.object(sys,'argv',args),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):benchmark.main()
            remote.ssh.assert_not_called()


class BenchmarkMenuTests(unittest.TestCase):
    def helpers(self):
        from runtime.fields import write_fields
        namespace=remote_helpers()
        self.clock=0
        def sleep(seconds):self.clock+=seconds
        namespace['time']=SimpleNamespace(monotonic=lambda:self.clock,monotonic_ns=lambda:int(self.clock*1e9),sleep=sleep)
        namespace['write_fields']=write_fields
        namespace['process_identity']=Mock(return_value=None)
        return namespace

    def test_helper_must_match_owner_ancestry_and_exact_profile(self):
        with tempfile.TemporaryDirectory() as name:
            proc=Path(name)/'proc';proc.mkdir();folder=Path(name)/'userdata/session'
            def process(pid,parent,start,comm='runner',args=()):
                directory=proc/str(pid);directory.mkdir()
                fields=['S',str(parent)]+['0']*17+[str(start)]
                (directory/'stat').write_text(str(pid)+' ('+comm+') '+' '.join(fields))
                (directory/'cmdline').write_bytes(b'\0'.join(os.fsencode(arg) for arg in args))
            process(100,1,200);process(101,100,201)
            args=['rgds-launcher','--game-menu','--data',str(folder.parent)]
            process(102,101,202,'rgds-launcher',args)
            process(103,1,203,'rgds-launcher',args)
            process(104,100,204,'rgds-launcher',args[:-1]+['/other/profile'])
            helpers=self.helpers()['menu_helpers']
            self.assertEqual([p['pid'] for p in helpers(dict(pid=100,start='200'),folder,proc)],[102])
            self.assertEqual(helpers(dict(pid=100,start='199'),folder,proc),[])

    def test_ready_pid_must_be_current_owned_live_helper(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            namespace['menu_helpers']=Mock(return_value=[dict(pid=42,start='7',state='S')])
            (folder/'emulator.control.blocked').touch()
            for value,state,expected in [('41','S',False),('42','Z',False),('42','S',True)]:
                namespace['menu_helpers'].return_value[0]['state']=state
                (folder/'game-menu.ready').write_text(value)
                self.assertEqual(namespace['menu_state'](folder,{},None)['presented'],expected)
            (folder/'emulator.control.blocked').unlink()
            self.assertFalse(namespace['menu_state'](folder,{},None)['presented'])

    def test_new_cycle_never_toggles_an_existing_helper(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            state=dict(blocked=True,helpers=[dict(pid=42,start='7')],ready_file=True,presented=True)
            namespace['menu_state']=Mock(return_value=state)
            record=namespace['open_menu'](folder,{},None,Mock(poll=Mock(return_value=None)))
            self.assertFalse(record['presented']);self.assertIn('Previous menu',record['error'])
            self.assertFalse((folder/'emulator.control.game-menu').exists())

    def test_quit_reuses_existing_ready_helper_without_toggle(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            state=dict(blocked=True,helpers=[dict(pid=42,start='7')],ready_file=True,presented=True)
            namespace['menu_state']=Mock(return_value=state)
            record=namespace['open_menu'](folder,{},None,Mock(poll=Mock(return_value=None)),reuse=True)
            self.assertTrue(record['presented']);self.assertTrue(record['reused'])
            self.assertEqual(record['helper'],dict(pid=42,start='7'))
            self.assertFalse((folder/'emulator.control.game-menu').exists())

    def test_resume_requires_reap_input_release_and_ready_cleanup(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            states=[dict(blocked=False,helpers=[],ready_file=False),
                    dict(blocked=False,helpers=[],ready_file=True),
                    dict(blocked=True,helpers=[],ready_file=False),
                    dict(blocked=False,helpers=[],ready_file=False)]
            namespace['menu_state']=Mock(side_effect=states)
            namespace['process_identity'].side_effect=['7',None,None,None]
            record=namespace['resume_menu'](folder,{},None,Mock(poll=Mock(return_value=None)),dict(pid=42,start='7'))
            self.assertTrue(record['resumed']);self.assertEqual(record['resume_seconds'],.03)
            self.assertIn('action=resume',(folder/'game-menu.request').read_text())

    def test_resume_timeout_is_explicit_and_bounded(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            namespace['menu_state']=Mock(return_value=dict(blocked=True,helpers=[dict(pid=42)],ready_file=True))
            namespace['process_identity'].return_value='7'
            record=namespace['resume_menu'](folder,{},None,Mock(poll=Mock(return_value=None)),dict(pid=42,start='7'),timeout=.03)
            self.assertFalse(record['resumed']);self.assertEqual(record['resume_seconds'],.03)
            self.assertIn('timeout',record['resume_error'])

    def test_open_timeout_does_not_accept_stale_ready_file(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);namespace=self.helpers()
            (folder/'game-menu.ready').write_text('dead-helper')
            namespace['menu_state']=Mock(return_value=dict(blocked=False,helpers=[],ready_file=False,presented=False))
            record=namespace['open_menu'](folder,{},None,Mock(poll=Mock(return_value=None)),timeout=.03)
            self.assertFalse(record['presented']);self.assertIn('timeout',record['error'])
            self.assertFalse((folder/'game-menu.ready').exists())
            self.assertTrue((folder/'emulator.control.game-menu').exists())


if __name__=='__main__':unittest.main()
