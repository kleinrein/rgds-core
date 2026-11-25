"""The second card as a games library: path mapping (play_options, the launcher's games folders) and Bring over
reading a card that is played in place (migrate.py). The mount helper has tests/test_games_card.py."""
from pathlib import Path
import re
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import migrate
from library import game_key, playtime
from play_options import FOLDER_SYSTEMS, SYSTEMS, system_of

CARD = Path("/media/tf2")


def write(path, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    return path


def launcher_folders():
    """src/launcher/model.hpp's games folders: name -> (shelf, the player's own system or '')."""
    source = (ROOT / "src/launcher/model.hpp").read_text()
    start = source.index("inline const GamesFolder* games_folder(const std::string& name)")
    table = source[start:source.index("for (const auto& f : folders)", start)]
    return {name: (shelf, own) for name, shelf, own in re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]*)"\}', table)}


def shelf(system):
    return {"gg": "sms", "sg": "sms", "segacd": "md", "pcecd": "pce"}.get(system, system)


class PathMappingTests(unittest.TestCase):
    def test_every_folder_bring_over_knows_is_a_games_folder_in_the_launcher_and_the_runtime(self):
        folders = launcher_folders()
        for table in (migrate.FOLDERS, migrate.STOCK_FOLDERS):
            for name, ours in table.items():
                if name == "nds": continue
                with self.subTest(folder=name):
                    self.assertIn(name, folders)
                    runtime = FOLDER_SYSTEMS.get(name, name if name in SYSTEMS else None)
                    self.assertIsNotNone(runtime)
                    # the same shelf in both; PICO is the one stock name that means another system (games_card.hpp)
                    if not (table is migrate.STOCK_FOLDERS and name == "pico"):
                        self.assertEqual(folders[name][0], shelf(ours))
                        self.assertEqual(shelf(runtime), shelf(ours))

    def test_a_game_on_a_stock_card_is_the_same_system_as_its_copy_here(self):
        # Saves go by system and name (migrate.save_destination, the players): a game played from the card keeps
        # the save Bring over put in place for it, and a copy of it on this card would share it.
        for folder, name in (
            ("NDS", "Kart.nds"), ("GBA", "Wars.gba"), ("GB", "Tetris.gb"), ("FC", "Mario.nes"), ("SFC", "Zelda.sfc"),
            ("MD", "Sonic.bin"), ("SEGA32X", "Knuckles.32x"), ("MDCD", "Sonic CD.cue"), ("GG", "Shinobi.gg"),
            ("SMS", "Wonder Boy.sms"), ("PS", "Spyro.cue"), ("PS", "Crash.pbp"), ("PSP", "Patapon.iso"),
            ("DREAMCAST", "Crazy Taxi.chd"), ("PCECD", "Ys.cue"), ("A2600", "Pitfall.bin"), ("A5200", "Galaxian.bin"),
            ("A7800", "Joust.bin"), ("WS", "Gunpey.ws"), ("NGP", "Metal Slug.ngc"), ("PICO", "celeste.p8.png"),
            ("PICO", "jelpi.p8"), ("FBNEO", "mslug.zip"), ("C64", "Elite.d64"), ("VB", "Wario.vb")
        ):
            with self.subTest(folder=folder, game=name):
                game = migrate.Game(folder, name, 1, folders=migrate.STOCK_FOLDERS)
                here = system_of(game.destination(Path("/data/roms")))
                self.assertEqual(system_of(CARD / "Roms" / folder / name), here)
        # ours on the card: roms/pico is the Sega Pico, as on /data/roms
        self.assertEqual(system_of(CARD / "roms/pico/Game.bin"), "pico")
        self.assertNotEqual(system_of(CARD / "Roms/PICO/Game.bin"), "pico")


class LiveCardTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.card = self.root / "card"
        self.data = self.card / "rgds/userdata"
        self.data.mkdir(parents=True)
        self.paths = {
            "player": self.data,
            "roms": self.card / "roms",
            "bios": self.card / "rgds/bios",
            "work": self.card / "rgds/import",
            "folder": self.card / "import",
        }
        self.tf2 = self.root / "tf2"

    def test_a_stock_card_played_in_place_keeps_its_games_and_gets_its_saves(self):
        write(self.tf2 / "Roms/NDS/Kart.nds", b"N" * 64)
        write(self.tf2 / "Roms/SFC/Zelda.sfc", b"Z" * 8)
        write(self.tf2 / "saves_RA/Roms/SFC/Zelda.srm", b"snes-save")
        source = migrate.Source(self.tf2, "slot", migrate.layout(self.tf2), live=True)
        plan = migrate.Plan(source, self.paths)
        summary = plan.summary()
        self.assertEqual(summary["import_games"], 2)
        self.assertEqual((summary["import_games_here"], summary["import_games_copy"]), (2, 0))
        result = plan.apply(games=True)
        self.assertEqual(result["games"], 0)
        self.assertFalse((self.card / "roms").exists())  # nothing copied over
        self.assertEqual((self.data / "emulators/rgds-play/saves/snes/Zelda.sav").read_bytes(), b"snes-save")

    def test_play_time_and_favourites_go_to_the_games_where_they_are(self):
        write(self.tf2 / "banana/playstats.tsv", "gba/Wars.gba\t3\t5400\t1700000000\n")
        write(self.tf2 / "banana/favorites.txt", "gba/Wars.gba\n")
        rom = write(self.tf2 / "roms/gba/Wars.gba", b"G" * 32)
        plan = migrate.Plan(migrate.Source(self.tf2, "slot", "bananaos", live=True), self.paths)
        plan.apply()
        # the launcher keys a game by its resolved path: the card's
        self.assertEqual(playtime(self.data, rom), (5400, 3))
        self.assertIn("favourite=1", (self.data / "library" / game_key(rom) / "game.ini").read_text())
        self.assertFalse((self.data / "library" / game_key(self.card / "roms/gba/Wars.gba")).exists())

    def test_a_card_mounted_for_play_is_read_where_it_is_and_left_mounted(self):
        write(self.tf2 / "Roms/SFC/Zelda.sfc", b"Z")
        write(self.tf2 / "saves_RA/Roms/SFC/Zelda.srm", b"save")
        with unittest.mock.patch.object(migrate, "other_partitions", return_value=[("mmcblk2p1", "mmcblk2")]), \
                unittest.mock.patch.object(migrate, "mounted_at", return_value=self.tf2), \
                unittest.mock.patch.object(migrate, "mount") as mount, \
                unittest.mock.patch.object(migrate.subprocess, "run") as run:
            found = migrate.sources(self.paths)
        mount.assert_not_called()
        run.assert_not_called()  # no umount of the games card
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].live)
        self.assertEqual(found[0].games[0].destination(self.paths["roms"]), self.tf2 / "Roms/SFC/Zelda.sfc")

    def test_an_old_card_not_mounted_for_play_is_still_mounted_read_only_and_copied(self):
        write(self.tf2 / "Roms/SFC/Zelda.sfc", b"Z")
        write(self.tf2 / "saves_RA/Roms/SFC/Zelda.srm", b"save")
        with unittest.mock.patch.object(migrate, "other_partitions", return_value=[("sda1", "sda")]), \
                unittest.mock.patch.object(migrate, "mounted_at", return_value=None), \
                unittest.mock.patch.object(migrate, "mount", return_value=self.tf2):
            found = migrate.sources(self.paths)
        self.assertFalse(found[0].live)
        self.assertEqual(found[0].games[0].destination(self.paths["roms"]), self.paths["roms"] / "snes/Zelda.sfc")

    def test_mounted_at_reads_the_mount_table(self):
        table = write(
            self.root / "mounts", "/dev/mmcblk1p4 /data exfat rw 0 0\n/dev/mmcblk2p1 /media/my\\040card exfat rw 0 0\n"
        )
        self.assertEqual(migrate.mounted_at("mmcblk2p1", str(table)), Path("/media/my card"))
        self.assertIsNone(migrate.mounted_at("mmcblk2p2", str(table)))
        self.assertIsNone(migrate.mounted_at("mmcblk2p1", str(self.root / "missing")))


class EjectServiceTests(unittest.TestCase):
    """Settings > System > Device information > Second card: device_services games-card-eject."""
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / "userdata"
        self.data.mkdir()
        import device_services
        self.module = device_services
        self.service = device_services.Services(self.root / "app", self.data, preview=True)
        self.service.preview = False  # the real path, with a stand-in helper
        self.helper = self.root / "games-card"

    def eject(self, code):
        self.helper.write_text(f"#!/bin/sh\n[ \"$1\" = eject ] || exit 9\nexit {code}\n")
        self.helper.chmod(0o755)
        with unittest.mock.patch.object(self.module, "GAMES_CARD", str(self.helper)):
            self.service.games_card_eject()
        return self.service.fields

    def test_ejected(self):
        self.assertEqual(self.eject(0)["message"], "Second card ejected. You can take it out now.")

    def test_a_card_in_use_stays_in(self):
        with self.assertRaisesRegex(ValueError, "still using the second card"):
            self.eject(2)


if __name__ == "__main__": unittest.main()
