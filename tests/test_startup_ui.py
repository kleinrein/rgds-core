"""Startup settings exercise only the preview backend; no hooks or device writes."""
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"
OPEN = "20 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key Down\n220 key Down\n260 key Down\n300 key A\n"   # Startup is the eighth System row


@unittest.skipUnless(BINARY.exists(), "Build host launcher first")
class StartupUITests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / "state.txt";self.state.write_text('1 0 0 ""\n')

    def run_ui(self, script, seconds=2, timeout=5, environment=None, screenshot=None):
        inputs = self.root / "input.txt";inputs.write_text(script)
        command = [str(BINARY), "--preview", "--library", "--settings", "--section", "system",
            "--data", str(self.root), "--preferences", str(self.root / "preferences.ini"), "--state", str(self.state),
            "--input-script", str(inputs), "--seconds", str(seconds)]
        if screenshot:command += ["--screenshot", str(screenshot)]
        result = subprocess.run(command, cwd=ROOT,
            env=environment or dict(os.environ, SDL_VIDEODRIVER="dummy"), capture_output=True, text=True, timeout=timeout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_startup_row_opens_airy_controls_and_back_restores_system_focus(self):
        self.run_ui(OPEN)
        self.assertTrue(self.state.read_text().endswith("24 5 0\n"))
        self.assertIn("boot_mode=stock", (self.root / "device/status.ini").read_text())
        self.run_ui(OPEN + "600 key Escape\n")
        self.assertTrue(self.state.read_text().endswith("9 5 7\n"))   # Back lands on the Startup row again

    def test_rgds_requires_confirmation_and_cancel_preserves_stock(self):
        self.run_ui(OPEN + "600 key A\n640 key Escape\n")
        self.assertFalse((self.root / "boot.ini").exists())
        self.assertIn("boot_mode=stock", (self.root / "device/status.ini").read_text())

    def test_boot_and_warning_choices_save_through_preview_backend(self):
        self.run_ui(OPEN + "600 key A\n640 key Right\n680 key A\n720 key Right\n760 key A\n")
        self.assertIn("mode=rgds\n", (self.root / "boot.ini").read_text())
        self.run_ui(OPEN + "600 key Down\n640 key A\n")
        self.assertIn("mode=rgds\n", (self.root / "boot.ini").read_text())
        self.assertIn("warning=0\n", (self.root / "boot.ini").read_text())
        self.run_ui(OPEN + "600 key A\n640 key Left\n680 key A\n")
        self.assertIn("mode=stock\n", (self.root / "boot.ini").read_text())

    def test_restart_confirmation_waits_for_ready_and_ends_preview(self):
        began = time.monotonic()
        self.run_ui(OPEN + "600 key Down\n640 key Down\n680 key A\n720 key Right\n760 key A\n", seconds=5, timeout=3)
        self.assertLess(time.monotonic() - began, 2)
        self.assertIn("Reboot simulated.", (self.root / "device/status.ini").read_text())

    def test_unsupported_hook_message_is_visible_on_the_top_screen(self):
        binaries = self.root / "bin";binaries.mkdir()
        helper = binaries / "python3"
        helper.write_text(f"#!{sys.executable}\n" + '''import pathlib,sys
data=pathlib.Path(sys.argv[sys.argv.index('--data')+1]);(data/'device').mkdir(parents=True,exist_ok=True)
failed='startup' in sys.argv or 'startup-info' in sys.argv
(data/'device/status.ini').write_text('state='+('error' if failed else 'ready')+'\\nboot_mode=stock\\nboot_warning=1\\nmessage='+('Unsupported stock startup hook.' if failed else '')+'\\n')
sys.exit(1 if failed else 0)
''')
        helper.chmod(0o755)
        environment = dict(os.environ, SDL_VIDEODRIVER="dummy", PATH=str(binaries)+os.pathsep+os.environ.get("PATH", ""))
        screenshot = self.root / "error.bmp"
        self.run_ui(OPEN, environment=environment, screenshot=screenshot)
        self.assertIn("Unsupported stock startup hook.", (self.root / "device/status.ini").read_text())
        image = screenshot.read_bytes();offset=struct.unpack_from("<I", image, 10)[0]
        width,height=struct.unpack_from("<ii", image, 18)
        ink = 0
        for y in range(414, 460):
            row = height-1-y if height>0 else y
            for x in range(28, 612):
                if image[offset+(row*width+x)*4:offset+(row*width+x)*4+3] == bytes((68,57,45)):ink += 1
        self.assertGreater(ink, 40, "The backend error must be drawn in the top-screen message area")


if __name__ == "__main__":
    unittest.main()
