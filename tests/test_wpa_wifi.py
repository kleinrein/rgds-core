"""RG DS OS Wi-Fi: the settings page against a fake wpa_supplicant on a real control socket."""
import binascii
import json
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import wpa_wifi
from device_services import Services

HOME = "aa:aa:aa:aa:aa:01"


class FakeSupplicant:
    """Enough of wpa_supplicant's control interface for the settings page; joins succeed with PASSWORD."""
    PASSWORD = "correct horse"

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(path))
        self.networks = {}  # id -> dict(ssid, psk, disabled, temp)
        self.next_id = 0
        self.current = None
        self.attached = set()
        self.commands = []
        self.saved = 0
        self.joining = None  # a saved network's name: STATUS reports it being joined
        self.scan = (
            f"bssid / frequency / signal level / flags / ssid\n"
            f"{HOME}\t2412\t-45\t[WPA2-PSK-CCMP][ESS]\tHome\\xe2\\x80\\x99s\n"
            "aa:aa:aa:aa:aa:02\t2437\t-80\t[WPA2-PSK-CCMP][ESS]\tHome\\xe2\\x80\\x99s\n"
            "aa:aa:aa:aa:aa:03\t5180\t-60\t[ESS]\tCafe\n"
            "aa:aa:aa:aa:aa:04\t5180\t-70\t[WPA2-SAE-CCMP][ESS]\tNew\n"
        )
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                data, client = self.sock.recvfrom(4096)
            except OSError:
                return
            command = data.decode()
            self.commands.append(command)
            reply = self.handle(command, client)
            self.sock.sendto(reply.encode(), client)
            if command == "SCAN":
                for other in self.attached:
                    self.sock.sendto(b"<2>CTRL-EVENT-SCAN-RESULTS ", other)

    def handle(self, command, client):
        words = command.split(" ", 3)
        if command == "ATTACH":
            self.attached.add(client)
        elif command == "DETACH":
            self.attached.discard(client)
        elif command == "SCAN":
            pass
        elif command == "SCAN_RESULTS":
            return self.scan
        elif command == "STATUS":
            if self.joining: return f"bssid={HOME}\nssid={self.joining}\nwpa_state=4WAY_HANDSHAKE\n"
            if self.current is None: return "wpa_state=SCANNING\n"
            ssid = self.networks[self.current]["ssid"]
            return f"bssid={HOME}\nssid={ssid}\nwpa_state=COMPLETED\nip_address=192.168.68.50\n"
        elif command == "SIGNAL_POLL":
            return "RSSI=-45\nLINKSPEED=72\n"
        elif command == "LIST_NETWORKS":
            rows = ["network id / ssid / bssid / flags"]
            for key, net in self.networks.items():
                flags = "[CURRENT]" if key == self.current else "[TEMP-DISABLED]" if net["temp"] else ""
                rows.append(f"{key}\t{net['ssid']}\tany\t{flags}")
            return "\n".join(rows) + "\n"
        elif command == "ADD_NETWORK":
            self.networks[self.next_id] = dict(ssid="", psk=None, temp=False)
            self.next_id += 1
            return f"{self.next_id - 1}\n"
        elif words[0] == "SET_NETWORK":
            net = self.networks[int(words[1])]
            if words[2] == "ssid":
                net["ssid"] = binascii.unhexlify(words[3]).decode().replace("’", "\\xe2\\x80\\x99")
            elif words[2] == "psk":
                net["psk"] = words[3][1:-1]
        elif words[0] == "SELECT_NETWORK":
            net = self.networks[int(words[1])]
            if net["psk"] == self.PASSWORD or net["psk"] is None and net["ssid"] == "Cafe":
                self.current = int(words[1])
            else:
                net["temp"], self.current = True, None
        elif words[0] == "REMOVE_NETWORK":
            self.networks.pop(int(words[1]))
        elif command in ("ENABLE_NETWORK all", "SAVE_CONFIG"):
            self.saved += command == "SAVE_CONFIG"
        else:
            return "UNKNOWN COMMAND\n"
        return "OK\n"

    def close(self):
        self.sock.close()


class WpaWifiTests(unittest.TestCase):
    def setUp(self):
        # AF_UNIX paths are short; the macOS temp folder is too long for them.
        self.root = Path(tempfile.mkdtemp(prefix="wpa", dir="/tmp"))
        self.addCleanup(shutil.rmtree, self.root)
        self.sockets = self.root / "s"
        self.sockets.mkdir()
        self.supplicant = FakeSupplicant(self.sockets / "wlan0")
        self.addCleanup(self.supplicant.close)
        data = self.root / "userdata"
        data.mkdir()
        with patch("device_services.shutil.which", return_value=None), \
             patch("wpa_wifi.supported", return_value=True):
            self.service = Services(self.root / "app", data, preview=False)
        self.service.wpa = wpa_wifi.WpaWifi(
            self.service,
            sockets=self.sockets,
            off=self.root / "wifi-off",
            control=lambda device, sockets: wpa_wifi.Control(device, sockets, client_dir=self.root)
        )

    def networks(self):
        return json.loads((self.service.data / "device/networks.json").read_text())

    def test_printf_encoded_names_decode_to_their_text(self):
        self.assertEqual(wpa_wifi.decode(r"Home\xe2\x80\x99s \"5G\" \\ x"), 'Home’s "5G" \\ x')

    def test_scan_lists_each_name_once_strongest_first_with_its_security(self):
        self.service.scan()
        self.assertEqual([n["ssid"] for n in self.networks()], ["Home’s", "Cafe", "New"])
        self.assertEqual(self.networks()[0]["signal"], 100)
        self.assertEqual(self.networks()[1]["security"], "")
        self.assertEqual(self.networks()[2]["security"], "WPA3")
        self.assertIn("ATTACH", self.supplicant.commands)

    def test_join_with_password_saves_it_and_reports_the_address(self):
        self.service.scan()
        home = self.networks()[0]["id"]
        self.service.connect(dict(network=home, password=FakeSupplicant.PASSWORD))
        self.assertEqual(self.service.fields["message"], "Connected.")
        self.assertEqual(self.service.fields["connection"], "Home’s")
        self.assertEqual(self.service.fields["ip"], "192.168.68.50")
        self.assertEqual(self.service.fields["wifi_signal"], "100")
        self.assertGreaterEqual(self.supplicant.saved, 1)
        self.assertEqual(self.service.saved_wifi(), {"Home’s": "0"})

    def test_wrong_password_leaves_saved_networks_untouched(self):
        self.service.scan()
        home = self.networks()[0]["id"]
        self.service.connect(dict(network=home, password=FakeSupplicant.PASSWORD))
        with self.assertRaisesRegex(ValueError, "Wrong password"):
            self.service.connect(dict(network=home, password="not the one"))
        self.assertEqual(list(self.supplicant.networks), [0])
        self.assertIn("ENABLE_NETWORK all", self.supplicant.commands[-4:])

    def test_new_password_replaces_the_old_entry_only_after_it_works(self):
        self.service.scan()
        home = self.networks()[0]["id"]
        self.service.connect(dict(network=home, password=FakeSupplicant.PASSWORD))
        self.supplicant.current = None
        self.service.connect(dict(network=home, password=FakeSupplicant.PASSWORD))
        self.assertEqual(list(self.supplicant.networks), [1])

    def test_password_never_appears_in_status_or_errors(self):
        self.service.scan()
        home = self.networks()[0]["id"]
        secret = "secret-passphrase-9"
        with self.assertRaises(ValueError) as caught:
            self.service.connect(dict(network=home, password=secret))
        self.assertNotIn(secret, str(caught.exception))
        self.assertNotIn(secret, self.service.status_file.read_text())

    def test_open_network_joins_without_a_key(self):
        self.service.scan()
        cafe = next(n["id"] for n in self.networks() if n["ssid"] == "Cafe")
        self.service.connect(dict(network=cafe, password=""))
        self.assertIn("SET_NETWORK 0 key_mgmt NONE", self.supplicant.commands)
        self.assertEqual(self.service.fields["connection"], "Cafe")

    def test_saved_network_joins_without_asking_again(self):
        self.service.scan()
        home = self.networks()[0]["id"]
        self.service.connect(dict(network=home, password=FakeSupplicant.PASSWORD))
        self.supplicant.current = None
        self.service.connect(dict(network=home, password="", use_saved=True))
        self.assertEqual(self.service.fields["connection"], "Home’s")
        self.assertEqual(self.supplicant.commands.count("ADD_NETWORK"), 1)

    def test_off_switch_and_missing_service_report_the_right_reason(self):
        (self.root / "wifi-off").touch()
        self.service.network_status()
        self.assertEqual(self.service.fields["wifi_radio"], "0")
        self.assertIn("Turn Wi-Fi on", self.service.fields["wifi_reason"])
        (self.root / "wifi-off").unlink()
        self.supplicant.close()
        (self.sockets / "wlan0").unlink()
        self.service.network_status()
        self.assertEqual(self.service.fields["wifi_radio"], "1")
        self.assertEqual(self.service.fields["wifi"], "0")

    def test_joining_and_looking_by_itself_are_reported(self):
        # no saved network: looking for one is not worth a spinner
        self.service.network_status()
        self.assertEqual((self.service.fields["wifi_looking"], self.service.fields["wifi_joining"]), ("0", ""))
        self.supplicant.networks[0] = dict(ssid="Home", psk="x", temp=False)
        self.service.network_status()
        self.assertEqual(self.service.fields["wifi_looking"], "1")
        self.assertEqual(self.service.fields["connection"], "Not connected")
        self.supplicant.joining = "Home"
        self.service.network_status()
        self.assertEqual((self.service.fields["wifi_joining"], self.service.fields["wifi_looking"]), ("Home", "0"))
        self.assertEqual(self.service.fields["wifi_connected"], "0")

    def test_radio_switch_runs_the_os_script(self):
        with patch.object(self.service, "run_command") as run:
            self.service.wpa.stop()
            self.service.wpa.start()
        self.assertEqual([c.args[0][1] for c in run.call_args_list], ["wifi-off", "wifi"])


if __name__ == "__main__":
    unittest.main()
