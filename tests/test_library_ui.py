"""Library gestures, icon rendering, account entry and emulator settings through SDL."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class LibraryUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        self.state = self.root / "state.txt"
        self.state.write_text('1 0 0 ""\n')
        self.prefs = self.root / "preferences.ini"
        self.bmp = self.root / "screen.bmp"

    def run_ui(self, script, seconds=2, extra=()):
        path = self.root / "input.txt";path.write_text(script)
        result = subprocess.run([str(BINARY), "--preview", "--data", str(self.data), "--preferences", str(self.prefs),
                                 "--state", str(self.state), "--input-script", str(path), "--seconds", str(seconds),
                                 "--screenshot", str(self.bmp), *extra], cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def selected(self): return int(self.state.read_text().split()[1])

    def test_drag_scrolls_and_release_snaps_without_launch(self):
        self.run_ui("20 tap 320 205\n60 drag 40 205\n100 release\n400 key Escape\n")
        self.assertEqual(self.selected(), 2)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_adjacent_tile_click_selects_and_endpoints_do_not_wrap(self):
        self.run_ui("20 click 456 205\n300 key Escape\n")
        self.assertEqual(self.selected(), 1)
        self.state.write_text('1 0 0 ""\n')
        self.run_ui("20 key Left\n300 key Escape\n")
        self.assertEqual(self.selected(), 0)

    def test_full_width_map_seeks_to_end_and_drag_clamps(self):
        self.run_ui("20 tap 610 362\n60 drag 700 362\n100 release\n400 key Escape\n")
        self.assertEqual(self.selected(), 4)
        self.run_ui("20 tap 320 205\n60 drag 1000 205\n100 release\n400 key Escape\n")
        self.assertEqual(self.selected(), 0)

    def test_emulator_choice_is_saved_in_library_settings(self):
        self.run_ui("20 key Right\n60 key Right\n100 key Escape\n140 key Escape\n180 key Escape\n",
                    extra=("--settings", "--section", "library"))
        self.assertIn("core=2\n", self.prefs.read_text())
        self.assertEqual(self.selected(), 0)

    def test_login_keyboard_accepts_text_masks_password_and_cancels(self):
        result = self.run_ui("""200 click 180 148
240 text test-player
280 key Return
450 click 180 218
490 text SECRET-password
530 key Return
700 key Down
740 key Return
""", seconds=1, extra=("--settings", "--section", "scraper"))
        self.assertNotIn("SECRET-password", result.stdout + result.stderr)
        log = self.data / "scraper/worker.log"
        if log.exists(): self.assertNotIn("SECRET-password", log.read_text())
        self.assertFalse((self.data / "private/screenscraper-account.json").exists())
        status = self.data / "scraper/status.ini"
        self.assertIn("not configured", status.read_text())

    def test_native_banner_icons_are_rendered(self):
        roms = self.root / "roms";roms.mkdir()
        content = bytearray(0x640)
        struct.pack_into("<I", content, 0x68, 0x200)
        content[0x220:0x420] = bytes([0x11]) * 512
        struct.pack_into("<H", content, 0x422, 31)
        title = "Original homebrew".encode("utf-16le")
        content[0x540:0x540+len(title)] = title
        (roms / "homebrew.nds").write_bytes(content)
        self.run_ui("600 key Escape\n", extra=("--rom-dir", str(roms)))
        image = self.bmp.read_bytes()
        offset = struct.unpack_from("<I", image, 10)[0]
        width, height = struct.unpack_from("<ii", image, 18)
        # Centre of the native DS icon on the bottom screen; solid palette-red.
        x, y = 320, 480+208
        row = height-1-y if height>0 else y
        self.assertEqual(image[offset + (row*width+x)*4:offset + (row*width+x)*4+3], b"\x00\x00\xff")


if __name__ == "__main__": unittest.main()
