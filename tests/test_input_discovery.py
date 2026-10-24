"""Community event numbering and two touch panels must not reuse stock guesses."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from input_devices import bits, discover


def kernel_bitmap(value):
    words = []
    while value:
        words.append(format(value & ((1 << 64) - 1), 'x'))
        value >>= 64
    return ' '.join(reversed(words or ['0'])) + '\n'


class InputDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sys = self.root / 'sys'
        self.dev = self.root / 'dev'
        self.dev.mkdir()

    def device(self, event, name, touch=False, pad=False, hint=''):
        real = self.root / 'platform' / hint / event
        real.mkdir(parents=True)
        (real / 'capabilities').mkdir()
        (real / 'name').write_text(name)
        (real / 'properties').write_text('2' if touch else '0')
        (real / 'capabilities/abs').write_text(kernel_bitmap((1 << 53) | (1 << 54) if touch else 3))
        (real / 'capabilities/key').write_text(kernel_bitmap(1 << 304 if pad else 1 << 330))
        entry = self.sys / event
        entry.mkdir(parents=True)
        (entry / 'device').symlink_to(real)
        (self.dev / event).touch()

    def test_numbering_changes_use_capabilities_and_board_bus_hint(self):
        self.device('event7', 'Goodix top', touch=True, hint='fe5c0000.i2c')
        self.device('event12', 'Goodix bottom', touch=True, hint='fe5e0000.i2c')
        self.device('event3', 'retrogame_joypad', pad=True, hint='joypad')
        result = discover(self.sys, self.dev, 'fe5e0000')
        self.assertEqual(result['touch'], str(self.dev / 'event12'))
        self.assertEqual(result['keys'], str(self.dev / 'event3'))
        self.assertFalse(result['touch_ambiguous'])

    def test_ambiguous_touch_without_verified_hint_does_not_choose_event_zero(self):
        self.device('event0', 'Goodix one', touch=True, hint='one')
        self.device('event1', 'Goodix two', touch=True, hint='two')
        result = discover(self.sys, self.dev)
        self.assertIsNone(result['touch'])
        self.assertTrue(result['touch_ambiguous'])

    def test_mainline_digital_pad_without_axes_is_detected(self):
        self.device('event0', 'gpio-keys-control', pad=True, hint='gpio-keys-control')
        device = (self.sys / 'event0/device').resolve()
        (device / 'capabilities/abs').write_text('0')
        result = discover(self.sys, self.dev)
        self.assertEqual(result['keys'], str(self.dev / 'event0'))
        self.assertEqual(result['profile'], 'mainline-rg-ds')

    def test_stock_rgds_hat_axes_do_not_require_abs_x_and_y(self):
        self.device('event14', 'ANBERNIC-rk3568-keys', pad=True, hint='gpio-keys-polled')
        device = (self.sys / 'event14/device').resolve()
        # Actual stock RG DS bitmaps: sticks use RX/RY and hats use HAT0X/Y.
        (device / 'capabilities/abs').write_text('3003c\n')
        (device / 'capabilities/key').write_text('400000000 1fff000000000000 0 0 c000000000000 2\n')
        result = discover(self.sys, self.dev)
        self.assertEqual(result['keys'], str(self.dev / 'event14'))
        self.assertEqual(result['profile'], 'stock')
        self.assertFalse(result['keys_ambiguous'])

    def test_two_stock_console_pads_still_require_disambiguation(self):
        for event in ('event7', 'event14'):
            self.device(event, 'ANBERNIC-rk3568-keys', pad=True, hint=event)
            (self.sys / event / 'device/capabilities/abs').write_text('3003c\n')
        result = discover(self.sys, self.dev)
        self.assertIsNone(result['keys'])
        self.assertTrue(result['keys_ambiguous'])

    def test_real_kernel_bitmap_enables_mainline_session_mapping(self):
        import linux_platform
        self.device('event3', 'gpio-keys-control', pad=True, hint='gpio-keys-control')
        device = (self.sys / 'event3/device').resolve()
        (device / 'capabilities/abs').write_text('0')
        (device / 'capabilities/key').write_text('f00000000 7ff000000000000 0 0 0 0 0 400000000000 0\n')
        # Include BTN_SOUTH while retaining the kernel's nine native words.
        keys = bits(device / 'capabilities/key') | (1 << 304)
        (device / 'capabilities/key').write_text(kernel_bitmap(keys))
        library = self.root / 'lib'
        library.mkdir()
        (library / 'libSDL2-2.0.so.0').touch()
        fake_socket = self.root / 'run/wayland-0'
        fake_socket.parent.mkdir()
        fake_socket.touch()
        profile = dict(top_display='1', bottom_display='0')
        with patch('linux_platform.hardware_profile',return_value=profile), \
                patch('input_devices.discover',side_effect=lambda **kw:discover(self.sys,self.dev,**kw)), \
                patch.object(Path,'is_socket',lambda path:path==fake_socket):
            env = linux_platform.environment({}, runtime_dirs=[fake_socket.parent], library_dirs=[library])
        self.assertEqual(env['RGDS_INPUT_PROFILE'], 'mainline-rg-ds')
        self.assertEqual(env['RGDS_KEYS_DEV'], str(self.dev / 'event3'))

    def test_missing_devices_have_no_stock_fallback(self):
        result = discover(self.sys, self.dev)
        self.assertIsNone(result['touch'])
        self.assertIsNone(result['keys'])

    def test_unpadded_words_keep_kernel_key_positions(self):
        path = self.root / 'bitmap'
        # BTN_SOUTH=304, KEY_UP=103 and KEY_ESC=1, in separate native words.
        path.write_text('1000000000000 0 0 8000000000 2\n')
        self.assertEqual(bits(path), (1 << 304) | (1 << 103) | (1 << 1))
        path.write_text('1000000000000 0 0 0 0\n')
        self.assertEqual(bits(path), 1 << 304)

    def test_missing_or_malformed_bitmap_does_not_invent_controls(self):
        path = self.root / 'bitmap'
        self.assertEqual(bits(path), 0)
        for content in ('oops', '-1', '10000000000000000', ''):
            path.write_text(content)
            self.assertEqual(bits(path), 0)

    def test_lid_switch_uses_kernel_wakeup_attribute(self):
        self.device('event9', 'gpio-keys', hint='gpio-keys')
        device = (self.sys / 'event9/device').resolve()
        (device / 'capabilities/sw').write_text('1')
        (device / 'power').mkdir()
        (device / 'power/wakeup').write_text('disabled')
        result = discover(self.sys, self.dev)
        self.assertEqual(result['lid'], str(self.dev / 'event9'))
        self.assertEqual(result['lid_wakeup'], str(device / 'power/wakeup'))


if __name__ == '__main__': unittest.main()
