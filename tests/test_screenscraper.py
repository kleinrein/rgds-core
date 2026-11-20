"""ScreenScraper as an artwork source: one lookup per game, details and pictures, public fallback, sign-in."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import library
import scraper
import screenscraper

JEU = {
    "id": "3",
    "noms": [{
        "region": "ss",
        "text": "Sonic"
    }, {
        "region": "eu",
        "text": "Sonic the Hedgehog"
    }],
    "editeur": {
        "text": "SEGA"
    },
    "developpeur": {
        "text": "Sonic Team"
    },
    "joueurs": {
        "text": "1-2"
    },
    "dates": [{
        "region": "us",
        "text": "1991-06-23"
    }, {
        "region": "eu",
        "text": "1991-06-21"
    }],
    "classifications": [{
        "type": "SS",
        "text": "3"
    }, {
        "type": "PEGI",
        "text": "3"
    }],
    "genres": [
        {
            "principale": "1",
            "noms": [{
                "langue": "fr",
                "text": "Plates-formes"
            }, {
                "langue": "en",
                "text": "Platform"
            }]
        }
    ],
    "synopsis": [{
        "langue": "en",
        "text": "Fast."
    }, {
        "langue": "fr",
        "text": "Rapide."
    }],
    "medias": [
        {
            "type": "box-2D",
            "region": "us"
        }, {
            "type": "box-2D",
            "region": "eu"
        }, {
            "type": "ss",
            "region": "wor"
        }, {
            "type": "sstitle",
            "region": "eu"
        }
    ]
}


class FakeClient(scraper.Client):
    def __init__(self, games=None, failure=None):
        super().__init__({})
        self.games, self.failure, self.calls = games if games is not None else {}, failure, []

    def request(self, endpoint, params=None, media=False):
        self.calls.append((endpoint, dict(params or {})))
        if self.failure and endpoint == "jeuInfos.php":
            error = scraper.ScraperError("Your daily ScreenScraper quota has been reached.")
            error.status = self.failure
            raise error
        if endpoint == "jeuInfos.php":
            if params.get("romnom") not in self.games: raise scraper.NotFound("No matching game found.")
            return {"jeu": json.loads(json.dumps(self.games[params["romnom"]]))}
        if endpoint == "mediaJeu.php": return b"PNG " + params["media"].encode()
        raise AssertionError(endpoint)


class FakePublic:
    def __init__(self, cache=None):
        self.cache, self.asked = cache, []

    def artwork(self, rom, name_override=""):
        self.asked.append(Path(rom).name)
        return b"PNG public", {"provider": "Libretro", "artwork_title": Path(rom).stem}


def save_picture(raw, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)


class ScreenScraperTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.games = self.root / "games/md"
        self.games.mkdir(parents=True)
        for target in ("scraper.image_to_bmp", "screenscraper.image_to_bmp"):
            patcher = patch(target, save_picture)
            patcher.start()
            self.addCleanup(patcher.stop)

    def rom(self, name, content=b"MEGA DRIVE GAME"):
        path = self.games / name
        path.write_bytes(content)
        return path

    def test_zipped_game_is_matched_by_its_directory_crc_without_unpacking(self):
        content = b"SEGA" * 1000
        raw = self.rom("Sonic the Hedgehog (Europe).md", content)
        archive = self.games / "Sonic.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("Sonic the Hedgehog (Europe).md", content)
        crc = "%08X" % zlib.crc32(content)
        self.assertEqual(screenscraper.game_entry(raw, "md"), (raw.name, len(content), crc))
        self.assertEqual(screenscraper.game_entry(archive, "md"), (raw.name, len(content), crc))
        with patch.object(screenscraper, "HASH_LIMIT", 10):
            self.assertEqual(screenscraper.game_entry(raw, "md"), (raw.name, len(content), ""))

    def test_cover_details_and_pictures_come_from_one_lookup(self):
        rom = self.rom("Sonic the Hedgehog (Europe).md")
        fake = FakeClient({rom.name: JEU})
        client = screenscraper.ScreenScraperClient(fake, FakePublic())
        extras = screenscraper.ScreenScraperInfo(client, ("info", "snap", "title"))
        data = self.root / "data"
        scraper.scrape(data, client, [rom], extras=extras)
        self.assertEqual([c[0] for c in fake.calls].count("jeuInfos.php"), 1)
        lookup = next(c[1] for c in fake.calls if c[0] == "jeuInfos.php")
        self.assertEqual((lookup["systemeid"], lookup["romnom"]), (1, rom.name))
        self.assertEqual(lookup["crc"], "%08X" % zlib.crc32(b"MEGA DRIVE GAME"))
        item = data / "library" / library.game_key(rom)
        # The European release: the ROM's own region comes first.
        self.assertEqual((item / "box.bmp").read_bytes(), b"PNG box-2D(eu)")
        self.assertEqual((item / "snap.bmp").read_bytes(), b"PNG ss(wor)")
        self.assertEqual((item / "title.bmp").read_bytes(), b"PNG sstitle(eu)")
        info = library.read_fields(item / "info.ini")
        self.assertEqual(info["provider"], "ScreenScraper")
        self.assertEqual(info["info_provider"], "ScreenScraper")
        self.assertEqual(info["info_title"], "Sonic the Hedgehog")
        self.assertEqual((info["publisher"], info["developer"], info["genre"]), ("SEGA", "Sonic Team", "Platform"))
        self.assertEqual((info["release"], info["rating"], info["players"]), ("1991-06-21", "PEGI 3", "2"))
        self.assertNotIn("title", info)  # the launcher's own title is never replaced
        self.assertEqual((item / "synopsis.EN.txt").read_text(), "Fast.\n")
        self.assertEqual((item / "synopsis.FR.txt").read_text(), "Rapide.\n")

    def test_a_genre_word_is_listed_once(self):
        jeu = dict(
            JEU,
            genres=[
                {
                    "noms": [{
                        "langue": "en",
                        "text": "Action"
                    }]
                }, {
                    "noms": [{
                        "langue": "en",
                        "text": "Action / Adventure"
                    }]
                }
            ]
        )
        self.assertEqual(screenscraper.information(jeu, ("eu",))[0]["genre"], "Action, Adventure")

    def test_unknown_game_falls_back_to_the_public_catalogs(self):
        rom = self.rom("Homebrew.md")
        public = FakePublic()
        client = screenscraper.ScreenScraperClient(FakeClient(), public)
        scraper.scrape(self.root / "data", client, [rom])
        item = self.root / "data/library" / library.game_key(rom)
        self.assertEqual((item / "box.bmp").read_bytes(), b"PNG public")
        self.assertEqual(public.asked, [rom.name])

    def test_a_reached_quota_stops_asking_and_the_public_catalogs_finish(self):
        roms = [self.rom("A.md"), self.rom("B.md")]
        fake, public = FakeClient(failure=430), FakePublic()
        client = screenscraper.ScreenScraperClient(fake, public)
        scraper.scrape(self.root / "data", client, roms)
        self.assertEqual([c[0] for c in fake.calls], ["jeuInfos.php"])
        self.assertEqual(public.asked, ["A.md", "B.md"])
        status = library.read_fields(self.root / "data/scraper/status.ini")
        self.assertEqual(status["state"], "done")
        self.assertIn("quota", status["message"])

    def test_a_busy_service_falls_back_per_game_and_stops_after_a_run_of_failures(self):
        roms = [self.rom("G%d.md" % i) for i in range(7)]
        fake, public = FakeClient(failure=503), FakePublic()
        client = screenscraper.ScreenScraperClient(fake, public)
        scraper.scrape(self.root / "data", client, roms)
        self.assertEqual(len(public.asked), 7)  # every game still got its public cover
        self.assertEqual([c[0] for c in fake.calls].count("jeuInfos.php"), screenscraper.FAILURE_LIMIT)
        self.assertTrue(client.stopped)

    def test_a_zipped_dos_game_is_matched_by_its_file_name(self):
        folder = self.games / "DOS"
        folder.mkdir()
        archive = folder / "Doom (1993).zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("DOOM/DOOM.EXE", b"x")
        name, size, crc = screenscraper.game_entry(archive, "dos")
        self.assertEqual((name, crc), ("Doom (1993).zip", ""))
        self.assertEqual(size, archive.stat().st_size)
        import public_artwork
        self.assertEqual(public_artwork.rom_identity(archive), ("dos", "Doom (1993)", ""))

    def test_a_disc_index_file_is_matched_by_name_and_size_not_its_own_crc(self):
        cue = self.rom("Crash (USA).cue", b'FILE "Crash.bin" BINARY')
        self.assertEqual(screenscraper.game_entry(cue, "psx"), (cue.name, cue.stat().st_size, ""))

    def test_odd_entries_in_a_reply_do_not_break_the_details(self):
        jeu = dict(JEU, genres=["x", None], classifications=["x", {"type": "PEGI", "text": "3"}], synopsis=[7])
        fields, synopses = screenscraper.information(jeu, ("eu",))
        self.assertEqual((fields["rating"], synopses), ("PEGI 3", {}))

    def test_a_system_without_catalog_lists_is_missing_once(self):
        import game_info
        calls = []

        class Public:
            cache = None

            def catalog(self, name, url, parse):
                calls.append(name)
                raise scraper.NotFound("none")

        info = game_info.GameInfo(Public(), ("info",))
        for _ in range(3):
            with self.assertRaises(scraper.NotFound):
                info.information(self.rom("A.dsk"), "cpc", "")
        self.assertEqual(len(calls), len(set(calls)))

    def test_a_matching_title_searches_by_name(self):
        rom = self.rom("sonic1.md")
        fake = FakeClient()
        fake.request = lambda endpoint, params=None, media=False: (
            fake.calls.append((endpoint, params)) or
            ({
                "jeux": [dict(JEU)]
            } if endpoint == "jeuRecherche.php" else b"PNG " + params["media"].encode())
        )
        client = screenscraper.ScreenScraperClient(fake, FakePublic())
        raw, meta = client.artwork(rom, "Sonic the Hedgehog")
        self.assertEqual(fake.calls[0], ("jeuRecherche.php", {"systemeid": 1, "recherche": "Sonic the Hedgehog"}))
        self.assertEqual((meta["provider"], meta["artwork_match"]), ("ScreenScraper", "title"))

    def test_login_keeps_the_account_private_and_the_name_visible(self):
        data, private = self.root / "data", self.root / "private"
        environment = {"SCREENSCRAPER_DEVID": "app", "SCREENSCRAPER_DEVPASSWORD": "APP-SECRET"}
        arguments = ["scraper.py", "login", "--data", str(data), "--private-dir", str(private)]
        stdin = io.StringIO('{"ssid": "player", "sspassword": "USER-SECRET"}')
        with patch.dict(os.environ, environment), patch.object(sys, "argv", arguments), \
                patch.object(sys, "stdin", stdin), \
                patch.object(scraper.Client, "login", return_value={"id": "player"}):
            self.assertEqual(scraper.main(), 0)
        account = private / "screenscraper-account.json"
        self.assertEqual(account.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(account.read_text())["sspassword"], "USER-SECRET")
        self.assertEqual(library.read_fields(data / "scraper/account.ini"), {"user": "player"})
        self.assertNotIn("USER-SECRET", (data / "scraper/status.ini").read_text())
        with patch.object(sys, "argv", ["scraper.py", "logout", "--data", str(data), "--private-dir", str(private)]):
            self.assertEqual(scraper.main(), 0)
        self.assertFalse(account.exists())
        self.assertFalse((data / "scraper/account.ini").exists())

    def test_a_refused_login_stores_nothing(self):
        data, private = self.root / "data", self.root / "private"
        environment = {"SCREENSCRAPER_DEVID": "app", "SCREENSCRAPER_DEVPASSWORD": "APP-SECRET"}
        arguments = ["scraper.py", "login", "--data", str(data), "--private-dir", str(private)]
        refused = scraper.ScraperError("Check your ScreenScraper login.")
        with patch.dict(os.environ, environment), patch.object(sys, "argv", arguments), \
                patch.object(sys, "stdin", io.StringIO('{"ssid": "player", "sspassword": "wrong"}')), \
                patch.object(scraper.Client, "login", side_effect=refused):
            self.assertEqual(scraper.main(), 1)
        self.assertFalse((private / "screenscraper-account.json").exists())
        self.assertIn("Check your ScreenScraper login.", (data / "scraper/status.ini").read_text())


if __name__ == "__main__":
    unittest.main()
