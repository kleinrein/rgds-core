import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime"))

import bluetooth
from fields import read_fields

FAKE = r'''#!/bin/sh
# a bluetoothctl that answers like bluez 5.79 for the calls bluetooth.py makes
log="$FAKE_LOG"; echo "$*" >> "$log"
case "$*" in
  "devices") printf 'Device 8C:0A:11:22:33:44 Pro Controller\nDevice AC:C1:22:33:44:55 Living room speaker\nDevice 11:22:33:44:55:66 11:22:33:44:55:66\n';;
  "devices Paired") printf 'Device 8C:0A:11:22:33:44 Pro Controller\n';;
  "devices Connected") [ -f "$FAKE_LOG.connected" ] && printf 'Device 8C:0A:11:22:33:44 Pro Controller\n';;
  "--timeout 30 pair AC:C1:22:33:44:55") echo "Pairing successful";;
  "--timeout 20 connect "*) touch "$FAKE_LOG.connected"; echo "Connection successful";;
  "--timeout 10 scan on") echo "Discovery started";;
  *) echo ok;;
esac
'''


class Services:
    def __init__(self, data, preview=False):
        self.preview = preview
        self.status_file = data / "status.ini"
        self.fields = {}

    def publish(self, **fields):
        self.fields.update({k: str(v) for k, v in fields.items()})
        from fields import write_fields
        write_fields(self.status_file, self.fields)


class BluetoothTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        (root / "bin").mkdir()
        (root / "bin/bluetoothctl").write_text(FAKE)
        (root / "bin/bluetoothctl").chmod(0o755)
        (root / "bin/pidof").write_text("#!/bin/sh\nexit 0\n")
        (root / "bin/pidof").chmod(0o755)
        self.env = dict(PATH=str(root / "bin") + os.pathsep + os.environ["PATH"], FAKE_LOG=str(root / "log"))
        self.old = {k: os.environ.get(k) for k in self.env}
        os.environ.update(self.env)
        self.hci = root / "hci0"
        self.hci.mkdir()
        bluetooth.HCI = self.hci
        bluetooth.BT = root / "bt"
        bluetooth.BT.write_text("#!/bin/sh\necho \"bt $*\" >> \"$FAKE_LOG\"\n")
        bluetooth.BT.chmod(0o755)
        self.services = Services(root)

    def tearDown(self):
        for k, v in self.old.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
        self.tmp.cleanup()

    def test_status_lists_named_devices_paired_first(self):
        bluetooth.Bluetooth(self.services).status()
        f = read_fields(self.services.status_file)
        self.assertEqual((f["bt_supported"], f["bt"], f["bt_count"]), ("1", "1", "2"))
        self.assertEqual((f["bt_0_name"], f["bt_0_paired"], f["bt_0_connected"]), ("Pro Controller", "1", "0"))
        self.assertEqual(f["bt_1_name"], "Living room speaker")  # the bare-address device is left out

    def test_pair_pairs_trusts_connects_and_saves(self):
        bt = bluetooth.Bluetooth(self.services)
        bt.status()
        bt.pair({"address": "ac:c1:22:33:44:55"})
        log = (Path(self.tmp.name) / "log").read_text()
        self.assertIn("--timeout 30 pair AC:C1:22:33:44:55", log)
        self.assertIn("trust AC:C1:22:33:44:55", log)
        self.assertIn("--timeout 20 connect AC:C1:22:33:44:55", log)
        self.assertIn("bt save", log)
        self.assertEqual(self.services.fields["message"], "Living room speaker connected.")

    def test_a_bad_address_is_refused(self):
        with self.assertRaises(ValueError):
            bluetooth.Bluetooth(self.services).pair({"address": "../etc"})

    def test_preview_has_devices_and_a_radio(self):
        services = Services(Path(self.tmp.name), preview=True)
        bt = bluetooth.Bluetooth(services)
        bt.status()
        self.assertEqual(services.fields["bt_count"], "2")
        bt.off()
        self.assertEqual(services.fields["bt"], "0")
        bt.forget({"address": "8C:0A:11:22:33:44"})
        self.assertEqual(services.fields["bt_0_paired"], "0")


if __name__ == "__main__":
    unittest.main()
