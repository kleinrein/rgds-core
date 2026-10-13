"""Owned autostart option, atomic fallthrough, and original-init preservation."""
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime'))
import boot_control as control
import boot


class BootControlTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.base=Path(temp.name);self.system=self.base/'system';self.root=self.base/'internal/rgds'
        supervisor=control.location(self.system,control.SUPERVISOR);supervisor.parent.mkdir(parents=True)
        supervisor.write_text('#!/bin/sh\n# vendor controller loads /mnt/mod/ctrl/autostart\n')
        (self.root/'state').mkdir(parents=True);(self.root/'state/active').write_text('test-build\n')
        entry=self.root/'releases/test-build/runtime/boot.py';entry.parent.mkdir(parents=True);entry.write_text('pass\n')
        self.verify=patch('activate.verify_release',return_value={});self.verify.start();self.addCleanup(self.verify.stop)
        self.ctl=control.location(self.system,control.CONTROL);self.hook=control.location(self.system,control.HOOK)

    def configure(self,mode='rgds',warning=True):return control.configure(self.root,mode,self.system,warning)

    def test_default_stock_is_a_shell_fallthrough_and_does_not_start_python(self):
        self.configure('stock')
        self.assertFalse(self.hook.exists())
        self.configure('rgds')
        previous = self.hook.read_bytes()
        self.configure('stock')
        self.assertEqual(control.inspect(self.root,self.system)['mode'],'stock')
        self.assertFalse((self.ctl/'enabled').exists())
        source=self.hook.read_text()
        self.assertEqual(self.hook.read_bytes(),previous)
        self.assertLess(source.index('/enabled'),source.index('/usr/bin/python3'))
        self.assertNotIn('sleep ',source)
        self.assertNotIn('mount ',source)
        self.assertNotIn('killall',source)

    def test_stock_choice_does_not_require_a_healthy_release_or_writable_card(self):
        self.configure()
        (self.root/'state/active').unlink()
        with patch('activate.verify_release',side_effect=ValueError('damaged engine')) as verify,patch('session.ensure_writable_data',side_effect=ValueError('read-only card')) as writable:
            result=self.configure('stock',warning=False)
        verify.assert_not_called();writable.assert_not_called()
        self.assertEqual(result['mode'],'stock');self.assertEqual(result['warning'],0)
        self.assertFalse((self.ctl/'enabled').exists())

    def test_stock_disable_happens_before_a_failed_preference_write(self):
        self.configure()
        with patch('boot_control.atomic',side_effect=OSError('config interrupted')):
            with self.assertRaises(OSError):self.configure('stock')
        self.assertFalse((self.ctl/'enabled').exists())
        self.assertEqual(control.inspect(self.root,self.system)['mode'],'stock')

    def test_shell_guard_returns_stock_once_even_if_python_entry_cannot_import(self):
        self.configure()
        entry=self.root/'releases/test-build/runtime/boot.py'
        entry.write_text('this is not valid Python !!!!\n')
        run=self.base/'run';run.mkdir()
        self.hook.write_text(control.hook_text(self.root,self.ctl,run))
        first=subprocess.run(['/bin/sh',str(self.hook)],text=True,capture_output=True,timeout=5)
        self.assertEqual(first.returncode,0);self.assertIn('SyntaxError',first.stderr)
        self.assertTrue((run/'rgds-boot-hook-attempted').is_dir())
        second=subprocess.run(['/bin/sh',str(self.hook)],text=True,capture_output=True,timeout=5)
        self.assertEqual(second.returncode,0);self.assertEqual(second.stderr,'')

    def test_enable_tracks_correct_root_and_owned_hook_hash(self):
        result=self.configure()
        self.assertEqual(result['installed'],1);self.assertEqual(result['mode'],'rgds')
        self.assertEqual(result['root'],str(self.root.resolve()))
        self.assertEqual(control.inspect(self.base/'another',self.system)['installed'],0)
        self.assertFalse((self.ctl/'installing').exists())
        self.assertEqual(self.hook.stat().st_mode & 0o777,0o755)

    def test_plan_is_readonly_and_preserves_all_vendor_boot_files(self):
        vendor=control.location(self.system,Path('/etc/init.d/S36wifibt-init.sh'));vendor.parent.mkdir(parents=True)
        vendor.write_text('# original vendor Wi-Fi sequence\n')
        before=vendor.read_bytes()
        proposal=control.plan(self.root,'rgds',self.system)
        self.assertEqual(proposal['mode'],'rgds')
        self.assertFalse(self.hook.exists());self.assertFalse(self.ctl.exists())
        self.configure();self.assertEqual(vendor.read_bytes(),before)

    def test_external_hook_and_symlink_are_never_overwritten(self):
        self.hook.parent.mkdir(parents=True);self.hook.write_text('# another manager\n')
        with self.assertRaisesRegex(ValueError,'another manager'):self.configure()
        self.assertEqual(self.hook.read_text(),'# another manager\n')
        self.hook.unlink();self.hook.symlink_to(self.root/'state/active')
        with self.assertRaisesRegex(ValueError,'symlink'):self.configure()

    def test_legacy_init_mutations_require_vendor_restore_before_install(self):
        init=control.location(self.system,Path('/etc/init.d/S50simpleos.sh'));init.parent.mkdir(parents=True)
        init.write_text('foreign boot manager\n')
        with self.assertRaisesRegex(ValueError,'previous boot manager'):self.configure()
        self.assertFalse(self.hook.exists())

    def test_interrupted_update_cannot_enable_boot(self):
        self.configure()
        with patch('boot_control.atomic',side_effect=OSError('write interrupted')):
            with self.assertRaises(OSError):self.configure()
        # The first failed write leaves the previous valid target intact.
        self.assertEqual(control.inspect(self.root,self.system)['mode'],'rgds')
        (self.ctl/'installing').write_text('unfinished\n')
        self.assertEqual(control.inspect(self.root,self.system)['mode'],'stock')

    def test_disable_and_uninstall_restore_unmanaged_stock_route(self):
        self.configure();control.disable(self.system)
        self.assertEqual(control.inspect(self.root,self.system)['mode'],'stock')
        control.uninstall(self.system);self.assertFalse(self.hook.exists())
        self.assertEqual((self.root/'state/active').read_text(),'test-build\n')

    def test_changed_owned_hook_is_not_deleted_on_uninstall(self):
        self.configure();self.hook.write_text('# externally changed\n')
        with self.assertRaisesRegex(ValueError,'changed externally'):control.uninstall(self.system)
        self.assertTrue(self.hook.exists())


class BootEntryTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.base=Path(temp.name);self.control=self.base/'control';self.run=self.base/'run';self.root=self.base/'rgds'
        self.control.mkdir();(self.control/'enabled').write_text('1\n')
        (self.root/'state').mkdir(parents=True);(self.root/'state/active').write_text('test-build\n')
        self.config=dict(schema=1,mode='rgds',root=str(self.root),warning=True)
        self.save()

    def save(self): (self.control/'config.json').write_text(json.dumps(self.config))

    def test_no_config_or_disabled_target_exits_without_initializing_runtime(self):
        (self.control/'enabled').unlink()
        with patch('activate.verify_startup') as verify:
            self.assertEqual(boot.run(self.control,self.run),0)
            verify.assert_not_called()
        self.assertFalse(self.run.exists())

    def test_foreground_route_warns_once_and_never_restarts_itself(self):
        calls=[]
        def launch(command,**kwargs):
            calls.append((command,kwargs));return type('Completed',(),{'returncode':0})()
        with patch('activate.verify_startup',return_value={}):
            self.assertEqual(boot.run(self.control,self.run,launch),0)
            self.assertEqual(boot.run(self.control,self.run,launch),0)
        self.assertEqual(len(calls),1);self.assertIn('--boot',calls[0][0]);self.assertIn('--boot-warning',calls[0][0])
        self.assertNotIn('LD_PRELOAD',calls[0][1]['env'])
        self.assertEqual(json.loads((self.run/'rgds-boot-result.json').read_text())['exit'],0)

    def test_failure_returns_to_stock_without_loop_or_forced_reset(self):
        with patch('activate.verify_startup',side_effect=ValueError('damaged launcher')):
            self.assertEqual(boot.run(self.control,self.run),0)
        self.assertEqual(json.loads((self.run/'rgds-boot-result.json').read_text())['exit'],1)
        with patch('activate.verify_startup') as verify:boot.run(self.control,self.run);verify.assert_not_called()

    def test_recovery_marker_and_stock_skip_prevent_entry(self):
        (self.root/'userdata').mkdir();(self.root/'userdata/boot_stock').write_text('1')
        with patch('activate.verify_startup') as verify:boot.run(self.control,self.run);verify.assert_not_called()
        (self.root/'userdata/boot_stock').unlink()
        with patch.dict(os.environ,RGDS_SKIP_BOOT_ONCE='1'),patch('activate.verify_startup') as verify:
            boot.run(self.control,self.run);verify.assert_not_called()

    def test_invalid_root_and_partial_install_fall_through(self):
        self.config['root']='../../wrong';self.save()
        with patch('activate.verify_startup') as verify:boot.run(self.control,self.run);verify.assert_not_called()

    def test_warning_disabled_still_uses_managed_boot_and_records_correct_timings(self):
        self.config['warning']=False;self.save()
        result=type('Completed',(),{'returncode':0})()
        with patch('activate.verify_startup',return_value={}),patch('boot.subprocess.run',return_value=result) as launch:
            self.assertEqual(boot.run(self.control,self.run),0)
        self.assertIn('--boot',launch.call_args.args[0]);self.assertNotIn('--boot-warning',launch.call_args.args[0])
        record=json.loads((self.run/'rgds-boot-result.json').read_text())
        self.assertIn('dispatch_seconds',record);self.assertIn('session_seconds',record)
        self.assertNotIn('entry_seconds',record)

    def test_unwritable_run_directory_returns_stock_instead_of_crashing_hook(self):
        with patch.object(Path,'mkdir',side_effect=OSError('run directory not writable')),patch('activate.verify_startup') as verify:
            self.assertEqual(boot.run(self.control,self.run),0)
        verify.assert_not_called()

    def test_bad_warning_type_does_not_start_a_session(self):
        self.config['warning']='false';self.save()
        with patch('activate.verify_startup') as verify:
            self.assertEqual(boot.run(self.control,self.run),0);verify.assert_not_called()
        self.config['root']=str(self.root);self.save();(self.control/'installing').write_text('pending')
        with patch('activate.verify_startup') as verify:boot.run(self.control,self.run);verify.assert_not_called()

    def test_network_readiness_never_blocks_dispatch_and_is_deferred_to_session(self):
        calls=[]
        def launch(command, **kwargs):
            calls.append(command)
            descriptor=int(command[command.index('--startup-fd')+1])
            with os.fdopen(os.dup(descriptor)) as stream:receipt=json.load(stream)
            self.assertIn('began',receipt);self.assertIn('verify_seconds',receipt)
            self.assertEqual(kwargs['pass_fds'],(descriptor,))
            return type('Completed',(),{'returncode':0})()
        with patch('activate.verify_startup',return_value={}), \
             patch('boot_network.ensure_boot_network',side_effect=AssertionError('network blocked startup')):
            self.assertEqual(boot.run(self.control,self.run,launch),0)
        self.assertEqual(len(calls),1)
        self.assertEqual(calls[0][calls[0].index('--boot-network-config')+1],str(self.control/'config.json'))
        self.assertEqual(json.loads((self.run/'rgds-boot-result.json').read_text())['exit'],0)

    def test_verified_session_runs_in_this_process_from_exact_path_and_restores_environment(self):
        app=self.root/'releases/test-build';entry=app/'runtime/session.py';entry.parent.mkdir(parents=True)
        observed=self.base/'observed.json'
        entry.write_text('import json, os, pathlib, signal, sys\n'
            'def main(argv=None, startup_receipt=None):\n'
            '    record=dict(argv=argv,receipt=startup_receipt,pid=os.getpid(),'
            'clean=all(key not in os.environ for key in ("RGDS_TEST","SIMPLEOS_TEST","LD_PRELOAD","SSH_CONNECTION")),'
            'path=os.environ["PATH"],bytecode=sys.dont_write_bytecode)\n'
            '    pathlib.Path('+repr(str(observed))+').write_text(json.dumps(record))\n'
            '    os.environ["RGDS_SESSION_LEAK"]="temporary"\n'
            '    sys.path.append("temporary session path")\n'
            '    signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            '    return 0\n')
        import signal
        handlers={signum:signal.getsignal(signum) for signum in (signal.SIGINT,signal.SIGTERM)}
        paths=list(sys.path);bytecode=sys.dont_write_bytecode
        with patch.dict(os.environ,RGDS_TEST='sentinel',SIMPLEOS_TEST='sentinel',LD_PRELOAD='sentinel',SSH_CONNECTION='sentinel'), \
             patch('activate.verify_startup',return_value=dict(schema=1,app=str(app),files={})), \
             patch('boot.same_runtime',return_value=True),patch('boot.subprocess.run') as spawn, \
             patch('boot.tempfile',create=True) as temporary:
            before=dict(os.environ)
            self.assertEqual(boot.run(self.control,self.run),0)
            self.assertEqual(dict(os.environ),before)
            spawn.assert_not_called();temporary.TemporaryFile.assert_not_called()
        self.assertEqual(sys.path,paths);self.assertEqual(sys.dont_write_bytecode,bytecode)
        for signum,handler in handlers.items():self.assertEqual(signal.getsignal(signum),handler)
        record=json.loads(observed.read_text())
        self.assertEqual(record['pid'],os.getpid());self.assertTrue(record['clean']);self.assertTrue(record['bytecode'])
        self.assertEqual(record['path'],'/usr/sbin:/usr/bin:/sbin:/bin')
        self.assertNotIn('--startup-fd',record['argv']);self.assertIn('--boot-warning',record['argv'])
        self.assertEqual(record['receipt']['app'],str(app))
        result=json.loads((self.run/'rgds-boot-result.json').read_text())
        self.assertEqual(result['dispatch_mode'],'in_process');self.assertEqual(result['exit'],0)
        self.assertIn('session_import_seconds',result)

    def test_session_import_or_runtime_failure_returns_stock_and_restores_environment(self):
        app=self.root/'releases/test-build';entry=app/'runtime/session.py';entry.parent.mkdir(parents=True)
        before=dict(os.environ);paths=list(sys.path);bytecode=sys.dont_write_bytecode
        for stage in ('import','main'):
            with self.subTest(stage=stage):
                lines=['os.environ["RGDS_IMPORT_LEAK"]="temporary"',
                       'sys.path.append("temporary import path")',
                       'raise RuntimeError("session '+stage+' failed")']
                source='import os, sys\n'
                if stage=='main':source+='def main(argv=None,startup_receipt=None):\n';lines=['    '+line for line in lines]
                entry.write_text(source+'\n'.join(lines)+'\n')
                run=self.run/stage
                with patch('activate.verify_startup',return_value={}),patch('boot.same_runtime',return_value=True):
                    self.assertEqual(boot.run(self.control,run),0)
                self.assertEqual(dict(os.environ),before);self.assertEqual(sys.path,paths)
                self.assertEqual(sys.dont_write_bytecode,bytecode)
                self.assertNotIn('_rgds_verified_boot_session',sys.modules)
                self.assertEqual(json.loads((run/'rgds-boot-result.json').read_text())['exit'],1)

    def test_changed_runtime_or_cached_dependency_keeps_fresh_process_isolation(self):
        app=self.root/'releases/test-build'
        self.assertFalse(boot.same_runtime(app,dict(files={})))
        with patch.object(boot,'__file__',str(app/'runtime/boot.py')):
            self.assertTrue(boot.same_runtime(app,dict(files={})))
            self.assertFalse(boot.same_runtime(app,dict(files={'runtime/activate.py':[]})))
        completed=type('Completed',(),{'returncode':0})()
        with patch('activate.verify_startup',return_value={}),patch('boot.same_runtime',return_value=False), \
             patch('boot.subprocess.run',return_value=completed) as spawn,patch('boot.run_verified_session') as direct:
            self.assertEqual(boot.run(self.control,self.run),0)
        direct.assert_not_called();spawn.assert_called_once()
        self.assertEqual(json.loads((self.run/'rgds-boot-result.json').read_text())['dispatch_mode'],'subprocess')


if __name__=='__main__':unittest.main()
