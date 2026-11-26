"""os/buildroot/rootfs/usr/libexec/rgds/board-check: Linux tells U-Boot which handheld the card is in (rgds-board.txt),
because U-Boot's own reading of the RG DS Plus's ID voltage is unreliable, and restarts once when the device tree was
wrong. Run against a made-up /sys, boot partition and command line; `reboot` is recorded, not done."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'os/buildroot/rootfs/usr/libexec/rgds/board-check'
NEW_UBOOT = 'root=PARTLABEL=rgds-root-a rgds.detected=rgds rgds.adc=1029325 rgds.from=adc\n'
OLD_UBOOT = 'root=PARTLABEL=rgds-root-a rgds.detected=rgds rgds.adc=1061000\n'


class BoardCheck(unittest.TestCase):

    def run_check(self, raw, model, cmdline=NEW_UBOOT, board_file=None, marker=False, uptime=25, readonly=False):
        tmp = Path(self.tmp.name)
        sysfs, boot = tmp / 'sys', tmp / 'boot'
        adc = sysfs / 'bus/iio/devices/iio:device0'
        adc.mkdir(parents=True, exist_ok=True)
        (adc / 'name').write_text('fe720000.saradc\n')
        (adc / 'in_voltage5_raw').write_text(f'{raw}\n')
        mux = sysfs / 'bus/iio/devices/iio:device1'  # the stick mux, also an iio device, never the ID
        mux.mkdir(parents=True, exist_ok=True)
        (mux / 'name').write_text('adc-mux\n')
        (mux / 'in_voltage5_raw').write_text('0\n')
        dt = sysfs / 'firmware/devicetree/base'
        dt.mkdir(parents=True, exist_ok=True)
        (dt / 'model').write_bytes(model.encode() + b'\0')
        boot.mkdir(exist_ok=True)
        for name in ('rgds-board.txt', 'rgds-board.reboot'):
            (boot / name).unlink(missing_ok=True)
        if board_file is not None: (boot / 'rgds-board.txt').write_text(board_file)
        if marker: (boot / 'rgds-board.reboot').touch()
        (tmp / 'cmdline').write_text(cmdline)
        (tmp / 'reboots').write_text('')
        (tmp / 'reboot.sh').write_text(f'#!/bin/sh\necho reboot >> "{tmp / "reboots"}"\n')
        (tmp / 'reboot.sh').chmod(0o755)
        (tmp / 'log').write_text('')
        (tmp / 'uptime').write_text(f'{uptime}.42 100.0\n')
        env = dict(
            os.environ,
            RGDS_BOARD_SYSFS=str(sysfs),
            RGDS_BOARD_BOOT=str(boot),
            RGDS_BOARD_CMDLINE=str(tmp / 'cmdline'),
            RGDS_BOARD_REBOOT=str(tmp / 'reboot.sh'),
            RGDS_BOARD_LOG=str(tmp / 'log'),
            RGDS_BOARD_UPTIME=str(tmp / 'uptime')
        )
        if readonly: boot.chmod(0o555)
        try:
            subprocess.run(['sh', str(SCRIPT)], env=env, check=True, timeout=10)
        finally:
            boot.chmod(0o755)
        board = (boot / 'rgds-board.txt').read_text() if (boot / 'rgds-board.txt').exists() else None
        return board, (tmp / 'reboots').read_text().count('reboot'), (boot / 'rgds-board.reboot').exists()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_plus_on_its_own_tree_only_records(self):
        self.assertEqual(self.run_check(1023, 'Anbernic RG DS Plus'), ('rgds_known=rgds-plus\n', 0, False))

    def test_rg_ds_on_its_own_tree_only_records(self):
        self.assertEqual(self.run_check(607, 'Anbernic RG DS'), ('rgds_known=rgds\n', 0, False))

    def test_plus_on_the_rg_ds_tree_restarts_once(self):
        # 2026-11-24: U-Boot read 1.03 V on a warm restart and loaded the RG DS's tree; the panels stayed white.
        self.assertEqual(self.run_check(1023, 'Anbernic RG DS'), ('rgds_known=rgds-plus\n', 1, True))

    def test_card_moved_from_a_plus_to_an_rg_ds_restarts_once(self):
        moved = 'root=x rgds.detected=rgds-plus rgds.adc=1061000 rgds.from=file\n'
        self.assertEqual(
            self.run_check(607, 'Anbernic RG DS Plus', moved, 'rgds_known=rgds-plus\n'), ('rgds_known=rgds\n', 1, True)
        )

    def test_a_second_mismatch_never_restarts_again(self):
        self.assertEqual(
            self.run_check(1023, 'Anbernic RG DS', board_file='rgds_known=rgds-plus\n', marker=True),
            ('rgds_known=rgds-plus\n', 0, True)
        )

    def test_the_marker_goes_once_the_tree_is_right(self):
        self.assertEqual(
            self.run_check(1023, 'Anbernic RG DS Plus', board_file='rgds_known=rgds-plus\n', marker=True),
            ('rgds_known=rgds-plus\n', 0, False)
        )

    def test_an_old_u_boot_gets_the_file_but_no_restart(self):
        # The OTA never writes U-Boot: one that does not read the file would restart for nothing, forever.
        self.assertEqual(self.run_check(1023, 'Anbernic RG DS', OLD_UBOOT), ('rgds_known=rgds-plus\n', 0, False))

    def test_an_old_u_boot_under_the_new_extlinux_template_gets_no_restart(self):
        # An updated card keeps its old U-Boot, which expands the template's rgds.from=${rgds_from} to nothing.
        empty = 'root=PARTLABEL=rgds-root-a rgds.detected=rgds rgds.adc=1061000 rgds.from= quiet\n'
        self.assertEqual(self.run_check(1023, 'Anbernic RG DS', empty), ('rgds_known=rgds-plus\n', 0, False))
        at_end = 'root=PARTLABEL=rgds-root-a rgds.detected= rgds.adc= rgds.from=\n'
        self.assertEqual(self.run_check(1023, 'Anbernic RG DS', at_end), ('rgds_known=rgds-plus\n', 0, False))

    def test_no_restart_when_the_boot_partition_cannot_be_written(self):
        self.assertEqual(
            self.run_check(1023, 'Anbernic RG DS', board_file='rgds_known=rgds-plus\n', readonly=True),
            ('rgds_known=rgds-plus\n', 0, False)
        )

    def test_an_early_rg_ds_like_reading_on_a_plus_is_not_trusted(self):
        # 2026-11-25: early in boot the Plus's ID still read inside the RG DS's window; the script wrote "rgds" and
        # the next boot came up white. Before 12 s a reading in the window decides nothing; with the clock stopped
        # here the script would wait, so a reading that rises to the Plus's level is what ends it.
        tmp = Path(self.tmp.name)
        result = {}

        def rise():
            import threading, time
            def later():
                time.sleep(1.5)
                (tmp / 'sys/bus/iio/devices/iio:device0/in_voltage5_raw').write_text('1023\n')
            threading.Thread(target=later, daemon=True).start()
        rise()
        result['plus'] = self.run_check(620, 'Anbernic RG DS Plus', uptime=3)
        self.assertEqual(result['plus'], ('rgds_known=rgds-plus\n', 0, False))

    def test_an_unknown_reading_changes_nothing(self):
        self.assertEqual(self.run_check(300, 'Anbernic RG DS'), (None, 0, False))
        self.assertEqual(self.run_check(820, 'Anbernic RG DS Plus'), (None, 0, False))


if __name__ == '__main__':
    unittest.main()
