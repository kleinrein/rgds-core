"""The crash record: each way of dying leaves one, the process still dies the same way, and the tool can read it."""
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from symbolize_crash import owner, parse


class CrashRecordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "crash_record_test"
        build = subprocess.run(
            [
                "clang++", "-std=c++17", "-O1", "-g", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
                "-Wno-infinite-recursion", "-I" + str(ROOT / "src"),
                str(ROOT / "tests/crash_record_test.cpp"), "-o",
                str(cls.binary)
            ],
            capture_output=True,
            text=True
        )
        assert build.returncode == 0, build.stderr

    def die(self, kind):
        with tempfile.TemporaryDirectory() as logs:
            run = subprocess.run([str(self.binary), logs, kind], capture_output=True, text=True, timeout=30)
            path = Path(logs) / "crashes.log"
            return run, path.read_text() if path.exists() else ""

    def test_a_fault_records_the_signal_the_place_and_the_screen(self):
        run, record = self.die("segv")
        self.assertIn(run.returncode, (-signal.SIGSEGV, -signal.SIGBUS))
        self.assertIn("kind=signal", record)
        self.assertIn("fault=0x0", record)
        self.assertIn("main=1", record)
        self.assertIn("release=test-release", record)
        self.assertIn("page=7", record)
        self.assertIn("games=6000", record)
        self.assertIn("collection_loading=1", record)
        self.assertIn("=== end ===", record)
        self.assertGreaterEqual(len(parse(record)[0]["frames"]), 3, record)
        self.assertIn("kind=signal", run.stderr)

    def test_the_stack_overflow_is_recorded_on_the_handlers_own_stack(self):
        run, record = self.die("stack")
        self.assertLess(run.returncode, 0)
        self.assertIn("kind=signal", record)
        self.assertIn("=== end ===", record)

    def test_a_worker_threads_fault_says_so(self):
        run, record = self.die("thread")
        self.assertLess(run.returncode, 0)
        self.assertIn("main=0", record)

    def test_an_uncaught_exception_names_what_was_thrown_then_aborts(self):
        run, record = self.die("throw")
        self.assertEqual(run.returncode, -signal.SIGABRT)
        self.assertIn("kind=terminate what=nobody caught this", record)
        self.assertIn("signal=SIGABRT", record)

    def test_abort_is_recorded(self):
        run, record = self.die("abort")
        self.assertEqual(run.returncode, -signal.SIGABRT)
        self.assertIn("signal=SIGABRT", record)

    def test_what_main_catches_last_is_recorded(self):
        run, record = self.die("caught")
        self.assertEqual(run.returncode, 1)
        self.assertIn("kind=exception what=card is read-only", record)

    def test_a_clean_run_records_nothing(self):
        run, record = self.die("fine")
        self.assertEqual(run.returncode, 0)
        self.assertEqual(record, "")

    def test_a_full_log_moves_aside_and_a_new_one_starts(self):
        with tempfile.TemporaryDirectory() as logs:
            (Path(logs) / "crashes.log").write_bytes(b"x" * (1024 * 1024 + 1))
            subprocess.run([str(self.binary), logs, "fine"], timeout=30)
            self.assertEqual((Path(logs) / "crashes.log.1").stat().st_size, 1024 * 1024 + 1)
            self.assertEqual((Path(logs) / "crashes.log").stat().st_size, 0)

    def test_the_tool_reads_frames_maps_and_a_torn_record(self):
        log = (
            "=== crash ===\nkind=signal signal=SIGSEGV code=1 fault=0x0\n"
            "base=0x1000 mode=launcher release=r build_id=abc\n"
            "#0 0x1234\n#1 0x7f0010\nmaps:\n"
            "00001000-00002000 r-xp 00000000 b3:04 12 /usr/share/rgds/bin/rgds-launcher\n"
            "7f0000-7f1000 r-xp 00040000 b3:02 9 /usr/lib/libSDL2-2.0.so.0\n=== end ===\n"
            "=== crash ===\nkind=signal signal=SIGABRT\nbase=0x0 mode=launcher release=r build_id=\n#0 0x20"
        )
        first, torn = parse(log)
        self.assertEqual((first["base"], first["build_id"]), (0x1000, "abc"))
        self.assertEqual(first["frames"], [0x1234, 0x7F0010])
        self.assertEqual(owner(first, 0x7F0010), ("/usr/lib/libSDL2-2.0.so.0", 0x40010))
        self.assertEqual(torn["frames"], [0x20])

    def test_the_session_keeps_the_exit_status_and_five_logs(self):
        session = (ROOT / "os/buildroot/rootfs/usr/libexec/rgds/session").read_text()
        self.assertEqual(
            subprocess.run(["sh", "-n", str(ROOT / "os/buildroot/rootfs/usr/libexec/rgds/session")]).returncode, 0
        )
        self.assertNotIn("exec $PIN", session)
        self.assertIn("status=$?", session)
        self.assertIn("LOG.$i", session)

    def test_kernel_crashes_can_survive_to_the_next_boot(self):
        for name in ("rk3568-anbernic-rg-ds.dts", "rk3568-anbernic-rg-ds-plus.dts"):
            tree = (ROOT / "os/buildroot/board/rgds/dts" / name).read_text()
            self.assertIn('compatible = "ramoops"', tree, name)
        self.assertIn("kernel.panic", (ROOT / "os/buildroot/rootfs/etc/sysctl.d/rgds.conf").read_text())
        self.assertIn("mount -t pstore", (ROOT / "os/buildroot/rootfs/etc/init.d/rcS").read_text())


if __name__ == "__main__":
    unittest.main()
