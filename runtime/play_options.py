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
PORT_CORE = "portmaster"
# Game files by system; anything else is a DS game for the DS engines.
SYSTEMS = {
    "gba": (".gba",),
    "gbc": (".gbc",),
    "gb": (".gb", ".sgb"),
    # libretro cores (RG DS OS only, /usr/lib/libretro): the player loads the core named in LIBRETRO_CORES
    "nes": (".nes", ".fds", ".unf", ".unif"),
    "snes": (".sfc", ".smc"),
    "md": (".md", ".gen", ".smd", ".32x"),
    "sms": (".sms",),
    "gg": (".gg",),
    # PlayStation discs by their index files only (.bin is the data track, and ambiguous)
    "psx": (".cue", ".chd", ".pbp", ".m3u"),
    # Nintendo 64: big-endian .z64, byte-swapped .v64, little-endian .n64; the core reads all three
    "n64": (".z64", ".n64", ".v64"),
}
# The libretro core for a system, as RG DS OS installs it (os/buildroot/package/libretro-*).
LIBRETRO_CORES = {
    "nes": "fceumm",
    "snes": "snes9x",
    "md": "picodrive",
    "sms": "picodrive",
    "gg": "picodrive",
    "psx": "pcsx_rearmed",
    "n64": "mupen64plus_next"
}
LIBRETRO_DIR = Path("/usr/lib/libretro")
MGBA_SYSTEMS = ("gba", "gbc", "gb")


def libretro_core(system):
    """The installed core library for a libretro system, or None (not such a system, or not installed)."""
    name = LIBRETRO_CORES.get(system)
    if not name: return None
    path = LIBRETRO_DIR / (name + "_libretro.so")
    return path if path.is_file() else None


FITS = ("sharp", "integer", "smooth")
SECONDS = ("companion", "off", "swap", "info")  # info: the second screen's own quiet controls
# A system's own screen when a game's shader is Default (assets/shaders/catalog.tsv): GBA screen
# (its LCD's colour and pixel gaps), Game Boy screen (the dot matrix), Game Boy Color screen (its
# faint grid). DS games use the default from Settings.
SYSTEM_SHADERS = {"gba": 30, "gb": 31, "gbc": 32}
# Shader groups made for one system's screen (the "... screens" families in
# src/platform/shader_catalog.hpp): offered, and used, only for that system's games.
DS_SCREENS = frozenset({4, 6, 8, 9, 11, 12, 14, 21, 22})
GBA_SCREENS = frozenset({29, 30, 33, 34})
GB_SCREENS = frozenset({31})
GBC_SCREENS = frozenset({32})
SCREENS = {"nds": DS_SCREENS, "gba": GBA_SCREENS, "gb": GB_SCREENS, "gbc": GBC_SCREENS}
# The Original family (Off, Sharp bilinear): fits every system. The other looks were drawn for the
# DS picture and are offered for DS games only.
ORIGINAL = frozenset({0, 1})
# Screen colours (play/src/ui/colour.hpp, play/src/ui/gb_palettes.hpp): the handheld an original Game
# Boy game is shown on, and how a Game Boy Color game's screen shows its colours. The first of each is
# the default. Indexes are the menu's play-color:<p>:<c>.
PALETTES = ("dmg", "pocket", "light", "color", "greys")
CORRECTIONS = ("gbc", "gbc-dim", "vivid")
# What the player can draw around the game: its own styles, each for the stock systems
# whose machines it looks like (play/src/ui/frame_styles.hpp, a test compares; comma
# separated, "" = every system), and picture frames the owner puts in userdata/frames/<system>/.
FRAMES = {
    "off": "",
    "dmg": "gb",
    "dmg-red": "gb",
    "dmg-yellow": "gb",
    "dmg-green": "gb",
    "dmg-blue": "gb",
    "dmg-black": "gb",
    "dmg-screen": "gb",
    "pocket-silver": "gb",
    "pocket-black": "gb",
    "pocket-yellow": "gb",
    "pocket-red": "gb",
    "pocket-green": "gb",
    "pocket-blue": "gb",
    "pocket-pink": "gb",
    "pocket-screen": "gb",
    "gbc-berry": "gbc",
    "gbc-grape": "gbc",
    "gbc-kiwi": "gbc",
    "gbc-dandelion": "gbc",
    "gbc-teal": "gbc",
    "gbc-atomic-purple": "gbc",
    "gbc-screen": "gbc",
    "indigo": "gba",
    "glacier": "gba",
    "black": "gba",
    "platinum": "gba",
    "sp-cobalt": "gba",
    "sp-onyx": "gba",
    "sp-platinum": "gba",
    "ngpc-silver": "ngp",
    "ngpc-black": "ngp",
    "wsc-white": "wonderswan",
    "wsc-blue": "wonderswan",
    "psp-black": "psp",
    "psp-white": "psp",
    "nes": "nes",
    "snes": "snes",
    "md": "md",
    "sms": "sms",
    "pce": "pce",
    "neo": "neo",
    "psx": "psx",
    "n64": "n64",
    "saturn": "saturn",
    "dc": "dc",
    "atari": "atari",
    "coleco": "coleco",
    "intellivision": "intellivision",
    "msx": "msx",
    "pico8": "pico8",
    "arcade": "arcade",
    "mvs": "neo",
    "vectrex": "vectrex",
    "tv-grey": "nes,snes,md,sms,pce,neo,psx,n64,saturn,dc,msx",
    "tv-wood": "nes,sms,atari,coleco,intellivision",
    "monitor": "dos,msx",
    "bar": ""
}


def _system_of_name(name):
    suffix = Path(name).suffix.lower()
    if suffix == ".sh": return "ports"  # a PortMaster port's script
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
    """Original presets fit every game; a system's screens fit only its games; the DS looks only DS games."""
    system = system_of(rom)
    if shader in ORIGINAL or shader in SCREENS.get(system, ()): return True
    return system == "nds" and not any(shader in screens for screens in SCREENS.values())


def shader_for(rom, own, default):
    """The shader rom plays with: its own choice (-1 for Default), else its system's
    screen, else the default from Settings; never another system's screen (then Off)."""
    if own >= 0 and shader_fits(rom, own): return own
    chosen = SYSTEM_SHADERS.get(system_of(rom), default)
    return chosen if shader_fits(rom, chosen) else 0


def engine_for(rom, chosen):
    """The engine to run rom with: PortMaster for ports, RG DS Play for other systems, else the DS engine chosen."""
    system = system_of(rom)
    return PORT_CORE if system == "ports" else PLAY_CORE if system != "nds" else chosen


def _path(data, rom):
    return Path(data) / "library" / game_key(rom) / "play.ini"


def frames_folder(data, rom):
    """Where the owner's picture frames for this game's system live."""
    return Path(data) / "frames" / system_of(rom)


def frame_known(data, rom, frame):
    """A built-in style for the game's system, or a picture frame (frame.png) in the system's folder."""
    systems = FRAMES.get(frame)
    if systems is not None and (systems == "" or system_of(rom) in systems.split(",")): return True
    if not frame or len(frame) > 64 or "/" in frame or frame.startswith("."): return False
    return (frames_folder(data, rom) / (frame + ".png")).is_file()


def view(data, rom):
    fields = read_fields(_path(data, rom))
    fit, second, frame = fields.get("fit", "sharp"), fields.get("second", "companion"), fields.get("frame", "off")
    return {
        "fit": fit if fit in FITS else "sharp",
        "second": second if second in SECONDS else "companion",
        "frame": frame if frame_known(data, rom, frame) else "off",
        "palette": fields.get("palette") if fields.get("palette") in PALETTES else PALETTES[0],
        "colour": fields.get("colour") if fields.get("colour") in CORRECTIONS else CORRECTIONS[0],
    }


def save_view(data, rom, fit, second, frame="off"):
    """fit and second as indexes, frame by name (the menu's play-graphics:<fit>:<second>:<frame>)."""
    path = _path(data, rom)
    fields = read_fields(path)
    fields.update(schema=1, fit=FITS[fit], second=SECONDS[second], frame=frame)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_fields(path, fields)


def save_colour(data, rom, palette, correction):
    """palette and correction as indexes (the menu's play-color:<palette>:<correction>)."""
    path = _path(data, rom)
    fields = read_fields(path)
    fields.update(schema=1, palette=PALETTES[palette], colour=CORRECTIONS[correction])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_fields(path, fields)
