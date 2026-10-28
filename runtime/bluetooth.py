"""Bluetooth for the settings Connectivity page on RG DS OS: bluez through bluetoothctl, the radio through
/usr/libexec/rgds/bt (docs/OWN_OS_PLAN.md, "Battery, headphones and Bluetooth").

Status fields (device/status.ini, read by the launcher): bt_supported, bt (the adapter is up), bt_count and
bt_<n>_{address,name,paired,connected}. Scanning lists what is in range and what is paired; choosing a device
pairs, trusts and connects it (a game controller is then a joystick like any other); forgetting removes it.
Pairings persist through `bt save` (a tar on the data partition: bluez's folders carry colons, which exFAT
refuses)."""
import re
import subprocess
from pathlib import Path

BT = Path('/usr/libexec/rgds/bt')
HCI = Path('/sys/class/bluetooth/hci0')
ADDRESS = re.compile(r'^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$')
PREVIEW_DEVICES = [
    dict(address='8C:0A:11:22:33:44', name='Pro Controller', paired=True, connected=True),
    dict(address='AC:C1:22:33:44:55', name='Living room speaker', paired=False, connected=False),
]


def supported():
    return BT.is_file()


def bluetoothctl(*args, timeout=15):
    result = subprocess.run(['bluetoothctl', *args], capture_output=True, text=True, timeout=timeout + 5)
    return result.stdout


def address_of(value):
    address = str(value).strip().upper()
    if not ADDRESS.match(address): raise ValueError('Choose a Bluetooth device from the list.')
    return address


class Bluetooth:
    def __init__(self, services):
        self.services = services
        self.preview = services.preview
        self.devices = list(PREVIEW_DEVICES) if self.preview else []
        self.radio = True

    # -- state --
    def up(self):
        # BusyBox's pgrep -x matches the whole command line, so the daemon is looked up by program name
        if self.preview: return self.radio
        return HCI.is_dir() and subprocess.run(['pidof', 'bluetoothd'], capture_output=True).returncode == 0

    def status(self):
        fields = self.services.fields
        for key in [k for k in fields if k.startswith('bt_')]:
            fields.pop(key)
        if not self.preview and not supported():
            self.services.publish(bt_supported=0, bt=0, bt_count=0)
            return
        up = self.up()
        devices = self.devices if (self.preview or not up) else self.listed()
        if up or self.preview: self.devices = devices
        for index, entry in enumerate(devices):
            for key, value in entry.items():
                fields[f'bt_{index}_{key}'] = str(int(value)) if isinstance(value, bool) else str(value)
        self.services.publish(bt_supported=1, bt=int(up), bt_count=len(devices))

    # -- the radio --
    def on(self):
        self.services.publish(state='busy', message='Turning Bluetooth on…')
        if self.preview: self.radio = True
        else:
            result = subprocess.run([str(BT), 'on'], capture_output=True, text=True, timeout=40)
            if result.returncode != 0:
                raise ValueError('Bluetooth could not start. Restart the handheld and try again.')
        self.status()
        self.services.publish(state='ready', message='')

    def off(self):
        self.services.publish(state='busy', message='Turning Bluetooth off…')
        if self.preview: self.radio = False
        else: subprocess.run([str(BT), 'off'], capture_output=True, timeout=30)
        self.status()
        self.services.publish(state='ready', message='')

    # -- devices --
    def listed(self):
        """Known devices: paired ones first, then what the last scan saw, connected marked."""
        def names(*args):
            out = {}
            for line in bluetoothctl('devices', *args).splitlines():
                parts = line.split(' ', 2)
                if len(parts) >= 2 and parts[0] == 'Device' and ADDRESS.match(parts[1].upper()):
                    out[parts[1].upper()] = parts[2] if len(parts) == 3 else parts[1]
            return out

        every, paired, connected = names(), names('Paired'), names('Connected')
        devices = [
            dict(address=a, name=n, paired=a in paired, connected=a in connected) for a, n in every.items()
            # a bare address is a device that has not told its name yet: not worth a row
            if n != a
        ]
        devices.sort(key=lambda d: (not d['connected'], not d['paired'], d['name'].lower()))
        return devices

    def scan(self):
        self.services.publish(state='busy', message='Looking for devices…')
        if not self.preview:
            if not self.up(): self.on()
            bluetoothctl('--timeout', '10', 'scan', 'on', timeout=15)
        self.status()
        self.services.publish(
            state='ready',
            message='' if self.devices else 'No devices found. Put the device in pairing mode and scan again.'
        )

    def pair(self, payload):
        address = address_of(payload.get('address') if isinstance(payload, dict) else '')
        entry = next((d for d in self.devices if d['address'] == address), None)
        name = entry['name'] if entry else address
        self.services.publish(state='busy', message=f'Connecting to {name}…')
        if self.preview:
            if entry: entry.update(paired=True, connected=True)
        else:
            if not self.up(): self.on()
            if not (entry and entry['paired']):
                out = bluetoothctl('--timeout', '30', 'pair', address, timeout=35)
                if 'Pairing successful' not in out and 'AlreadyExists' not in out:
                    raise ValueError(f'{name} did not pair. Put it in pairing mode and try again.')
                bluetoothctl('trust', address)
            out = bluetoothctl('--timeout', '20', 'connect', address, timeout=25)
            subprocess.run([str(BT), 'save'], capture_output=True, timeout=15)
            if 'Connection successful' not in out:
                self.status()
                raise ValueError(f'{name} is paired but did not connect. Turn it on and choose it again.')
        self.status()
        self.services.publish(state='ready', message=f'{name} connected.')

    def forget(self, payload):
        address = address_of(payload.get('address') if isinstance(payload, dict) else '')
        entry = next((d for d in self.devices if d['address'] == address), None)
        name = entry['name'] if entry else address
        self.services.publish(state='busy', message=f'Forgetting {name}…')
        if self.preview:
            if entry: entry.update(paired=False, connected=False)
        else:
            bluetoothctl('remove', address)
            subprocess.run([str(BT), 'save'], capture_output=True, timeout=15)
        self.status()
        self.services.publish(state='ready', message=f'{name} forgotten.')
