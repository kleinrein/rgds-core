"""Bring over (runtime/migrate.py): BananaOS and ROCKNIX files found, mapped onto this card, never replacing anything."""
import io
import json
import os
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
import unittest.mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import migrate
from library import game_key, playtime
from fields import read_fields


def write(path, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    return path


class MigrateTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.card = self.root / "card"  # this card's /data
        self.data = self.card / "rgds/userdata"
        self.data.mkdir(parents=True)
        self.paths = {
            "player": self.data,
            "roms": self.card / "roms",
            "bios": self.card / "rgds/bios",
            "work": self.card / "rgds/import",
            "folder": self.card / "import",
        }

    def banana(self, root):
        """A BananaOS data partition as its docs describe it."""
        write(root / "roms/nds/Mario Kart DS.nds", b"N" * 64)
        write(root / "roms/gba/Advance Wars.gba", b"G" * 32)
        write(root / "roms/megadrive/Sonic.md", b"M" * 16)
        write(root / "roms/nes/Tetris.nes", b"T")
        write(root / "roms/gb/Tetris.gb", b"T")
        write(root / "saves/nds/Mario Kart DS.sav", b"melonds")
        os.utime(root / "saves/nds/Mario Kart DS.sav", (1000, 1000))
        # DraStic's newer copy, with DeSmuME's footer: the raw save is what comes over
        raw = b"drastic-raw!"
        footer = b"\0" * 86 + struct.pack("<I", len(raw)) + b"\0" * 16 + b"|-DESMUME SAVE-|"
        write(root / "drastic/backup/Mario Kart DS.dsv", raw + b"\xff" * 4 + footer)
        os.utime(root / "drastic/backup/Mario Kart DS.dsv", (2000, 2000))
        write(root / "saves/Advance Wars.srm", b"gba-save")
        write(root / "saves/Sonic.srm", b"md-save")
        os.utime(root / "saves/Sonic.srm", (3000, 3000))
        write(root / "saves/Tetris.srm", b"which one?")
        # a compressed RetroArch state, newer than the save: only states of the same core come over
        state = b"picodrive-state" * 10
        packed = zlib.compress(state)
        rzip = b"#RZIPv\x01#" + struct.pack("<IQ", 131072, len(state)) + struct.pack("<I", len(packed)) + packed
        write(root / "states/Sonic.state1", rzip)
        os.utime(root / "states/Sonic.state1", (4000, 4000))
        write(root / "states/Advance Wars.state", b"gpsp")  # gpSP here is mGBA: not brought
        write(root / "bios/bios7.bin", b"7")
        write(
            root / "banana/playstats.tsv",
            "nds/Mario Kart DS.nds\t3\t7200\t1700000000\nmegadrive/Sonic.md\t1\t600\t1600000000\n"
        )
        write(root / "banana/favorites.txt", "gba/Advance Wars.gba\n")
        return root

    def test_bananaos_card_comes_over_once_and_nothing_is_replaced(self):
        old = self.banana(self.root / "old")
        source = migrate.Source(old, "slot", migrate.layout(old))
        self.assertEqual(source.kind, "bananaos")
        plan = migrate.Plan(source, self.paths)
        summary = plan.summary()
        self.assertEqual((summary["import_saves"], summary["import_states"]), (3, 1))  # Tetris is ambiguous
        self.assertEqual(
            (summary["import_played"], summary["import_hours"], summary["import_favourites"]), (2, "2.2", 1)
        )
        self.assertEqual(summary["import_games_copy"], 5)
        result = plan.apply(games=True)
        self.assertEqual((result["saves"], result["states"], result["games"], result["bios"]), (3, 1, 5, 1))
        ds = self.data / "emulators/rgds-experimental/saves/Mario Kart DS.sav"
        self.assertEqual(ds.read_bytes(), b"drastic-raw!")
        self.assertEqual((self.data / "emulators/rgds-play/saves/md/Sonic.sav").read_bytes(), b"md-save")
        self.assertEqual((self.data / "emulators/rgds-play/saves/gba/Advance Wars.sav").read_bytes(), b"gba-save")
        sonic = self.card / "roms/md/Sonic.md"
        self.assertTrue(sonic.is_file())
        auto = self.data / "library" / game_key(sonic) / "auto-rgds-play.dss"
        self.assertEqual(auto.read_bytes(), b"picodrive-state" * 10)
        self.assertEqual(playtime(self.data, self.card / "roms/nds/Mario Kart DS.nds"), (7200, 3))
        fav = read_fields(self.data / "library" / game_key(self.card / "roms/gba/Advance Wars.gba") / "game.ini")
        self.assertEqual(fav["favourite"], "1")
        self.assertTrue((self.card / "rgds/bios/bios7.bin").is_file())
        # again: play time is not added twice, and the saves now here are not touched
        ds.write_bytes(b"played on since")
        again = migrate.Plan(source, self.paths)
        self.assertEqual(again.summary()["import_kept"], 1)
        again.apply()
        self.assertEqual(ds.read_bytes(), b"played on since")
        self.assertEqual(playtime(self.data, self.card / "roms/nds/Mario Kart DS.nds"), (7200, 3))
        # the old card was only read
        self.assertEqual((old / "saves/Sonic.srm").read_bytes(), b"md-save")
        self.assertTrue((old / "roms/md").exists() is False and (old / "roms/megadrive/Sonic.md").is_file())

    def test_flasher_bundle_without_games_keys_play_time_where_the_games_will_go(self):
        old = self.banana(self.root / "old")
        manifest = {
            "format": 1,
            "source": "bananaos",
            "roms": [["nds/Mario Kart DS.nds", 64], ["megadrive/Sonic.md", 16]]
        }
        bundle = io.BytesIO()
        with tarfile.open(fileobj=bundle, mode="w:gz") as archive:
            for sub in ("saves", "states", "drastic", "bios", "banana"):
                archive.add(old / sub, sub)
            blob = json.dumps(manifest).encode()
            info = tarfile.TarInfo("manifest.json")
            info.size = len(blob)
            archive.addfile(info, io.BytesIO(blob))
        write(self.paths["work"] / "flasher.tar.gz", bundle.getvalue())
        plan = migrate.best_plan(self.paths, devices=False)
        self.assertEqual(plan.source.place, "flasher")
        summary = plan.summary()
        self.assertEqual((summary["import_games_copy"], summary["import_games_away"]), (0, 2))
        plan.apply()
        self.assertEqual(playtime(self.data, self.card / "roms/md/Sonic.md"), (600, 1))
        self.assertEqual((self.data / "emulators/rgds-play/saves/md/Sonic.sav").read_bytes(), b"md-save")
        # nothing left to bring: the step is not shown again
        self.assertIsNone(migrate.best_plan(self.paths, devices=False))

    def test_unsafe_bundle_paths_are_left_out(self):
        bundle = io.BytesIO()
        with tarfile.open(fileobj=bundle, mode="w:gz") as archive:
            for name in ("../escape.txt", "manifest.json"):
                blob = b'{"format": 1, "source": "bananaos", "roms": []}'
                info = tarfile.TarInfo(name)
                info.size = len(blob)
                archive.addfile(info, io.BytesIO(blob))
        write(self.paths["work"] / "flasher.tar.gz", bundle.getvalue())
        migrate.unpack_handoff(self.paths["work"])
        self.assertFalse((self.paths["work"] / "escape.txt").exists())
        self.assertTrue((self.paths["work"] / "flasher/manifest.json").is_file())

    def test_rocknix_saves_beside_games_and_gamelist_time(self):
        old = self.root / "storage"
        write(old / ".config/emulationstation/es_settings.cfg", "")
        write(old / "roms/snes/Zelda.sfc", b"Z" * 8)
        write(old / "roms/snes/Zelda.srm", b"snes-save")
        write(old / "roms/snes/images/Zelda.png", b"art")
        write(
            old / "roms/snes/gamelist.xml", "<gameList><game><path>./Zelda.sfc</path><playcount>4</playcount>"
            "<gametime>5400</gametime><lastplayed>20240102T030405</lastplayed></game></gameList>"
        )
        source = migrate.Source(old, "usb", migrate.layout(old))
        self.assertEqual(source.kind, "rocknix")
        self.assertEqual([str(g.rel) for g in source.games], ["Zelda.sfc"])
        plan = migrate.Plan(source, self.paths)
        self.assertEqual(plan.summary()["import_states"], 0)
        plan.apply(games=True)
        self.assertEqual((self.data / "emulators/rgds-play/saves/snes/Zelda.sav").read_bytes(), b"snes-save")
        self.assertEqual(playtime(self.data, self.card / "roms/snes/Zelda.sfc"), (5400, 4))
        self.assertFalse((self.card / "roms/snes/images").exists())

    def test_only_the_arcade_sets_the_core_runs_come_over(self):
        old = self.root / "storage"
        write(old / ".config/emulationstation/es_settings.cfg", "")
        write(old / "roms/mame/punchout.zip", b"P" * 8)
        write(old / "roms/mame/VSTENNIS.ZIP", b"V" * 8)
        write(old / "roms/mame/sf2.zip", b"S" * 8)  # a set this core is not built with
        write(old / "roms/mame/punchout.txt", b"notes")
        source = migrate.Source(old, "usb", migrate.layout(old))
        self.assertEqual(sorted(str(g.rel) for g in source.games), ["VSTENNIS.ZIP", "punchout.zip"])
        migrate.Plan(source, self.paths).apply(games=True)
        self.assertTrue((self.card / "roms/arcade/punchout.zip").is_file())
        self.assertFalse((self.card / "roms/arcade/sf2.zip").exists())

    def test_import_folder_moves_games_on_this_card(self):
        old = self.banana(self.paths["folder"] / "BANANA")
        plan = migrate.best_plan(self.paths, devices=False)
        self.assertEqual(plan.source.place, "folder")
        plan.apply(games=True)
        self.assertTrue((self.card / "roms/gba/Advance Wars.gba").is_file())
        self.assertFalse((old / "roms/gba/Advance Wars.gba").exists())

    def test_a_damaged_bundle_is_set_aside_not_retried(self):
        write(self.paths["work"] / "flasher.tar.gz", b"\x1f\x8b not really gzip")
        self.assertIsNone(migrate.best_plan(self.paths, devices=False))
        self.assertTrue((self.paths["work"] / "flasher.tar.gz.bad").is_file())
        self.assertFalse((self.paths["work"] / "flasher.tar.gz").exists())

    def test_a_card_too_full_for_the_games_still_gets_the_saves(self):
        old = self.banana(self.root / "old")
        plan = migrate.Plan(migrate.Source(old, "usb", "bananaos"), self.paths)
        full = type("Usage", (), {"free": 1})()
        with unittest.mock.patch("migrate.shutil.disk_usage", return_value=full):
            with self.assertRaisesRegex(ValueError, "saves and play time are in place"):
                plan.apply(games=True)
        self.assertTrue((self.data / "emulators/rgds-experimental/saves/Mario Kart DS.sav").is_file())
        self.assertEqual(playtime(self.data, self.card / "roms/nds/Mario Kart DS.nds"), (7200, 3))
        self.assertFalse((self.card / "roms/nds").exists())

    def stock(self, root):
        """Anbernic's stock games partition (FAT, /mnt/mmc) and its vendor partition (ext4, DraStic's home)."""
        games, vendor = root / "mmc", root / "vendor"
        write(games / "Roms/NDS/Mario Kart DS.nds", b"N" * 64)
        write(games / "Roms/SFC/Zelda.sfc", b"Z" * 8)
        write(games / "Roms/PICO/celeste.p8", b"p8")
        write(games / "saves_RA/Roms/SFC/Zelda.srm", b"snes-save")
        write(games / "states_RA/Roms/SFC/Zelda.state1", b"unknown core")
        write(vendor / "deep/drastic_aarch64/backup/Mario Kart DS.dsv", b"kart-save")
        write(vendor / "deep/drastic_aarch64/backup/Pokemon Platinum.dsv", b"pokemon-save")
        return games, vendor

    def test_stock_card_brings_retroarch_and_drastic_saves(self):
        games, vendor = self.stock(self.root / "stock")
        self.assertEqual((migrate.layout(games), migrate.layout(vendor)), ("stock", "stock"))
        source = migrate.Source(games, "slot", "stock", [vendor])
        plan = migrate.Plan(source, self.paths)
        summary = plan.summary()
        # three games on the card (PICO is PICO-8 there); Pokemon is known only by its save; states stay behind
        self.assertEqual((summary["import_games"], summary["import_saves"], summary["import_states"]), (3, 3, 0))
        plan.apply(games=True)
        saves = self.data / "emulators"
        self.assertEqual((saves / "rgds-play/saves/snes/Zelda.sav").read_bytes(), b"snes-save")
        self.assertEqual((saves / "rgds-experimental/saves/Mario Kart DS.sav").read_bytes(), b"kart-save")
        self.assertEqual((saves / "rgds-experimental/saves/Pokemon Platinum.sav").read_bytes(), b"pokemon-save")
        self.assertTrue((self.card / "roms/pico8/celeste.p8").is_file())
        self.assertTrue((self.card / "roms/snes/Zelda.sfc").is_file())

    def test_stock_flasher_bundle_with_drastic_saves_read_at_flash_time(self):
        root = self.paths["work"] / "flasher"
        write(root / "manifest.json", json.dumps({"format": 1, "source": "stock", "roms": [["SFC/Zelda.sfc", 8]]}))
        write(root / "saves_RA/Roms/SFC/Zelda.srm", b"snes-save")
        write(root / "vendor/deep/drastic64/backup/Kart.dsv", b"kart")
        plan = migrate.best_plan(self.paths, devices=False)
        self.assertEqual((plan.source.kind, plan.source.name), ("stock", "the stock system"))
        self.assertEqual((plan.summary()["import_saves"], plan.summary()["import_games_away"]), (2, 1))
        plan.apply()
        self.assertEqual((self.data / "emulators/rgds-experimental/saves/Kart.sav").read_bytes(), b"kart")

    def test_retroarch_state_container(self):
        mem = b"core-bytes"
        block = b"MEM " + struct.pack("<I", len(mem)) + mem + b"\0" * 6 + b"END " + struct.pack("<I", 0)
        self.assertEqual(migrate.retroarch_state(b"RASTATE\x01" + block), mem)
        self.assertEqual(migrate.retroarch_state(b"plain"), b"plain")


if __name__ == "__main__": unittest.main()
