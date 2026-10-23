"""RG DS Play: the games it plays (every system but the DS) and each game's view settings.

Every launch path (the library, the game switcher, a restart) asks engine_for()
which engine a game needs, so a system is played the same way however it is
started. A game's view lives in library/<key>/play.ini: the launcher rewrites
game.ini with only the keys it knows.
"""
from pathlib import Path
import zipfile

from fields import read_fields, write_fields
from library import game_key

PLAY_CORE = "rgds-play"
# Game files by system; anything else is a DS game for the DS engines.
SYSTEMS = {"gba": (".gba",), "gbc": (".gbc",), "gb": (".gb", ".sgb")}
FITS = ("sharp", "integer", "smooth")
SECONDS = ("companion", "off", "swap")
# A system's own screen when a game's shader is Default (assets/shaders/catalog.tsv):
# GBA screen, the GBA LCD's colour and pixel gaps. Other systems use the default for every game.
SYSTEM_SHADERS = {"gba": 30}
# Shader groups made for one system's screen (the DS screens and GBA screens families in
# src/platform/shader_catalog.hpp): offered, and used, only for that system's games.
DS_SCREENS = frozenset({4, 6, 8, 9, 11, 12, 14, 21, 22})
GBA_SCREENS = frozenset({29, 30})
# What the player can draw around the game (play/src/ui/frame.hpp): its own
# styles, and picture frames the owner puts in userdata/frames/<system>/.
FRAMES = ("off", "indigo", "glacier", "black", "platinum", "bar", "sp-onyx", "sp-cobalt", "sp-platinum")


def _system_of_name(name):
    suffix = Path(name).suffix.lower()
    return next((system for system, suffixes in SYSTEMS.items() if suffix in suffixes), None)


def system_of(rom):
    """'gba', 'gbc', 'gb', or 'nds'. A .zip is the system of the game inside it."""
    rom = Path(rom)
    system = _system_of_name(rom.name)
    if system:
        return system
    if rom.suffix.lower() == ".zip":
        try:
            with zipfile.ZipFile(rom) as archive:
                for name in archive.namelist():
                    system = _system_of_name(name)
                    if system:
                        return system
        except (OSError, zipfile.BadZipFile):
            pass
    return "nds"


def shader_fits(rom, shader):
    """False for another system's screen: DS screens on a GBA game, GBA screens on a DS one."""
    system = system_of(rom)
    if shader in DS_SCREENS: return system == "nds"
    if shader in GBA_SCREENS: return system == "gba"
    return True


def shader_for(rom, own, default):
    """The shader rom plays with: its own choice (-1 for Default), else its system's
    screen, else the default from Settings; never another system's screen (then Off)."""
    if own >= 0 and shader_fits(rom, own): return own
    chosen = SYSTEM_SHADERS.get(system_of(rom), default)
    return chosen if shader_fits(rom, chosen) else 0


def engine_for(rom, chosen):
    """The engine to run rom with: RG DS Play for other systems, else the DS engine chosen."""
    return PLAY_CORE if system_of(rom) != "nds" else chosen


def _path(data, rom):
    return Path(data) / "library" / game_key(rom) / "play.ini"


def frames_folder(data, rom):
    """Where the owner's picture frames for this game's system live."""
    return Path(data) / "frames" / system_of(rom)


def frame_known(data, rom, frame):
    """A built-in style, or a picture frame (frame.png) in the system's folder."""
    if frame in FRAMES: return True
    if not frame or len(frame) > 64 or "/" in frame or frame.startswith("."): return False
    return (frames_folder(data, rom) / (frame + ".png")).is_file()


def view(data, rom):
    fields = read_fields(_path(data, rom))
    fit, second, frame = fields.get("fit", "sharp"), fields.get("second", "companion"), fields.get("frame", "off")
    return {
        "fit": fit if fit in FITS else "sharp",
        "second": second if second in SECONDS else "companion",
        "frame": frame if frame_known(data, rom, frame) else "off"
    }


def save_view(data, rom, fit, second, frame="off"):
    """fit and second as indexes, frame by name (the menu's play-graphics:<fit>:<second>:<frame>)."""
    path = _path(data, rom)
    fields = read_fields(path)
    fields.update(schema=1, fit=FITS[fit], second=SECONDS[second], frame=frame)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_fields(path, fields)
