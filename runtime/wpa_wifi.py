"""Wi-Fi on RG DS OS: wpa_supplicant without NetworkManager (os/buildroot, docs/OWN_OS_PLAN.md).

The settings page talks to wpa_supplicant's control socket, like wpa_cli does, so a password only ever travels
over that socket and never through argv or a log. Networks are saved by wpa_supplicant itself into
data/rgds/wifi.conf on the card. The OS script /usr/libexec/rgds/net starts and stops it, with an address from
udhcpc whenever a network is joined.
"""
import binascii
import itertools
import os
from pathlib import Path
import socket
import time

HELPER = Path("/usr/libexec/rgds/net")
SOCKETS = Path("/run/wpa_supplicant")
OFF = Path("/data/rgds/wifi-off")
_clients = itertools.count()


def supported():
    return HELPER.is_file()


def decode(text):
    """wpa_supplicant's printf_encode: \\xNN, \\e, \\n, \\r, \\t, \\\\ and \\\" around raw UTF-8 bytes."""
    out, chars = bytearray(), iter(text)
    for char in chars:
        if char != "\\":
            out += char.encode()
            continue
        escape = next(chars, "")
        if escape == "x":
            digits = next(chars, "") + next(chars, "")
            try:
                out.append(int(digits, 16))
            except ValueError:
                out += ("\\x" + digits).encode()
        else:
            out += {"e": b"\x1b", "n": b"\n", "r": b"\r", "t": b"\t"}.get(escape, escape.encode())
    return out.decode("utf-8", "replace")


def percent(dbm):
    return max(0, min(100, 2 * (dbm + 100)))


def security(flags):
    if "SAE" in flags: return "WPA3"
    if "WPA2" in flags or "RSN" in flags: return "WPA2"
    if "WPA" in flags: return "WPA"
    if "WEP" in flags: return "WEP"
    return ""


class Control:
    """One wpa_supplicant control connection (a datagram socket pair, as in wpa_ctrl.c)."""
    def __init__(self, interface, sockets=SOCKETS, client_dir=Path("/run/rgds")):
        self.local = Path(client_dir) / f"wpa-{os.getpid()}-{next(_clients)}"
        self.local.unlink(missing_ok=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.sock.bind(str(self.local))
            self.sock.connect(str(Path(sockets) / interface))
        except OSError:
            self.close()
            raise ValueError("Wi-Fi service is not running. Turn Wi-Fi on to retry.") from None
        self.events = []

    def close(self):
        self.sock.close()
        self.local.unlink(missing_ok=True)

    def request(self, command, timeout=5):
        self.sock.settimeout(timeout)
        try:
            self.sock.send(command.encode())
            while True:
                reply = self.sock.recv(16384).decode("utf-8", "replace")
                # Attached connections also get unsolicited "<level>EVENT" messages.
                if reply.startswith("<"): self.events.append(reply)
                else: return reply
        except OSError:
            raise ValueError("Wi-Fi service did not answer. Try again.") from None

    def ok(self, command):
        if self.request(command).strip() != "OK":
            # The command may carry a password; never echo it.
            raise ValueError("Wi-Fi request failed. Check the password and signal, then try again.")

    def wait_event(self, name, timeout):
        deadline = time.monotonic() + timeout
        while not any(name in event for event in self.events):
            left = deadline - time.monotonic()
            if left <= 0: return False
            self.sock.settimeout(left)
            try:
                self.events.append(self.sock.recv(16384).decode("utf-8", "replace"))
            except OSError:
                return False
        return True


class WpaWifi:
    def __init__(self, service, sockets=SOCKETS, off=OFF, control=Control):
        self.service, self.sockets, self.off, self.control = service, Path(sockets), Path(off), control

    def interface(self):
        names = sorted(p.name for p in self.sockets.glob("*") if not p.name.startswith("p2p")) \
            if self.sockets.is_dir() else []
        return names[0] if names else None

    def open(self, device=None):
        device = device or self.interface()
        if not device: raise ValueError("Wi-Fi service is not running. Turn Wi-Fi on to retry.")
        return self.control(device, self.sockets)

    @staticmethod
    def fields(reply):
        return dict(line.partition("=")[::2] for line in reply.splitlines() if "=" in line)

    def network_status(self):
        radio = not self.off.exists()
        device = self.interface() if radio else None
        status = {}
        if device:
            try:
                ctrl = self.open(device)
            except ValueError:
                device = None
            else:
                try:
                    status = self.fields(ctrl.request("STATUS"))
                    signal = self.fields(ctrl.request("SIGNAL_POLL")) if status.get("wpa_state") == "COMPLETED" else {}
                finally:
                    ctrl.close()
        if not device:
            self.service.publish(
                wifi_supported=1,
                wifi=0,
                wifi_radio=int(radio),
                wifi_reason="Turn Wi-Fi on to find networks."
                if not radio else "Wi-Fi adapter is not ready. Turn Wi-Fi on to retry.",
                interface="",
                connection="Not connected",
                ip="",
                wifi_signal=-1,
                wifi_connected=0
            )
            return None
        connected = status.get("wpa_state") == "COMPLETED"
        try:
            strength = percent(int(signal.get("RSSI", ""))) if connected else -1
        except ValueError:
            strength = -1
        self.service.publish(
            wifi_supported=1,
            wifi=1,
            wifi_radio=1,
            wifi_reason="",
            wifi_signal=strength,
            wifi_connected=int(connected),
            connection=decode(status.get("ssid", "")) if connected else "Not connected",
            ip=status.get("ip_address", "") if connected else "",
            interface=device
        )
        return device

    def saved(self):
        """{ssid: network id}; the newest entry wins when one SSID was saved twice."""
        ctrl = self.open()
        try:
            rows = ctrl.request("LIST_NETWORKS").splitlines()[1:]
        finally:
            ctrl.close()
        result = {}
        for line in rows:
            row = line.split("\t")
            if len(row) >= 2 and row[0].isdigit() and row[1]: result[decode(row[1])] = row[0]
        return result

    def scan(self, device):
        """Rows in the nmcli layout device_services.scan reads: in-use, SSID, signal, security."""
        ctrl = self.open(device)
        try:
            ctrl.ok("ATTACH")
            if ctrl.request("SCAN").strip() not in ("OK", "FAIL-BUSY"):
                raise ValueError("Could not scan for networks. Try again.")
            ctrl.wait_event("CTRL-EVENT-SCAN-RESULTS", 10)
            ctrl.request("DETACH")
            current = self.fields(ctrl.request("STATUS"))
            rows = ctrl.request("SCAN_RESULTS").splitlines()[1:]
        finally:
            ctrl.close()
        joined = current.get("bssid") if current.get("wpa_state") == "COMPLETED" else None
        result = []
        for line in rows:
            row = line.split("\t")
            if len(row) != 5 or not row[4]: continue
            try:
                strength = percent(int(row[2]))
            except ValueError:
                continue
            result.append(["*" if row[0] == joined else "", decode(row[4]), str(strength), security(row[3]) or "--"])
        return result

    def connect(self, device, ssid, password, use_saved):
        """Join ssid with an address, keeping every saved network unless this join is confirmed."""
        ctrl = self.open(device)
        candidate = None
        try:
            if use_saved:
                network = self.saved().get(ssid)
                if not network: raise ValueError('No saved details for this network. Choose Enter password.')
            else:
                flags = next((row[3] for row in self.scan_rows(ctrl) if decode(row[4]) == ssid), "")
                if "WEP" in flags: raise ValueError("WEP networks are not supported. Choose another network.")
                candidate = network = ctrl.request("ADD_NETWORK").strip()
                if not network.isdigit(): raise ValueError("Wi-Fi request failed. Try again.")
                ctrl.ok(f"SET_NETWORK {network} ssid {binascii.hexlify(ssid.encode()).decode()}")
                if not password:
                    ctrl.ok(f"SET_NETWORK {network} key_mgmt NONE")
                else:
                    if not 8 <= len(password.encode()) <= 63:
                        raise ValueError("Wi-Fi passwords are 8 to 63 characters. Check it and try again.")
                    if "SAE" in flags:
                        ctrl.ok(f"SET_NETWORK {network} key_mgmt SAE WPA-PSK")
                        ctrl.ok(f"SET_NETWORK {network} ieee80211w 1")
                    ctrl.ok(f'SET_NETWORK {network} psk "{password}"')
                ctrl.ok(f"SET_NETWORK {network} priority 10")
            # SELECT_NETWORK disables every other network until ENABLE_NETWORK all.
            ctrl.ok(f"SELECT_NETWORK {network}")
            deadline = time.monotonic() + 20
            while True:
                status = self.fields(ctrl.request("STATUS"))
                if status.get("wpa_state") == "COMPLETED" and decode(status.get("ssid", "")) == ssid \
                        and status.get("ip_address"):
                    break
                listed = ctrl.request("LIST_NETWORKS")
                if any(line.split("\t")[0] == network and "TEMP-DISABLED" in line for line in listed.splitlines()):
                    raise ValueError(
                        "Wrong password. Check it and try again." if candidate else
                        'Saved connection failed. Choose Enter password to update it, or scan and retry.'
                    )
                if time.monotonic() >= deadline:
                    raise ValueError("Could not join the network. Check the password and signal, then try again.")
                time.sleep(.25)
            if candidate:
                # The new details replace older saved ones for the same name.
                for line in ctrl.request("LIST_NETWORKS").splitlines()[1:]:
                    row = line.split("\t")
                    if row[0] != candidate and len(row) >= 2 and decode(row[1]) == ssid:
                        ctrl.request(f"REMOVE_NETWORK {row[0]}")
            candidate = None
        except BaseException:
            if candidate:
                # Only this attempt's network goes; every saved one stays.
                ctrl.request(f"REMOVE_NETWORK {candidate}")
            raise
        finally:
            try:
                ctrl.request("ENABLE_NETWORK all")
                ctrl.request("SAVE_CONFIG")
            finally:
                ctrl.close()

    @staticmethod
    def scan_rows(ctrl):
        return [
            row for row in (line.split("\t") for line in ctrl.request("SCAN_RESULTS").splitlines()[1:]) if len(row) == 5
        ]

    def start(self):
        self.service.run_command([str(HELPER), "wifi"], failure="Could not start Wi-Fi. Restart the handheld.")

    def stop(self):
        self.service.run_command([str(HELPER), "wifi-off"], failure="Could not stop Wi-Fi. Try again.")
