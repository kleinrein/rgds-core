"""Missing-adapter recovery cannot reset an existing WLAN or unrelated services."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import boot_network as network
import boot_control


class Clock:
    def __init__(self, callback=None):
        self.now = 0
        self.delays = []
        self.callback = callback

    def time(self):
        return self.now

    def sleep(self, delay):
        self.delays.append(delay)
        self.now += delay
        if self.callback: self.callback(self.now)


class BootNetworkTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sys = self.root / 'sys'
        (self.sys / 'class/net').mkdir(parents=True)
        self.helper = self.root / 'usr/bin/wifibt-init.sh'
        self.helper.parent.mkdir(parents=True)
        self.helper.write_text('#!/bin/sh\nexit 0\n')
        self.helper.chmod(0o755)
        digest = hashlib.sha256(self.helper.read_bytes()).hexdigest()
        reviewed = patch.object(network, 'REVIEWED_HELPERS', frozenset({digest}))
        reviewed.start()
        self.addCleanup(reviewed.stop)
        self.config = dict(network_guard=network.capture_helper(self.root))
        self.process = Mock()
        self.process.wait.return_value = 0
        self.process.poll.return_value = 0
        self.launch = Mock(return_value=self.process)
        self.clock = Clock()

    def iface(self, name='wlan0'):
        path = self.sys / 'class/net' / name
        path.mkdir(exist_ok=True)
        (path / 'wireless').mkdir(exist_ok=True)
        return path

    def guard(self, configuration=None):
        return network.ensure_boot_network(
            self.config if configuration is None else configuration,
            sysfs=self.sys,
            helper=self.helper,
            launch=self.launch,
            clock=self.clock.time,
            sleep=self.clock.sleep
        )

    def test_existing_wlan_is_zero_wait_without_helper_hash_or_mutations(self):
        self.iface()
        with patch.object(Path, 'read_bytes', side_effect=AssertionError('existing WLAN must not hash helpers')):
            result = self.guard(configuration={})
        self.assertEqual(result, dict(state='existing', interfaces=['wlan0']))
        self.launch.assert_not_called()
        self.assertEqual(self.clock.delays, [])

    def test_nonstandard_wireless_interface_is_also_preserved(self):
        path = self.sys / 'class/net/station0'
        path.mkdir()
        (path / 'uevent').write_text('INTERFACE=station0\nDEVTYPE=wlan\n')
        self.assertEqual(self.guard()['interfaces'], ['station0'])
        self.launch.assert_not_called()
        self.assertEqual(self.clock.delays, [])

    def test_wlan_name_alone_does_not_make_ethernet_a_wireless_station(self):
        (self.sys / 'class/net/wlan_fake').mkdir()
        with self.assertRaises(ValueError):
            self.guard(configuration={})
        self.launch.assert_not_called()

    def test_existing_phy_without_interface_is_not_initialized_or_accepted(self):
        (self.sys / 'class/ieee80211/phy0').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'PHY has no usable interface'):
            self.guard()
        self.assertAlmostEqual(self.clock.now, 2)
        self.launch.assert_not_called()

    def test_existing_phy_can_finish_naturally_without_a_helper(self):
        (self.sys / 'class/ieee80211/phy0').mkdir(parents=True)
        self.clock.callback = lambda now: self.iface() if now >= .4 else None
        self.assertEqual(self.guard()['state'], 'existing')
        self.launch.assert_not_called()
        self.assertLess(self.clock.now, 1)

    def test_p2p_only_is_not_a_real_station_but_is_kept_untouched(self):
        self.iface('p2p0')
        with self.assertRaisesRegex(ValueError, 'PHY has no usable interface'):
            self.guard()
        self.launch.assert_not_called()

    def test_missing_adapter_calls_only_verified_start_wifi_once_and_waits_for_interface(self):
        self.clock.callback = lambda now: self.iface() if now >= .6 else None
        result = self.guard()
        self.assertEqual(result, dict(state='initialized', interfaces=['wlan0']))
        self.launch.assert_called_once()
        self.assertEqual(self.launch.call_args.args[0], [str(self.helper), 'start_wifi'])
        self.assertEqual(self.launch.call_args.kwargs['stdout'], subprocess.DEVNULL)
        self.assertEqual(self.launch.call_args.kwargs['stderr'], subprocess.DEVNULL)
        self.assertTrue(self.launch.call_args.kwargs['start_new_session'])
        self.assertLess(self.clock.now, 1)

    def test_background_parent_exit_zero_is_not_adapter_readiness(self):
        with self.assertRaisesRegex(ValueError, 'did not create an interface'):
            self.guard()
        self.assertAlmostEqual(self.clock.now, 10)
        self.launch.assert_called_once()
        self.process.terminate.assert_not_called()
        self.process.kill.assert_not_called()

    def test_unknown_or_changed_helper_falls_back_without_executing(self):
        for guard in (
            {}, dict(schema=1, helper=str(network.VENDOR_HELPER),
                     sha256='0' * 64), dict(schema=1, helper=str(network.VENDOR_HELPER), sha256=[])
        ):
            with self.subTest(guard=guard), self.assertRaises(ValueError):
                self.guard(dict(network_guard=guard))
        self.helper.write_text('# changed vendor helper\n')
        with self.assertRaisesRegex(ValueError, 'changed since configuration'):
            self.guard()
        self.launch.assert_not_called()
        self.assertEqual(self.clock.delays, [])

    def test_helper_failure_or_parent_timeout_never_waits_indefinitely(self):
        self.process.wait.return_value = 1
        with self.assertRaisesRegex(ValueError, 'initializer failed'):
            self.guard()
        self.assertEqual(self.clock.delays, [])
        self.process.poll.return_value = None
        self.process.wait.side_effect = [subprocess.TimeoutExpired('helper', 2), 0]
        with self.assertRaises(subprocess.TimeoutExpired):
            self.guard()
        self.process.terminate.assert_called_once()
        self.assertEqual(self.process.wait.call_args_list[-1].kwargs['timeout'], .5)

    def test_guard_environment_cannot_load_legacy_helpers(self):
        self.clock.callback = lambda now: self.iface() if now >= .2 else None
        with patch.dict(
            network.os.environ,
            PATH='/old/bin',
            RGDS_ROOT='/old',
            LD_PRELOAD='old.so',
            LD_LIBRARY_PATH='/old/lib'
        ):
            self.guard()
        environment = self.launch.call_args.kwargs['env']
        self.assertEqual(environment['PATH'], '/usr/sbin:/usr/bin:/sbin:/bin')
        self.assertFalse(any(key.startswith('RGDS_') for key in environment))
        self.assertNotIn('LD_PRELOAD', environment)
        self.assertNotIn('LD_LIBRARY_PATH', environment)

    def test_capture_records_exact_helper_bytes_and_unknown_versions_are_not_approved(self):
        recorded = network.capture_helper(self.root)
        self.assertEqual(recorded['sha256'], hashlib.sha256(self.helper.read_bytes()).hexdigest())
        self.assertTrue(recorded['reviewed'])
        self.helper.write_text('# unreviewed firmware revision\n')
        self.assertFalse(network.capture_helper(self.root)['reviewed'])

    def test_enable_plan_persists_guard_metadata_without_touching_vendor_helper(self):
        app = self.root / 'rgds'
        (app / 'state').mkdir(parents=True)
        (app / 'state/active').write_text('test-build\n')
        (app / 'releases/test-build/runtime').mkdir(parents=True)
        (app / 'releases/test-build/runtime/boot.py').write_text('pass\n')
        supervisor = self.root / str(boot_control.SUPERVISOR).lstrip('/')
        supervisor.parent.mkdir(parents=True)
        supervisor.write_text('# vendor hook /mnt/mod/ctrl/autostart\n')
        before = self.helper.read_bytes()
        with patch('activate.verify_release', return_value={}):
            boot_control.configure(app, 'rgds', self.root)
        recorded = boot_control.read_json(self.root / str(boot_control.CONTROL).lstrip('/') / 'config.json')
        self.assertEqual(recorded['network_guard'], self.config['network_guard'])
        self.assertEqual(self.helper.read_bytes(), before)

    def test_deferred_recovery_publishes_readiness_only_after_adapter_exists(self):
        config = self.root / 'config.json'
        config.write_text(json.dumps(self.config))
        result = self.root / 'run/network.json'
        self.clock.callback = lambda now: self.iface() if now >= .6 else None
        recover = lambda configuration: network.ensure_boot_network(
            configuration,
            sysfs=self.sys,
            helper=self.helper,
            launch=self.launch,
            clock=self.clock.time,
            sleep=self.clock.sleep
        )
        record = network.run_deferred(config, result, recover=recover)
        self.assertEqual(record['state'], 'initialized')
        self.assertEqual(record['interfaces'], ['wlan0'])
        self.assertEqual(json.loads(result.read_text()), record)
        self.assertGreaterEqual(self.clock.now, .6)

    def test_deferred_missing_adapter_is_diagnostic_failure_without_stock_fallback(self):
        config = self.root / 'config.json'
        config.write_text('{}')
        result = self.root / 'run/network.json'
        recover = lambda configuration: network.ensure_boot_network(
            configuration,
            sysfs=self.sys,
            helper=self.helper,
            launch=self.launch,
            clock=self.clock.time,
            sleep=self.clock.sleep
        )
        record = network.run_deferred(config, result, recover=recover)
        self.assertEqual(record['state'], 'unavailable')
        self.assertEqual(record['interfaces'], [])
        self.assertIn('No verified Wi-Fi-only', record['error'])
        self.assertNotIn('returning to stock', record['error'])
        self.assertEqual(json.loads(result.read_text()), record)
        self.launch.assert_not_called()

    def test_deferred_parent_timeout_is_recorded_and_only_owned_parent_is_stopped(self):
        config = self.root / 'config.json'
        config.write_text(json.dumps(self.config))
        self.process.poll.return_value = None
        self.process.wait.side_effect = [subprocess.TimeoutExpired('helper', 2), 0]
        recover = lambda configuration: network.ensure_boot_network(
            configuration,
            sysfs=self.sys,
            helper=self.helper,
            launch=self.launch,
            clock=self.clock.time,
            sleep=self.clock.sleep
        )
        record = network.run_deferred(config, self.root / 'run/network.json', recover=recover)
        self.assertEqual(record['state'], 'unavailable')
        self.process.terminate.assert_called_once()
        self.process.kill.assert_not_called()

    def test_deferred_bad_configuration_cannot_run_helper(self):
        config = self.root / 'config.json'
        recover = Mock()
        for contents in ('{broken', '[]', 'null'):
            with self.subTest(contents=contents):
                config.write_text(contents)
                record = network.run_deferred(config, self.root / 'run/network.json', recover=recover)
                self.assertEqual(record['state'], 'unavailable')
        recover.assert_not_called()

    def test_deferred_result_is_atomic_and_unwritable_diagnostics_are_optional(self):
        config = self.root / 'config.json'
        config.write_text('{}')
        result = self.root / 'network.json'
        result.write_text('{"state":"previous"}\n')
        recover = Mock(return_value=dict(state='existing', interfaces=['station0']))
        with patch.object(network.os, 'replace', side_effect=OSError('read-only diagnostics')):
            record = network.run_deferred(config, result, recover=recover)
        self.assertEqual(record['state'], 'existing')
        self.assertEqual(json.loads(result.read_text()), dict(state='previous'))
        self.assertEqual(list(self.root.glob('.network.json-*')), [])

    def test_deferred_cli_reports_failure_without_mutating_network(self):
        config = self.root / 'config.json'
        config.write_text('[]')
        result = self.root / 'run/network.json'
        worker = subprocess.run(
            [
                sys.executable, '-B',
                str(ROOT / 'runtime/boot_network.py'), '--deferred',
                str(config), '--result',
                str(result)
            ],
            capture_output=True,
            text=True,
            timeout=2
        )
        self.assertEqual(worker.returncode, 1)
        self.assertEqual(worker.stdout, '')
        self.assertEqual(worker.stderr, '')
        self.assertEqual(json.loads(result.read_text())['state'], 'unavailable')


if __name__ == '__main__': unittest.main()
