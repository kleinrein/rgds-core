"""Wi-Fi parsing and secret transport; maintenance stays inside test fixtures."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"runtime"))
from device_services import Services, split_terse, main
from emulator_profiles import display_filter, game_options
from library import write_fields, game_key


class DeviceServiceTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.root=Path(temp.name);self.data=self.root/"userdata";self.data.mkdir()
        self.service=Services(self.root/"app",self.data,preview=True)

    def test_colons_and_backslashes_in_network_names_are_not_field_separators(self):
        self.assertEqual(split_terse(r"*:My\: Wi\\Fi:87:WPA2"),["*","My: Wi\\Fi","87","WPA2"])

    def test_scan_keeps_connected_first_and_strongest_duplicate_without_passwords(self):
        self.service.preview=False
        rows="*:Current:50:WPA2\n:Other:20:WPA2\n:Other:90:WPA2\n:Guest:60:--\n"
        with patch.object(self.service,"network_status",return_value="wlan0"),patch.object(self.service,"command",return_value=rows):
            self.service.scan()
        networks=json.loads((self.data/"device/networks.json").read_text())
        self.assertEqual([entry["ssid"] for entry in networks],["Current","Other","Guest"])
        self.assertEqual(networks[1]["signal"],90)
        self.assertEqual(networks[2]["security"],"")

    def test_connected_strength_uses_cached_network_list_without_rescanning(self):
        self.service.preview=False
        replies=["wlan0:wifi:connected\n","enabled\n","enabled\n","RGDS-profile-name\n","192.168.1.42/24\n",":Other:99\n*:My Wi-Fi:37\n"]
        with patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=replies) as command:
            self.service.network_status()
        self.assertEqual(self.service.fields["wifi_signal"],"37")
        self.assertEqual(self.service.fields["wifi_connected"],"1")
        self.assertEqual(self.service.fields["connection"],"My Wi-Fi")
        self.assertIn("--rescan",command.call_args.args[0])
        self.assertEqual(command.call_args.args[0][-1],"no")

    def test_missing_strength_does_not_fabricate_a_full_connection(self):
        self.service.preview=False
        replies=["wlan0:wifi:connected\n","enabled\n","enabled\n","My Wi-Fi\n","192.168.1.42/24\n",ValueError("unavailable")]
        with patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=replies):
            self.service.network_status()
        self.assertEqual(self.service.fields["wifi_signal"],"-1")
        self.service.preview=True;self.service.radio(False)
        self.assertEqual(self.service.fields["wifi_signal"],"-1")
        self.assertEqual(self.service.fields["wifi_connected"],"0")

    def test_independent_missing_adapter_never_calls_vendor_initializer(self):
        with patch.dict('os.environ',{'RGDS_SESSION_PLATFORM':'independent'}), \
                patch('boot_network.wireless_state',return_value=([],False)), \
                patch('boot_network.capture_helper') as capture,patch('boot_network.ensure_boot_network') as vendor, \
                patch('device_services.time.monotonic',side_effect=[0,3]), \
                patch.object(self.service,'command'):
            with self.assertRaisesRegex(ValueError,'OS driver'):
                self.service.prepare_wifi(self.root/'sys')
            capture.assert_not_called();vendor.assert_not_called()

    def test_wifi_password_is_on_stdin_and_never_echoed_from_cli_failure(self):
        secret="private $(not a shell) password"
        result=subprocess.CompletedProcess([],4,stdout=secret,stderr=secret)
        with patch("device_services.subprocess.run",return_value=result) as run:
            with self.assertRaises(ValueError) as error:
                self.service.command(["--ask","device","wifi","connect","My Wi-Fi"],password=secret)
        self.assertNotIn(secret,str(error.exception))
        self.assertNotIn(secret,run.call_args.args[0])
        self.assertEqual(run.call_args.kwargs["input"],secret+"\n")
        self.assertFalse(run.call_args.kwargs.get("shell",False))
        self.assertEqual(run.call_args.args[0][:3],["nmcli","--colors","no"])

    def test_p2p_adapter_is_never_selected_even_when_listed_first(self):
        self.service.preview=False
        replies=["p2p0:wifi:unmanaged\nwlan0:wifi:disconnected\n","enabled\n","enabled\n"]
        with patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=replies):
            self.assertEqual(self.service.network_status(),"wlan0")
        self.assertEqual(self.service.fields["wifi"],"1")
        self.assertEqual(self.service.fields["wifi_connected"],"0")

    def test_radio_flag_without_usable_adapter_does_not_claim_wifi_is_on(self):
        self.service.preview=False
        for devices,networking,reason in (("p2p0:wifi:unmanaged\n",None,"No Wi-Fi adapter"),
                ("wlan0:wifi:unavailable\n","enabled\n","not ready"),
                ("wlan0:wifi:unmanaged\n","enabled\n","not ready"),
                ("wlan0:wifi:disconnected\n","disabled\n","Networking is disabled")):
            with self.subTest(devices=devices,networking=networking):
                self.service.publish(connection="stale SSID",ip="192.168.1.42",wifi_connected=1)
                replies=[devices,"enabled\n"]+([networking] if networking else [])
                with patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=replies):
                    self.service.network_status()
                self.assertEqual(self.service.fields["wifi"],"0")
                self.assertEqual(self.service.fields["wifi_connected"],"0")
                self.assertEqual(self.service.fields["ip"],"")
                self.assertNotEqual(self.service.fields["connection"],"stale SSID")
                self.assertIn(reason,self.service.fields["wifi_reason"])

    def test_prepare_wifi_unblocks_only_wlan_and_enables_networking_and_station(self):
        sysfs=self.root/"sys"
        for name,kind in (("rfkill0","bluetooth"),("rfkill1","wlan")):
            entry=sysfs/"class/rfkill"/name;entry.mkdir(parents=True)
            (entry/"type").write_text(kind);(entry/"soft").write_text("1\n");(entry/"hard").write_text("0\n")
        (sysfs/"class/net/wlan0/wireless").mkdir(parents=True)
        (sysfs/"class/net/p2p0/wireless").mkdir(parents=True)
        with patch.object(self.service,"command") as command,patch.object(self.service,"run_command") as run,patch("boot_network.ensure_boot_network") as recover:
            self.service.prepare_wifi(sysfs)
        recover.assert_not_called()
        self.assertEqual((sysfs/"class/rfkill/rfkill0/soft").read_text(),"1\n")
        self.assertEqual((sysfs/"class/rfkill/rfkill1/soft").read_text(),"0\n")
        self.assertEqual([call.args[0] for call in command.call_args_list],
                         [["networking","on"],["radio","wifi","on"],["device","set","wlan0","managed","yes"]])
        self.assertEqual(run.call_args.args[0],["ip","link","set","dev","wlan0","up"])

    def test_prepare_missing_adapter_uses_verified_wifi_only_recovery_without_boot_install(self):
        sysfs=self.root/"sys";(sysfs/"class/net").mkdir(parents=True)
        guard={"reviewed":True}
        with patch("boot_network.capture_helper",return_value=guard),patch("boot_network.ensure_boot_network",return_value={"interfaces":["wlan0"]}) as recover,patch.object(self.service,"command"),patch.object(self.service,"run_command"):
            self.service.prepare_wifi(sysfs)
        recover.assert_called_once_with({"network_guard":guard},sysfs=sysfs)

    def test_hardware_block_does_not_claim_success_or_touch_bluetooth(self):
        sysfs=self.root/"sys";entry=sysfs/"class/rfkill/rfkill0";entry.mkdir(parents=True)
        (entry/"type").write_text("wlan");(entry/"hard").write_text("1");(entry/"soft").write_text("1")
        with patch.object(self.service,"command") as command,self.assertRaisesRegex(ValueError,"blocked by the hardware"):
            self.service.prepare_wifi(sysfs)
        command.assert_not_called();self.assertEqual((entry/"soft").read_text(),"1")

    def test_radio_on_waits_for_adapter_instead_of_trusting_command_exit(self):
        self.service.preview=False
        readings=iter((0,0,1))
        def status():self.service.publish(wifi=next(readings),wifi_radio=1)
        with patch.object(self.service,"prepare_wifi") as prepare,patch.object(self.service,"network_status",side_effect=status),patch("device_services.time.sleep") as sleep:
            self.service.radio(True)
        prepare.assert_called_once();self.assertEqual(sleep.call_count,2)
        self.assertEqual(self.service.fields["message"],"Wi-Fi on.")

    def test_unsuccessful_toggle_never_publishes_success(self):
        self.service.preview=False
        for enabled in (True,False):
            with self.subTest(enabled=enabled):
                self.service.publish(wifi=0,wifi_radio=1,wifi_reason="Wi-Fi adapter is not ready.")
                with patch.object(self.service,"prepare_wifi"),patch.object(self.service,"command"),patch.object(self.service,"network_status"),patch("device_services.time.monotonic",side_effect=[0,9]),self.assertRaises(ValueError):
                    self.service.radio(enabled)
                self.assertNotIn(self.service.fields["message"],("Wi-Fi on.","Wi-Fi off."))

    def test_failed_scan_clears_stale_network_choices_and_preserves_reason(self):
        self.service.scan()
        self.assertGreater(int(self.service.fields["network_count"]),0)
        self.service.preview=False
        replies=["wlan0:wifi:unavailable\n","enabled\n","enabled\n"]
        with patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=replies),patch.object(self.service,'radio',side_effect=ValueError('Wi-Fi adapter is not ready.')):
            with self.assertRaisesRegex(ValueError,'not ready'):self.service.scan()
        self.assertEqual(self.service.fields["network_count"],"0")
        self.assertIn("not ready",self.service.fields["wifi_reason"])
        self.assertEqual(json.loads((self.data/"device/networks.json").read_text()),[])
        self.assertNotIn("network_0_ssid",self.service.fields)

    def test_scan_recovers_enabled_unavailable_adapter_without_reboot(self):
        self.service.preview=False
        def unavailable():
            self.service.publish(wifi=0,wifi_radio=1,interface='wlan0')
            return 'wlan0'
        def recovered(enabled):
            self.assertTrue(enabled)
            self.service.publish(wifi=1,interface='wlan0')
        with patch.object(self.service,'network_status',side_effect=unavailable),patch.object(self.service,'radio',side_effect=recovered) as radio,patch.object(self.service,'command',return_value=':New home:72:WPA2\n'):
            self.service.scan()
        radio.assert_called_once_with(True)
        self.assertEqual(self.service.fields['network_count'],'1')

    def test_scan_never_enables_an_explicitly_disabled_radio(self):
        self.service.preview=False
        self.service.publish(wifi=0,wifi_radio=0,wifi_reason='Turn Wi-Fi on first.')
        with patch.object(self.service,'network_status',return_value='wlan0'),patch.object(self.service,'radio') as radio:
            self.service.scan()
        radio.assert_not_called()
        self.assertEqual(self.service.fields['network_count'],'0')

    def test_stronger_duplicate_ap_does_not_erase_current_connection(self):
        self.service.preview=False
        with patch.object(self.service,'network_status',return_value='wlan0'),patch.object(self.service,'command',return_value='*:Mesh:25:WPA2\n:Mesh:90:WPA2\n'):
            self.service.scan()
        self.assertEqual(self.service.fields['network_0_connected'],'1')
        self.assertEqual(self.service.fields['network_0_signal'],'90')

    def test_connection_verification_uses_actual_ssid_not_profile_label(self):
        self.service.preview=False
        self.service.publish(wifi_connected=1,connection='Renamed profile')
        with patch.object(self.service,'network_status',return_value='wlan0'),patch.object(self.service,'command',return_value=r'*:My\: Wi\\Fi'+'\n'):
            self.service.verify_connection('wlan0','My: Wi\\Fi')
        self.assertEqual(self.service.fields['connection'],'My: Wi\\Fi')

    def test_different_connected_network_never_counts_as_success(self):
        self.service.preview=False
        self.service.publish(wifi_connected=1,connection='Old home')
        with patch.object(self.service,'network_status',return_value='wlan0'),patch.object(self.service,'command',return_value='*:Old home\n'),patch('device_services.time.monotonic',side_effect=[0,9]):
            with self.assertRaisesRegex(ValueError,'confirm'):self.service.verify_connection('wlan0','New home')

    def test_failed_join_refreshes_stale_connection_and_can_be_retried(self):
        self.service.scan()
        key=self.service.fields['network_0_id'];self.service.preview=False
        def disconnected():
            self.service.publish(wifi_connected=0,connection='Not connected',ip='')
            return 'wlan0'
        with patch.object(self.service,'ready_wifi',return_value='wlan0'),patch.object(self.service,'network_status',side_effect=disconnected),patch.object(self.service,'command',side_effect=ValueError('Check password')):
            with self.assertRaisesRegex(ValueError,'password'):self.service.connect(dict(network=key,password='wrong'))
        self.assertEqual(self.service.fields['wifi_connected'],'0')
        self.assertEqual(self.service.fields['ip'],'')
        with patch.object(self.service,'ready_wifi',return_value='wlan0'),patch.object(self.service,'verify_connection') as verify,patch.object(self.service,'command'):
            self.service.connect(dict(network=key,password='correct'))
        verify.assert_called_once_with('wlan0','Preview Wi-Fi')
        self.assertEqual(self.service.fields['message'],'Connected.')

    def test_timeout_never_echoes_captured_secrets(self):
        secret='private passphrase'
        with patch('device_services.subprocess.run',side_effect=subprocess.TimeoutExpired(['nmcli'],10,output=secret,stderr=secret)):
            with self.assertRaisesRegex(ValueError,'timed out') as caught:self.service.command(['radio','wifi'])
        self.assertNotIn(secret,str(caught.exception))

    def test_saved_profile_uses_exact_ssid_not_display_name_or_hotspot(self):
        self.service.preview=False
        one='12345678-1234-1234-1234-123456789abc'
        two='abcdefab-1234-1234-1234-123456789abc'
        replies=[one+':802-11-wireless\n'+two+':802-11-wireless\n',
                 'Home\\: Wi\\\\Fi\ninfrastructure\n', 'Hotspot\nap\n']
        with patch.object(self.service,'command',side_effect=replies):
            self.assertEqual(self.service.saved_wifi(), {'Home: Wi\\Fi':one})

    def test_saved_connection_activates_uuid_without_requesting_or_logging_password(self):
        self.service.scan();key=self.service.fields['network_0_id'];self.service.preview=False
        with patch.object(self.service,'ready_wifi',return_value='wlan0'), \
             patch.object(self.service,'saved_wifi',return_value={'Preview Wi-Fi':'profile-uuid'}), \
             patch.object(self.service,'command') as command,patch.object(self.service,'verify_connection'):
            self.service.connect(dict(network=key,use_saved=True))
        command.assert_called_once_with(['--wait','20','connection','up','uuid','profile-uuid','ifname','wlan0'],timeout=25)

    def test_saved_selection_prefers_successful_replacement_over_old_details(self):
        self.service.preview=False
        one='12345678-1234-1234-1234-123456789abc'
        two='abcdefab-1234-1234-1234-123456789abc'
        replies=[one+':802-11-wireless\n'+two+':802-11-wireless\n',
                 'Home\ninfrastructure\n0\n300\n', 'Home\ninfrastructure\n10\n400\n']
        with patch.object(self.service,'command',side_effect=replies):
            self.assertEqual(self.service.saved_wifi(), {'Home':two})

    def test_new_password_does_not_reuse_or_modify_old_saved_secret(self):
        self.service.scan();key=self.service.fields['network_0_id'];self.service.preview=False
        with patch.object(self.service,'ready_wifi',return_value='wlan0'), \
             patch.object(self.service,'command') as command,patch.object(self.service,'verify_connection'):
            self.service.connect(dict(network=key,password='a new password'))
        call=command.call_args_list[0]
        self.assertEqual(call.kwargs['password'],'a new password')
        self.assertNotIn('a new password',call.args[0])
        self.assertEqual(call.args[0][-2],'name')
        self.assertRegex(call.args[0][-1],r'^RGDS-[0-9a-f]{32}$')
        self.assertNotIn('a new password',(self.data/'device/status.ini').read_text())

    def test_failed_new_password_removes_only_its_candidate_and_keeps_old_profiles(self):
        self.service.scan();key=self.service.fields['network_0_id'];self.service.preview=False
        with patch.object(self.service,'ready_wifi',return_value='wlan0'), \
             patch.object(self.service,'network_status'),patch.object(self.service,'command') as command, \
             patch.object(self.service,'verify_connection',side_effect=ValueError('Wrong password')):
            with self.assertRaisesRegex(ValueError,'Wrong password'):
                self.service.connect(dict(network=key,password='bad password'))
        candidate=command.call_args_list[0].args[0][-1]
        self.assertEqual(command.call_args_list[1].args[0],['connection','delete','id',candidate])
        self.assertEqual(command.call_count,2)

    def test_autoconnect_priority_failure_keeps_verified_connection(self):
        self.service.scan();key=self.service.fields['network_0_id'];self.service.preview=False
        with patch.object(self.service,'ready_wifi',return_value='wlan0'), \
             patch.object(self.service,'command',side_effect=['',ValueError('Unavailable')]) as command, \
             patch.object(self.service,'verify_connection'):
            self.service.connect(dict(network=key,password='a new password'))
        self.assertEqual(self.service.fields['message'],'Connected.')
        self.assertFalse(any('delete' in call.args[0] for call in command.call_args_list))

    def test_worker_returns_error_when_radio_command_succeeds_but_adapter_is_missing(self):
        self.service.preview=False
        self.service.publish(message="Wi-Fi on.",wifi=1,interface="stale0",ip="192.168.1.42")
        with patch.object(sys,"argv",["device_services.py","wifi-on","--app",str(self.root/"app"),"--data",str(self.data)]),patch("device_services.Services",return_value=self.service),patch("device_services.signal.signal"),patch.object(self.service,"prepare_wifi"),patch("device_services.shutil.which",return_value="/usr/bin/nmcli"),patch.object(self.service,"command",side_effect=["p2p0:wifi:unmanaged\n","enabled\n"]),patch("device_services.time.monotonic",side_effect=[0,9]):
            self.assertEqual(main(),1)
        self.assertEqual(self.service.fields["state"],"error")
        self.assertEqual(self.service.fields["wifi"],"0")
        self.assertEqual(self.service.fields["interface"],"")
        self.assertIn("No Wi-Fi adapter",self.service.fields["message"])

    def test_failed_initializer_stops_enable_and_reports_its_error(self):
        sysfs=self.root/"sys";(sysfs/"class/net").mkdir(parents=True)
        with patch("boot_network.capture_helper",return_value={}),patch("boot_network.ensure_boot_network",side_effect=ValueError("The configured Wi-Fi helper has not been reviewed.")),patch.object(self.service,"command") as command,patch.object(self.service,"run_command") as run,self.assertRaisesRegex(ValueError,"has not been reviewed"):
            self.service.prepare_wifi(sysfs)
        command.assert_not_called();run.assert_not_called()

    def test_preview_network_toggle_scan_and_connect_are_persistent_simulations(self):
        self.service.radio(False);self.service.scan()
        self.assertEqual(self.service.fields["network_count"],"0")
        self.service.radio(True);self.service.scan()
        networks=json.loads((self.data/"device/networks.json").read_text())
        key=next(entry["id"] for entry in networks if entry["ssid"]=="Guest network")
        self.service.connect({"network":key,"password":""})
        self.assertEqual(self.service.fields["connection"],"Guest network")
        self.assertNotIn("password",(self.data/"device/status.ini").read_text())

    def test_clear_cache_preserves_games_saves_and_artwork_and_rejects_symlinks(self):
        (self.data/"cache/roms").mkdir(parents=True);(self.data/"cache/roms/game.nds").write_bytes(b"cache")
        save=self.data/"emulators/game.sav";save.parent.mkdir();save.write_bytes(b"save")
        self.service.clear_cache()
        self.assertFalse((self.data/"cache").exists());self.assertEqual(save.read_bytes(),b"save")
        outside=self.root/"outside";outside.mkdir();(self.data/"cache").symlink_to(outside,target_is_directory=True)
        with self.assertRaises(ValueError):self.service.clear_cache()
        self.assertTrue(outside.exists())

    def test_device_report_excludes_connection_names_addresses_and_credentials(self):
        self.service.fields.update(connection="private SSID",password="secret",ip="192.168.1.42")
        self.service.diagnostics()
        report=next((self.data/"support").glob("*.json")).read_text()
        self.assertNotIn("secret",report);self.assertNotIn("private SSID",report);self.assertNotIn("192.168",report)

    def test_game_filter_override_and_ui_extension_are_independent_of_base_preferences(self):
        rom=self.root/"game.nds"
        write_fields(self.data/"preferences.ini",dict(schema=1,mode="dark",font="rounded",core=0))
        write_fields(self.data/"ui.ini",dict(schema=1,filter=2))
        self.assertEqual(display_filter(self.data,rom),2)
        write_fields(self.data/"library"/game_key(rom)/"game.ini",dict(schema=1,core=1,filter=1,favourite=1))
        self.assertEqual(display_filter(self.data,rom),1)
        self.assertEqual(game_options(self.data,rom)["core"],1)


if __name__=="__main__":unittest.main()
