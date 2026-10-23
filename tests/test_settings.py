"""Exercise settings through real SDL keyboard/touch events in the host preview."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
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
        (self.root / "ui.ini").write_text(
            "schema=1\nhome_layout=carousel\nbackground=plain\n"
        )  # pixel checks read the plain background

    def run_preview(self, script=None, extra=()):
        entry = [] if "--home" in extra else ["--library"]
        args = [
            str(BINARY), "--preview", *entry, "--state",
            str(self.state), "--preferences",
            str(self.prefs), "--screenshot",
            str(self.bmp)
        ]
        if script:
            path = self.root / "input.txt"
            path.write_text(script)
            args += ["--input-script", str(path), "--seconds", "2"]
        else:
            args += ["--frames", "1"]
        result = subprocess.run(
            [*args, *extra],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
            capture_output=True,
            text=True,
            timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Font fallback", result.stderr)
        return self.bmp.read_bytes()

    @staticmethod
    def background(data):
        offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        stride = width * 4
        row = abs(height) - 1 - 101 if height > 0 else 101
        b, g, r = data[offset + row * stride + 4:offset + row * stride + 7]
        return r, g, b

    def test_keyboard_changes_appearance_and_back_never_exits_inside_settings(self):
        result = self.run_preview(
            """20 key S
60 key Return
100 key Right
140 key Down
150 key Down
160 key Down
180 key Right
220 key Escape
260 key Down
300 key Return
340 key Right
380 key Return
420 key Escape
460 key S
500 key Escape
"""
        )
        self.assertEqual(self.prefs.read_text(), "schema=1\nmode=dark\nfont=pixel\ncore=4\nauto_load=1\nauto_save=1\n")
        self.assertEqual(self.state.read_text().splitlines()[0], '1 3 2 ""')
        self.assertEqual(self.background(result), (12, 17, 26))

    def test_touch_choices_save_immediately_and_persist_on_next_launch(self):
        result = self.run_preview(
            """20 key S
60 click 240 128
100 click 520 129
140 click 500 375
180 click 350 375
220 click 72 452
260 click 240 211
300 click 240 128
340 click 72 452
380 click 72 452
420 key Escape
"""
        )
        self.assertEqual(
            self.prefs.read_text(), "schema=1\nmode=dark\nfont=rounded\ncore=4\nauto_load=1\nauto_save=1\n"
        )
        self.assertEqual(self.state.read_text().splitlines()[0], '1 3 2 ""')
        self.assertEqual(self.background(result), (12, 17, 26))
        restarted = self.run_preview()
        self.assertEqual(self.background(restarted), (12, 17, 26))

    def test_placeholders_cannot_change_preferences_or_game_selection(self):
        self.run_preview(
            """20 key S
60 key Down
80 key Down
90 key Down
95 key Down
100 key Return
140 key Right
180 key A
220 key Down
260 key Left
300 key Escape
340 key Escape
380 key Escape
"""
        )
        self.assertFalse(self.prefs.exists())
        self.assertFalse(self.system_prefs.exists())
        self.assertEqual(self.state.read_text().splitlines()[0], '1 3 2 ""')

    def test_sound_slider_mute_and_restart_share_one_saved_level(self):
        self.run_preview(
            "20 key Right\n30 key-up Right\n60 key Return\n100 key Return\n140 key Left\n160 key-up Left\n",
            extra=("--settings", "--section", "sound")
        )
        volume = self.root / "volume.ini"
        self.assertEqual(volume.read_text(), "schema=1\nvolume=25\n")
        self.run_preview(
            "20 tap 56 146\n60 drag 320 146\n100 drag 700 146\n140 release\n180 drag 56 146\n",  # the Sound slider card sits at y 96-186
            extra=("--settings", "--section", "sound")
        )
        self.assertEqual(volume.read_text(), "schema=1\nvolume=100\n")
        self.run_preview("20 key Left\n60 key-up Left\n", extra=("--settings", "--section", "sound"))
        self.assertEqual(volume.read_text(), "schema=1\nvolume=95\n")

    def test_held_slider_key_repeats_and_release_stops_changes(self):
        volume = self.root / "volume.ini"
        volume.write_text("schema=1\nvolume=50\n")
        self.run_preview("20 key Right\n620 key-up Right\n", extra=("--settings", "--section", "sound"))
        level = int(volume.read_text().split("volume=")[1])
        self.assertGreater(level, 55)
        self.assertLess(level, 100)

    def test_game_options_favourites_and_view_extensions_keep_base_preferences_readable(self):
        self.run_preview(
            "20 key X\n60 key A\n100 key Down\n140 key A\n180 key Down\n220 key A\n240 key Down\n250 key A\n260 key Right\n280 key Right\n300 key A\n320 key Escape\n340 key Y\n400 key Right\n450 key A\n500 key Escape\n"
        )
        options = next((self.root / "library").glob("*/game.ini")).read_text()
        self.assertIn("favourite=1", options)
        self.assertIn("core=0", options)
        self.assertIn("filter=1", options)
        self.assertIn("view=1\n", (self.root / "ui.ini").read_text())
        self.assertNotIn("view=", self.prefs.read_text())
        self.run_preview("20 key X\n60 key A\n100 key Escape\n")
        self.assertIn("favourite=0", next((self.root / "library").glob("*/game.ini")).read_text())

    def test_each_engine_uses_the_same_quick_menu_and_back_resumes(self):
        bottoms = []
        (self.root / 'ui.ini').write_text('schema=1\nanimations=0\n')
        for core in ("drastic", "dsperate", "nnddss", "melonds"):
            self.state.write_text('1 3 2 ""\n')
            data = self.run_preview(extra=("--game-menu", "--game-core", core))
            offset = struct.unpack_from("<I", data, 10)[0]
            bottoms.append(data[offset:offset + 640 * 480 * 4])
        self.assertEqual(bottoms[0], bottoms[1])
        self.assertEqual(bottoms[1], bottoms[2])
        self.run_preview("20 tap 120 198\n60 key Escape\n", extra=("--game-menu", "--game-core", "dsperate"))
        self.assertFalse((self.root / "session/core.json").exists())

    def test_wifi_password_keyboard_masks_and_cancel_does_not_store_credentials(self):
        self.run_preview(
            "600 key Down\n650 key Return\n1000 key Down\n1050 key Down\n1100 key Return\n1200 text private-test-password\n1300 key Escape\n",
            extra=("--settings", "--section", "connectivity")
        )
        for path in (self.root / "device").glob("*"):
            if path.is_file(): self.assertNotIn("private-test-password", path.read_text())
        self.assertIn("wifi=1", (self.root / "device/status.ini").read_text())

    def test_animation_switch_is_saved_without_changing_the_default_theme(self):
        self.run_preview(
            "".join(f"{20+i*20} key Down\n" for i in range(6)) + "140 key A\n200 key A\n",
            extra=("--settings", "--section", "theme")
        )
        self.assertIn("animations=0", (self.root / "ui.ini").read_text())
        self.assertIn("mode=light", self.prefs.read_text())

    def test_view_chip_and_options_key_work_in_child_game_list(self):
        self.run_preview("20 click 120 98\n100 key Right\n160 key Right\n220 key A\n260 key Escape\n300 key S\n")
        self.assertIn("view=2", (self.root / "ui.ini").read_text())
        self.assertTrue(self.state.read_text().endswith("1 0 0\n"))
        self.state.write_text('1 3 2 ""\n')
        self.run_preview("20 key Y\n70 key Left\n120 key Left\n170 key A\n210 key Escape\n240 key X\n300 key A\n")
        self.assertIn("favourite=1", next((self.root / "library").glob("*/game.ini")).read_text())

    def test_full_shader_picker_saves_high_ids_globally_and_for_a_cartridge(self):
        # The carousel is grouped; Amber (19) is the last place.
        select_last = "".join(f"{220 + index * 40} key Right\n" for index in range(28))
        self.run_preview(
            "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n" + select_last + "1500 key A\n",
            extra=("--settings", "--section", "library")
        )
        self.assertIn("filter=19\n", (self.root / "ui.ini").read_text())
        self.state.write_text('1 3 2 ""\n')
        select_last = "".join(f"{180 + index * 40} key Right\n" for index in range(29))
        self.run_preview("20 key X\n60 key Down\n100 key Down\n140 key A\n" + select_last + "1500 key A\n")
        self.assertIn("filter=19\n", next((self.root / "library").glob("*/game.ini")).read_text())

    def test_shader_groups_jump_with_up_down_shoulders_and_chips(self):
        picker = "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n"
        self.run_preview(picker + "220 key Down\n300 key A\n", extra=("--settings", "--section", "library"))
        self.assertIn("filter=21\n", (self.root / "ui.ini").read_text(), "Down opens DS screens at DS Classic")
        self.state.write_text('1 3 2 ""\n')
        (self.root / "ui.ini").unlink()
        self.run_preview(picker + "220 key Up\n300 key A\n", extra=("--settings", "--section", "library"))
        self.assertIn("filter=16\n", (self.root / "ui.ini").read_text(), "Up wraps to Moods")
        self.state.write_text('1 3 2 ""\n')
        (self.root / "ui.ini").unlink()
        self.run_preview(picker + "220 key ]\n260 key ]\n300 key A\n", extra=("--settings", "--section", "library"))
        self.assertIn("filter=23\n", (self.root / "ui.ini").read_text(), "R steps two groups to Natural")
        self.state.write_text('1 3 2 ""\n')
        (self.root / "ui.ini").unlink()
        self.run_preview(picker + "220 click 460 100\n300 key A\n", extra=("--settings", "--section", "library"))
        self.assertIn("filter=27\n", (self.root / "ui.ini").read_text(), "The Pixel art chip opens Pixel beads")

    def test_dsperate_picker_offers_the_full_preset_pack(self):
        self.prefs.write_text("schema=1\nmode=light\nfont=rounded\ncore=1\n")
        self.run_preview(
            "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n220 key Right\n260 key Right\n300 key A\n",
            extra=("--settings", "--section", "library")
        )
        self.assertIn("filter=21\n", (self.root / "ui.ini").read_text())

    def test_shader_rail_drag_selects_the_last_shader_and_a_tile_tap_applies(self):
        self.run_preview(
            "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n220 tap 300 440\n260 drag 600 440\n300 release\n600 click 320 200\n",
            extra=("--settings", "--section", "library")
        )
        self.assertIn("filter=19\n", (self.root / "ui.ini").read_text())

    def test_shader_focus_restores_during_rebuilds_and_scrolls_into_view(self):
        select_last = "".join(f"{220 + index * 40} key Right\n" for index in range(14))
        self.run_preview(
            "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n" + select_last,
            extra=("--settings", "--section", "library")
        )
        self.assertIn("20 2 14\nshader 0\n", self.state.read_text())
        self.assertTrue(self.state.read_text().endswith("shader-preview 0 2 0.5\n"))
        # The restored carousel is centred on place 14 (LCD + Natural Vision).
        self.run_preview("20 click 320 200\n")
        self.assertIn("filter=7\n", (self.root / "ui.ini").read_text())

    def test_shader_left_and_right_move_one_item(self):
        self.run_preview(
            "20 key Down\n40 key Down\n60 key Down\n100 key Down\n140 key Down\n180 key A\n220 key Right\n260 key Right\n300 key Left\n340 key A\n",
            extra=("--settings", "--section", "library")
        )
        self.assertIn("filter=1", (self.root / "ui.ini").read_text())

    def test_settings_scrollbar_reaches_system_and_footer_keeps_focus_visible(self):
        self.run_preview(
            "20 tap 614 130\n60 drag 614 410\n100 release\n140 click 160 138\n180 click 72 452\n220 click 550 452\n",
            extra=("--settings",)
        )
        # At the end of the ten-row list the first whole row is Updates (section 9, below System).
        self.assertTrue(self.state.read_text().endswith("17 9 0\n"), self.state.read_text())
        self.assertFalse(self.system_prefs.exists())

    def test_threaded_3d_only_writes_a_setting_on_drastic(self):
        for core in ("drastic", "dsperate", "nnddss", "melonds"):
            with self.subTest(core=core):
                self.state.write_text('1 3 2 ""\n')
                for path in (self.root / "library").glob("*/game.ini"):
                    path.unlink()
                self.run_preview(
                    "20 click 120 280\n60 key Down\n100 key Down\n140 key Down\n180 key Right\n",
                    extra=("--game-menu", "--game-core", core)
                )
                saved = list((self.root / "library").glob("*/game.ini"))
                if core == "drastic":
                    self.assertEqual(len(saved), 1)
                    self.assertIn("threaded_3d=0\n", saved[0].read_text())
                else:
                    self.assertEqual(saved, [], "Automatic threading must not save an ineffective toggle")

    def test_in_game_shader_button_opens_options_for_the_active_engine(self):
        for core, expected in (("drastic", 22), ("dsperate", 22), ("nnddss", 22), ("melonds", 22)):
            with self.subTest(core=core):
                self.state.write_text('1 3 2 ""\n')
                for path in (self.root / "library").glob("*/game.ini"):
                    path.unlink()
                self.run_preview(
                    "20 click 520 200\n60 key Right\n100 key Right\n140 key Right\n180 key Right\n220 key A\n",
                    extra=("--game-menu", "--game-core", core)
                )
                saved = next((self.root / "library").glob("*/game.ini")).read_text()
                self.assertIn("filter=" + str(expected) + "\n", saved)
                focus = 4
                self.assertIn(
                    f"20 0 {focus}\nshader 1\n", self.state.read_text(), "Applying a shader must keep the picker open"
                )
                self.assertTrue(self.state.read_text().endswith("shader-preview 0 2 0.5\n"))
                self.run_preview("20 key Escape\n", extra=("--game-menu", "--game-core", core))
                self.assertTrue(
                    self.state.read_text().endswith("12 0 3\n"),
                    "Back from the restored picker must return to the shared pause menu"
                )

    def test_shader_choice_then_explicit_restart_saves_and_ends_the_pause_menu(self):
        path = self.root / "shader-restart.txt"
        path.write_text(
            "20 click 520 200\n60 key Right\n100 key Right\n140 key A\n180 key Escape\n220 key Down\n260 key Down\n300 key Left\n340 key Left\n380 key A\n"
        )
        for core in ("drastic", "dsperate", "nnddss"):
            with self.subTest(core=core):
                self.state.write_text('1 3 2 ""\n')
                for saved in (self.root / "library").glob("*/game.ini"):
                    saved.unlink()
                began = time.monotonic()
                result = subprocess.run(
                    [
                        str(BINARY), "--preview", "--game-menu", "--game-core", core, "--preferences",
                        str(self.prefs), "--state",
                        str(self.state), "--data",
                        str(self.root), "--input-script",
                        str(path), "--seconds", "5"
                    ],
                    cwd=ROOT,
                    env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                    capture_output=True,
                    text=True,
                    timeout=3
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertLess(time.monotonic() - began, 2)
                self.assertIn("filter=1\n", next((self.root / "library").glob("*/game.ini")).read_text())

    def test_dsperate_default_shader_requests_the_full_global_preset(self):
        self.prefs.write_text("schema=1\nmode=light\nfont=rounded\ncore=1\n")
        (self.root / "ui.ini").write_text("schema=1\nanimations=0\nfilter=14\nview=0\n")
        script = self.root / "default-shader.txt"
        script.write_text("20 click 520 200\n60 key A\n")
        result = subprocess.run(
            [
                str(BINARY), "--preview", "--game-menu", "--game-core", "dsperate", "--preferences",
                str(self.prefs), "--data",
                str(self.root), "--state",
                str(self.state), "--input-script",
                str(script), "--seconds", "1"
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
            capture_output=True,
            text=True,
            timeout=4
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Shader request (preview): shader:14", result.stdout)
        self.assertIn("filter=-1\n", next((self.root / "library").glob("*/game.ini")).read_text())
        self.assertIn("filter=14\n", (self.root / "ui.ini").read_text())

    def test_return_home_button_closes_each_shared_game_menu(self):
        path = self.root / "return-home.txt"
        # Three Downs reach the bottom grid row; a fourth would land on the Cheats bar below it.
        path.write_text("20 key Down\n60 key Down\n100 key Down\n160 key Right\n180 key A\n")
        for core in ("drastic", "dsperate", "nnddss", "melonds"):
            with self.subTest(core=core):
                began = time.monotonic()
                result = subprocess.run(
                    [
                        str(BINARY), "--preview", "--game-menu", "--game-core", core, "--preferences",
                        str(self.prefs), "--data",
                        str(self.root), "--input-script",
                        str(path), "--seconds", "5"
                    ],
                    cwd=ROOT,
                    env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                    capture_output=True,
                    text=True,
                    timeout=3
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertLess(
                    time.monotonic() - began, 2, "Return home must end the pause UI, not wait out the preview timeout"
                )

    def test_power_sleep_preferences_and_simulated_sleep_keep_selection(self):
        self.run_preview(
            """20 key A
40 key Right
60 key Right
80 key A
100 key Down
120 key A
140 key Right
180 key Right
200 key A
220 key Down
260 key Right
300 key Down
330 key Down
340 key Return
900 key Escape
1100 key Escape
1300 key Escape
""",
            extra=("--settings", "--section", "power")
        )
        self.assertEqual(
            self.system_prefs.read_text(),
            "schema=1\npower=2\nbrightness=60\nidle=2\nlid=1\nbattery_percent=1\ntimezone=Europe/Oslo\n"
        )
        self.assertFalse(self.prefs.exists())
        self.assertEqual(self.state.read_text().splitlines()[0], '1 3 2 ""')
        self.run_preview(
            "20 key A\n40 key Left\n50 key A\n60 key Escape\n100 key Escape\n140 key Escape\n",
            extra=("--settings", "--section", "power")
        )
        self.assertIn("power=1\n", self.system_prefs.read_text())
        self.assertIn("idle=2\nlid=1\nbattery_percent=1\ntimezone=Europe/Oslo\n", self.system_prefs.read_text())

    def test_new_settings_enable_lid_sleep_but_existing_off_stays_off(self):
        script = "20 key A\n40 key Right\n60 key A\n"
        self.run_preview(script, extra=("--settings", "--section", "power"))
        self.assertIn("lid=1\n", self.system_prefs.read_text())
        self.system_prefs.write_text("schema=1\npower=0\nbrightness=60\nidle=0\nlid=0\n")
        self.run_preview(script, extra=("--settings", "--section", "power"))
        self.assertIn("lid=0\n", self.system_prefs.read_text())

    def test_touch_brightness_slider_clamps_and_back_returns_to_appearance(self):
        self.run_preview(
            """10 wheel 0 -8
20 click 200 215
60 tap 596 219
100 key Right
140 tap 44 219
180 key Left
220 key Right
260 click 72 452
""",
            extra=("--settings", "--section", "theme")
        )
        self.assertIn("brightness=10\n", self.system_prefs.read_text())
        self.assertTrue(self.state.read_text().endswith("2 0 5\n"))
        self.run_preview("20 key Return\n60 key Right\n100 key Escape\n", extra=("--settings", "--section", "theme"))
        # Opening Settings explicitly resets focus; existing brightness remains intact.
        self.assertIn("brightness=10\n", self.system_prefs.read_text())

    def test_invalid_system_preferences_are_preserved_and_defaults_can_be_saved(self):
        self.system_prefs.write_text("schema=1\npower=999\nbrightness=0\nidle=99\nlid=2\n")
        self.run_preview(extra=("--settings", "--section", "power"))
        self.assertIn("power=999", self.system_prefs.read_text())
        self.run_preview(
            "20 key A\n40 key Right\n50 key A\n60 key Escape\n100 key Escape\n140 key Escape\n",
            extra=("--settings", "--section", "power")
        )
        self.assertEqual(
            self.system_prefs.read_text(),
            "schema=1\npower=1\nbrightness=60\nidle=0\nlid=1\nbattery_percent=1\ntimezone=Europe/Oslo\n"
        )

    def test_every_tzdata_zone_loads_and_left_right_jump_by_region(self):
        # Buenos Aires was not in the old short list; it must load, not fall back to defaults.
        self.system_prefs.write_text(
            "schema=1\npower=0\nbrightness=60\nidle=0\nlid=0\nbattery_percent=1\ntimezone=America/Argentina/Buenos_Aires\n"
        )
        # Right lands on the first Antarctica zone, Left on the first Americas zone; A saves it.
        self.run_preview(
            "20 key Down\n40 key A\n80 key Right\n120 key Left\n160 key A\n",
            extra=("--settings", "--section", "system")
        )
        self.assertIn("timezone=America/Adak\n", self.system_prefs.read_text())

    def test_saved_battery_percentage_choice_survives_other_system_changes(self):
        self.system_prefs.write_text(
            "schema=1\npower=0\nbrightness=60\nidle=0\nlid=0\nbattery_percent=0\ntimezone=Europe/Oslo\n"
        )
        self.run_preview("20 key A\n40 key Right\n60 key A\n", extra=("--settings", "--section", "power"))
        self.assertIn("power=1\n", self.system_prefs.read_text())
        self.assertIn("battery_percent=0\n", self.system_prefs.read_text())

    def test_brightness_drag_clamps_and_release_stops_changes(self):
        self.run_preview(
            """20 tap 56 219
60 drag 320 219
100 drag 700 219
140 release
180 drag 56 219
220 key Escape
260 key Escape
300 key Escape
340 key Escape
""",
            extra=("--settings", "--section", "brightness")
        )
        self.assertIn("brightness=100\n", self.system_prefs.read_text())

    def test_older_placeholder_power_state_opens_working_controls(self):
        self.state.write_text('1 3 2 ""\n3 1 0\n')
        self.run_preview("20 key A\n40 key Right\n50 key A\n60 key Escape\n100 key Escape\n140 key Escape\n")
        self.assertIn("power=1\n", self.system_prefs.read_text())

    def test_malformed_preferences_fall_back_without_crashing(self):
        self.prefs.write_text("schema=1\nmode=corrupt\nfont=anything\n")
        image = self.run_preview()
        self.assertEqual(self.background(image), (242, 245, 247))
        self.assertEqual(self.prefs.read_text(), "schema=1\nmode=corrupt\nfont=anything\n")

    def test_settings_and_font_variants_render_in_both_modes(self):
        for mode in ("light", "dark"):
            for section in ("theme", "power", "brightness"):
                shades = {}
                for font in ("rounded", "pixel"):
                    with self.subTest(mode=mode, font=font, section=section):
                        self.state.write_text('1 3 2 ""\n')
                        image = self.run_preview(
                            extra=("--settings", "--section", section, "--mode", mode, "--font", font)
                        )
                        self.assertEqual(self.background(image), (12, 17, 26) if mode == "dark" else (242, 245, 247))
                        # Measure only the section title in the top screen's note: rich
                        # icons/status glyphs are antialiased independently of the text font.
                        offset = struct.unpack_from("<I", image, 10)[0]
                        width, height = struct.unpack_from("<ii", image, 18)
                        shades[font] = len(
                            {
                                image[offset + ((height - 1 - y if height > 0 else y) * width + x) * 4:offset +
                                      ((height - 1 - y if height > 0 else y) * width + x) * 4 + 3]
                                for y in range(208, 244)
                                for x in range(28, 612)
                            }
                        )
                self.assertGreater(shades["rounded"], shades["pixel"], "Rounded text should have antialiased edges")


if __name__ == "__main__":
    unittest.main()
