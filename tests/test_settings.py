"""Exercise settings through real SDL keyboard/touch events in the host preview."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(BINARY.exists(), "Build host launcher first")
class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state.txt"
        self.state.write_text('1 3 2 ""\n')
        self.prefs = self.root / "preferences.ini"
        self.system_prefs = self.root / "system.ini"
        self.bmp = self.root / "screen.bmp"

    def run_preview(self, script=None, extra=()):
        args = [str(BINARY), "--preview", "--state", str(self.state), "--preferences", str(self.prefs),
                "--screenshot", str(self.bmp)]
        if script:
            path = self.root / "input.txt"
            path.write_text(script)
            args += ["--input-script", str(path), "--seconds", "2"]
        else:
            args += ["--frames", "1"]
        result = subprocess.run([*args, *extra], cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Font fallback", result.stderr)
        return self.bmp.read_bytes()

    @staticmethod
    def background(data):
        offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        stride = width * 4
        row = abs(height) - 1 if height > 0 else 0
        b, g, r = data[offset + row * stride:offset + row * stride + 3]
        return r, g, b

    def test_keyboard_changes_appearance_and_back_never_exits_inside_settings(self):
        result = self.run_preview("""20 key S
60 key Return
100 key Right
140 key Down
180 key Right
220 key Escape
260 key Down
300 key Return
340 key Right
380 key Return
420 key Escape
460 key S
500 key Escape
""")
        self.assertEqual(self.prefs.read_text(), "schema=1\nmode=dark\nfont=pixel\ncore=0\n")
        self.assertEqual(self.state.read_text(), '1 3 2 ""\n')
        self.assertEqual(self.background(result), (16, 26, 44))

    def test_touch_choices_save_immediately_and_persist_on_next_launch(self):
        result = self.run_preview("""20 tap 577 49
60 tap 240 104
100 tap 440 140
140 tap 440 244
180 tap 140 244
220 tap 51 44
260 tap 240 160
300 tap 240 165
340 tap 51 44
380 tap 51 44
420 key Escape
""")
        self.assertEqual(self.prefs.read_text(), "schema=1\nmode=dark\nfont=rounded\ncore=0\n")
        self.assertEqual(self.state.read_text(), '1 3 2 ""\n')
        self.assertEqual(self.background(result), (16, 26, 44))
        restarted = self.run_preview()
        self.assertEqual(self.background(restarted), (16, 26, 44))

    def test_placeholders_cannot_change_preferences_or_game_selection(self):
        self.run_preview("""20 key S
60 key Down
80 key Down
90 key Down
100 key Return
140 key Right
180 key A
220 key Down
260 key Left
300 key Escape
340 key Escape
380 key Escape
""")
        self.assertFalse(self.prefs.exists())
        self.assertFalse(self.system_prefs.exists())
        self.assertEqual(self.state.read_text(), '1 3 2 ""\n')

    def test_power_sleep_preferences_and_simulated_sleep_keep_selection(self):
        self.run_preview("""20 key Right
60 key Right
100 key Down
140 key Right
180 key Right
220 key Down
260 key Right
300 key Down
340 key Return
380 key Escape
420 key Escape
460 key Escape
""", extra=("--settings", "--section", "power"))
        self.assertEqual(self.system_prefs.read_text(), "schema=1\npower=2\nbrightness=60\nidle=2\nlid=1\n")
        self.assertFalse(self.prefs.exists())
        self.assertEqual(self.state.read_text(), '1 3 2 ""\n')
        self.run_preview("20 key Left\n60 key Escape\n100 key Escape\n140 key Escape\n",
                         extra=("--settings", "--section", "power"))
        self.assertIn("power=1\n", self.system_prefs.read_text())
        self.assertIn("idle=2\nlid=1\n", self.system_prefs.read_text())

    def test_touch_brightness_slider_clamps_and_back_returns_to_appearance(self):
        self.run_preview("""20 tap 200 333
60 tap 596 219
100 key Right
140 tap 44 219
180 key Left
220 key Right
260 tap 51 44
""", extra=("--settings", "--section", "theme"))
        self.assertIn("brightness=10\n", self.system_prefs.read_text())
        self.assertTrue(self.state.read_text().endswith("2 0 2\n"))
        self.run_preview("20 key Return\n60 key Right\n100 key Escape\n",
                         extra=("--settings", "--section", "theme"))
        # Opening Settings explicitly resets focus; existing brightness remains intact.
        self.assertIn("brightness=10\n", self.system_prefs.read_text())

    def test_invalid_system_preferences_are_preserved_and_defaults_can_be_saved(self):
        self.system_prefs.write_text("schema=1\npower=999\nbrightness=0\nidle=99\nlid=2\n")
        self.run_preview(extra=("--settings", "--section", "power"))
        self.assertIn("power=999", self.system_prefs.read_text())
        self.run_preview("20 key Right\n60 key Escape\n100 key Escape\n140 key Escape\n",
                         extra=("--settings", "--section", "power"))
        self.assertEqual(self.system_prefs.read_text(), "schema=1\npower=1\nbrightness=60\nidle=0\nlid=0\n")

    def test_brightness_drag_clamps_and_release_stops_changes(self):
        self.run_preview("""20 tap 56 219
60 drag 320 219
100 drag 700 219
140 release
180 drag 56 219
220 key Escape
260 key Escape
300 key Escape
340 key Escape
""", extra=("--settings", "--section", "brightness"))
        self.assertIn("brightness=100\n", self.system_prefs.read_text())

    def test_older_placeholder_power_state_opens_working_controls(self):
        self.state.write_text('1 3 2 ""\n3 1 0\n')
        self.run_preview("20 key Right\n60 key Escape\n100 key Escape\n140 key Escape\n")
        self.assertIn("power=1\n", self.system_prefs.read_text())

    def test_malformed_preferences_fall_back_without_crashing(self):
        self.prefs.write_text("schema=1\nmode=corrupt\nfont=anything\n")
        image = self.run_preview()
        self.assertEqual(self.background(image), (241, 245, 251))
        self.assertEqual(self.prefs.read_text(), "schema=1\nmode=corrupt\nfont=anything\n")

    def test_settings_and_font_variants_render_in_both_modes(self):
        for mode in ("light", "dark"):
            for section in ("theme", "power", "brightness"):
                shades = {}
                for font in ("rounded", "pixel"):
                    with self.subTest(mode=mode, font=font, section=section):
                        self.state.write_text('1 3 2 ""\n')
                        image = self.run_preview(extra=("--settings", "--section", section, "--mode", mode, "--font", font))
                        self.assertEqual(self.background(image), (16, 26, 44) if mode == "dark" else (241, 245, 251))
                        shades[font] = len(set(image[10000:]))
                self.assertGreater(shades["rounded"], shades["pixel"], "Rounded text should have antialiased edges")


if __name__ == "__main__":
    unittest.main()
