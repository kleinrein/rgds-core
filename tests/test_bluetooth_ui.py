"""UI acceptance for the Connectivity page's Bluetooth rows and the Bluetooth devices page (preview backend)."""
from pathlib import Path
import unittest
import test_visual_options as helpers

ROOT = Path(__file__).resolve().parents[1]
BLUETOOTH_PAGE = 44  # Page::Bluetooth, appended last in settings.hpp


class BluetoothUITests(unittest.TestCase):
    setUp = helpers.VisualOptionsTests.setUp
    run_ui = helpers.VisualOptionsTests.run_ui

    def test_bluetooth_devices_page_opens_from_connectivity(self):
        # Connectivity rows: Wi-Fi, Available networks, Bluetooth, Bluetooth devices, ...
        script = '20 key Down\n40 key-up Down\n80 key Down\n100 key-up Down\n140 key Down\n160 key-up Down\n260 key A\n280 key-up A\n'
        self.run_ui(script, ('--settings', '--section', 'connectivity'))
        self.assertIn(f'{BLUETOOTH_PAGE} 4 0', (self.root / 'state').read_text())
        status = (self.root / 'data/device/status.ini').read_text()
        self.assertIn('bt_supported=1', status)
        self.assertIn('bt_0_name=Pro Controller', status)

    def test_back_returns_to_connectivity(self):
        script = '20 key Down\n40 key-up Down\n80 key Down\n100 key-up Down\n140 key Down\n160 key-up Down\n260 key A\n280 key-up A\n600 key Escape\n620 key-up Escape\n'
        self.run_ui(script, ('--settings', '--section', 'connectivity'))
        self.assertIn('14 4 3', (self.root / 'state').read_text())  # Connectivity, focus on Bluetooth devices


if __name__ == '__main__':
    unittest.main()
