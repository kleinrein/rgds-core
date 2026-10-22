"""Engine output cannot block play or create a stream of storage writes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from engine_log import EngineLog


class EngineLogTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.data = Path(temporary.name)

    def test_noisy_engine_drains_without_disk_writes_and_retains_the_final_error(self):
        command = [
            sys.executable, "-c",
            "import os;os.write(1,b'frame diagnostic\\n'*200000);os.write(2,b'FINAL STARTUP FAILURE\\n')"
        ]
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        log = EngineLog(self.data, "drastic", command, self.data)
        log.start(child.stdout)
        self.assertEqual(child.wait(timeout=5), 0, "A full pipe must not block the engine")
        self.assertFalse((self.data / "logs").exists(), "Capture stays in RAM while playing")
        path = log.finish(0)
        output = path.read_text()
        self.assertTrue(output.endswith("FINAL STARTUP FAILURE\n"))
        self.assertGreater(log.dropped, 0)
        self.assertLessEqual(log.size, 128 * 1024)
        self.assertLess(path.stat().st_size, 129 * 1024)
        self.assertFalse(log.worker.is_alive())
        self.assertTrue(child.stdout.closed)

    def test_inherited_open_writer_does_not_hang_cleanup(self):
        reader, writer = os.pipe()
        self.addCleanup(os.close, writer)
        stream = os.fdopen(reader, "rb", buffering=0)
        log = EngineLog(self.data, "nnddss", ["nnddss"], self.data)
        log.start(stream)
        os.write(writer, b"first frame\n")
        began = time.monotonic()
        output = log.finish(-15).read_text()
        self.assertLess(time.monotonic() - began, .8)
        self.assertIn("first frame", output)
        self.assertIn("Exit code: -15", output)
        self.assertFalse(log.worker.is_alive())
        self.assertTrue(stream.closed)

    def test_silent_engine_blocks_until_output_or_explicit_shutdown(self):
        import select
        reader, writer = os.pipe()
        stream = os.fdopen(reader, "rb", buffering=0)
        log = EngineLog(self.data, "dsperate", ["dsperate"], self.data)
        entered = threading.Event()
        original = select.select
        timeouts = []

        def observed(readers, writers, errors, timeout):
            timeouts.append(timeout)
            entered.set()
            return original(readers, writers, errors, timeout)

        try:
            with patch("engine_log.select.select", side_effect=observed):
                log.start(stream)
                self.assertTrue(entered.wait(1))
                time.sleep(.2)
                self.assertEqual(timeouts, [None], "No timer polling while a game is silent")
                os.write(writer, b"wake on actual output\n")
                output = log.finish(0).read_text()
            self.assertIn("wake on actual output", output)
            self.assertFalse(log.worker.is_alive())
            self.assertTrue(stream.closed)
        finally:
            os.close(writer)
            if log.worker and log.worker.is_alive(): log.finish(-1)

    def test_readonly_game_storage_retains_crash_diagnostics_in_a_private_tempfile(self):
        log = EngineLog(self.data, 'nnddss', ['nnddss'], self.data)
        log.append(b'LAST CRASH DETAIL\n')
        with patch.object(Path, 'mkdir', side_effect=OSError(30, 'Read-only file system')):
            recovered = log.finish(-11)
        self.addCleanup(recovered.unlink)
        self.assertIn('LAST CRASH DETAIL', recovered.read_text())
        self.assertIn('Exit code: -11', recovered.read_text())
        self.assertNotEqual(recovered.parent, self.data / 'logs')
        self.assertEqual(recovered.stat().st_mode & 0o777, 0o600)

    def test_spawn_failure_replaces_previous_diagnostics_atomically(self):
        log = EngineLog(self.data, "dsperate", ["missing-engine"], self.data)
        log.path.parent.mkdir()
        log.path.write_text("previous run")
        log.finish(None, FileNotFoundError("Engine loader is missing"))
        self.assertIn("Runtime error: Engine loader is missing", log.path.read_text())
        self.assertNotIn("previous run", log.path.read_text())
        self.assertFalse(list(log.path.parent.glob("*.partial")))


if __name__ == "__main__": unittest.main()
