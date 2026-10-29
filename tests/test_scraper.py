"""ScreenScraper authentication, ROM identity, credentials and offline cache behaviour."""
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import urllib.parse
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import library
import scraper


class Response(io.BytesIO):
    def geturl(self):
        return scraper.API + "ssuserInfos.php"


class ScraperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.auth = dict(
            devid="app-id",
            devpassword="APP-SECRET",
            ssid="player",
            sspassword="USER-SECRET",
            output="json",
            softname="RGDS"
        )

    def test_login_separates_player_account_from_project_credentials(self):
        calls = []

        def opener(request, **kwargs):
            calls.append(urllib.parse.parse_qs(urllib.parse.urlsplit(request.full_url).query))
            return Response(json.dumps({"response": {"ssuser": {"id": "12", "maxrequestspermin": "10"}}}).encode())

        client = scraper.Client(self.auth, opener)
        self.assertEqual(client.login()["id"], "12")
        self.assertEqual(calls[0]["devid"], ["app-id"])
        self.assertEqual(calls[0]["ssid"], ["player"])
        self.assertEqual(client.interval, 6)

    def test_request_errors_never_include_credential_bearing_url(self):
        def opener(request, **kwargs):
            raise urllib.error.HTTPError(request.full_url, 403, "secret", None, None)

        with self.assertRaises(scraper.ScraperError) as caught:
            scraper.Client(self.auth, opener).login()
        self.assertNotIn("APP-SECRET", str(caught.exception))
        self.assertNotIn("USER-SECRET", str(caught.exception))
        self.assertNotIn("https://", str(caught.exception))

    def test_redirect_cannot_send_credentials_to_another_host(self):
        request = urllib.request.Request(scraper.API + "ssuserInfos.php?sspassword=secret")
        with self.assertRaises(scraper.ScraperError):
            scraper.ServiceRedirect().redirect_request(request, None, 302, "Found", {}, "https://example.com/stolen")

    def test_rom_hashes_are_identical_for_raw_and_single_game_zip(self):
        content = b"test DS game\x00" * 100
        raw = self.root / "A game.nds"
        raw.write_bytes(content)
        archive = self.root / "A game.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("folder/A game.nds", content)
        identity = scraper.rom_identity(raw)
        self.assertEqual(identity, scraper.rom_identity(archive))
        self.assertEqual(identity["md5"], hashlib.md5(content).hexdigest())
        self.assertEqual(identity["crc"], "%08X" % zlib.crc32(content))
        self.assertEqual(identity["romtaille"], len(content))

    def test_saved_credentials_have_private_permissions(self):
        path = self.root / "private/account.json"
        scraper.save_private(path, {"ssid": "player", "sspassword": "secret"})
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(scraper.read_private(path)["ssid"], "player")

    def test_cached_art_is_not_downloaded_again(self):
        rom = self.root / "game.nds"
        rom.write_bytes(b"game")
        art = self.root / "data/library" / library.game_key(rom) / "box.bmp"
        art.parent.mkdir(parents=True)
        art.write_bytes(b"existing artwork")
        with patch.object(scraper.Client, "game") as lookup:
            scraper.scrape(self.root / "data", scraper.Client(self.auth), [rom])
            lookup.assert_not_called()
        self.assertEqual(art.read_bytes(), b"existing artwork")
        self.assertEqual(library.read_fields(self.root / "data/scraper/status.ini")["state"], "done")

    def test_cancel_keeps_cached_art_and_does_not_call_provider(self):
        data = self.root / "data"
        (data / "scraper").mkdir(parents=True)
        (data / "scraper/cancel").touch()
        with patch.object(scraper.Client, "game") as lookup:
            scraper.scrape(data, scraper.Client(self.auth), [self.root / "game.nds"])
            lookup.assert_not_called()
        self.assertEqual(library.read_fields(data / "scraper/status.ini")["state"], "cancelled")

    def test_missing_project_configuration_does_not_store_player_password(self):
        with self.assertRaisesRegex(scraper.ScraperError, "not configured"):
            scraper.credentials(self.root, {"ssid": "player", "sspassword": "secret"})
        self.assertEqual(list(self.root.iterdir()), [])


class BannerAndPlaytimeTests(unittest.TestCase):
    def test_banner_icon_tiles_and_utf16_title_work_for_zip_and_raw(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = bytearray(0x640)
            struct.pack_into("<I", data, 0x68, 0x200)
            data[0x220:0x420] = bytes([0x11]) * 512
            struct.pack_into("<H", data, 0x422, 31)  # Palette 1 is red.
            title = "Café Adventure".encode("utf-16le")
            data[0x540:0x540 + len(title)] = title
            raw = root / "game.nds"
            raw.write_bytes(data)
            archive = root / "game.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("game.nds", data)
            for rom in (raw, archive):
                library.index_banner(root / "cache", rom)
                item = root / "cache/library" / library.game_key(rom)
                self.assertEqual(library.read_fields(item / "banner.ini")["title"], "Café Adventure")
                bmp = (item / "icon.bmp").read_bytes()
                self.assertEqual(bmp[122:126], bytes([0, 0, 255, 255]))
                self.assertEqual(len(bmp), 4218)

    def test_playtime_accumulates_without_touching_rom_or_artwork(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rom = root / "game.nds"
            rom.write_bytes(b"game")
            data = root / "data"
            library.record_playtime(data, rom, 120, 1, "2026-10-05")
            seconds, sessions = library.playtime(data, rom)
            library.record_playtime(data, rom, seconds + 90, sessions + 1, "2026-10-06")
            self.assertEqual(library.playtime(data, rom), (210, 2))
            self.assertEqual(rom.read_bytes(), b"game")

    def test_session_history_rewrites_checkpoints_and_keeps_the_newest_rows(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rom = root / "game.nds"
            rom.write_bytes(b"game")
            data = root / "data"
            history = data / "library" / library.game_key(rom) / "sessions.tsv"
            first, second = "2026-10-05T18:00:00+00:00", "2026-10-06T07:30:00+00:00"
            library.record_playtime(data, rom, 30, 1, first, 30)
            library.record_playtime(data, rom, 60, 1, first, 60)  # a checkpoint of the same session
            library.record_playtime(data, rom, 75, 2, second, 15)
            self.assertEqual(history.read_text(), "1791223200\t60\n1791271800\t15\n")
            library.record_playtime(data, rom, 75, 2, "not a date", 15)
            self.assertEqual(len(history.read_text().splitlines()), 2)
            for minute in range(library.SESSION_LIMIT + 5):
                library.record_session(history, "2026-10-07T00:%02d:00+00:00" % (minute % 60), 1)
            self.assertLessEqual(len(history.read_text().splitlines()), library.SESSION_LIMIT)


if __name__ == "__main__": unittest.main()
