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
    def test_a_zipped_c64_game_boots_its_loader_disk_first(self):
        from play_options import disk_order, unpacked_disks
        names = ['G [Side A_ESI].d64', 'G [Side A_REM].d64', 'G [Side B_ESI].d64', 'G [Side Load_ESI].d64']
        self.assertEqual(disk_order(names), ['G [Side Load_ESI].d64', 'G [Side A_ESI].d64', 'G [Side B_ESI].d64'])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'c64').mkdir()
            several, single = root / 'c64/several.zip', root / 'c64/single.zip'
            with zipfile.ZipFile(several, 'w') as archive:
                for name in names:
                    archive.writestr(name, name)
            with zipfile.ZipFile(single, 'w') as archive:
                archive.writestr('one.d64', 'one')
            listing = unpacked_disks(several, root / 'data')
            self.assertEqual(listing.suffix, '.m3u')
            self.assertEqual(listing.read_text().splitlines()[0], 'G [Side Load_ESI].d64')
            self.assertEqual((listing.parent / 'G [Side Load_ESI].d64').read_text(), 'G [Side Load_ESI].d64')
            # the game's own writes to its disk survive the next launch
            (listing.parent / 'G [Side A_ESI].d64').write_text('G [Side A_ES!].d64')
            unpacked_disks(several, root / 'data')
            self.assertEqual((listing.parent / 'G [Side A_ESI].d64').read_text(), 'G [Side A_ES!].d64')
            self.assertEqual(unpacked_disks(single, root / 'data'), single)

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
            self.root / "a.sg": "sg",
            self.root / "a.sc": "sg",
            self.root / "a.col": "coleco",
            self.root / "a.int": "intellivision",
            self.root / "a.mx2": "msx",
            self.root / "a.cas": "msx",
            self.root / "a.cdt": "cpc",
            self.root / "a.cpr": "cpc",
            self.root / "a.pce": "pce",
            self.root / "a.cue": "psx",
            self.root / "a.chd": "psx",
            self.root / "a.z64": "n64",
            self.root / "a.v64": "n64",
            self.root / "a.tzx": "zx",
            self.root / "a.tap": "zx",
            self.root / "a.z80": "zx",
            self.root / "a.mgw": "gw",
            self.root / "a.d64": "c64",
            self.root / "a.t64": "c64",
            self.root / "a.crt": "c64",
            self.root / "a.cci": "3ds",
            self.root / "a.iso": "psp",
            self.root / "a.cso": "psp",
            self.root / "a.gdi": "dc",
            self.root / "a.cdi": "dc",
            self.root / "a.3ds": "3ds",
            self.root / "a.3dsx": "3ds",
        }
        for rom, system in cases.items():
            self.assertEqual(play_options.system_of(rom), system, rom)
            expected = "rgds-play" if system != "nds" else "rgds-experimental"  # a stored choice no longer counts
            self.assertEqual(play_options.engine_for(rom, "melonds"), expected)

    def test_a_plain_bin_is_a_mega_drive_game_only_in_a_mega_drive_folder(self):
        for folder in ("md", "genesis", "Genesis", "megadrive"):
            rom = self.root / "roms" / folder / "Sonic.bin"
            self.assertEqual(play_options.system_of(rom), "md", folder)
            packed = self.root / "roms" / folder / "Sonic.zip"
            packed.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(packed, "w") as archive:
                archive.writestr("Sonic The Hedgehog (USA, Europe).bin", b"x")
            self.assertEqual(play_options.system_of(packed), "md", folder)
            self.assertEqual(play_options.engine_for(packed, "melonds"), "rgds-play")
        self.assertEqual(play_options.system_of(self.root / "roms" / "psx" / "Game.bin"), "nds")

    def test_a_7z_game_is_the_system_of_its_folder(self):
        # Python reads no 7z: the games folder names the system (and RG DS Play unpacks it).
        for folder, system in (
            ("n64", "n64"), ("gb", "gb"), ("GBC", "gbc"), ("gba", "gba"), ("psp", "psp"), ("nds", "nds"),
            ("genesis", "md"), ("snes", "snes")
        ):
            rom = self.root / "roms" / folder / "Game (USA).7z"
            self.assertEqual(play_options.system_of(rom), system, folder)
            self.assertEqual(
                play_options.engine_for(rom, "melonds"), "rgds-play" if system != "nds" else "rgds-experimental"
            )
        self.assertEqual(play_options.system_of(self.root / "Game.7z"), "nds")
        # a zip of nothing the lists know, in a PlayStation folder: that folder's
        packed = self.root / "roms" / "psx" / "Disc.zip"
        packed.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(packed, "w") as archive:
            archive.writestr("Disc (Track 1).bin", b"x")
        self.assertEqual(play_options.system_of(packed), "psx")

    def test_a_ds_game_in_a_7z_is_unpacked_by_the_player(self):
        rom = self.root / "roms" / "nds" / "Game.7z"
        rom.parent.mkdir(parents=True, exist_ok=True)
        rom.write_bytes(b"7z")
        unpacked = self.root / "Game.nds"
        unpacked.write_bytes(b"x")
        done = type("Done", (), {"returncode": 0, "stdout": str(unpacked) + "\n", "stderr": ""})
        player = Path(emulators.__file__).resolve().parent.parent / "emulators/rgds-play/rgds-play"
        with patch.object(Path, "is_file", lambda p: True if p == player else Path.exists(p)), \
             patch.object(emulators.subprocess, "run", return_value=done) as run:
            self.assertEqual(emulators.resolve_rom(rom, self.root), unpacked)
        command = run.call_args[0][0]
        self.assertEqual(command[1:3], ["--unpack", str(rom.resolve())])
        self.assertEqual(command[-1], ".nds,.dsi")
        failed = type(
            "Failed", (), {
                "returncode": 1,
                "stdout": "",
                "stderr": "rgds-play: No game for this system inside the archive\n"
            }
        )
        with patch.object(Path, "is_file", lambda p: True if p == player else Path.exists(p)), \
             patch.object(emulators.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(ValueError, "No game for this system"):
                emulators.resolve_rom(rom, self.root)

    def test_a_tape_or_disk_list_in_a_c64_folder_is_the_c64s(self):
        self.assertEqual(play_options.system_of(self.root / "roms" / "scummvm" / "Monkey" / "MONKEY.000"), "scummvm")
        for folder in ("c64", "C64", "commodore"):
            self.assertEqual(play_options.system_of(self.root / "roms" / folder / "Game.tap"), "c64", folder)
            self.assertEqual(play_options.system_of(self.root / "roms" / folder / "Game.m3u"), "c64", folder)
        self.assertEqual(play_options.system_of(self.root / "roms" / "zx" / "Game.tap"), "zx")
        self.assertEqual(play_options.system_of(self.root / "roms" / "psx" / "Game.m3u"), "psx")
        for name in ("Game.pbp", "Game.chd"):
            self.assertEqual(play_options.system_of(self.root / "roms" / "psp" / name), "psp", name)
        self.assertEqual(play_options.system_of(self.root / "roms" / "psx" / "Game.chd"), "psx")
        # A Dreamcast disc's .chd, .cue or .m3u is the Dreamcast's only in its folder; a zip there too.
        for folder in ("dc", "Dreamcast"):
            for name in ("Game.chd", "Game.cue", "Game.m3u", "Game.zip"):
                self.assertEqual(play_options.system_of(self.root / "roms" / folder / name), "dc", (folder, name))

    def test_mega_cd_pico_coleco_and_intellivision_go_by_their_folders(self):
        roms = self.root / "roms"
        for folder in ("segacd", "megacd", "Mega-CD", "Sega CD"):
            for name in ("Game.cue", "Game.chd", "Game.m3u"):
                self.assertEqual(play_options.system_of(roms / folder / name), "segacd", (folder, name))
        # A disc beside the Mega Drive's cartridges is a Mega-CD one; a cartridge there stays the Mega Drive's.
        self.assertEqual(play_options.system_of(roms / "md" / "Game.cue"), "segacd")
        self.assertEqual(play_options.system_of(roms / "md" / "Game.bin"), "md")
        self.assertEqual(play_options.system_of(roms / "psx" / "Game.cue"), "psx")
        for folder in ("pico", "Sega Pico"):
            self.assertEqual(play_options.system_of(roms / folder / "Game.md"), "pico", folder)
            self.assertEqual(play_options.system_of(roms / folder / "Game.bin"), "pico", folder)
        self.assertEqual(play_options.system_of(roms / "coleco" / "Game.rom"), "coleco")
        self.assertEqual(play_options.system_of(roms / "intellivision" / "Game.bin"), "intellivision")
        for system in ("sg", "segacd", "pico"):
            self.assertEqual(play_options.LIBRETRO_CORES[system], "picodrive")
        self.assertEqual(play_options.LIBRETRO_CORES["coleco"], "gearcoleco")
        self.assertEqual(play_options.LIBRETRO_CORES["intellivision"], "freeintv")
        # Their frames are their shelves': the Mega Drive's for the Mega-CD and the Pico, the Master System's for the SG-1000.
        self.assertIn("segacd", play_options.FRAMES["md"].split(","))
        self.assertIn("pico", play_options.FRAMES["md"].split(","))
        self.assertIn("sg", play_options.FRAMES["sms"].split(","))
        self.assertEqual(play_options.FRAMES["coleco"], "coleco")

    def test_msx_and_pc_engine_cd_go_by_their_folders(self):
        roms = self.root / "roms"
        for folder in ("msx", "MSX2"):
            self.assertEqual(play_options.system_of(roms / folder / "Game.rom"), "msx", folder)
            self.assertEqual(play_options.system_of(roms / folder / "Game.dsk"), "msx", folder)
        self.assertEqual(play_options.system_of(roms / "zx" / "Game.dsk"), "zx")  # a .dsk elsewhere stays the Spectrum's
        for folder in ("cpc", "Amstrad CPC"):
            for name in ("Game.dsk", "Game.sna", "Game.m3u"):
                self.assertEqual(play_options.system_of(roms / folder / name), "cpc", folder + "/" + name)
        self.assertEqual(play_options.system_of(roms / "zx" / "Game.sna"), "zx")
        self.assertEqual(play_options.LIBRETRO_CORES["cpc"], "cap32")
        for folder in ("pcecd", "TurboGrafx-CD"):
            self.assertEqual(play_options.system_of(roms / folder / "Game.cue"), "pcecd", folder)
        self.assertEqual(play_options.system_of(roms / "pce" / "Game.chd"), "pcecd")
        self.assertEqual(play_options.system_of(roms / "pce" / "Game.pce"), "pce")
        self.assertEqual(play_options.system_of(roms / "pce" / "Game.ccd"), "pcecd")
        self.assertEqual(play_options.system_of(roms / "pcecd" / "Game.iso"), "psp")  # Beetle PCE Fast reads no .iso

    def test_a_zipped_disc_goes_by_its_sheet_not_its_first_track(self):
        folder = self.root / "roms" / "md"
        folder.mkdir(parents=True)
        packed = folder / "Disc.zip"
        with zipfile.ZipFile(packed, "w") as archive:
            archive.writestr("Disc (Track 1).bin", b"\0")
            archive.writestr("Disc.cue", 'FILE "Disc (Track 1).bin" BINARY\n')
        self.assertEqual(play_options.system_of(packed), "segacd")
        self.assertEqual(play_options.LIBRETRO_CORES["msx"], "bluemsx")
        self.assertEqual(play_options.LIBRETRO_CORES["pce"], "mednafen_pce_fast")
        self.assertEqual(play_options.LIBRETRO_CORES["pcecd"], "mednafen_pce_fast")
        self.assertIn("pcecd", play_options.FRAMES["pce"].split(","))

    def test_the_images_msx_machines_are_copied_into_the_bios_folder_without_overwriting(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, bios = Path(tmp) / "bluemsx", Path(tmp) / "bios"
            (source / "Machines" / "MSX - C-BIOS").mkdir(parents=True)
            (source / "Machines" / "MSX - C-BIOS" / "config.ini").write_text("image")
            (source / "Databases").mkdir()
            (source / "Databases" / "msxromdb.xml").write_text("image")
            (bios / "Databases").mkdir(parents=True)
            (bios / "Databases" / "msxromdb.xml").write_text("owner")
            with patch.dict(play_options.SYSTEM_FILES, {"msx": source}):
                play_options.provide_system_files("msx", bios)
                play_options.provide_system_files("pce", bios)  # nothing to give: no error
            self.assertEqual((bios / "Machines" / "MSX - C-BIOS" / "config.ini").read_text(), "image")
            self.assertEqual((bios / "Databases" / "msxromdb.xml").read_text(), "owner")

    def test_atari_2600_and_amiga(self):
        roms = self.root / "roms"
        self.assertEqual(play_options.system_of(self.root / "Game.a26"), "atari")
        self.assertEqual(play_options.system_of(roms / "atari2600" / "Game.bin"), "atari")
        self.assertEqual(play_options.system_of(roms / "md" / "Game.bin"), "md")
        for name in ("Game.adf", "Game.ipf", "Game.lha", "Game.hdf"):
            self.assertEqual(play_options.system_of(self.root / name), "amiga", name)
        self.assertEqual(play_options.system_of(roms / "amiga" / "Game.m3u"), "amiga")
        self.assertEqual(play_options.LIBRETRO_CORES["atari"], "stella2014")
        self.assertEqual(play_options.LIBRETRO_CORES["amiga"], "puae")
        self.assertIn("amiga", play_options.FRAMES["monitor"].split(","))

    def test_other_atari_systems(self):
        roms = self.root / "roms"
        self.assertEqual(play_options.system_of(self.root / "Ballblazer.a78"), "atari7800")
        self.assertEqual(play_options.system_of(roms / "atari7800" / "Food Fight.bin"), "atari7800")
        self.assertEqual(play_options.system_of(self.root / "Pac-Man.a52"), "atari5200")
        self.assertEqual(play_options.system_of(roms / "Atari 5200" / "Qix.bin"), "atari5200")
        self.assertEqual(play_options.system_of(self.root / "Gates of Zendocon.lnx"), "lynx")
        self.assertEqual(play_options.system_of(roms / "lynx" / "Klax.zip"), "lynx")
        self.assertEqual(play_options.system_of(roms / "atari2600" / "Pitfall.bin"), "atari")
        self.assertEqual(play_options.LIBRETRO_CORES["atari7800"], "prosystem")
        self.assertEqual(play_options.LIBRETRO_CORES["atari5200"], "a5200")
        self.assertEqual(play_options.LIBRETRO_CORES["lynx"], "handy")
        self.assertEqual(play_options.FRAMES["lynx"], "lynx")
        self.assertIn("atari7800", play_options.FRAMES["tv-grey"].split(","))
        self.assertIn("atari5200", play_options.FRAMES["tv-wood"].split(","))

    def test_arcade_two_monitors(self):
        roms = self.root / "roms"
        for folder in ("arcade", "MAME", "mame2003-plus"):
            self.assertEqual(play_options.system_of(roms / folder / "punchout.zip"), "arcade", folder)
        self.assertEqual(play_options.system_of(roms / "punchout.zip"), "nds")  # a zip elsewhere is a DS game's
        self.assertEqual(play_options.LIBRETRO_CORES["arcade"], "mame2003_plus")
        self.assertEqual(play_options.FRAMES["arcade"], "arcade")
        # The sets the core is built with are the ones the launcher lists and the profiles name.
        import re
        script = (ROOT / "os/buildroot/package/libretro-mame2003-plus/two_monitors.py").read_text()
        start = script.index("GAMES = [")
        games = set(re.findall(r'"([a-z0-9]+)"', script[start:script.index("]", start)]))
        model = (ROOT / "src/launcher/model.hpp").read_text()
        listed = model[model.index("arcade_sets() {"):model.index("return sets;")]
        self.assertEqual(set(re.findall(r'\{"([a-z0-9]+)", "', listed)), games)
        self.assertEqual(set(play_options.ARCADE_SETS), games)
        codes = set()
        for profile in (ROOT / "play/profiles/arcade").glob("*.ini"):
            codes |= set(re.search(r"^codes = (.*)$", profile.read_text(), re.M).group(1).split())
        self.assertEqual(codes, {g.upper() for g in games})

    def test_wonderswan_and_neo_geo_pocket(self):
        import public_artwork
        import screenscraper
        roms = self.root / "roms"
        self.assertEqual(play_options.system_of(self.root / "Gunpey.ws"), "wonderswan")
        self.assertEqual(play_options.system_of(self.root / "Final Fantasy.wsc"), "wonderswan")
        self.assertEqual(play_options.system_of(roms / "WonderSwan Color" / "Klonoa.zip"), "wonderswan")
        self.assertEqual(play_options.system_of(self.root / "Metal Slug 1st Mission.ngc"), "ngp")
        self.assertEqual(play_options.system_of(self.root / "King of Fighters R-1.ngp"), "ngp")
        self.assertEqual(play_options.system_of(roms / "ngpc" / "SNK vs Capcom.zip"), "ngp")
        self.assertEqual(play_options.LIBRETRO_CORES["wonderswan"], "mednafen_wswan")
        self.assertEqual(play_options.LIBRETRO_CORES["ngp"], "mednafen_ngp")
        self.assertEqual(play_options.FRAMES["wsc-blue"], "wonderswan")
        self.assertEqual(play_options.FRAMES["ngpc-black"], "ngp")
        # A Color game on the shared shelf is looked up in the Color's lists, plain or zipped.
        colour = self.root / "Card Fighters Clash.ngc"
        colour.write_bytes(bytes(0xB0))
        self.assertEqual(public_artwork.rom_identity(colour)[0], "ngpc")
        mono = self.root / "Gunpey.ws"
        mono.write_bytes(bytes(0xB0))
        self.assertEqual(public_artwork.rom_identity(mono)[0], "wonderswan")
        zipped = self.root / "Judgement Silversword.zip"
        with zipfile.ZipFile(zipped, "w") as archive:
            archive.writestr("Judgement Silversword.wsc", bytes(0xB0))
        self.assertEqual(public_artwork.rom_identity(zipped)[0], "wsc")
        for system in ("wonderswan", "wsc", "ngp", "ngpc"):
            self.assertIn(system, public_artwork.CATALOGS)
            self.assertIn(system, screenscraper.SYSTEMS)

    def test_virtual_boy(self):
        roms = self.root / "roms"
        self.assertEqual(play_options.system_of(self.root / "Wario Land.vb"), "vb")
        self.assertEqual(play_options.system_of(self.root / "Red Alarm.vboy"), "vb")
        self.assertEqual(play_options.system_of(roms / "Virtual Boy" / "Galactic Pinball.bin"), "vb")
        self.assertEqual(play_options.system_of(roms / "vb" / "Teleroboxer.bin"), "vb")
        self.assertEqual(play_options.LIBRETRO_CORES["vb"], "mednafen_vb")
        self.assertEqual(play_options.FRAMES["vb"], "vb")
        # The 3D choice is the Second screen picker's fifth place (the keyboard's slug), as the VMU's is.
        self.assertIn("keyboard", play_options.SECONDS)

    def test_dos_games_go_by_their_folder(self):
        roms = self.root / "roms"
        # A DOS game is a zip of its folder (DOSBox Pure opens it), even one holding a CD's .cue or an .iso.
        game = roms / "dos" / "Doom.zip"
        game.parent.mkdir(parents=True)
        with zipfile.ZipFile(game, "w") as archive:
            archive.writestr("DOOM/GAME.CUE", "FILE")
            archive.writestr("DOOM/DOOM.EXE", "MZ")
        self.assertEqual(play_options.system_of(game), "dos")
        for name in ("Game.iso", "Game.cue", "Game.img", "Game.m3u"):
            self.assertEqual(play_options.system_of(roms / "dos" / name), "dos", name)
        self.assertEqual(play_options.system_of(roms / "MS-DOS" / "Game.zip"), "dos")
        self.assertEqual(play_options.system_of(self.root / "Game.dosz"), "dos")
        self.assertEqual(play_options.system_of(roms / "psx" / "Game.cue"), "psx")  # elsewhere a .cue stays a disc
        self.assertEqual(play_options.engine_for(game), play_options.PLAY_CORE)
        self.assertEqual(play_options.LIBRETRO_CORES["dos"], "dosbox_pure")
        self.assertIn("dos", play_options.FRAMES["monitor"].split(","))

    def test_flash_movies_and_flashpoint_packs(self):
        roms = self.root / "roms"
        # A movie anywhere; a Flashpoint pack (a zip of web files, no game file a core knows) only in a Flash folder.
        self.assertEqual(play_options.system_of(self.root / "Game.swf"), "flash")
        pack = roms / "Flashpoint" / "Game.zip"
        pack.parent.mkdir(parents=True)
        with zipfile.ZipFile(pack, "w") as archive:
            archive.writestr("content/www.example.com/game.swf", "FWS")
        self.assertEqual(play_options.system_of(pack), "flash")
        self.assertEqual(play_options.engine_for(pack), play_options.PLAY_CORE)
        self.assertEqual(play_options.LIBRETRO_CORES["flash"], "ruffle")
        # Ruffle saves no states: Flash games keep their own saves, so no checkpoints are asked for.
        self.assertIn("flash", play_options.NO_CHECKPOINTS)
        self.assertIn("flash", play_options.FRAMES["monitor"].split(","))

    def test_pico8_carts_and_their_pictures(self):
        roms = self.root / "roms"
        self.assertEqual(play_options.system_of(self.root / "celeste.p8"), "pico8")
        self.assertEqual(play_options.system_of(roms / "pico8" / "celeste.p8.png"), "pico8")
        self.assertEqual(play_options.system_of(roms / "nes" / "cover.png"), "nds")  # a picture elsewhere is no cart

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
                self.assertEqual(play_options.LIBRETRO_CORES["pico8"], "fake08")  # back 2026-11-18
                self.assertEqual(play_options.LIBRETRO_CORES["n64"], "mupen64plus_next")
                self.assertEqual(play_options.LIBRETRO_CORES["zx"], "fuse")
                self.assertEqual(play_options.LIBRETRO_CORES["c64"], "vice_x64")
                self.assertEqual(play_options.LIBRETRO_CORES["gw"], "gw")
                self.assertEqual(play_options.LIBRETRO_CORES["psp"], "ppsspp")
                self.assertEqual(play_options.LIBRETRO_CORES["dc"], "flycast")
        # Appended: the DS engines keep their stored indexes.
        self.assertEqual(emulators.CORES[:5], ("drastic", "dsperate", "nnddss", "melonds", "rgds-experimental"))
        self.assertEqual(emulators.CORES[5], "rgds-play")

    def test_a_3ds_game_runs_tanuki_from_the_data_partition_with_its_files_in_userdata(self):
        with tempfile.TemporaryDirectory() as tmp:
            engine, data = Path(tmp) / "engine", Path(tmp) / "data"
            with self.assertRaisesRegex(ValueError, "No emulator for 3DS"):
                emulators.tanuki_plan(data, Path(tmp) / "a.cci", engine=engine)
            engine.mkdir()
            (engine / "run.sh").write_text("")
            (engine / "ctremu.ini").write_text("Volume = 150")
            plan = emulators.tanuki_plan(data, Path(tmp) / "a.cci", engine=engine)
            # the shared menu, with the engine holding the display
            self.assertEqual(plan["env"]["RGDS_MENU_LIVE"], "1")
            self.assertEqual(plan["env"]["RGDS_CONTROL_FILE"], str(data / "session/emulator.control"))
            self.assertNotIn("RGDS_NATIVE_AUTOLOAD", plan["env"])
            # the looks made for sharp modern screens fit a 3DS game, another handheld's screen does not
            rom = Path(tmp) / "a.cci"
            self.assertTrue(play_options.shader_fits(rom, 15))
            self.assertFalse(play_options.shader_fits(rom, 21))
            self.assertFalse(play_options.shader_fits(rom, 30))
            self.assertEqual(plan["env"]["HOME"], str(data / "emulators/rgds-play/3ds"))
            settings = data / "emulators/rgds-play/3ds/.local/share/Tanuki3DS/ctremu.ini"
            self.assertEqual(settings.read_text(), "Volume = 150")
            settings.write_text("Volume = 90")  # the owner's own settings survive the next launch
            plan = emulators.tanuki_plan(data, Path(tmp) / "a.cci", engine=engine)
            self.assertEqual(settings.read_text(), "Volume = 90")
            # the internal resolution is per game, 1x unless the game's play.ini says otherwise
            self.assertEqual(plan["env"]["RGDS_3DS_SCALE"], "1")
            play_options.save_scale(data, rom, 2)
            self.assertEqual(emulators.tanuki_plan(data, rom, engine=engine)["env"]["RGDS_3DS_SCALE"], "2")
            self.assertEqual(play_options.view(data, rom)["scale"], 2)
            play_options.save_scale(data, rom, 1)
            self.assertEqual(emulators.tanuki_plan(data, rom, engine=engine)["env"]["RGDS_3DS_SCALE"], "1")
            self.assertNotIn("scale", play_options.read_fields(play_options._path(data, rom)))
            with self.assertRaises(ValueError):
                play_options.save_scale(data, rom, 5)

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
        self.assertEqual(
            [int(n) for n in defaults.split(",")],
            [play_options.SYSTEM_SHADERS[s] for s in ("gba", "gb", "gbc", "3ds")]
        )

    def test_system_defaults_sit_between_the_built_in_view_and_a_game(self):
        # Nothing set anywhere: the built-in view.
        self.assertEqual(play_options.view(self.data, self.gba)["frame"], "off")
        defaults = self.data / "system-defaults" / "gba.ini"
        defaults.parent.mkdir(parents=True)
        defaults.write_text("schema=1\nfit=smooth\nsecond=info\nframe=black\nfilter=33\n")
        shown = play_options.view(self.data, self.gba)
        self.assertEqual((shown["fit"], shown["second"], shown["frame"]), ("smooth", "info", "black"))
        # A game that differs keeps only that key; picking the default again follows it.
        play_options.save_view(self.data, self.gba, 1, 3, "indigo")
        stored = play_options.read_fields(play_options._path(self.data, self.gba))
        self.assertEqual((stored.get("fit"), stored.get("second"), stored.get("frame")), ("integer", None, "indigo"))
        play_options.save_view(self.data, self.gba, 2, 3, "black")
        self.assertNotIn("fit", play_options.read_fields(play_options._path(self.data, self.gba)))
        defaults.write_text("schema=1\nfit=sharp\n")
        self.assertEqual(play_options.view(self.data, self.gba)["fit"], "sharp")
        # Only valid keys count; another system's defaults are another file.
        defaults.write_text("fit=bogus\nframe=sp-cobalt\npalette=nope\n")
        self.assertEqual(play_options.view(self.data, self.gba)["fit"], "sharp")
        self.assertEqual(play_options.view(self.data, self.gba)["frame"], "sp-cobalt")
        self.assertEqual(play_options.system_defaults(self.data, "gb"), {})

    def test_a_systems_default_shader_beats_its_own_screen_but_not_the_games(self):
        defaults = self.data / "system-defaults" / "gba.ini"
        defaults.parent.mkdir(parents=True)
        defaults.write_text("filter=33\n")
        choice = play_options.system_defaults(self.data, "gba")["filter"]
        self.assertEqual(play_options.shader_for(self.gba, -1, 0, choice), 33)  # system default
        self.assertEqual(play_options.shader_for(self.gba, 34, 0, choice), 34)  # the game's own choice
        self.assertEqual(play_options.shader_for(self.gba, -1, 0), 30)  # none: the GBA screen
        self.assertEqual(play_options.shader_for(self.gba, -1, 0, 31), 30)  # another system's screen never applies

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

    def test_keyboard_first_is_a_second_screen_choice_in_the_menu_and_the_files(self):
        self.assertEqual(play_options.SECONDS.index("keyboard"), 4)
        menu = self.menu("rgds-play")
        self.request(menu, "play-graphics:0:4:off")
        self.assertEqual(menu.pending["action"], "play-graphics:0:4:off")
        self.assertIn("play-graphics:0:4:off", menu.control.read_text())
        # One past the last choice is still refused.
        bad = self.menu("rgds-play")
        self.request(bad, "play-graphics:0:5:off")
        self.assertIsNone(bad.pending)
        data = self.root / "data"
        play_options.save_view(data, self.rom, 0, 4, "off")
        self.assertEqual(play_options.view(data, self.rom)["second"], "keyboard")

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
                "colour": "gbc",
                "scale": 1
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
        for rom, frame, known in (
            (self.rom, "indigo", True), (self.rom, "sp-cobalt", True), (self.rom, "dmg", False), (gb, "dmg", True),
            (gb, "pocket-black", True), (gb, "indigo", False), (gb, "sp-onyx", False), (gb, "gbc-berry",
                                                                                        False), (gbc, "gbc-teal", True),
            (gbc, "dmg", False), (self.rom, "bar", True), (gb, "bar", False), (gbc, "bar", False), (gbc, "off", True)
        ):
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
        self.assertEqual(play_options.view(self.root / "data", self.rom)["frame"], "Perfect GBA (mugwomp93)")
        # A picture that is gone falls back to no frame at the next launch.
        (folder / "Perfect GBA (mugwomp93).png").unlink()
        self.assertEqual(play_options.view(self.root / "data", self.rom)["frame"], "off")

    def test_view_changes_are_refused_for_other_engines_and_bad_values(self):
        for core, action in (
            ("rgds-experimental",
             "play-graphics:0:0:off"), ("rgds-play", "play-graphics:3:0:off"), ("rgds-play", "play-graphics:0:0"),
            ("rgds-play", "play-graphics:0:0:no such picture"), ("rgds-play", "play-graphics:0:0:../escape")
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
