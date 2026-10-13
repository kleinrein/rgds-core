"""Library gestures, icon rendering, account entry and emulator settings through SDL."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
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

    def run_ui(self, script, seconds=2, extra=(), initial_library=True):
        path = self.root / "input.txt";path.write_text(script)
        entry = ["--library"] if initial_library and "--home" not in extra else []
        result = subprocess.run([str(BINARY), "--preview", *entry, "--data", str(self.data), "--preferences", str(self.prefs),
                                 "--state", str(self.state), "--input-script", str(path), "--seconds", str(seconds),
                                 "--screenshot", str(self.bmp), *extra], cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def selected(self): return int(self.state.read_text().split()[1])

    def test_drag_scrolls_and_release_snaps_without_launch(self):
        self.run_ui("20 tap 320 205\n60 drag 40 205\n100 release\n")
        self.assertEqual(self.selected(), 2)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_adjacent_tile_click_selects_and_endpoints_wrap(self):
        self.run_ui("20 click 456 205\n")
        self.assertEqual(self.selected(), 1)
        self.state.write_text('1 0 0 ""\n')
        self.run_ui("20 key Left\n300 key Escape\n")
        self.assertEqual(self.selected(), 4)

    def test_full_width_map_seeks_to_end_and_drag_clamps(self):
        self.run_ui("20 tap 534 438\n60 drag 700 438\n100 release\n")
        self.assertEqual(self.selected(), 4)
        self.run_ui("20 tap 320 205\n60 drag 1000 205\n100 release\n")
        self.assertEqual(self.selected(), 0)

    def test_dsi_range_arrows_step_and_wrap_without_launching(self):
        self.run_ui("20 click 33 438\n300 key Escape\n")
        self.assertEqual(self.selected(),4)
        self.run_ui("20 click 605 438\n300 key Escape\n")
        self.assertEqual(self.selected(),0)
        self.assertFalse((self.data/"session/core.json").exists())

    def test_root_favourites_card_opens_an_empty_child(self):
        self.run_ui("20 key Right\n40 key-up Right\n60 key A\n", extra=("--home",))
        self.assertIn("view=1", (self.root / "ui.ini").read_text())
        self.assertEqual(self.selected(), 0)

    def test_fresh_start_defaults_to_games_in_the_root_carousel(self):
        self.state.unlink()
        self.run_ui("", seconds=1, initial_library=False)
        self.assertIn("home 1", self.state.read_text().splitlines())

    def test_root_recents_card_opens_an_empty_child(self):
        self.run_ui("20 key Right\n40 key-up Right\n60 key Right\n80 key-up Right\n100 key A\n", extra=("--home",))
        self.assertIn("view=2", (self.root / "ui.ini").read_text())

    def test_root_settings_card_opens_settings_and_footer_returns_home(self):
        self.run_ui("20 key Left\n40 key-up Left\n60 key A\n", extra=("--home",))
        self.assertIn("1 0 0", self.state.read_text().splitlines())
        self.assertIn("home 0", self.state.read_text().splitlines())
        self.assertFalse((self.data / "session/core.json").exists())
        self.state.write_text('1 0 0 ""\n')
        self.run_ui("20 key Left\n40 key-up Left\n60 key A\n100 click 72 452\n140 key Right\n160 key-up Right\n180 key A\n220 key Right\n240 key-up Right\n260 key Right\n280 key-up Right\n300 key X\n340 key A\n", extra=("--home",))
        self.assertEqual(self.selected(), 2)
        self.assertIn("favourite=1", next((self.data / "library").glob("*/game.ini")).read_text())

    def test_root_adjacent_card_selects_and_center_card_opens_settings(self):
        self.run_ui("20 click 184 205\n300 click 320 205\n", extra=("--home",))
        self.assertIn("1 0 0", self.state.read_text().splitlines())
        self.assertIn("home 0", self.state.read_text().splitlines())

    def test_back_from_empty_favourites_retains_root_category_selection(self):
        self.run_ui("20 key Right\n40 key-up Right\n60 key A\n100 click 52 48\n", extra=("--home",))
        self.assertIn("home 2", self.state.read_text().splitlines())
        self.run_ui("20 key Left\n40 key-up Left\n60 key A\n", extra=("--home",))
        self.assertIn("view=0", (self.root / "ui.ini").read_text(), "Back must retain Favourites so one step left opens Games")
        self.assertFalse((self.data / "session/core.json").exists())

    def test_keyboard_back_to_root_and_reenter_games_preserves_game_selection(self):
        self.state.write_text('1 2 0 ""\n')
        self.run_ui("20 key Escape\n60 key A\n100 key Right\n140 key-up Right\n")
        self.assertEqual(self.selected(), 3)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_repeated_back_stays_home_and_later_navigation_still_works(self):
        self.run_ui("20 key Escape\n60 key Escape\n100 joy-down 1\n120 joy-up 1\n160 key Right\n180 key-up Right\n", seconds=1)
        self.assertIn("home 2", self.state.read_text().splitlines())

    def test_restored_game_options_back_stays_in_child_without_library_flag(self):
        self.state.write_text('1 2 0 ""\n19 2 0\n')
        self.run_ui("20 key Escape\n60 key Right\n100 key-up Right\n", initial_library=False)
        self.assertEqual(self.selected(), 3)
        self.assertFalse(any(line.startswith("home ") for line in self.state.read_text().splitlines()))

    def test_back_cancels_a_child_drag_before_release(self):
        self.run_ui("20 tap 320 205\n60 key Escape\n100 release\n")
        self.assertIn("home 1", self.state.read_text().splitlines())
        self.assertFalse((self.data / "session/core.json").exists())

    def test_blank_root_placeholder_tap_does_not_open_selected_settings(self):
        self.run_ui("20 key Left\n40 key-up Left\n100 click 40 205\n", extra=("--home",))
        self.assertEqual(self.state.read_text().splitlines(), ['1 0 0 ""', "home 0"])

    def test_emulator_choice_is_saved_in_library_settings(self):
        self.run_ui("20 key A\n40 key Right\n60 key Right\n80 key A\n100 key Escape\n140 key Escape\n180 key Escape\n",
                    extra=("--settings", "--section", "library"))
        # DSperate starts at core 1; two explicit steps select melonDS (3).
        self.assertIn("core=3\n", self.prefs.read_text())
        self.assertEqual(self.selected(), 0)

    def test_old_account_provider_migrates_to_public_artwork(self):
        self.prefs.write_text("schema=1\nmode=light\nfont=rounded\n")
        (self.root / "ui.ini").write_text("schema=1\nartwork_provider=3\n")
        self.run_ui("", seconds=1, extra=("--settings", "--section", "scraper"))
        self.assertFalse((self.data / "private/screenscraper-account.json").exists())
        status = (self.data / "scraper/status.ini").read_text()
        self.assertIn("Ready to fetch artwork.", status)
        self.assertIn("provider=auto", status)

    def test_browser_layout_can_be_selected_by_touch_and_survives_restart(self):
        self.run_ui("20 key Y\n160 click 510 244\n260 click 530 452\n")
        self.assertIn("layout=2",(self.root / "ui.ini").read_text())
        self.run_ui("20 key Down\n40 key-up Down\n")
        self.assertEqual(self.selected(),1)

    def test_combined_view_modal_changes_collection_and_layout_without_reopening(self):
        self.run_ui("20 click 460 46\n160 click 310 146\n260 click 510 244\n360 click 530 452\n")
        saved = (self.root / "ui.ini").read_text()
        self.assertIn("view=1", saved)
        self.assertIn("layout=2", saved)
        self.run_ui("", seconds=1)
        self.assertIn("view=1", (self.root / "ui.ini").read_text())
        self.assertIn("layout=2", (self.root / "ui.ini").read_text())
        self.assertFalse((self.data / "session/core.json").exists())

    def test_combined_view_modal_controller_navigates_both_rows(self):
        self.run_ui("20 key Y\n100 key Right\n120 key-up Right\n180 key A\n"
                    "260 key Down\n280 key-up Down\n340 key Right\n360 key-up Right\n"
                    "420 key A\n500 key B\n")
        saved = (self.root / "ui.ini").read_text()
        self.assertIn("view=1", saved)
        self.assertIn("layout=2", saved)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_list_swipe_changes_selection_without_launching(self):
        self.prefs.write_text("schema=1\nmode=light\nfont=rounded\n")
        (self.root / "ui.ini").write_text("schema=1\nlayout=2\n")
        self.run_ui("20 tap 160 205\n80 drag 160 96\n120 release\n")
        self.assertEqual(self.selected(),0)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_letter_picker_touch_closes_and_jumps_without_launching(self):
        self.run_ui("20 key Y\n100 click 170 320\n200 click 80 184\n")
        self.assertEqual(self.selected(),0)
        self.assertFalse((self.data / "session/core.json").exists())

    def test_cover_neighbours_are_loaded_before_selection(self):
        import sys
        sys.path.insert(0,str(ROOT/'runtime'))
        from library import game_key
        roms=self.root/'roms';roms.mkdir()
        for i,color in enumerate((b'\x00\x00\xff',b'\x00\xff\x00',b'\xff\x00\x00')):
            rom=roms/('Game %d.nds'%i);rom.write_bytes(b'header')
            folder=self.data/'library'/game_key(rom);folder.mkdir(parents=True)
            # A 32x32 uncompressed 24-bit BMP fixture, no third-party artwork.
            pixels=(color*32)*32
            header=struct.pack('<2sIHHI',b'BM',54+len(pixels),0,0,54)+struct.pack('<IiiHHIIiiII',40,32,32,1,24,0,len(pixels),0,0,0,0)
            (folder/'box.bmp').write_bytes(header+pixels)
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        (self.root/'ui.ini').write_text('schema=1\nlayout=1\n')
        self.run_ui('',seconds=1,extra=('--rom-dir',str(roms)))
        image=self.bmp.read_bytes();offset=struct.unpack_from('<I',image,10)[0];width,height=struct.unpack_from('<ii',image,18)
        def pixel(x,y):return image[offset+((height-1-y)*width+x)*4:offset+((height-1-y)*width+x)*4+3]
        self.assertEqual(pixel(320,480+230),b'\x00\x00\xff')
        self.assertEqual(pixel(478,480+230),b'\x00\xff\x00')
        self.assertEqual(pixel(630,480+230),b'\xff\x00\x00')
        self.assertEqual(self.selected(),0)

    def test_list_rail_and_row_hit_use_same_scrolled_viewport(self):
        roms=self.root/'roms';roms.mkdir()
        for i in range(30):(roms/('Game %02d.nds'%i)).write_bytes(b'header')
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        (self.root/'ui.ini').write_text('schema=1\nlayout=2\n')
        self.run_ui('20 tap 607 170\n60 drag 607 400\n100 release\n220 click 160 224\n',extra=('--rom-dir',str(roms)))
        self.assertEqual(self.selected(),23)
        self.assertFalse((self.data/'session/core.json').exists())

    def test_artwork_match_editor_saves_plain_title_without_renaming_game(self):
        self.run_ui("""20 key X
80 key Down
100 key-up Down
140 key Down
160 key-up Down
200 key Down
220 key-up Down
260 key Down
280 key-up Down
320 key A
400 key A
500 text Puzzle 2
600 key Return
620 key-up Return
""")
        matches=list((self.data/'library').glob('*/artwork.ini'))
        self.assertEqual(len(matches),1)
        self.assertIn('match_title=Puzzle 2',matches[0].read_text())
        self.assertEqual(self.selected(),0)
        self.assertFalse((self.data/'session/core.json').exists())

    def test_native_banner_icons_are_rendered(self):
        roms = self.root / "roms";roms.mkdir()
        content = bytearray(0x640)
        struct.pack_into("<I", content, 0x68, 0x200)
        content[0x220:0x420] = bytes([0x11]) * 512
        struct.pack_into("<H", content, 0x422, 31)
        title = "Original homebrew".encode("utf-16le")
        content[0x540:0x540+len(title)] = title
        (roms / "homebrew.nds").write_bytes(content)
        self.run_ui("", seconds=1, extra=("--rom-dir", str(roms)))
        image = self.bmp.read_bytes()
        offset = struct.unpack_from("<I", image, 10)[0]
        width, height = struct.unpack_from("<ii", image, 18)
        # Centre of the native DS icon on the bottom screen; solid palette-red.
        x, y = 320, 480+247
        row = height-1-y if height>0 else y
        self.assertEqual(image[offset + (row*width+x)*4:offset + (row*width+x)*4+3], b"\x00\x00\xff")


if __name__ == "__main__": unittest.main()
