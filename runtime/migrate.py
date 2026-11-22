"""Coming from another system: in-game saves, play time, favourites, BIOS files (and the games) from BananaOS or
ROCKNIX, in one press.

Where an old system's files can be found (sources, newest first):
- flasher: RG DS Flasher read the old card before it erased it and left a bundle at the end of this card; rcS
  (usr/libexec/rgds/handoff) copies it to /data/rgds/import/flasher.tar.gz on the first boot. The games stayed on
  the computer, so the bundle lists them (manifest.json) for play time and saves to find them later.
- card: the old card itself, in the RG DS Plus's second slot or a USB reader, mounted read-only here.
- folder: the old card's files copied into /data/import on this card.

BananaOS (its docs/EMULATORS.md and ARCHITECTURE.md): an exFAT partition with roms/<folder>, saves/ (RetroArch's
.srm straight in saves/, BananaDS's in saves/nds), states/, drastic/backup/*.dsv, bios/, banana/playstats.tsv
("<system>/<rel>\tplays\tseconds\tlast"), banana/favorites.txt ("<system>/<rel>").
Anbernic's stock firmware (RG DS and RG DS Plus): games in Roms/<FC|SFC|MD|GBA|NDS|...> on the FAT/exFAT games
partition (/mnt/mmc; on a Plus with two cards, the card in slot 2), RetroArch's saves mirrored in
saves_RA/Roms/<system>/, its states in states_RA/Roms/<system>/, standalone emulators' saves in save/Roms/<system>/.
DraStic keeps its DS saves in /mnt/vendor/deep/drastic*/backup/*.dsv on an ext4 partition of the system card (a card
in a reader shows that partition on its own: its root holds deep/); RG DS Flasher reads it at flash time into
vendor/deep/... in its bundle. The stock menu keeps no play time. Its states stay behind (which core made them is
not known). Folder layout from setup guides and the RG DS's own card (docs/PORTMASTER_PLAN.md), not yet from a stock
card's saves.
ROCKNIX: an ext4 STORAGE partition with roms/<folder> holding the saves beside the games and EmulationStation's
gamelist.xml (playcount, gametime, lastplayed); states in roms/savestates/<folder>. Not checked on a real ROCKNIX
card yet.

What is never done: a save or state already here is never replaced (the new one is counted as kept), a game
already here is never copied over, and the source is only ever read (a card is mounted read-only, ext4 without
replaying its journal). States carry over only where the old system ran the same emulator (BananaOS's FCEUmm,
Snes9x, PicoDrive, PCSX-ReArmed and Mupen64Plus-Next); DS states never do (BananaDS and DraStic are other engines).

    migrate.py scan  --data DIR [--json]      what can come over, also written to import/plan.json
    migrate.py apply --data DIR [--games]     bring it over
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ElementTree
import zlib

from fields import read_fields, write_fields
from library import game_key
from play_options import ARCADE_SETS

# Folder names the old systems use, to this card's folders (rcS makes them; the session scans them).
FOLDERS = {
    "nds": "nds",
    "gba": "gba",
    "gb": "gb",
    "gbc": "gbc",
    "nes": "nes",
    "famicom": "nes",
    "fc": "nes",
    "snes": "snes",
    "sfc": "snes",
    "superfamicom": "snes",
    "megadrive": "md",
    "genesis": "md",
    "md": "md",
    "mastersystem": "sms",
    "sms": "sms",
    "gamegear": "sms",
    "gg": "sms",
    "psx": "psx",
    "ps1": "psx",
    "playstation": "psx",
    "n64": "n64",
    "psp": "psp",
    "dreamcast": "dc",
    "dc": "dc",
    "zxspectrum": "zx",
    "zx": "zx",
    "c64": "c64",
    "amiga": "amiga",
    "amstradcpc": "cpc",
    "cpc": "cpc",
    "pcengine": "pce",
    "pce": "pce",
    "tg16": "pce",
    "pcenginecd": "pcecd",
    "pcecd": "pcecd",
    "segacd": "segacd",
    "megacd": "segacd",
    "atari2600": "atari",
    "atari": "atari",
    "atari7800": "atari7800",
    "atari5200": "atari5200",
    "atarilynx": "lynx",
    "lynx": "lynx",
    # an arcade folder's sets: only the two-monitor cabinets the core runs come over (Game.wanted)
    "arcade": "arcade",
    "mame": "arcade",
    "mame2003": "arcade",
    "mame2003-plus": "arcade",
    "mame2003plus": "arcade",
    "wonderswan": "wonderswan",
    "wonderswancolor": "wonderswan",
    "wswan": "wonderswan",
    "wswanc": "wonderswan",
    "ngp": "ngp",
    "ngpc": "ngp",
    "virtualboy": "vb",
    "vb": "vb",
    "colecovision": "coleco",
    "coleco": "coleco",
    "intellivision": "intellivision",
    "msx": "msx",
    "msx2": "msx",
    "gameandwatch": "gw",
    "gw": "gw",
    "scummvm": "scummvm",
    "pico-8": "pico8",
    "pico8": "pico8",
    "dos": "dos",
    "pc": "dos",
    "flash": "flash",
    "3ds": "3ds",
}
# Anbernic stock firmware's Roms folders (its own names, kept apart: its PICO is PICO-8, ours is the Sega Pico).
STOCK_FOLDERS = {
    "nds": "nds",
    "gba": "gba",
    "gb": "gb",
    "gbc": "gbc",
    "fc": "nes",
    "sfc": "snes",
    "md": "md",
    "sega32x": "md",
    "mdcd": "segacd",
    "sms": "sms",
    "gg": "sms",
    "ps": "psx",
    "psp": "psp",
    "n64": "n64",
    "dreamcast": "dc",
    "pce": "pce",
    "pcecd": "pcecd",
    "msx": "msx",
    "a2600": "atari",
    "a5200": "atari5200",
    "a7800": "atari7800",
    "lynx": "lynx",
    "vb": "vb",
    "ws": "wonderswan",
    "ngp": "ngp",
    "c64": "c64",
    "amiga": "amiga",
    "dos": "dos",
    "scummvm": "scummvm",
    "gw": "gw",
    "pico": "pico8",
    "arcade": "arcade",
    "mame": "arcade",
}
# BananaOS's systems.conf ids whose folder is not the id (its own banana/systems.conf can say otherwise)
BANANA_FOLDERS = {"megadrive": "megadrive", "pico-8": "pico-8"}
# BananaOS's cores that are ours too (play_options.LIBRETRO_CORES): only their states can load here.
SAME_CORE = {"bananaos": ("nes", "snes", "md", "psx", "n64")}
SAVE_SUFFIXES = (".srm", ".sav")
STATE_SUFFIX = re.compile(r"\.state(\d+|\.auto)?$")
# What sits in a games folder but is not a game: saves, states, EmulationStation's lists and pictures.
NOT_GAMES = {
    ".srm", ".sav", ".dsv", ".rtc", ".state", ".auto", ".bak", ".xml", ".png", ".jpg", ".jpeg", ".mp4", ".txt", ".cfg",
    ".pdf", ".db"
}
NOT_GAME_FOLDERS = {"images", "videos", "media", "manuals", "snap", "snaps", "savestates", "saves", "downloaded_images"}
NAMES = {"bananaos": "BananaOS", "rocknix": "ROCKNIX", "stock": "the stock system", "other": "another system"}


def device_paths(data):
    """Where things go on this card (data is the launcher's --data, /data/rgds/userdata)."""
    from linux_platform import RGDS_OS_ROMS
    from profiles import player_root
    card = Path(data).parent.parent if Path(data).name == "userdata" else Path(data)
    return {
        "player": player_root(data),
        "roms": Path(os.environ.get("RGDS_IMPORT_ROMS", RGDS_OS_ROMS)),
        "bios": Path(os.environ.get("RGDS_IMPORT_BIOS", card / "rgds/bios")),
        "work": Path(os.environ.get("RGDS_IMPORT_WORK", card / "rgds/import")),
        "folder": Path(os.environ.get("RGDS_IMPORT_FOLDER", card / "import")),
    }


# -- reading the old files ------------------------------------------------------------------------------------------


def dsv_raw(data):
    """DraStic's .dsv as the raw save: one written by DeSmuME has a footer ("|-DESMUME SAVE-|" last, the padded size
    36 bytes from the end), as BananaOS's nds-saves reads it."""
    if len(data) > 122 and data[-16:] == b"|-DESMUME SAVE-|":
        padded = struct.unpack_from("<I", data, len(data) - 36)[0]
        end = padded if 0 < padded <= len(data) - 40 else len(data) - 122
        return data[:end]
    return data


def retroarch_state(data):
    """A RetroArch state as the core's own bytes: RZIP-compressed, an RASTATE container or raw. None when unknown."""
    if data[:6] == b"#RZIPv":
        # "#RZIPv<n>#", the chunk size (u32), the whole size (u64), then chunks: length (u32) and zlib data
        total = struct.unpack_from("<Q", data, 12)[0]
        out, at = bytearray(), 20
        while at + 4 <= len(data) and len(out) < total:
            size = struct.unpack_from("<I", data, at)[0]
            out += zlib.decompress(data[at + 4:at + 4 + size])
            at += 4 + size
        data = bytes(out[:total])
    if data[:7] == b"RASTATE":
        # blocks after the 8-byte header: an id (4), its size (u32), its data padded to 8 bytes; MEM is the core's
        at = 8
        while at + 8 <= len(data):
            name, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
            if name == b"MEM ": return data[at + 8:at + 8 + size]
            if name == b"END ": break
            at += 8 + ((size + 7) & ~7)
        return None
    return data or None


def layout(root):
    """Which system's files are in root: 'bananaos', 'rocknix', 'other' (a roms folder with saves) or None."""
    root = Path(root)
    manifest = read_manifest(root)
    if manifest: return manifest.get("source", "other")
    if (root / "banana").is_dir() and ((root / "saves").is_dir() or (root / "roms").is_dir()): return "bananaos"
    if stock_drastic(root): return "stock"  # the stock system card's vendor partition, seen on its own
    games = roms_folder(root)
    if games is not None and games.name == "Roms" and (
        any((root / name).is_dir() for name in ("saves_RA", "states_RA", "save")) or (games / "NDS").is_dir()
    ):
        return "stock"
    if games is None: return None
    if (root / ".config/emulationstation").is_dir() or (root / ".config/system").is_dir() or any(
        (games / sub / "gamelist.xml").is_file() for sub in list_dirs(games)[:40]
    ):
        return "rocknix"
    return "other"


def read_manifest(root):
    try:
        manifest = json.loads((Path(root) / "manifest.json").read_text(encoding="utf-8"))
        return manifest if manifest.get("format") == 1 else None
    except (OSError, ValueError, AttributeError):
        return None


def stock_drastic(root):
    """DraStic's homes on a stock card's vendor partition (deep/drastic_aarch64, deep/drastic64...)."""
    found = []
    for base in (Path(root) / "deep", Path(root) / "vendor/deep"):
        try:
            found += [d for d in sorted(base.iterdir()) if d.name.startswith("drastic") and (d / "backup").is_dir()]
        except OSError:
            pass
    return found


def roms_folder(root):
    for sub in ("Roms", "roms", "games-internal/roms", "games-external/roms", "storage/roms"):
        if (Path(root) / sub).is_dir():
            # the name as it is on the card: FAT is case-blind, and the stock layout is told apart by it
            try:
                return next(
                    p for p in Path(root).joinpath(*Path(sub).parts[:-1]).iterdir()
                    if p.name.lower() == Path(sub).name.lower() and p.is_dir()
                )
            except (OSError, StopIteration):
                return Path(root) / sub
    return None


def list_dirs(path):
    try:
        return sorted(p.name for p in Path(path).iterdir() if p.is_dir() and not p.name.startswith("."))
    except OSError:
        return []


def walk_files(folder, depth=3):
    """Files under folder, as paths relative to it, a few levels deep (games in subfolders, discs in theirs)."""
    folder = Path(folder)
    try:
        entries = sorted(folder.iterdir())
    except OSError:
        return
    for entry in entries:
        if entry.name.startswith("."): continue
        if entry.is_dir():
            if depth > 0 and entry.name.lower() not in NOT_GAME_FOLDERS:
                for rel in walk_files(entry, depth - 1):
                    yield Path(entry.name) / rel
        elif entry.is_file():
            yield Path(entry.name)


class Game:
    """One game as the old system knew it, and where it is (or will be) on this card."""
    def __init__(self, folder, rel, size, source=None, folders=FOLDERS, listed=True):
        self.folder, self.rel, self.size, self.source = folder, Path(rel), size, source
        self.ours = folders.get(folder.lower())
        # False for a DS game known only from its save (stock DraStic's backup/): it has a save, not a place
        self.listed = listed

    @property
    def wanted(self):
        """A game this card has a place for: in an arcade folder, only a set the core is built with."""
        if self.ours != "arcade": return bool(self.ours)
        return self.rel.suffix.lower() == ".zip" and self.rel.stem.lower() in ARCADE_SETS

    def destination(self, roms):
        return Path(roms) / self.ours / self.rel

    @property
    def stem(self):
        return self.rel.stem


class Source:
    """An old system's files under root (read only)."""
    def __init__(self, root, place, kind, extra=()):
        self.root, self.place, self.kind = Path(root), place, kind
        # more partitions of the same old card (stock: the vendor partition with DraStic's saves)
        self.extra = [Path(p) for p in extra]
        self.folders = STOCK_FOLDERS if kind == "stock" else FOLDERS
        self.manifest = read_manifest(root) or {}
        self.games = self.find_games()
        if kind == "stock":
            # DS games known only by DraStic's saves (the games may be on the computer, or nowhere)
            stems = {g.stem for g in self.games if g.ours == "nds"}
            for home in self.drastic_homes():
                try:
                    names = sorted(p.name for p in (home / "backup").iterdir() if p.suffix.lower() == ".dsv")
                except OSError:
                    continue
                for name in names:
                    stem = name[:-4]
                    if stem not in stems:
                        stems.add(stem)
                        self.games.append(Game("NDS", stem + ".nds", 0, folders=self.folders, listed=False))

    def drastic_homes(self):
        return [home for root in [self.root] + self.extra for home in stock_drastic(root)]

    @property
    def name(self):
        return NAMES.get(self.kind, NAMES["other"])

    def find_games(self):
        games = []
        listed = self.manifest.get("roms")
        if listed is not None and roms_folder(self.root) is None:
            # The flasher's bundle: the games stayed on the computer, only their names came along.
            for entry in listed:
                try:
                    path, size = Path(entry[0]), int(entry[1])
                except (TypeError, ValueError, IndexError):
                    continue
                if len(path.parts) >= 2 and ".." not in path.parts:
                    games.append(Game(path.parts[0], Path(*path.parts[1:]), size, folders=self.folders))
            return [g for g in games if g.wanted]
        folder = roms_folder(self.root)
        if folder is None: return []
        for system in list_dirs(folder):
            if not self.folders.get(system.lower()): continue
            for rel in walk_files(folder / system):
                if rel.suffix.lower() in NOT_GAMES or rel.name.lower().endswith(".state.auto"): continue
                if STATE_SUFFIX.search(rel.name): continue
                path = folder / system / rel
                try:
                    size = path.stat().st_size
                except OSError:
                    continue
                game = Game(system, rel, size, path, folders=self.folders)
                if game.wanted: games.append(game)
        return games

    def folder_of(self, system):
        """BananaOS: the folder of a systems.conf id (its own banana/systems.conf first)."""
        conf = self.root / "banana/systems.conf"
        try:
            section = None
            for line in conf.read_text(encoding="utf-8", errors="replace").splitlines():
                line = line.strip()
                if line.startswith("[") and line.endswith("]"): section = line[1:-1]
                elif section == system and line.startswith("folder") and "=" in line:
                    value = line.split("=", 1)[1].strip()
                    if not value.startswith("/"): return value
        except OSError:
            pass
        return BANANA_FOLDERS.get(system, system)

    def game(self, folder, rel):
        for g in self.games:
            if g.folder.lower() == folder.lower() and g.rel == Path(rel):
                return g
        return Game(folder, rel, 0, folders=self.folders) if self.folders.get(folder.lower()) else None

    # -- saves ------------------------------------------------------------------------------------------------------

    def save_for(self, game):
        """The newest in-game save the old system kept for game: (bytes, mtime) or None."""
        found = []

        def add(path, convert=None):
            try:
                if path.is_file() and path.stat().st_size > 0:
                    data = path.read_bytes()
                    found.append((convert(data) if convert else data, path.stat().st_mtime))
            except OSError:
                pass

        stem = game.stem
        if self.kind == "stock":
            # RetroArch mirrors the game's path under saves_RA; standalone emulators keep theirs under save/
            for root in [self.root] + self.extra:
                for base in (root / "saves_RA/Roms" / game.folder, root / "save/Roms" / game.folder):
                    for suffix in SAVE_SUFFIXES + (".dsv",):
                        add(base / game.rel.parent / (stem + suffix), dsv_raw if suffix == ".dsv" else None)
            if game.ours == "nds":
                for home in self.drastic_homes():
                    add(home / "backup" / (stem + ".dsv"), dsv_raw)
        elif self.kind == "bananaos":
            if game.ours == "nds":
                add(self.root / "saves/nds" / (stem + ".sav"))
                add(self.root / "drastic/backup" / (stem + ".dsv"), dsv_raw)
            else:
                for suffix in SAVE_SUFFIXES:
                    add(self.root / "saves" / (stem + suffix))
                    add(self.root / "saves" / game.folder / (stem + suffix))
        else:
            folder = roms_folder(self.root)
            beside = (folder / game.folder / game.rel).parent if folder else None
            for suffix in SAVE_SUFFIXES:
                if beside: add(beside / (stem + suffix))
                add(self.root / "saves" / game.folder / (stem + suffix))
            if game.ours == "nds":
                for base in (self.root / ".config/drastic/backup", self.root / "drastic/backup"):
                    add(base / (stem + ".dsv"), dsv_raw)
        return max(found, key=lambda item: item[1]) if found else None

    def state_for(self, game, save_time):
        """The newest state, as the core's bytes, when the same emulator runs it here and it is no older than the
        save (an older one would put an older save back)."""
        if game.ours not in SAME_CORE.get(self.kind, ()): return None
        best = None
        for folder in (self.root / "states", self.root / "states" / game.folder):
            try:
                entries = list(folder.iterdir())
            except OSError:
                continue
            for path in entries:
                if not path.name.startswith(game.stem + ".state"): continue
                if not STATE_SUFFIX.search(path.name) or path.name[len(game.stem):].count(".") > 2: continue
                try:
                    mtime = path.stat().st_mtime
                except OSError:
                    continue
                if best is None or mtime > best[1]: best = (path, mtime)
        if best is None or (save_time is not None and best[1] < save_time): return None
        try:
            data = retroarch_state(best[0].read_bytes())
        except (OSError, zlib.error, struct.error):
            return None
        return (data, best[1]) if data else None

    # -- play time and favourites -----------------------------------------------------------------------------------

    def play_time(self):
        """[(game, plays, seconds, last epoch)] for every game the old system timed."""
        rows = []
        if self.kind == "bananaos":
            try:
                lines = (self.root / "banana/playstats.tsv").read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                lines = []
            for line in lines:
                cells = line.split("\t")
                if len(cells) < 4 or "/" not in cells[0]: continue
                system, rel = cells[0].split("/", 1)
                game = self.game(self.folder_of(system), rel)
                try:
                    plays, seconds, last = int(cells[1]), int(cells[2]), int(cells[3])
                except ValueError:
                    continue
                if game and seconds > 0: rows.append((game, plays, seconds, last))
        else:
            folder = roms_folder(self.root)
            for system in list_dirs(folder) if folder else []:
                listing = folder / system / "gamelist.xml"
                if not listing.is_file() or not FOLDERS.get(system.lower()): continue
                try:
                    tree = ElementTree.parse(listing)
                except (OSError, ElementTree.ParseError):
                    continue
                for node in tree.getroot().iter("game"):
                    path = (node.findtext("path") or "").strip()
                    if path.startswith("./"): path = path[2:]
                    try:
                        seconds = int(node.findtext("gametime") or 0)
                        plays = int(node.findtext("playcount") or 0)
                    except ValueError:
                        continue
                    last = 0
                    try:
                        last = int(
                            datetime.strptime((node.findtext("lastplayed") or "")[:15],
                                              "%Y%m%dT%H%M%S").replace(tzinfo=timezone.utc).timestamp()
                        )
                    except ValueError:
                        pass
                    game = self.game(system, path) if path and ".." not in Path(path).parts else None
                    if game and seconds > 0: rows.append((game, plays, seconds, last))
        return rows

    def favourites(self):
        if self.kind != "bananaos": return []
        try:
            lines = (self.root / "banana/favorites.txt").read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return []
        found = []
        for line in lines:
            if "/" not in line.strip(): continue
            system, rel = line.strip().split("/", 1)
            game = self.game(self.folder_of(system), rel)
            if game: found.append(game)
        return found

    def bios_files(self):
        for folder in (self.root / "bios", (roms_folder(self.root) or self.root / "-") / "bios"):
            try:
                for path in sorted(folder.iterdir()):
                    if path.is_file() and not path.name.startswith("."): yield path
            except OSError:
                pass

    def fingerprint(self):
        """The same old system seen again (on the card, then copied into the folder) is imported once."""
        digest = hashlib.sha256(self.kind.encode())
        for name in ("banana/playstats.tsv", "manifest.json"):
            try:
                digest.update((self.root / name).read_bytes())
            except OSError:
                pass
        for game in self.games[:200]:
            digest.update(str(game.folder + "/" + str(game.rel)).encode())
        return digest.hexdigest()[:16]


# -- where they go ----------------------------------------------------------------------------------------------------


def save_destination(player, roms, game):
    if game.ours == "nds": return Path(player) / "emulators/rgds-experimental/saves" / (game.stem + ".sav")
    if game.ours == "3ds": return None
    from play_options import system_of
    return Path(player) / "emulators/rgds-play/saves" / system_of(game.destination(roms)) / (game.stem + ".sav")


class Plan:
    """Everything one source would bring: counted by scan, carried out by apply."""
    def __init__(self, source, paths):
        self.source, self.paths = source, paths
        player, roms = paths["player"], paths["roms"]
        self.saves, self.states, self.kept, self.played, self.favourites, self.bios = [], [], 0, [], [], []
        stems = {}
        for game in source.games:
            if game.ours != "nds": stems[game.stem] = stems.get(game.stem, 0) + 1
        for game in source.games:
            # RetroArch kept BananaOS's saves in one folder by name: two systems' games of one name share it, and
            # which system it belongs to can't be told.
            if game.ours != "nds" and source.kind == "bananaos" and stems[game.stem] > 1: continue
            target = save_destination(player, roms, game)
            if target is None: continue
            save = source.save_for(game)
            if save:
                if target.exists(): self.kept += target.read_bytes() != save[0]
                else: self.saves.append((game, target, save))
            state = source.state_for(game, save[1] if save else None)
            if state:
                auto = Path(player) / "library" / game_key(game.destination(roms)) / "auto-rgds-play.dss"
                if not auto.exists(): self.states.append((game, auto, state))
        self.played = source.play_time()
        self.favourites = source.favourites()
        self.bios = [p for p in source.bios_files() if not (paths["bios"] / p.name).exists()]
        listed = [g for g in source.games if g.listed]
        self.games_here = sum(1 for g in listed if g.destination(roms).exists())
        self.games_to_copy = [g for g in listed if g.source and not g.destination(roms).exists()]

    def summary(self):
        seconds = sum(row[2] for row in self.played)
        return {
            "import_found": 1,
            "import_source": self.source.name,
            "import_kind": self.source.kind,
            "import_place": self.source.place,
            "import_saves": len(self.saves),
            "import_states": len(self.states),
            "import_kept": self.kept,
            "import_played": len(self.played),
            "import_hours": ("%.1f" if seconds < 36000 else "%.0f") % max(0.1, seconds / 3600) if seconds else "0",
            "import_favourites": len(self.favourites),
            "import_bios": len(self.bios),
            "import_games": sum(1 for g in self.source.games if g.listed),
            "import_games_here": self.games_here,
            "import_games_copy": len(self.games_to_copy),
            "import_games_bytes": sum(g.size for g in self.games_to_copy),
            "import_games_away": sum(1 for g in self.source.games if g.listed) - self.games_here -
            len(self.games_to_copy),
        }

    def apply(self, games=False, progress=lambda done, total: None):
        """Bring it all over; returns what was done. Play time is added once per source (done.ini)."""
        paths = self.paths
        work = paths["work"]
        done_file = work / "done.ini"
        done = read_fields(done_file)
        fingerprint = self.source.fingerprint()
        result = {"saves": 0, "states": 0, "played": 0, "favourites": 0, "bios": 0, "games": 0}
        copy = self.games_to_copy if games else []
        total = len(self.saves) + len(self.states) + len(self.bios) + sum(g.size for g in copy) // (1 << 20) + 1
        step = 0
        # The saves, play time and BIOS files first: small, and what can't be found again. The games after them,
        # so a card too full for the games still gets everything else.
        for game, target, (data, mtime) in self.saves:
            if not target.exists():
                write_file(target, data, mtime)
                result["saves"] += 1
            step += 1
            progress(step, total)
        for game, target, (data, mtime) in self.states:
            if not target.exists():
                write_file(target, data, mtime)
                result["states"] += 1
            step += 1
        for path in self.bios:
            target = paths["bios"] / path.name
            if not target.exists():
                copy_file(path, target)
                result["bios"] += 1
            step += 1
        if done.get(fingerprint) != "1":
            for game, plays, seconds, last in self.played:
                add_play_time(paths["player"], game.destination(paths["roms"]), plays, seconds, last)
                result["played"] += 1
            for game in self.favourites:
                result["favourites"] += favourite(paths["player"], game.destination(paths["roms"]))
            done[fingerprint] = "1"
            work.mkdir(parents=True, exist_ok=True)
            write_fields(done_file, done)
        if copy:
            free = shutil.disk_usage(paths["roms"] if paths["roms"].exists() else paths["roms"].parent).free
            need = sum(g.size for g in copy if not g.destination(paths["roms"]).exists())
            # Moving games from the import folder takes no room.
            if self.source.place != "folder" and need > free - (256 << 20):
                raise ValueError(
                    "Your saves and play time are in place, but there is not enough room on this card for the games "
                    "(%.1f GB needed, %.1f GB free)." % (need / 1e9, free / 1e9)
                )
            for game in copy:
                target = game.destination(paths["roms"])
                if target.exists(): continue
                target.parent.mkdir(parents=True, exist_ok=True)
                moved = False
                if self.source.place == "folder":
                    # On this card already: a move is instant and needs no room.
                    try:
                        os.replace(game.source, target)
                        moved = True
                    except OSError:
                        pass
                if not moved: copy_file(game.source, target)
                result["games"] += 1
                step += game.size // (1 << 20)
                progress(step, total)
        progress(total, total)
        return result


def write_file(path, data, mtime=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + ".importing")
    with open(pending, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(pending, path)
    if mtime: os.utime(path, (mtime, mtime))


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    pending = target.with_name(target.name + ".importing")
    try:
        with open(source, "rb") as src, open(pending, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(pending, target)
    finally:
        pending.unlink(missing_ok=True)


def add_play_time(player, rom, plays, seconds, last):
    """The old system's time on top of any here (stats.ini as library.record_playtime writes it). No session rows:
    the Playtime page's days and streaks start with the next game, as for any card with totals only."""
    stats = Path(player) / "library" / game_key(rom) / "stats.ini"
    fields = read_fields(stats)
    try:
        have, sessions = max(0, int(fields.get("seconds", 0))), max(0, int(fields.get("sessions", 0)))
    except ValueError:
        have, sessions = 0, 0
    fields["seconds"] = have + max(0, seconds)
    fields["sessions"] = sessions + max(1, plays)
    if last > 0:
        old = datetime.fromtimestamp(last, timezone.utc).isoformat()
        if not fields.get("last_played") or fields["last_played"] < old: fields["last_played"] = old
    stats.parent.mkdir(parents=True, exist_ok=True)
    write_fields(stats, fields)


def favourite(player, rom):
    """Marks a favourite in game.ini (the launcher's GameOptions), unless the game has options already."""
    options = Path(player) / "library" / game_key(rom) / "game.ini"
    if options.exists(): return 0
    options.parent.mkdir(parents=True, exist_ok=True)
    options.write_text("schema=1\nfavourite=1\n")
    return 1


# -- finding sources ------------------------------------------------------------------------------------------------


def unpack_handoff(work):
    """The flasher's bundle (flasher.tar.gz, put there by rcS) unpacked once into import/flasher."""
    bundle, folder = work / "flasher.tar.gz", work / "flasher"
    if not bundle.is_file() or (folder / "manifest.json").is_file(): return folder if folder.is_dir() else None
    pending = work / "flasher.unpacking"
    shutil.rmtree(pending, ignore_errors=True)
    pending.mkdir(parents=True)
    try:
        with tarfile.open(bundle, "r:gz") as archive:
            for member in archive.getmembers():
                name = Path(member.name)
                # Plain files and folders inside the bundle only.
                if name.is_absolute() or ".." in name.parts or not (member.isfile() or member.isdir()): continue
                archive.extract(member, pending, set_attrs=False)
                if member.isfile(): os.utime(pending / name, (member.mtime, member.mtime))
    except (tarfile.TarError, EOFError, zlib.error, OSError) as error:
        # handoff checked its sha256, so this is a damaged card or a full one: kept aside, never retried at every scan
        print("bring over: the flasher's bundle can't be unpacked:", error, file=sys.stderr)
        shutil.rmtree(pending, ignore_errors=True)
        os.replace(bundle, work / "flasher.tar.gz.bad")
        return None
    shutil.rmtree(folder, ignore_errors=True)
    os.replace(pending, folder)
    return folder


def own_disk():
    try:
        return Path(Path("/run/rgds/disk").read_text().strip()).name
    except OSError:
        return ""


def other_partitions():
    """Block devices that may hold an old card: SD cards (the Plus's second slot, a USB reader's) and USB disks.
    Never this card and never the eMMC (the factory system: an MMC, not an SD)."""
    mine = own_disk()
    base = Path("/sys/class/block")
    try:
        names = sorted(p.name for p in base.iterdir())
    except OSError:
        return []
    found = []
    for name in names:
        if not (name.startswith("mmcblk") or name.startswith("sd")) or "boot" in name or "rpmb" in name: continue
        node = base / name
        partition = (node / "partition").exists()
        disk = node.resolve().parent.name if partition else name
        if not disk or disk == mine: continue
        if disk.startswith("mmcblk"):
            try:
                if (Path("/sys/block") / disk / "device/type").read_text().strip() != "SD": continue
            except OSError:
                continue
        if not partition and any(n.startswith(name) and n != name for n in names): continue
        found.append((name, disk))
    return found


def mount(name, place):
    point = Path("/run/rgds/import") / name
    point.mkdir(parents=True, exist_ok=True)
    if os.path.ismount(point): return point
    for kind, options in (("exfat", "ro,iocharset=utf8"), ("vfat", "ro,utf8"), ("ext4", "ro,noload")):
        result = subprocess.run(["mount", "-t", kind, "-o", options, "/dev/" + name, str(point)], capture_output=True)
        if result.returncode == 0: return point
    return None


def unmount_all():
    base = Path("/run/rgds/import")
    try:
        for point in base.iterdir():
            if os.path.ismount(point): subprocess.run(["umount", str(point)], capture_output=True)
    except OSError:
        pass


def sources(paths, devices=True):
    """Every old system in reach, best first: the flasher's bundle, an old card, the import folder."""
    found = []
    handed = unpack_handoff(paths["work"])
    if handed and layout(handed): found.append(Source(handed, "flasher", layout(handed)))
    if devices:
        # One old card is one source, whatever its partitions: the stock system card has its games on one and
        # DraStic's saves on another (the vendor partition, whose root holds deep/).
        cards = {}
        for name, disk in other_partitions():
            point = mount(name, "card")
            if point is None: continue
            kind = layout(point)
            if kind: cards.setdefault(disk, []).append((point, kind))
            else: subprocess.run(["umount", str(point)], capture_output=True)
        for disk, parts in cards.items():
            # the partition with the games leads; the rest of the same system come along
            parts.sort(key=lambda part: roms_folder(part[0]) is None)
            (main, kind), rest = parts[0], [point for point, other in parts[1:] if other == parts[0][1]]
            found.append(Source(main, "usb" if disk.startswith("sd") else "slot", kind, rest))
    folder = paths["folder"]
    for root in [folder] + [folder / name for name in list_dirs(folder)]:
        kind = layout(root) if root.is_dir() else None
        if kind:
            found.append(Source(root, "folder", kind))
            break
    return found


def best_plan(paths, devices=True):
    """The first source that still has something to bring."""
    done = read_fields(paths["work"] / "done.ini")
    for source in sources(paths, devices):
        plan = Plan(source, paths)
        fresh = done.get(source.fingerprint()) != "1"
        if plan.saves or plan.states or plan.bios or plan.games_to_copy or (fresh and (plan.played or plan.favourites)):
            return plan
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("scan", "apply"))
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--games", action="store_true")
    parser.add_argument("--no-devices", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    paths = device_paths(args.data)
    try:
        plan = best_plan(paths, not args.no_devices)
        if args.action == "scan":
            summary = plan.summary() if plan else {"import_found": 0}
            print(json.dumps(summary) if args.json else "\n".join("%s=%s" % kv for kv in summary.items()))
        elif plan is None:
            print("nothing to bring over")
        else:
            print(json.dumps(plan.apply(args.games, lambda d, t: print("%d/%d" % (d, t), flush=True))))
    finally:
        if not args.no_devices: unmount_all()
    return 0


if __name__ == "__main__": sys.exit(main())
