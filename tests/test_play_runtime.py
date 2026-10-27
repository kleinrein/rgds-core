"""RG DS Play in the runtime: which engine a game gets, its launch plan, and the menu's view changes."""
import os
from pathlib import Path
import sys
import re
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import emulators
from game_menu import GameMenu
from library import game_key, read_fields, write_fields
import play_options


class Process:
    def __init__(self):
        self.result = None
        self.pid = 456
        self.signals = []

    def poll(self):
        return self.result

    def send_signal(self, value):
        self.signals.append(value)

    def terminate(self):
        self.result = 0

    def kill(self):
        self.result = -9

    def wait(self, timeout=None):
        return self.result


class Inputs:
    def poll(self):
        return False

    def close(self):
        pass


class PlayRuntimeTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.app, self.data = self.root / "app", self.root / "data"
        self.app.mkdir()
        self.gba = self.root / "Mario Kart - Super Circuit (Europe).gba"
        self.gba.write_bytes(bytes(0xC0))
        self.nds = self.root / "game.nds"
        self.nds.write_bytes(b"0123456789abTEST" + bytes(512))

    def binary(self, core):
        path = self.app / "emulators" / core / core
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
        return path

    def test_the_engine_follows_the_system_however_a_game_is_started(self):
        packed = self.root / "Pokemon.zip"
        with zipfile.ZipFile(packed, "w") as archive:
            archive.writestr("Pokemon Emerald.gba", b"x")
        ds_packed = self.root / "ds.zip"
        with zipfile.ZipFile(ds_packed, "w") as archive:
            archive.writestr("game.nds", b"x")
        cases = {
            self.gba: "gba",
            self.root / "a.gbc": "gbc",
            self.root / "a.gb": "gb",
            self.nds: "nds",
            packed: "gba",
            ds_packed: "nds",
            # libretro systems: RG DS Play too, with a core named per system
            self.root / "a.nes": "nes",
            self.root / "a.sfc": "snes",
            self.root / "a.smc": "snes",
            self.root / "a.md": "md",
            self.root / "a.sms": "sms",
            self.root / "a.gg": "gg",
            self.root / "a.cue": "psx",
            self.root / "a.chd": "psx",
            self.root / "a.z64": "n64",
            self.root / "a.v64": "n64",
            self.root / "a.p8": "pico8",
        }
        for rom, system in cases.items():
            self.assertEqual(play_options.system_of(rom), system, rom)
            expected = "rgds-play" if system != "nds" else "melonds"
            self.assertEqual(play_options.engine_for(rom, "melonds"), expected)

    def test_libretro_core_is_named_per_system_and_only_when_installed(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(play_options, "LIBRETRO_DIR", Path(tmp)):
                self.assertIsNone(play_options.libretro_core("nes"))  # not installed here
                (Path(tmp) / "fceumm_libretro.so").write_bytes(b"")
                self.assertEqual(play_options.libretro_core("nes"), Path(tmp) / "fceumm_libretro.so")
                self.assertIsNone(play_options.libretro_core("gba"))  # mGBA's, never a libretro core
                for system in ("md", "sms", "gg"):
                    self.assertEqual(play_options.LIBRETRO_CORES[system], "picodrive")
                self.assertEqual(play_options.LIBRETRO_CORES["psx"], "pcsx_rearmed")
                self.assertEqual(play_options.LIBRETRO_CORES["pico8"], "fake08")
                self.assertEqual(play_options.LIBRETRO_CORES["n64"], "mupen64plus_next")
        # Appended: the DS engines keep their stored indexes.
        self.assertEqual(emulators.CORES[:5], ("drastic", "dsperate", "nnddss", "melonds", "rgds-experimental"))
        self.assertEqual(emulators.CORES[5], "rgds-play")

    def test_roms_are_refused_by_the_wrong_engine(self):
        self.assertEqual(emulators.resolve_rom(self.gba, self.data, "rgds-play"), self.gba.resolve())
        with self.assertRaisesRegex(ValueError, "RG DS Play opens"):
            emulators.resolve_rom(self.nds, self.data, "rgds-play")
        with self.assertRaisesRegex(ValueError, "played with RG DS Play"):
            emulators.resolve_rom(self.gba, self.data, "melonds")
        self.assertEqual(emulators.resolve_rom(self.nds, self.data, "melonds"), self.nds.resolve())

    def test_a_gba_game_on_default_gets_the_gba_screen_and_its_own_choice_wins(self):
        self.binary("rgds-play")
        write_fields(self.data / "ui.ini", {"filter": "22"})  # the DS default: DS Lite
        plan = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)
        self.assertEqual(plan["env"]["RGDS_SHADER"], "30")
        write_fields(self.data / "library" / game_key(self.gba) / "game.ini", {"filter": "29"})
        plan = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)
        self.assertEqual(plan["env"]["RGDS_SHADER"], "29")
        # A look drawn for the DS picture (Natural Vision) is not offered for a GBA game: its own screen.
        write_fields(self.data / "library" / game_key(self.gba) / "game.ini", {"filter": "5"})
        plan = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)
        self.assertEqual(plan["env"]["RGDS_SHADER"], "30")
        # Another system's screen is never used: a DS screen on a GBA game falls back to the GBA screen.
        # Other systems get only the Original presets and their own screens: a Game Boy Color game on
        # Default (or a look drawn for the DS) gets its own screen, and keeps Off or Sharp when chosen.
        write_fields(self.data / "library" / game_key(self.gba) / "game.ini", {"filter": "22"})
        plan = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)
        self.assertEqual(plan["env"]["RGDS_SHADER"], "30")
        gbc = self.root / "game.gbc"
        self.assertEqual(play_options.shader_for(gbc, -1, 22), 32)
        self.assertEqual(play_options.shader_for(gbc, 27, 0), 32)
        self.assertEqual(play_options.shader_for(gbc, 30, 0), 32)
        self.assertEqual(play_options.shader_for(gbc, 31, 0), 32)
        self.assertEqual(play_options.shader_for(gbc, 0, 22), 0)
        self.assertEqual(play_options.shader_for(gbc, 1, 22), 1)
        self.assertEqual(play_options.shader_for(self.root / "game.gb", -1, 27), 31)
        self.assertEqual(play_options.shader_for(self.root / "game.nds", -1, 30), 0)
        self.assertEqual(play_options.shader_for(self.root / "game.nds", -1, 27), 27)
        self.assertEqual(play_options.shader_for(self.root / "game.nds", 31, 27), 27)

    def test_the_system_shader_groups_match_the_launchers_picker_families(self):
        import re
        catalog = (Path(__file__).resolve().parents[1] / "src/platform/shader_catalog.hpp").read_text()
        names = re.search(r"shader_family_names\[\] = \{([^}]*)\}", catalog).group(1)
        families = [n.strip().strip('"') for n in names.split(",")]
        order = re.search(r"shader_order\[\] = \{(.*?)\};", catalog, re.S).group(1)
        places = [(int(i), int(f)) for i, f in re.findall(r"\{(\d+), (\d+)\}", order)]
        self.assertEqual({i for i, f in places if families[f] == "DS screens"}, set(play_options.DS_SCREENS))
        self.assertEqual({i for i, f in places if families[f] == "GBA screens"}, set(play_options.GBA_SCREENS))
        self.assertEqual({i for i, f in places if families[f] == "GB screens"}, set(play_options.GB_SCREENS))
        self.assertEqual({i for i, f in places if families[f] == "GBC screens"}, set(play_options.GBC_SCREENS))
        self.assertEqual({i for i, f in places if families[f] == "Original"}, set(play_options.ORIGINAL))
        defaults = re.search(r"system_default_shader.*?shaders\[\] = \{([^}]*)\}", catalog, re.S).group(1)
        self.assertEqual([int(n) for n in defaults.split(",")], [play_options.SYSTEM_SHADERS[s] for s in ("gba", "gb", "gbc")])

    def test_launch_plan_uses_its_own_data_and_the_shared_menu(self):
        binary = self.binary("rgds-play")
        hook = self.app / "emulators/hooks/librgdsbridge.so"
        hook.parent.mkdir(parents=True)
        hook.write_bytes(b"bridge")
        play_options.save_view(self.data, self.gba, 1, 2, "black")
        plan = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)
        command, env = plan["command"], plan["env"]
        self.assertEqual(command[0], str(binary.resolve()))
        self.assertEqual(command[command.index("--rom") + 1], str(self.gba))
        self.assertEqual(command[command.index("--data") + 1], str(self.data / "emulators/rgds-play"))
        self.assertEqual(command[command.index("--fit") + 1], "integer")
        self.assertEqual(command[command.index("--second") + 1], "swap")
        self.assertEqual(command[command.index("--frame") + 1], "black")
        self.assertEqual(command[command.index("--frames") + 1], str(self.data / "frames/gba"))
        self.assertIn(str(binary.resolve().parent / "profiles/gba"), command)
        self.assertEqual(env["RGDS_CORE"], "rgds-play")
        self.assertEqual(env["RGDS_SHARED_MENU"], "1")
        self.assertIn("librgdsbridge.so", env["LD_PRELOAD"])
        self.assertEqual(Path(env["RGDS_STATE_SLOT"]).parent, self.data / "emulators/rgds-play/states")
        self.assertTrue(env["RGDS_AUTOSTATE_FILE"].endswith("auto-rgds-play.dss"))
        self.assertIn("a:b0", env["SDL_GAMECONTROLLERCONFIG"])
        # The DS library's own files are untouched by planning a GBA launch.
        self.assertFalse((self.data / "library" / game_key(self.gba) / "game.ini").exists())

    def test_auto_load_happens_before_the_first_frame(self):
        # Like melonDS: the player loads the checkpoint itself, so the game's boot never shows first.
        self.binary("rgds-play")
        hook = self.app / "emulators/hooks/librgdsbridge.so"
        hook.parent.mkdir(parents=True)
        hook.write_bytes(b"bridge")
        write_fields(self.data / "preferences.ini", dict(schema=1, core=4, auto_load=1, auto_save=1))
        checkpoint = self.data / "library" / game_key(self.gba) / "auto-rgds-play.dss"
        checkpoint.parent.mkdir(parents=True)
        checkpoint.write_bytes(b"checkpoint")
        env = emulators.launch_plan("rgds-play", self.app, self.data, self.gba)["env"]
        self.assertEqual(env["RGDS_NATIVE_AUTOLOAD"], "1")
        self.assertNotEqual(env.get("RGDS_AUTOLOAD"), "1")  # no F7 from the bridge seconds later
        self.assertEqual(Path(env["RGDS_STATE_SLOT"]).read_bytes(), b"checkpoint")

    def test_a_restart_keeps_other_systems_in_rgds_play(self):
        write_fields(self.data / "preferences.ini", dict(schema=1, core=4))
        with patch.object(emulators, "_run_once", side_effect=[75, 0]) as launch:
            self.assertEqual(emulators.run("rgds-play", self.app, self.data, self.gba), 0)
        self.assertEqual([c.args[0] for c in launch.call_args_list], ["rgds-play", "rgds-play"])
        with patch.object(emulators, "_run_once", side_effect=[75, 0]) as launch:
            emulators.run("melonds", self.app, self.data, self.nds)
        self.assertEqual(launch.call_args_list[1].args[0], "rgds-experimental")

class PlayArtworkTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_gba_covers_come_from_the_gba_catalogue(self):
        import public_artwork as art
        rom = self.root / "Mario Kart - Super Circuit (Europe).gba"
        rom.write_bytes(bytes(0xAC) + b"AMKP" + bytes(16))
        self.assertEqual(art.rom_identity(rom), ("gba", "Mario Kart - Super Circuit (Europe)", "AMKP"))
        client = art.PublicClient("gametdb")  # GameTDB has DS covers only: other systems still get Libretro's
        with patch.object(client, "fetch", return_value=b"png") as fetch:
            raw, meta = client.artwork(rom)
        self.assertEqual(raw, b"png")
        url = fetch.call_args.args[0]
        self.assertIn("/Nintendo%20-%20Game%20Boy%20Advance/Named_Boxarts/", url)
        self.assertIn("Mario%20Kart%20-%20Super%20Circuit%20%28Europe%29.png", url)
        self.assertEqual(meta["provider"], "Libretro")

    def test_a_renamed_gba_dump_is_found_by_its_game_code(self):
        import public_artwork as art
        import scraper
        rom = self.root / "mksc.gba"
        rom.write_bytes(bytes(0xAC) + b"AMKP" + bytes(16))
        client = art.PublicClient("auto")
        client.indexes["gba"] = ["Mario Kart - Super Circuit (Europe)", "Mario Kart - Super Circuit (USA)"]
        client.serial_sets["gba"] = {"AMKP": ["Mario Kart - Super Circuit (Europe)"]}
        with patch.object(client, "fetch", side_effect=[scraper.NotFound(), b"png"]) as fetch:
            _, meta = client.artwork(rom)
        self.assertEqual(meta["provider_id"], "Mario Kart - Super Circuit (Europe)")
        self.assertEqual(meta["artwork_match"], "game_code")
        self.assertIn("Game%20Boy%20Advance", fetch.call_args.args[0])

    def test_the_scraper_lists_other_systems_folders(self):
        import scraper
        (self.root / "GBA").mkdir()
        (self.root / "GBA/A.gba").write_bytes(b"x")
        (self.root / "GBA/notes.txt").write_bytes(b"x")
        (self.root / "GB").mkdir()
        (self.root / "GB/B.gb").write_bytes(b"x")
        found = scraper.system_games(["gba=" + str(self.root / "GBA"), "gb=" + str(self.root / "GB"), "nds=/x"])
        self.assertEqual(sorted(p.name for p in found), ["A.gba", "B.gb"])


class PlayMenuTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.clock = 100
        clock = patch("game_menu.time.monotonic", side_effect=lambda: self.clock)
        clock.start()
        self.addCleanup(clock.stop)
        self.rom = self.root / "Mario Kart - Super Circuit (Europe).gba"
        self.rom.write_bytes(bytes(0xC0))
        self.child, self.ui = Process(), Process()

    def menu(self, core):
        menu = GameMenu(self.child, self.root / "app", self.root / "data", core, self.rom, {}, Inputs())
        self.addCleanup(menu.close)
        with patch("game_menu.subprocess.Popen", return_value=self.ui):
            menu.open()
            menu.ready.write_text(str(self.ui.pid))
        return menu

    def request(self, menu, action, token="1"):
        write_fields(menu.request, dict(token=token, action=action))
        menu.poll()

    def result(self, menu, state):
        write_fields(Path(str(menu.control) + ".result"), dict(token=menu.pending["token"], state=state))
        self.clock += 1
        menu.poll()

    def test_view_change_is_saved_only_once_the_player_applied_it(self):
        menu = self.menu("rgds-play")
        view = self.root / "data/library" / game_key(self.rom) / "play.ini"
        self.request(menu, "play-graphics:2:1:indigo")
        self.assertEqual(menu.pending["action"], "play-graphics:2:1:indigo")
        self.assertIn("play-graphics:2:1:indigo", menu.control.read_text())
        self.assertFalse(view.exists())
        self.assertEqual(read_fields(menu.status)["message"], "Applying graphics…")
        self.result(menu, "play-graphics-applied")
        self.assertIsNone(menu.pending)
        self.assertEqual(read_fields(view)["fit"], "smooth")
        self.assertEqual(read_fields(view)["second"], "off")
        self.assertEqual(read_fields(menu.status)["state"], "paused")
        self.assertEqual(read_fields(view)["frame"], "indigo")
        self.assertEqual(
            play_options.view(self.root / "data", self.rom), {
                "fit": "smooth",
                "second": "off",
                "frame": "indigo",
                "palette": "dmg",
                "colour": "gbc"
            }
        )

    def test_colours_are_saved_beside_the_view_once_the_player_applied_them(self):
        menu = self.menu("rgds-play")
        view = self.root / "data/library" / game_key(self.rom) / "play.ini"
        self.request(menu, "play-graphics:2:1:indigo")
        self.result(menu, "play-graphics-applied")
        self.request(menu, "play-color:1:1", token="2")
        self.assertIn("play-color:1:1", menu.control.read_text())
        self.assertEqual(read_fields(menu.status)["message"], "Applying graphics…")
        self.assertNotIn("palette", read_fields(view))
        self.result(menu, "play-color-applied")
        shown = play_options.view(self.root / "data", self.rom)
        self.assertEqual((shown["palette"], shown["colour"], shown["frame"]), ("pocket", "gbc-dim", "indigo"))
        # A view change afterwards keeps the colours.
        self.request(menu, "play-graphics:0:0:off", token="3")
        self.result(menu, "play-graphics-applied")
        self.assertEqual(play_options.view(self.root / "data", self.rom)["palette"], "pocket")

    def test_palette_and_correction_names_match_the_players_list(self):
        # play/src/ui/gb_palettes.hpp is the one list the player and the launcher read; the runtime repeats its slugs.
        header = (Path(__file__).resolve().parents[1] / "play/src/ui/gb_palettes.hpp").read_text()
        palettes = re.findall(r'\{"([a-z-]+)", "[^"]+", \{0x', header)
        corrections = re.findall(r'\{"([a-z-]+)", "[^"]+"\},', header)
        self.assertEqual(tuple(palettes), play_options.PALETTES)
        self.assertEqual(tuple(corrections), play_options.CORRECTIONS)

    def test_frame_names_and_systems_match_the_players_list(self):
        # play/src/ui/frame_styles.hpp is the one list the player and the launcher read; the runtime repeats it.
        header = (Path(__file__).resolve().parents[1] / "play/src/ui/frame_styles.hpp").read_text()
        styles = re.findall(r'^    \{"([a-z0-9-]+)", "([a-z0-9,]*)", "[^"]+", FrameKind::', header, re.M)
        self.assertEqual(dict(styles), play_options.FRAMES)
        self.assertEqual([slug for slug, _ in styles], list(play_options.FRAMES))

    def test_a_frame_is_known_only_for_its_own_system(self):
        data = self.root / "data"
        gb, gbc = self.root / "Tetris.gb", self.root / "Wario Land 3.gbc"
        for rom, frame, known in ((self.rom, "indigo", True), (self.rom, "sp-cobalt", True), (self.rom, "dmg", False),
                                  (gb, "dmg", True), (gb, "pocket-black", True), (gb, "indigo", False),
                                  (gb, "sp-onyx", False), (gb, "gbc-berry", False), (gbc, "gbc-teal", True),
                                  (gbc, "dmg", False), (gb, "bar", True), (gbc, "off", True)):
            self.assertEqual(play_options.frame_known(data, rom, frame), known, (rom.suffix, frame))
        # An old GBA frame kept for a Game Boy game falls back to off; the owner's own picture of that name is kept.
        save = lambda: play_options.save_view(data, gb, 0, 0, "indigo")
        save()
        self.assertEqual(play_options.view(data, gb)["frame"], "off")
        (data / "frames/gb").mkdir(parents=True)
        (data / "frames/gb/indigo.png").write_bytes(b"")
        self.assertEqual(play_options.view(data, gb)["frame"], "indigo")

    def test_a_failed_colour_change_keeps_the_saved_colours(self):
        menu = self.menu("rgds-play")
        self.request(menu, "play-color:1:0")
        self.result(menu, "play-color-failed")
        self.assertEqual(read_fields(menu.status)["state"], "error")
        self.assertFalse((self.root / "data/library" / game_key(self.rom) / "play.ini").exists())

    def test_colour_requests_are_refused_for_other_engines_and_bad_values(self):
        for core, action in (
            ("rgds-experimental", "play-color:1:0"), ("rgds-play", "play-color:8:0"), ("rgds-play", "play-color:0:3"),
            ("rgds-play", "play-color:x:0"), ("rgds-play", "play-color:1"), ("rgds-play", "play-color:1:0:2"),
            ("rgds-play", "play-color:-1:0")
        ):
            menu = self.menu(core)
            self.request(menu, action)
            self.assertIsNone(menu.pending, action)
            self.assertEqual(read_fields(menu.status)["state"], "error", action)

    def test_a_failed_view_change_keeps_the_saved_view(self):
        menu = self.menu("rgds-play")
        self.request(menu, "play-graphics:1:0:off")
        self.result(menu, "play-graphics-failed")
        self.assertEqual(read_fields(menu.status)["state"], "error")
        self.assertFalse((self.root / "data/library" / game_key(self.rom) / "play.ini").exists())

    def test_a_picture_frame_from_the_owners_folder_is_taken_by_name(self):
        folder = self.root / "data/frames/gba"
        folder.mkdir(parents=True)
        (folder / "Perfect GBA (mugwomp93).png").write_bytes(b"\x89PNG")
        menu = self.menu("rgds-play")
        self.request(menu, "play-graphics:0:0:Perfect GBA (mugwomp93)")
        self.result(menu, "play-graphics-applied")
        self.assertEqual(
            play_options.view(self.root / "data", self.rom)["frame"], "Perfect GBA (mugwomp93)"
        )
        # A picture that is gone falls back to no frame at the next launch.
        (folder / "Perfect GBA (mugwomp93).png").unlink()
        self.assertEqual(play_options.view(self.root / "data", self.rom)["frame"], "off")

    def test_view_changes_are_refused_for_other_engines_and_bad_values(self):
        for core, action in (
            ("rgds-experimental", "play-graphics:0:0:off"), ("rgds-play", "play-graphics:3:0:off"),
            ("rgds-play", "play-graphics:0:0"), ("rgds-play", "play-graphics:0:0:no such picture"),
            ("rgds-play", "play-graphics:0:0:../escape")
        ):
            with self.subTest(core=core, action=action):
                menu = self.menu(core)
                self.request(menu, action)
                self.assertIsNone(menu.pending)
                self.assertEqual(read_fields(menu.status)["state"], "error")

    def test_the_switcher_offers_other_systems_and_picks_their_engine(self):
        menu = self.menu("rgds-experimental")
        nds = self.root / "game.nds"
        nds.write_bytes(b"x")
        write_fields(
            menu.folder / "switcher-library.ini",
            dict(game_0_rom=str(self.rom), game_0_title="Super Circuit", game_1_rom=str(nds), game_1_title="DS")
        )
        menu.rom = nds
        menu.prepare_switcher()
        self.assertIn(self.rom.resolve(), [game[0] for game in menu.switch_games])
        index = [game[0] for game in menu.switch_games].index(self.rom.resolve())
        write_fields(self.root / "data/preferences.ini", dict(schema=1, core=4))
        self.request(menu, "switch:" + str(index))
        self.assertEqual(menu.switch_target.core, "rgds-play")
        self.assertEqual(menu.pending["action"], "save-quit")


if __name__ == "__main__":
    unittest.main()
