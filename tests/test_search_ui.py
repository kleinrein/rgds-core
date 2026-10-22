"""Search, cancellation and full keyboard actions through real SDL input."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"
sys.path.insert(0, str(ROOT / "runtime"))
from fields import write_fields
from library import game_key


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class SearchUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        self.roms = self.root / "roms"
        self.roms.mkdir()
        for name in ("Mario Kart DS", "Super Mario 64 DS", "Zelda", "Animal Crossing"):
            (self.roms / (name + ".nds")).write_bytes(bytes(512))
        self.mario = self.data / "library" / game_key(self.roms / "Mario Kart DS.nds")
        write_fields(self.root / "preferences.ini", dict(schema=1, font="rounded", mode="light"))

    def run_ui(self, script, **ui):
        write_fields(self.root / "ui.ini", dict(schema=1, **ui))
        (self.root / "input").write_text(script)
        report = self.root / "report.json"
        result = subprocess.run(
            [
                str(BINARY), "--preview", "--library", "--rom-dir",
                str(self.roms), "--data",
                str(self.data), "--preferences",
                str(self.root / "preferences.ini"), "--state",
                str(self.root / "state"), "--input-script",
                str(self.root / "input"), "--seconds", "2", "--cache-report",
                str(report)
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
            capture_output=True,
            text=True,
            timeout=6
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        state = json.loads(report.read_text())
        self.assertEqual(state["render_file_reads"], 0)
        self.assertEqual(state["render_stamp_checks"], 0)
        self.assertFalse((self.data / "session/core.json").exists())
        return state

    def test_header_search_live_preview_and_submission(self):
        r = self.run_ui("100 click 270 46\n250 text   mario  \n")
        self.assertEqual(r["page"], 8)
        self.assertEqual(r["search_preview_games"], 2)
        self.assertFalse(r["search_preview_loading"])
        self.assertEqual(r["search_query"], "")  # Drafts do not apply filters.
        r = self.run_ui("100 click 270 46\n250 text   MARIO  \n550 click 550 394\n")
        self.assertEqual(r["page"], 0)
        self.assertEqual(r["search_query"], "MARIO")
        self.assertEqual(r["filtered_games"], 2)
        self.assertTrue(r["discovery_active"])
        self.assertEqual(r["discovery_description"], "MARIO")

    def test_l2_r2_popup_shortcuts_work_with_controller_and_touch(self):
        for opener in ("100 key Q\n250 joy-down 2\n280 joy-up 2\n", "100 key E\n250 click 180 120\n"):
            with self.subTest(opener=opener):
                r = self.run_ui(opener + "450 text zelda\n650 joy-down 7\n680 joy-up 7\n")
                self.assertEqual(r["search_query"], "zelda")
                self.assertEqual(r["filtered_games"], 1)
                self.assertFalse(r["picker_open"])

    def test_popup_filter_shortcut(self):
        r = self.run_ui("100 key Q\n250 key X\n")
        self.assertEqual(r["page"], 29)
        self.assertFalse(r["picker_open"])

    def test_empty_results_still_offer_popup_search_and_clear(self):
        script = "100 click 270 46\n250 text nonexistent\n400 key Return\n"
        r = self.run_ui(script + "650 key Q\n850 key Y\n")
        self.assertEqual(r["page"], 8)
        self.assertEqual(r["search_query"], "nonexistent")
        r = self.run_ui(script + "650 click 420 350\n")
        self.assertEqual(r["filtered_games"], 4)
        self.assertFalse(r["discovery_active"])

    def test_cancel_preserves_applied_search_and_original_page(self):
        r = self.run_ui(
            "100 click 270 46\n250 text mario\n400 key Return\n"
            "650 click 270 46\n800 text missing\n950 key Escape\n"
        )
        self.assertEqual(r["page"], 0)
        self.assertEqual(r["search_query"], "mario")
        self.assertEqual(r["filtered_games"], 2)

    def test_clear_cancels_pending_results_and_preserves_collection(self):
        write_fields(self.mario / "game.ini", dict(favourite=1))
        r = self.run_ui("100 click 270 46\n250 text missing\n400 key Return\n"
                        "450 click 593 46\n", view=1)
        self.assertFalse(r["discovery_active"])
        self.assertEqual(r["filtered_games"], 1)

    def test_preview_and_search_respect_favourites_and_played_filter(self):
        write_fields(self.mario / "game.ini", dict(favourite=1))
        write_fields(self.mario / "stats.ini", dict(sessions=1))
        # L2/R2 -> X Filters -> Played -> header search.
        r = self.run_ui(
            "100 key Q\n200 key X\n300 key Down\n320 key-up Down\n"
            "400 key Return\n500 key Down\n520 key-up Down\n"
            "600 key Down\n620 key-up Down\n700 key Return\n"
            "900 click 270 46\n1050 text mario\n",
            view=1
        )
        self.assertEqual(r["discovery_filter"], 2)
        self.assertEqual(r["search_preview_games"], 1)
        self.assertEqual(r["page"], 8)

    def test_full_keyboard_shift_numbers_punctuation_space_and_backspace(self):
        r = self.run_ui(
            "100 click 270 46\n200 click 50 344\n"  # Shift
            "300 click 123 344\n400 click 105 298\n"  # Z then a
            "500 click 270 392\n600 click 459 344\n"  # Space then dot
            "700 click 593 206\n800 click 73 206\n"  # Backspace then 1
            "900 click 513 252\n1100 click 550 392\n"
        )  # [ then Search
        self.assertEqual(r["search_query"], "Za 1[")

    def test_controller_uses_key_positions_for_vertical_navigation(self):
        r = self.run_ui(
            "100 click 270 46\n200 key Down\n220 key-up Down\n"
            "300 key Down\n320 key-up Down\n400 joy-down 0\n420 joy-up 0\n"
            "750 joy-down 7\n780 joy-up 7\n"
        )
        # q -> a -> z follows the staggered letter rows, past Tab/Caps/Shift.
        self.assertEqual(r["search_query"], "z")

    def test_caps_keeps_letters_uppercase_without_shifting_numbers(self):
        r = self.run_ui(
            "100 click 270 46\n200 click 47 298\n"  # Caps
            "300 click 105 298\n400 click 73 206\n"  # A then 1
            "500 click 50 344\n600 click 105 298\n"  # Shift + Caps -> a
            "700 click 50 344\n800 click 73 206\n"  # Shift + 1 -> !
            "1000 click 550 392\n"
        )
        self.assertEqual(r["search_query"], "A1a!")
        self.assertTrue(r["keyboard_caps"])

    def test_cursor_arrows_insert_into_the_middle_of_a_query(self):
        r = self.run_ui(
            "100 click 270 46\n250 text mrio\n"
            "400 click 436 392\n500 click 436 392\n600 click 436 392\n"
            "750 text a\n1000 click 550 392\n"
        )
        self.assertEqual(r["search_query"], "mario")
        self.assertEqual(r["filtered_games"], 2)

    def test_forward_delete_and_backspace_keep_utf8_cursor_boundaries(self):
        r = self.run_ui(
            "100 click 270 46\n250 text aéz\n350 click 436 392\n400 click 436 392\n450 click 436 392\n"
            "550 click 480 392\n700 click 100 392\n1000 key Return\n"
        )
        self.assertEqual(r["search_query"], "az")
        r = self.run_ui(
            "100 click 270 46\n250 text aéz\n400 click 436 392\n"
            "550 key Backspace\n700 text é\n1000 key Return\n"
        )
        self.assertEqual(r["search_query"], "aéz")

    def test_enter_tab_and_escape_keys_are_functional(self):
        r = self.run_ui("100 click 270 46\n250 text mario\n500 click 589 298\n")
        self.assertEqual(r["search_query"], "mario")
        r = self.run_ui(
            "100 click 270 46\n250 text zelda\n400 key Tab\n420 key-up Tab\n"
            "550 joy-down 0\n580 joy-up 0\n"
        )  # Tab jumps to the Search key, A presses it
        self.assertEqual(r["search_query"], "zelda")
        r = self.run_ui("100 click 270 46\n250 text missing\n500 click 41 392\n")
        self.assertEqual(r["page"], 0)
        self.assertFalse(r["discovery_active"])

    def test_accents_layer_types_accented_letters_and_returns_to_the_letters(self):
        # The key where Tab used to be opens the accents, as in PictoChat; it shows ABC while they are open.
        r = self.run_ui(
            "100 click 270 46\n200 click 41 252\n300 click 471 252\n"  # accents, then e-acute
            "400 click 41 252\n500 click 95 252\n600 click 550 392\n"
        )  # back to letters, then q
        self.assertEqual(r["search_query"], "\u00e9q")
        r = self.run_ui(
            "100 click 270 46\n200 click 41 252\n300 click 50 344\n"  # accents, Shift
            "400 click 471 252\n500 click 550 392\n"
        )
        self.assertEqual(r["search_query"], "\u00c9")

    def test_footer_submit_does_not_type_the_selected_key(self):
        r = self.run_ui("100 click 270 46\n250 text zelda\n500 click 530 451\n")
        self.assertEqual(r["search_query"], "zelda")
        self.assertEqual(r["filtered_games"], 1)

    def test_utf8_backspace_and_field_clear(self):
        r = self.run_ui("100 click 270 46\n250 text marioé\n400 key Backspace\n550 key Return\n")
        self.assertEqual(r["search_query"], "mario")
        r = self.run_ui("100 click 270 46\n250 text zelda\n400 click 588 122\n550 key Return\n")
        self.assertFalse(r["discovery_active"])
        self.assertEqual(r["filtered_games"], 4)

    def test_live_preview_discards_older_query(self):
        r = self.run_ui("100 click 270 46\n250 text mario\n500 click 588 122\n550 text zelda\n")
        self.assertEqual(r["search_preview_games"], 1)
        self.assertFalse(r["search_preview_loading"])


if __name__ == "__main__":
    unittest.main()
