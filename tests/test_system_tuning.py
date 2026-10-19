"""Stock fstrim deferral is reviewed, journalled, reversible and weekly."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import boot_control
import system_tuning as tuning

STOCK = b"#!/bin/sh -e\n# reviewed stock fstrim fixture\n"


class SystemTuningTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory();self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.script = self.root / "etc/init.d/S99fstrim"
        self.script.parent.mkdir(parents=True);self.script.write_bytes(STOCK);self.script.chmod(0o755)
        reviewed = patch.object(tuning, "REVIEWED_FSTRIM", frozenset({hashlib.sha256(STOCK).hexdigest()}))
        reviewed.start();self.addCleanup(reviewed.stop)

    def mode(self):return stat.S_IMODE(self.script.stat().st_mode)

    def test_apply_clears_only_execute_bits_and_remove_restores_exact_mode(self):
        result = tuning.apply(str(self.root), proc=self.root / "proc")
        self.assertEqual(result["fstrim"], "deferred");self.assertTrue(result["changed"])
        self.assertEqual(self.mode(), 0o644);self.assertEqual(self.script.read_bytes(), STOCK)
        journal = json.loads(tuning.journal_path(str(self.root)).read_text())
        self.assertEqual(journal["fstrim"]["previous_mode"], 0o755)
        self.assertFalse(tuning.apply(str(self.root), proc=self.root / "proc")["changed"])
        self.assertTrue(tuning.remove(str(self.root))["changed"])
        self.assertEqual(self.mode(), 0o755)
        self.assertFalse(tuning.journal_path(str(self.root)).exists())

    def test_unreviewed_or_owner_disabled_script_is_left_alone(self):
        self.script.write_bytes(STOCK + b"# vendor update\n")
        self.assertEqual(tuning.apply(str(self.root))["fstrim"], "unreviewed");self.assertEqual(self.mode(), 0o755)
        self.script.write_bytes(STOCK);self.script.chmod(0o644)
        self.assertEqual(tuning.apply(str(self.root))["fstrim"], "disabled-by-owner")
        self.assertFalse(tuning.journal_path(str(self.root)).exists())
        self.assertFalse(tuning.remove(str(self.root))["changed"]);self.assertEqual(self.mode(), 0o644)

    def test_remove_never_touches_a_script_changed_after_deferral(self):
        tuning.apply(str(self.root), proc=self.root / "proc")
        self.script.write_bytes(STOCK + b"# replaced\n")
        self.assertFalse(tuning.remove(str(self.root))["changed"]);self.assertEqual(self.mode(), 0o644)

    def test_returning_to_stock_startup_restores_the_service(self):
        tuning.apply(str(self.root), proc=self.root / "proc")
        boot_control.disable(str(self.root))
        self.assertEqual(self.mode(), 0o755)

    def process(self, proc, pid, parent, *command):
        (proc / str(pid)).mkdir(parents=True)
        (proc / str(pid) / "cmdline").write_bytes(b"\0".join(part.encode() for part in command) + b"\0")
        (proc / str(pid) / "stat").write_text(f"{pid} (x) S {parent} {pid} {pid} 0\n")

    def test_stock_loop_and_its_sleep_are_stopped_only_when_matched_exactly(self):
        proc = self.root / "proc"
        self.process(proc, 867, 1, "/bin/sh", "-e", "/etc/init.d/S99fstrim", "start")
        self.process(proc, 979, 867, "sleep", "3600")
        self.process(proc, 500, 1, "/bin/sh", "-e", "/etc/init.d/S99fstrim", "stop")
        self.process(proc, 600, 42, "/bin/sh", "-e", "/etc/init.d/S99fstrim", "start")
        self.process(proc, 700, 1, "/usr/bin/python3", "game.py")
        with patch.object(tuning.os, "kill") as kill:
            self.assertTrue(tuning.stop_stock_loop(str(self.root), proc))
        self.assertEqual(sorted(call.args[0] for call in kill.call_args_list), [867, 979])
        with patch.object(tuning.os, "kill") as kill:
            self.assertFalse(tuning.stop_stock_loop(str(self.root), self.root / "empty"))
        kill.assert_not_called()

    def test_weekly_trim_covers_writable_card_mounts_only_while_deferred(self):
        mounts = self.root / "mounts"
        mounts.write_text("/dev/root / ext4 rw 0 0\n/dev/mmcblk1p10 /userdata ext4 rw,relatime 0 0\n"
                          "/dev/mmcblk1p11 /mnt/mmc vfat rw,noatime 0 0\ntmpfs /tmp tmpfs rw 0 0\n"
                          "/dev/mmcblk1p9 /oem ext4 ro 0 0\n")
        calls = []
        def run(command, **_options):
            calls.append(command[-1]);return subprocess.CompletedProcess(command, 0)
        self.assertEqual(tuning.trim_if_due(str(self.root), clock=lambda: 10**9, run=run, mounts=mounts)["state"], "stock")
        tuning.apply(str(self.root), proc=self.root / "proc")
        self.assertEqual(tuning.trim_if_due(str(self.root), clock=lambda: 10**9, run=run, mounts=mounts)["state"], "trimmed")
        self.assertEqual(calls, ["/", "/userdata", "/mnt/mmc"])
        self.assertEqual(tuning.trim_if_due(str(self.root), clock=lambda: 10**9 + 3600, run=run, mounts=mounts)["state"], "skipped")
        self.assertEqual(tuning.trim_if_due(str(self.root), clock=lambda: 10**9 + 8 * 86400, run=run, mounts=mounts)["state"], "trimmed")


INITTAB = (b"::sysinit:/etc/init.d/rcS\n::respawn:-/bin/login\n"
           b"::shutdown:/etc/init.d/rcK\n::shutdown:/sbin/swapoff -a\n::shutdown:/bin/umount -a -r\n")


class RegisterPowerOffTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory();self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.inittab = self.root / "etc/inittab";self.inittab.parent.mkdir(parents=True)
        self.inittab.write_bytes(INITTAB);self.inittab.chmod(0o644)
        reviewed = patch.object(tuning, "REVIEWED_INITTAB", frozenset({hashlib.sha256(INITTAB).hexdigest()}))
        reviewed.start();self.addCleanup(reviewed.stop)
        self.helper = self.root / "usr/local/sbin/rgds-final-poweroff"

    def test_step_runs_last_after_read_only_remount_and_is_reversible(self):
        self.assertTrue(tuning.apply_poweroff(str(self.root))["changed"])
        lines = self.inittab.read_text().splitlines()
        self.assertEqual(lines[-2:], ["::shutdown:/bin/umount -a -r", "::shutdown:/usr/local/sbin/rgds-final-poweroff"])
        self.assertTrue(os.access(self.helper, os.X_OK))
        script = self.helper.read_text()
        self.assertIn("/dev/.rgds-poweroff", script);self.assertIn("still writable", script)
        self.assertFalse(tuning.apply_poweroff(str(self.root))["changed"])
        self.assertTrue(tuning.remove_poweroff(str(self.root))["changed"])
        self.assertEqual(self.inittab.read_bytes(), INITTAB);self.assertFalse(self.helper.exists())
        self.assertFalse(tuning.journal_path(str(self.root)).exists())

    def test_unreviewed_inittab_is_never_edited(self):
        self.inittab.write_bytes(INITTAB + b"::shutdown:/custom\n")
        self.assertEqual(tuning.apply_poweroff(str(self.root))["poweroff"], "unreviewed")
        self.assertFalse(self.helper.exists())

    def test_remove_leaves_an_inittab_changed_after_install(self):
        tuning.apply_poweroff(str(self.root))
        changed = self.inittab.read_bytes() + b"::shutdown:/owner\n";self.inittab.write_bytes(changed)
        self.assertFalse(tuning.remove_poweroff(str(self.root))["changed"])
        self.assertEqual(self.inittab.read_bytes(), changed)

    def test_returning_to_stock_restores_inittab(self):
        tuning.apply_poweroff(str(self.root))
        boot_control.disable(str(self.root))
        self.assertEqual(self.inittab.read_bytes(), INITTAB)

    def test_only_power_off_dispatch_requests_the_register_path(self):
        import stock_handoff
        with tempfile.TemporaryDirectory() as data:
            for action in ("reboot", "poweroff"):
                with patch("emulators.core_running", return_value=False), \
                     patch.object(stock_handoff.shutil, "which", return_value="/sbin/" + action), \
                     patch.object(stock_handoff.subprocess, "run"), \
                     patch("system_tuning.request_register_poweroff") as request:
                    stock_handoff.execute_system_action(action, data)
                self.assertEqual(request.called, action == "poweroff")


if __name__ == "__main__":unittest.main()
