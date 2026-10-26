"""NetworkManager Wi-Fi and local maintenance for the RGDS settings screens."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

sys.dont_write_bytecode = True
from library import read_fields, write_fields


def split_terse(line):
    values, value, escaped = [], "", False
    for char in line:
        if escaped:
            value += char
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == ":":
            values.append(value)
            value = ""
        else:
            value += char
    values.append(value)
    return values


class Services:
    def __init__(self, app, data, preview=False):
        self.app, self.data, self.preview = Path(app), Path(data), preview
        self.status_file = self.data / "device/status.ini"
        self.fields = read_fields(self.status_file)

    def publish(self, **fields):
        self.fields.update({key: str(value) for key, value in fields.items()})
        write_fields(self.status_file, self.fields)

    def run_command(
        self,
        args,
        password=None,
        timeout=10,
        failure="Wi-Fi request failed. Check the password and signal, then try again."
    ):
        environment = os.environ.copy()
        environment.update(LC_ALL="C", NO_COLOR="1", PATH="/usr/sbin:/usr/bin:/sbin:/bin")
        environment.pop("LD_PRELOAD", None)
        environment.pop("LD_LIBRARY_PATH", None)
        for key in tuple(environment):
            if key.startswith("RGDS_"): environment.pop(key, None)
        try:
            result = subprocess.run(
                args,
                text=True,
                input=(password + "\n") if password is not None else None,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
                timeout=timeout
            )
        except subprocess.TimeoutExpired:
            # A timeout exception can contain captured prompts/secrets. Keep it
            # out of UI status and let the user retry without restarting RGDS.
            raise ValueError("Wi-Fi request timed out. Check signal and try again.") from None
        if result.returncode:
            # nmcli can echo its input/error context. Never surface it or credentials.
            raise ValueError(failure)
        return result.stdout

    def command(self, args, password=None, timeout=10):
        return self.run_command(["nmcli", "--colors", "no", *args], password, timeout)

    def clear_networks(self):
        for key in list(self.fields):
            if key.startswith("network_"): self.fields.pop(key)
        self.publish(network_count=0)
        (self.data / "device/networks.json").write_text("[]")
        write_fields(self.data / "device/networks.ini", {})

    def network_status(self):
        if self.preview:
            enabled = self.fields.get("wifi", "1") != "0"
            self.publish(
                wifi_supported=1,
                wifi=int(enabled),
                wifi_radio=int(enabled),
                wifi_reason="" if enabled else "Turn Wi-Fi on to find networks.",
                connection=self.fields.get("connection", "Preview Wi-Fi") if enabled else "Not connected",
                ip="192.168.1.42" if enabled else "",
                wifi_signal=90 if enabled else -1,
                wifi_connected=int(enabled)
            )
            return "wlan0"
        if not shutil.which("nmcli"):
            self.publish(
                wifi_supported=0,
                wifi=0,
                wifi_radio=0,
                wifi_reason="Wi-Fi service is unavailable.",
                interface="",
                connection="Wi-Fi unavailable",
                ip="",
                wifi_signal=-1,
                wifi_connected=0
            )
            return None
        devices = [split_terse(line) for line in self.command(["-t", "-f", "DEVICE,TYPE,STATE", "device"]).splitlines()]
        # The vendor also exposes p2p0 as type wifi. It cannot scan/join as a station.
        stations = [row for row in devices if len(row) >= 3 and row[1] == "wifi" and not row[0].startswith("p2p")]
        stations.sort(
            key=lambda row: (row[2] in ("unmanaged", "unavailable", "unknown"), not row[2].startswith("connected"))
        )
        device = stations[0][0] if stations else None
        radio = self.command(["radio", "wifi"]).strip() == "enabled"
        if not device:
            self.publish(
                wifi_supported=0,
                wifi=0,
                wifi_radio=int(radio),
                wifi_reason="No Wi-Fi adapter. Turn Wi-Fi on to initialize it.",
                interface="",
                connection="No Wi-Fi adapter",
                ip="",
                wifi_signal=-1,
                wifi_connected=0
            )
            return None
        networking = self.command(["networking"]).strip() == "enabled"
        state = stations[0][2]
        usable = radio and networking and state not in ("unavailable", "unmanaged", "unknown")
        reason = (
            "Turn Wi-Fi on to find networks." if not radio else "Networking is disabled. Turn Wi-Fi on to enable it."
            if not networking else "Wi-Fi adapter is not ready. Turn Wi-Fi on to retry." if not usable else ""
        )
        connected = usable and state.startswith("connected")
        name = self.command(["-g", "GENERAL.CONNECTION", "device", "show", device]).strip() if connected else ""
        ip = self.command(["-g", "IP4.ADDRESS", "device", "show", device]).splitlines() if connected else []
        strength = -1
        if connected:
            try:
                rows = self.command(
                    ["-t", "-f", "IN-USE,SSID,SIGNAL", "device", "wifi", "list", "ifname", device, "--rescan", "no"]
                ).splitlines()
                for line in rows:
                    row = split_terse(line)
                    if len(row) == 3 and row[0] == "*":
                        name = row[1] or name
                        strength = max(0, min(100, int(row[2])))
                        break
            except (ValueError, OSError, subprocess.SubprocessError):
                pass
        self.publish(
            wifi_supported=1,
            wifi=int(usable),
            wifi_radio=int(radio),
            wifi_reason=reason,
            wifi_signal=strength,
            wifi_connected=int(connected),
            connection=name if name and name != "--" else "Not connected",
            ip=ip[0].split("/")[0] if ip else "",
            interface=device
        )
        return device

    def ready_wifi(self):
        """Recover an enabled but unavailable station without resetting its PHY."""
        device = self.network_status()
        if (not device or self.fields.get('wifi') == '0') and self.fields.get('wifi_radio') == '1':
            self.radio(True)
            device = self.fields.get('interface') or None
        return device

    def saved_wifi(self):
        """Identify station profiles by SSID and UUID, never by their display name."""
        if self.preview: return {}
        rows = self.command(['-t', '-f', 'UUID,TYPE', 'connection', 'show']).splitlines()
        result, ranks = {}, {}
        for line in rows:
            row = split_terse(line)
            if len(row) != 2 or row[1] != '802-11-wireless' or not re.fullmatch(r'[0-9a-fA-F-]{36}', row[0]): continue
            try:
                values = self.command(
                    [
                        '-g',
                        '802-11-wireless.ssid,802-11-wireless.mode,connection.autoconnect-priority,connection.timestamp',
                        'connection', 'show', 'uuid', row[0]
                    ]
                ).splitlines()
            except ValueError:
                continue  # A profile may disappear during a scan.
            if len(values) < 2 or values[1] not in ('infrastructure', '--', ''): continue
            ssid = split_terse(values[0])
            if len(ssid) != 1 or not ssid[0]: continue
            try:
                rank = (int(values[2]), int(values[3])) if len(values) >= 4 else (0, 0)
            except ValueError:
                rank = (0, 0)
            if ssid[0] not in ranks or rank > ranks[ssid[0]]:
                result[ssid[0]], ranks[ssid[0]] = row[0], rank
        return result

    def scan(self):
        self.publish(state="busy", message="Looking for networks…")
        self.clear_networks()
        device = self.ready_wifi()
        if not device:
            raise ValueError(self.fields["wifi_reason"])
        if self.fields.get("wifi") == "0":
            self.publish(state="ready", network_count=0, message=self.fields["wifi_reason"])
            return
        if self.preview:
            rows = [
                ["*", "Preview Wi-Fi", "90", "WPA2"], ["", "Guest network", "65", ""],
                ["", "Library Wi-Fi", "40", "WPA2"]
            ]
        else:
            rows = [
                split_terse(line) for line in self.command(
                    [
                        "-t", "--escape", "yes", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                        "ifname", device, "--rescan", "yes"
                    ],
                    timeout=15
                ).splitlines()
            ]
        saved = self.saved_wifi()
        networks = {}
        for row in rows:
            if len(row) != 4 or not row[1]: continue
            try:
                strength = max(0, min(100, int(row[2])))
            except ValueError:
                continue
            key = hashlib.sha256(row[1].encode()).hexdigest()[:16]
            entry = dict(
                id=key,
                ssid=row[1],
                signal=strength,
                security="" if row[3] == "--" else row[3],
                connected=row[0] == "*",
                saved=row[1] in saved
            )
            if key in networks:
                # One SSID may have multiple APs. A stronger neighbour must not
                # erase the connected mark from the AP we're actually using.
                entry['connected'] = entry['connected'] or networks[key]['connected']
            if key not in networks or networks[key]["signal"] < strength: networks[key] = entry
            else: networks[key]['connected'] = entry['connected']
        ordered = sorted(networks.values(), key=lambda entry: (not entry["connected"], -entry["signal"], entry["ssid"]))
        write_fields(self.data / "device/networks.ini", {f"{entry['id']}.ssid": entry["ssid"] for entry in ordered})
        (self.data / "device/networks.json").write_text(json.dumps(ordered))
        for key in list(self.fields):
            if key.startswith("network_"): self.fields.pop(key)
        for index, entry in enumerate(ordered):
            for key, value in entry.items():
                self.fields[f"network_{index}_{key}"] = str(int(value)) if isinstance(value, bool) else str(value)
        self.publish(
            state="ready",
            message="" if ordered else "No networks found. Try scanning again.",
            network_count=len(ordered)
        )

    def connect(self, payload):
        if not isinstance(payload, dict): raise ValueError("Select a network again.")
        key, password = payload.get("network", ""), payload.get("password", "")
        use_saved = payload.get('use_saved', False)
        if type(use_saved) is not bool: raise ValueError('Select a network again.')
        try:
            ssid = next(
                (
                    entry["ssid"]
                    for entry in json.loads((self.data / "device/networks.json").read_text()) if entry["id"] == key
                ), None
            )
        except (OSError, ValueError):
            ssid = None
        if not isinstance(password, str) or any(c in password
                                                for c in ('\n', '\r', '\0')) or len(password) > 128 or not ssid:
            raise ValueError("Select a network again and enter its password.")
        if self.fields.get('sharing') == '1':
            self.sharing(False)
        device = self.ready_wifi()
        if not device: raise ValueError("Wi-Fi is unavailable.")
        if self.fields.get('wifi') == '0': raise ValueError(self.fields.get('wifi_reason') or "Turn Wi-Fi on first.")
        self.publish(state="busy", message="Connecting…")
        if not self.preview:
            # This is an interactive request from the settings UI. The missing
            # secret is answered on stdin, never included in argv or our logs.
            candidate = None
            try:
                if use_saved:
                    profile = self.saved_wifi().get(ssid)
                    if not profile: raise ValueError('No saved details for this network. Choose Enter password.')
                    self.command(['--wait', '20', 'connection', 'up', 'uuid', profile, 'ifname', device], timeout=25)
                else:
                    args = ["--ask", "--wait", "20", "device", "wifi", "connect", ssid, "ifname", device]
                    if password:
                        # nmcli --ask silently keeps an existing profile's secret.
                        # A unique candidate makes it request the entered password
                        # without modifying a previously working profile.
                        candidate = 'RGDS-' + uuid.uuid4().hex
                        args += ['name', candidate]
                    self.command(args, password=password, timeout=25)
                self.verify_connection(device, ssid)
            except (ValueError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
                if candidate:
                    # Only this operation's random profile name is eligible for
                    # cleanup. Existing saved profiles are never deleted.
                    try:
                        self.command(['connection', 'delete', 'id', candidate])
                    except (ValueError, OSError, subprocess.SubprocessError):
                        pass
                # Refresh stale "connected"/IP fields even when activation
                # fails; do not claim the previously displayed network survived.
                try:
                    self.network_status()
                except (ValueError, OSError, subprocess.SubprocessError):
                    self.publish(wifi_connected=0, wifi_signal=-1, connection="Not connected", ip="")
                if use_saved and not isinstance(error, KeyboardInterrupt):
                    raise ValueError(
                        'Saved connection failed. Choose Enter password to update it, or scan and retry.'
                    ) from None
                raise
            if candidate:
                # Preference failure must not tear down a verified connection.
                try:
                    self.command(
                        [
                            'connection', 'modify', 'id', candidate, 'connection.autoconnect', 'yes',
                            'connection.autoconnect-priority', '10'
                        ]
                    )
                except (ValueError, OSError, subprocess.SubprocessError):
                    pass
        else:
            self.network_status()
        self.publish(state="ready", message="Connected.", **({"connection": ssid} if self.preview else {}))

    def verify_connection(self, device, ssid):
        """Check the active AP, not a profile name or an nmcli exit status."""
        deadline = time.monotonic() + 8
        while True:
            actual = self.network_status()
            rows = self.command(
                [
                    '-t', '--escape', 'yes', '-f', 'IN-USE,SSID', 'device', 'wifi', 'list', 'ifname', device,
                    '--rescan', 'no'
                ]
            ).splitlines()
            matched = any(split_terse(row) == ['*', ssid] for row in rows)
            if actual == device and self.fields.get('wifi_connected') == '1' and matched:
                self.publish(connection=ssid)
                return
            if time.monotonic() >= deadline:
                raise ValueError("Could not confirm the selected Wi-Fi connection. Scan and try again.")
            time.sleep(.25)

    def prepare_wifi(self, sysfs=Path("/sys")):
        from boot_network import capture_helper, ensure_boot_network, wireless_state
        # Stock Bluetooth initialization can remove /dev/rfkill. The kernel's
        # WLAN-only sysfs controls still work, without touching Bluetooth.
        rfkill = Path(sysfs) / "class/rfkill"
        for entry in sorted(rfkill.glob("rfkill*")):
            if (entry / "type").read_text().strip() != "wlan": continue
            if (entry / "hard").read_text().strip() == "1":
                raise ValueError("Wi-Fi is blocked by the hardware. Restart the handheld and try again.")
            if (entry / "soft").read_text().strip() == "1":
                (entry / "soft").write_text("0\n")
        interfaces, _phy = wireless_state(sysfs)
        if not interfaces:
            from linux_platform import independent
            if independent():
                # Kernel/NetworkManager own community-board initialization.
                # Never run a vendor helper or reset a PHY from this OS.
                self.command(["networking", "on"])
                self.command(["radio", "wifi", "on"])
                deadline = time.monotonic() + 2
                while not interfaces and time.monotonic() < deadline:
                    time.sleep(.1)
                    interfaces, _phy = wireless_state(sysfs)
                if not interfaces:
                    raise ValueError("Wi-Fi adapter is not ready. Check the OS driver or restart the handheld.")
            else:
                # Also works for APPS launches without a direct-boot configuration.
                # The helper is hash-checked against the reviewed vendor version.
                interfaces = ensure_boot_network({"network_guard": capture_helper()}, sysfs=sysfs)["interfaces"]
        self.command(["networking", "on"])
        self.command(["radio", "wifi", "on"])
        for device in interfaces:
            self.command(["device", "set", device, "managed", "yes"])
            self.run_command(
                ["ip", "link", "set", "dev", device, "up"], failure="Could not bring the Wi-Fi adapter up. Try again."
            )

    def radio(self, enabled):
        self.publish(state="busy", message="Turning Wi-Fi on…" if enabled else "Turning Wi-Fi off…")
        if not enabled:
            self.sharing(False)
        if self.preview:
            self.publish(wifi=int(enabled), connection="Preview Wi-Fi" if enabled else "Not connected")
        elif enabled:
            self.prepare_wifi()
        else:
            self.command(["radio", "wifi", "off"])
        deadline = time.monotonic() + 8
        while True:
            self.network_status()
            if (enabled and self.fields.get("wifi") == "1") or (not enabled and self.fields.get("wifi_radio") == "0"):
                break
            if time.monotonic() >= deadline:
                raise ValueError(
                    (self.fields.get("wifi_reason") or "Wi-Fi did not turn on. Try again."
                     ) if enabled else "Wi-Fi is still on. Try turning it off again."
                )
            time.sleep(.25)
        if not enabled: self.clear_networks()
        self.publish(state="ready", message="Wi-Fi on." if enabled else "Wi-Fi off.")

    def sharing_status(self):
        if self.preview:
            enabled = self.fields.get('sharing') == '1' and self.fields.get('wifi_connected') == '1'
            self.publish(
                sharing_supported=1,
                sharing=int(enabled),
                sharing_mode=self.fields.get('sharing_mode', 'password') if enabled else '',
                sharing_name='RG DS' if enabled else '',
                sharing_address='smb://' + self.fields.get('ip', '') if enabled else '',
                sharing_login=str(self.data / 'device/sharing/login.ini') if enabled else ''
            )
        else:
            from file_sharing import Sharing
            self.publish(
                **Sharing(self.data, self.app
                          ).status(self.fields.get('ip', '') if self.fields.get('wifi_connected') == '1' else '')
            )

    def sharing(self, enabled, guest=False):
        from file_sharing import Sharing, preview
        if enabled:
            self.network_status()
            if self.fields.get('wifi_connected') != '1' or not self.fields.get('ip'):
                raise ValueError('Connect to Wi-Fi before turning file sharing on.')
        self.publish(state='busy', message='Starting file sharing…' if enabled else 'Stopping file sharing…')
        if self.preview:
            preview(self.data, enabled)
            self.publish(sharing=int(enabled), sharing_mode=('open' if guest else 'password') if enabled else '')
        elif enabled:
            from session import rom_roots, system_roots
            from linux_platform import independent
            roots = [Path('/userdata/roms/nds'), Path('/userdata/roms/dsi')
                     ] if independent() else rom_roots(self.data.parent)
            # Other systems' folders follow Settings > Library > Show all systems,
            # like the launcher's own scan.
            systems = []
            if read_fields(self.data / 'ui.ini').get('show_systems') == '1' and not independent():
                systems = system_roots(self.data.parent)
                # PortMaster's ports, so a computer can bring a port's game files in its own file manager.
                from portmaster import Folders, installed
                if installed(): systems.append(('ports', Folders().ports))
            Sharing(self.data, self.app).start(self.fields['ip'], roots, guest=guest, systems=systems)
        else:
            Sharing(self.data, self.app).stop()
        self.sharing_status()
        self.publish(
            state='ready',
            message=('File sharing on. Find "RG DS" under Network.' if guest else 'File sharing on.')
            if enabled else 'File sharing off.'
        )

    def info(self):
        try:
            manifest = json.loads((self.app / "manifest.json").read_text())
        except (OSError, ValueError):
            manifest = {"build": "Desktop preview" if self.preview else "Development build"}
        firmware = platform.platform() if not Path("/etc/os-release").exists() else next(
            (
                line.partition("=")[2].strip('"')
                for line in Path("/etc/os-release").read_text().splitlines() if line.startswith("PRETTY_NAME=")
            ), "Linux"
        )
        usage = shutil.disk_usage(self.data if self.data.exists() else self.data.parent)
        root = self.data.parent
        try:
            previous = (root / "state/previous").read_text().strip()
        except OSError:
            previous = ""
        self.publish(
            build=manifest.get("build", "Development build"),
            firmware="Simulated handheld" if self.preview else firmware,
            kernel="Preview" if self.preview else platform.release(),
            free_gb=f"{usage.free/1024**3:.1f}",
            previous=previous,
            state="ready",
            message=""
        )
        self.network_status()
        self.sharing_status()

    def rollback(self):
        if self.preview:
            self.publish(state="ready", message="Rollback simulated.")
            return
        from activate import rollback
        from emulators import activity_lock, core_running
        with activity_lock(self.data):
            if core_running(self.data): raise ValueError("Return home from your game first.")
            version = rollback(self.data.parent, stop=False)
        self.publish(
            state="ready", message="Previous build selected. Return to stock, then open RGDS again.", build=version
        )

    def clear_cache(self):
        from emulators import core_running
        if core_running(self.data): raise ValueError("Return home from your game first.")
        folder = self.data / "cache"
        if folder.is_symlink(): raise ValueError("Cache location is invalid.")
        if folder.exists(): shutil.rmtree(folder)
        self.publish(state="ready", message="Temporary game cache cleared. Saves and artwork are kept.")

    def diagnostics(self):
        self.info()
        folder = self.data / "support"
        folder.mkdir(parents=True, exist_ok=True)
        safe = {key: self.fields.get(key, "") for key in ("build", "firmware", "kernel", "free_gb", "wifi_supported")}
        path = folder / f"device-{int(time.time())}.json"
        path.write_text(json.dumps(safe, indent=2) + "\n")
        self.publish(state="ready", message="Device report saved in userdata/support/.")

    def startup_info(self):
        from linux_platform import independent
        if not self.preview and independent():
            fields = read_fields(self.data / 'boot.ini')
            self.publish(
                boot_installed=1,
                boot_mode="rgds",
                boot_warning=int(fields.get('warning', '0') == '1'),
                state="ready",
                message="Startup is managed by RGDS OS."
            )
            return
        if self.preview:
            fields = read_fields(self.data / "boot.ini")
            self.publish(
                boot_installed=1,
                boot_mode=fields.get("mode", "stock"),
                boot_warning=fields.get("warning", "1"),
                state="ready",
                message=""
            )
        else:
            from boot_control import inspect
            state = inspect(self.data.parent)
            self.publish(
                boot_installed=state["installed"],
                boot_mode=state["mode"],
                boot_warning=state["warning"],
                state="ready",
                message=""
            )

    def startup(self, payload):
        from linux_platform import independent
        if not isinstance(payload, dict): raise ValueError("Choose Anbernic or RG DS startup.")
        mode = payload.get("mode")
        warning = payload.get("warning", True)
        if mode not in ("stock", "rgds") or not isinstance(warning, bool):
            raise ValueError("Choose Anbernic or RG DS startup.")
        if not self.preview and independent():
            if mode != 'rgds': raise ValueError("Anbernic startup is unavailable in RGDS OS.")
            from emulators import core_running
            if core_running(self.data): raise ValueError("Return home before changing startup.")
            write_fields(self.data / 'boot.ini', dict(schema=1, mode='rgds', warning=int(warning)))
            self.startup_info()
            self.publish(state='ready', message='Startup choice saved.')
            return
        if self.preview:
            write_fields(self.data / "boot.ini", dict(schema=1, mode=mode, warning=int(warning)))
        else:
            from emulators import core_running
            if core_running(self.data): raise ValueError("Return home before changing startup.")
            from boot_control import configure
            configure(self.data.parent, mode, warning=warning)
        self.startup_info()
        self.publish(
            state="ready", message="Startup choice saved." if not self.preview else "Startup choice simulated."
        )

    def system_exit(self, action):
        if self.preview:
            self.publish(state="ready", message=action.title() + " simulated.")
            return
        from stock_handoff import request_exit, commit_exit, cancel_exit
        receipt = request_exit(self.data, action)
        try:
            if action == 'poweroff':
                from quick_resume import arm
                arm(self.data)
            self.publish(state="ready", message="Restarting…" if action == "reboot" else "Powering off…")
            commit_exit(self.data, receipt)
        except BaseException:
            (self.data / 'resume/armed.json').unlink(missing_ok=True)
            try:
                cancel_exit(self.data, receipt)
            except (OSError, ValueError):
                pass
            raise

    def poweroff(self):
        self.system_exit("poweroff")

    def reboot(self):
        self.system_exit("reboot")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        choices=(
            "info", "scan", "connect", "wifi-on", "wifi-off", "sharing-on", "sharing-open", "sharing-off", "rollback",
            "clear-cache", "diagnostics", "poweroff", "reboot", "startup", "startup-info", "update-info",
            "update-check", "update-download", "update-apply", "update-discard", "update-auto", "library"
        )
    )
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--preview", action="store_true")
    args = parser.parse_args()
    service = Services(args.app, args.data, args.preview)

    def cancelled(_signal, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    try:
        if args.action == "library":
            from library_services import run
            run(service, json.loads(sys.stdin.read(16384)))
        elif args.action.startswith("update-"):
            from ota import Updater
            Updater(args.app, args.data, args.preview).run(args.action[7:])
            service.publish(state="ready", message="")
        elif args.action in ("connect", "startup"):
            getattr(service, args.action)(json.loads(sys.stdin.read(4096)))
        elif args.action in ("wifi-on", "wifi-off"):
            service.radio(args.action == "wifi-on")
        elif args.action in ("sharing-on", "sharing-open", "sharing-off"):
            service.sharing(args.action != "sharing-off", guest=args.action == "sharing-open")
        else:
            getattr(service, args.action.replace("-", "_"))()
    except KeyboardInterrupt:
        service.publish(state="cancelled", message="Request cancelled.")
        return 130
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        service.publish(
            state="error",
            message=str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError) else
            "Request failed. Check the connection or device log and try again."
        )
        return 1
    return 0


if __name__ == "__main__": sys.exit(main())
