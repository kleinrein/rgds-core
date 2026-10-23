"""SMB ownership, isolated configuration, and the Connectivity controls."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from device_services import Services
from fields import read_fields
from file_sharing import Sharing, serve
from records import write_record


class FileSharingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = self.root / 'userdata'
        self.data.mkdir()
        self.sharing = Sharing(self.data, self.root / 'app', runtime_root=self.data / 'device/sharing')
        state = patch('file_sharing.process_state', return_value='S')
        state.start()
        self.addCleanup(state.stop)
        boot = patch('file_sharing.boot_id', return_value='test-boot')
        boot.start()
        self.addCleanup(boot.stop)

    def test_open_mode_allows_guests_and_both_modes_veto_mac_clutter(self):
        games = self.root / 'Roms/NDS'
        games.mkdir(parents=True)
        self.sharing.folder.mkdir(parents=True)
        config, shares = self.sharing.configure('192.168.1.42', [games], 'root', guest=True)
        text = config.read_text()
        self.assertIn('map to guest = Bad User', text)
        self.assertIn('guest ok = yes', text)
        self.assertNotIn('valid users', text)
        self.assertIn('force user = root', text)
        self.assertIn('veto files = /._*/.DS_Store/', text)
        self.assertIn('delete veto files = yes', text)
        config, _shares = self.sharing.configure('192.168.1.42', [games], 'root')
        text = config.read_text()
        self.assertIn('map to guest = Never', text)
        self.assertIn('valid users = root', text)
        self.assertNotIn('guest ok = yes', text)
        self.assertIn('veto files = /._*/.DS_Store/', text)

    def test_bonjour_service_is_advertised_only_while_sharing(self):
        service = self.root / 'etc/avahi/services/rgds-smb.service'
        self.sharing.avahi_service = service
        with patch('file_sharing.shutil.which', return_value='/usr/sbin/avahi-daemon'), \
             patch('file_sharing.subprocess.run') as run:
            self.sharing.advertise(True)
            self.assertIn('<type>_smb._tcp</type><port>445</port>', service.read_text())
            self.assertIn('<name>RG DS</name>', service.read_text())
            self.sharing.advertise(True)
            self.assertEqual(run.call_count, 1)  # Unchanged advertisement: no reload.
            self.sharing._stop(None)
            self.assertFalse(service.exists())
            self.assertEqual(run.call_count, 2)
            # The firmware's umask 077 must not hide the file from avahi's own
            # user, and a root-only copy left by an older release is repaired.
            previous = os.umask(0o077)
            try:
                self.sharing.advertise(True)
                self.assertEqual(service.stat().st_mode & 0o777, 0o644)
                service.chmod(0o600)
                self.sharing.advertise(True)
                self.assertEqual(service.stat().st_mode & 0o777, 0o644)
                self.assertEqual(run.call_count, 4)
            finally:
                os.umask(previous)
        # Discovery is best effort: an unwritable folder never blocks sharing.
        self.sharing.avahi_service = Path('/proc/no-such-folder/rgds-smb.service')
        self.sharing.advertise(True)

    def test_damaged_release_requests_repair_and_never_uses_firmware_binaries(self):
        with patch('file_sharing.subprocess.Popen') as launch:
            self.assertEqual(self.sharing.status('192.168.1.42')['sharing_supported'], 0)
            with self.assertRaisesRegex(ValueError, 'Reinstall the RGDS update'):
                self.sharing.start('192.168.1.42', [])
            launch.assert_not_called()

    def test_bundle_is_supported_without_samba_on_the_system_path(self):
        for name in ('smbd', 'smbpasswd', 'smbclient', 'samba-dcerpcd', 'rpcd_classic', 'rpcd_winreg', 'rpcd_lsad'):
            path = self.sharing.bundle / 'bin' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'bundled executable')
        with patch('file_sharing.os.geteuid', return_value=0), patch.dict(os.environ, {'PATH': '/no-samba-here'}):
            self.assertTrue(self.sharing.supported())
        (self.sharing.bundle / 'bin/smbd').unlink()
        self.assertFalse(self.sharing.supported())

    def test_linux_keeps_sockets_and_passwords_off_the_fat_games_card(self):
        with patch('file_sharing.sys.platform', 'linux'):
            first = Sharing(self.data, self.root / 'app')
            second = Sharing(self.data, self.root / 'other-release')
        self.assertTrue(first.folder.is_relative_to('/run/rgds-samba'))
        self.assertEqual(first.folder, second.folder)
        self.assertNotEqual(first.folder, self.data / 'device/sharing')

    def test_supervisor_stops_both_smb_and_rpc_children(self):
        rpc = Mock()
        rpc.poll.return_value = None
        smb = Mock()
        smb.poll.return_value = None
        with patch('file_sharing.subprocess.Popen', side_effect=[rpc, smb]) as launch, \
                patch('file_sharing.signal.signal'), patch('file_sharing.time.sleep', side_effect=KeyboardInterrupt):
            self.assertEqual(serve(self.sharing.bundle, self.sharing.folder / 'smb.conf'), 0)
        for call in launch.call_args_list:
            self.assertIn('--foreground', call.args[0])
            self.assertIn('--no-process-group', call.args[0])
        self.assertNotIn('--libexec-rpcds', launch.call_args_list[0].args[0])
        for name in ('rpcd_classic', 'rpcd_winreg', 'rpcd_lsad'):
            self.assertIn(str(self.sharing.bundle / 'bin' / name), launch.call_args_list[0].args[0])
        rpc.terminate.assert_called_once()
        smb.terminate.assert_called_once()
        rpc.wait.assert_called_once()
        smb.wait.assert_called_once()

    def test_configuration_limits_access_to_games_and_save_transfers(self):
        games = self.root / 'Games card/NDS'
        games.mkdir(parents=True)
        alias = self.root / 'alias'
        alias.symlink_to(games, target_is_directory=True)
        with self.sharing.locked():
            config, shares = self.sharing.configure(
                '192.168.1.42', [games, games, alias, self.root / 'missing'], 'root'
            )
        content = config.read_text()
        self.assertEqual(shares, 'Games, Imports, Exports')
        self.assertIn('path = ' + str(games.resolve()), content)
        self.assertIn('interfaces = 192.168.1.42\nbind interfaces only = yes', content)
        self.assertIn('server min protocol = SMB2', content)
        self.assertIn('map to guest = Never', content)
        self.assertIn('valid users = root', content)
        self.assertIn('follow symlinks = no', content)
        self.assertNotIn('[homes]', content)
        self.assertNotIn('path = ' + str(self.data) + '\n', content)
        self.assertEqual(self.sharing.folder.stat().st_mode & 0o777, 0o700)

    def test_configuration_rejects_expansion_and_newline_paths_and_symlinked_transfers(self):
        for name in ('%U', 'games\n[secret]'):
            folder = self.root / name
            folder.mkdir()
            with self.sharing.locked(), self.assertRaisesRegex(ValueError, 'path is unsupported'):
                self.sharing.configure('192.168.1.42', [folder], 'root')
        (self.data / 'imports').symlink_to(self.root, target_is_directory=True)
        with self.sharing.locked(), self.assertRaisesRegex(ValueError, 'transfer folder'):
            self.sharing.configure('192.168.1.42', [], 'root')

    def test_start_uses_private_password_stdin_and_waits_for_the_server(self):
        process = Mock(pid=321)
        process.poll.return_value = None
        with patch.object(self.sharing, 'supported', return_value=True), \
                patch.object(self.sharing, 'reachable', return_value=True), \
                patch('file_sharing.socket.socket'), \
                patch('file_sharing.secrets.token_hex', return_value='session-secret'), \
                patch('file_sharing.subprocess.run', return_value=Mock(returncode=0)) as password, \
                patch('file_sharing.subprocess.Popen', return_value=process) as launch, \
                patch('file_sharing.process_identity', return_value='1234'):
            self.sharing.start('192.168.1.42', [])
        self.assertEqual(password.call_args_list[0].kwargs['input'], 'session-secret\nsession-secret\n')
        self.assertNotIn('session-secret', ' '.join(password.call_args_list[0].args[0]))
        self.assertEqual(password.call_args_list[0].args[0][0], str(self.sharing.bundle / 'bin/smbpasswd'))
        self.assertEqual(password.call_args_list[1].args[0][0], str(self.sharing.bundle / 'bin/smbclient'))
        self.assertTrue(launch.call_args.kwargs['start_new_session'])
        self.assertIn('serve', launch.call_args.args[0])
        self.assertEqual(launch.call_args.kwargs['env']['LD_LIBRARY_PATH'], str(self.sharing.bundle / 'lib'))
        self.assertNotIn('LD_PRELOAD', launch.call_args.kwargs['env'])
        self.assertNotIn('session-secret', self.sharing.record.read_text())
        login = self.sharing.folder / 'login.ini'
        self.assertEqual(read_fields(login)['password'], 'session-secret')
        self.assertEqual(login.stat().st_mode & 0o777, 0o600)

    def test_busy_port_never_reconfigures_or_stops_another_server(self):
        with patch.object(self.sharing, 'supported', return_value=True), \
                patch('file_sharing.socket.socket') as socket, \
                patch.object(self.sharing, 'configure') as config, patch('file_sharing.os.killpg') as kill:
            socket.return_value.__enter__.return_value.bind.side_effect = OSError('busy')
            with self.assertRaisesRegex(ValueError, 'already in use'):
                self.sharing.start('192.168.1.42', [])
            config.assert_not_called()
            kill.assert_not_called()

    def test_listening_port_without_working_login_never_counts_as_on(self):
        process = Mock(pid=321)
        process.poll.return_value = None
        with patch.object(self.sharing, 'supported', return_value=True), patch('file_sharing.socket.socket'), \
                patch.object(self.sharing, 'reachable', return_value=True), \
                patch('file_sharing.subprocess.run', side_effect=[Mock(returncode=0), Mock(returncode=1)]), \
                patch('file_sharing.subprocess.Popen', return_value=process), \
                patch('file_sharing.process_identity', return_value='1234'), \
                patch('file_sharing.time.monotonic', side_effect=[0, 10]), \
                patch('file_sharing.os.getpgid', side_effect=ProcessLookupError), \
                patch('file_sharing.os.killpg') as stop:
            with self.assertRaisesRegex(ValueError, 'could not start'):
                self.sharing.start('192.168.1.42', [])
        stop.assert_called_once_with(321, signal.SIGTERM)
        process.wait.assert_called_once()
        self.assertFalse((self.sharing.folder / 'login.ini').exists())

    def test_stop_signals_only_the_verified_private_process_group(self):
        write_record(self.sharing.record, dict(pid=321, start='1234', boot='test-boot', ip='192.168.1.42'))
        with patch('file_sharing.process_identity', side_effect=['1234', None]), \
                patch('file_sharing.os.getpgid', return_value=321), patch('file_sharing.os.killpg') as kill:
            self.sharing.stop()
            kill.assert_called_once_with(321, signal.SIGTERM)
        self.assertFalse(self.sharing.record.exists())

    def test_reused_pid_and_foreign_process_group_are_never_signalled(self):
        for identity, group in (('5678', 321), ('1234', 999)):
            with self.subTest(identity=identity, group=group):
                write_record(self.sharing.record, dict(pid=321, start='1234', boot='test-boot', ip='192.168.1.42'))
                with patch('file_sharing.process_identity', return_value=identity), \
                        patch('file_sharing.os.getpgid', return_value=group), patch('file_sharing.os.killpg') as kill:
                    self.sharing.stop()
                    kill.assert_not_called()

    def test_zombie_server_is_off_and_does_not_block_a_future_start(self):
        write_record(self.sharing.record, dict(pid=321, start='1234', boot='test-boot', ip='192.168.1.42'))
        with patch('file_sharing.process_identity', return_value='1234'), \
                patch('file_sharing.process_state', return_value='Z'), patch('file_sharing.os.killpg') as kill:
            self.sharing.stop()
            kill.assert_not_called()
        self.assertFalse(self.sharing.record.exists())

    def test_record_from_a_previous_boot_never_authorizes_a_signal(self):
        write_record(self.sharing.record, dict(pid=321, start='1234', boot='previous-boot', ip='192.168.1.42'))
        with patch('file_sharing.process_identity', return_value='1234'), patch('file_sharing.os.killpg') as kill:
            self.sharing.stop()
            kill.assert_not_called()

    def test_network_loss_stops_sharing_and_clears_the_address(self):
        write_record(self.sharing.record, dict(pid=321, start='1234', boot='test-boot', ip='192.168.1.42'))
        with patch.object(self.sharing, 'supported', return_value=True), \
                patch.object(self.sharing, 'alive', side_effect=[True, True, False, False, False]), \
                patch('file_sharing.os.killpg') as kill:
            state = self.sharing.status('')
        kill.assert_called_once_with(321, signal.SIGTERM)
        self.assertEqual(state['sharing'], 0)
        self.assertEqual(state['sharing_address'], '')

    def test_failed_start_removes_login_and_reaps_child(self):
        process = Mock(pid=321)
        process.poll.return_value = 1
        with patch.object(self.sharing, 'supported', return_value=True), patch('file_sharing.socket.socket'), \
                patch('file_sharing.subprocess.run', return_value=Mock(returncode=0)), \
                patch('file_sharing.subprocess.Popen', return_value=process), \
                patch('file_sharing.process_identity', return_value='1234'), \
                patch('file_sharing.os.getpgid', side_effect=ProcessLookupError):
            with self.assertRaisesRegex(ValueError, 'could not start'):
                self.sharing.start('192.168.1.42', [])
        process.wait.assert_called_once()
        self.assertFalse(self.sharing.record.exists())
        self.assertFalse((self.sharing.folder / 'login.ini').exists())

    def test_preview_toggle_and_wifi_off_clear_login_and_ip(self):
        service = Services(self.root, self.data, preview=True)
        service.info()
        service.sharing(True)
        self.assertEqual(service.fields['sharing_address'], 'smb://192.168.1.42')
        self.assertNotIn('password', service.fields)
        service.radio(False)
        self.assertEqual(service.fields['sharing'], '0')
        self.assertEqual(service.fields['ip'], '')
        self.assertFalse((self.sharing.folder / 'login.ini').exists())
        with self.assertRaisesRegex(ValueError, 'Connect to Wi-Fi'):
            service.sharing(True)


@unittest.skipUnless((ROOT / 'build/host/rgds-launcher').exists(), 'Build host launcher first')
class ConnectivityUiTests(unittest.TestCase):
    def test_keyboard_can_toggle_sharing_on_and_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            script = folder / 'input.txt'
            # Row 4 opens Off / Open on this Wi-Fi / Password protected.
            choices = (
                (True, 'password', '800 key Down\n850 key Down\n'), (False, '', '800 key Up\n850 key Up\n'),
                (True, 'open', '800 key Down\n')
            )
            for enabled, mode, choose in choices:
                script.write_text(
                    '600 key Down\n650 key Down\n700 key Down\n750 key Return\n' + choose + '950 key Return\n'
                )
                result = subprocess.run(
                    [
                        str(ROOT / 'build/host/rgds-launcher'), '--preview', '--settings', '--section', 'connectivity',
                        '--data',
                        str(folder), '--input-script',
                        str(script), '--seconds', '2'
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
                    timeout=8
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                state = read_fields(folder / 'device/status.ini')
                self.assertEqual(state['sharing'], str(int(enabled)))
                self.assertEqual(state.get('sharing_mode', ''), mode)
                self.assertEqual(state['ip'], '192.168.1.42')
                self.assertEqual((folder / 'device/sharing/login.ini').exists(), enabled)

    def test_touch_toggle_and_keyboard_details_show_the_login(self):
        import json
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            script = folder / 'input.txt'
            script.write_text(
                '600 click 280 374\n700 key Down\n750 key Down\n800 key Return\n1100 key Down\n1200 key Return\n'
            )
            report = folder / 'report.json'
            result = subprocess.run(
                [
                    str(ROOT / 'build/host/rgds-launcher'), '--preview', '--settings', '--section', 'connectivity',
                    '--data',
                    str(folder), '--input-script',
                    str(script), '--seconds', '2', '--cache-report',
                    str(report)
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
                timeout=8
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(read_fields(folder / 'device/status.ini')['sharing'], '1')
            detail = json.loads(report.read_text())
            self.assertEqual(detail['picker_title'], 'Sharing details')
            self.assertTrue(detail['picker_detail'])
            self.assertEqual(detail['detail_lines'], 6)


if __name__ == '__main__':
    unittest.main()
