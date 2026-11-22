import sqlite3
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))

import flashpoint  # noqa: E402
from fields import read_fields  # noqa: E402

GAME_ID = "1a2b3c4d-0000-4000-8000-000000000001"
LEGACY_ID = "5e6f7a8b-0000-4000-8000-000000000002"


def flashpoint_install(root):
    """A small Flashpoint folder: the database's game and game_data tables as Flashpoint 13 has them."""
    data = root / "Data"
    (data / "Games").mkdir(parents=True)
    db = sqlite3.connect(data / "flashpoint.sqlite")
    db.execute(
        "CREATE TABLE game (id TEXT, title TEXT, developer TEXT, publisher TEXT, platformName TEXT, extreme INTEGER,"
        " playMode TEXT, releaseDate TEXT, originalDescription TEXT, tagsStr TEXT, series TEXT, library TEXT,"
        " notes TEXT, launchCommand TEXT)"
    )
    db.execute(
        "CREATE TABLE game_data (id INTEGER, gameId TEXT, title TEXT, dateAdded TEXT, path TEXT,"
        " launchCommand TEXT)"
    )
    rows = [
        (GAME_ID, "Rocket: The Game?", "Studio A", "Portal B", "Flash", 0, "Single Player", "2007-05-01",
         "Fly the rocket.\nAvoid rocks.", "Action; Arcade; Space", "", "arcade", "", ""),
        ("x-extreme", "Grim Game", "", "", "Flash", 1, "Single Player", "", "", "", "", "arcade", "", ""),
        ("x-unity", "Unity Game", "", "", "Unity", 0, "", "", "", "", "", "arcade", "", "http://u.com/a.html"),
        (LEGACY_ID, "Old One", "Dev", "", "Flash", 0, "Multiplayer", "2004", "", "Puzzle", "", "arcade", "",
         "http://old.example.com/games/old.swf"),
    ]
    db.executemany("INSERT INTO game VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
    db.execute(
        "INSERT INTO game_data VALUES (1, ?, 'Rocket', '2023-01-01', ?, 'http://www.rocket.com/play/rocket.swf?lvl=2')",
        (GAME_ID, GAME_ID + "-1690000000.zip")
    )
    db.execute(
        "INSERT INTO game_data VALUES (2, 'x-extreme', 'Grim', '2023-01-01', 'x-extreme-1.zip',"
        " 'http://grim.com/g.swf')"
    )
    db.commit()
    db.close()
    with zipfile.ZipFile(data / "Games" / (GAME_ID + "-1690000000.zip"), "w") as z:
        z.writestr("content/www.rocket.com/play/rocket.swf", b"FWS\x08")
    with zipfile.ZipFile(data / "Games" / "x-extreme-1.zip", "w") as z:
        z.writestr("content/grim.com/g.swf", b"FWS\x08")
    logo = data / "Images" / "Logos" / GAME_ID[:2] / GAME_ID[2:4] / (GAME_ID + ".png")
    logo.parent.mkdir(parents=True)
    logo.write_bytes(b"\x89PNG logo")
    shot = data / "Images" / "Screenshots" / GAME_ID[:2] / GAME_ID[2:4] / (GAME_ID + ".png")
    shot.parent.mkdir(parents=True)
    shot.write_bytes(b"\x89PNG shot")
    legacy = root / "Legacy" / "htdocs" / "old.example.com" / "games"
    legacy.mkdir(parents=True)
    (legacy / "old.swf").write_bytes(b"FWS\x07")
    (legacy / "level1.xml").write_text("<level/>")
    return root


class FlashpointImportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.install = flashpoint_install(self.root / "Flashpoint")
        self.flash = self.root / "roms" / "flash"

    def tearDown(self):
        self.tmp.cleanup()

    def test_import_names_packs_and_writes_details(self):
        imported, skipped = flashpoint.import_flashpoint(self.install, self.flash, log=lambda _: None)
        self.assertEqual(imported, 2)  # the zipped game and the legacy one; not the extreme one, not Unity
        pack = self.flash / "Rocket The Game.zip"
        self.assertTrue(pack.is_file())
        side = read_fields(pack.with_suffix(".flashpoint"))
        self.assertEqual(side["launch"], "http://www.rocket.com/play/rocket.swf")
        self.assertEqual(side["flashvars"], "lvl=2")
        self.assertEqual(side["developer"], "Studio A")
        self.assertEqual(side["year"], "2007")
        self.assertEqual(side["players"], "1")
        self.assertEqual(side["genre"], "Action, Arcade, Space")
        self.assertEqual(side["tags"], "Action, Arcade, Space")
        self.assertNotIn("\n", side["description"])
        # Copied, not moved, by default: the computer's Flashpoint keeps its files.
        self.assertTrue((self.install / "Data" / "Games" / (GAME_ID + "-1690000000.zip")).is_file())
        self.assertEqual(flashpoint.image(pack, "logo"), b"\x89PNG logo")
        self.assertEqual(flashpoint.image(pack, "screenshot"), b"\x89PNG shot")
        fields, synopses = flashpoint.information(pack)
        self.assertEqual(fields["info_title"], "Rocket: The Game?")
        self.assertEqual(fields["publisher"], "Portal B")
        self.assertIn("EN", synopses)
        old = self.flash / "Old One.swf"
        self.assertTrue(old.is_file())
        legacy = read_fields(old.with_suffix(".flashpoint"))
        self.assertEqual(legacy["launch"], "http://old.example.com/games/old.swf")
        self.assertTrue(legacy["htdocs"].endswith("Legacy/htdocs"))
        self.assertEqual(legacy["players"], "2+")

    def test_import_again_adds_nothing(self):
        flashpoint.import_flashpoint(self.install, self.flash, log=lambda _: None)
        imported, skipped = flashpoint.import_flashpoint(self.install, self.flash, log=lambda _: None)
        self.assertEqual(imported, 0)
        self.assertEqual(len(list(self.flash.glob("*.zip"))), 1)

    def test_extreme_only_when_asked(self):
        imported, _ = flashpoint.import_flashpoint(self.install, self.flash, extreme=True, log=lambda _: None)
        self.assertEqual(imported, 3)
        self.assertTrue((self.flash / "Grim Game.zip").is_file())

    def test_scan_moves_a_copied_install_once(self):
        inside = self.flash / "Flashpoint"
        self.flash.mkdir(parents=True)
        self.install.rename(inside)
        self.assertEqual(flashpoint.scan(self.flash), 2)
        self.assertTrue((self.flash / "Rocket The Game.zip").is_file())
        self.assertFalse((inside / "Data" / "Games" / (GAME_ID + "-1690000000.zip")).exists())  # moved
        self.assertEqual(flashpoint.scan(self.flash), 0)

    def test_system_by_folder(self):
        from play_options import system_of
        self.flash.mkdir(parents=True)
        pack = self.flash / "Game.zip"
        with zipfile.ZipFile(pack, "w") as z:
            z.writestr("content/a.com/b.swf", b"FWS")
        self.assertEqual(system_of(pack), "flash")
        loose = self.root / "Game.swf"
        loose.write_bytes(b"FWS")
        self.assertEqual(system_of(loose), "flash")


if __name__ == "__main__":
    unittest.main()
