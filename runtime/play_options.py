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
DS_CORE = "rgds-experimental"  # DSCore, the only DS engine
# Game files by system; anything else is a DS game for DSCore.
SYSTEMS = {
    "gba": (".gba",),
    "gbc": (".gbc",),
    "gb": (".gb", ".sgb"),
    # libretro cores (RG DS OS only, /usr/lib/libretro): the player loads the core named in LIBRETRO_CORES
    "nes": (".nes", ".fds", ".unf", ".unif"),
    "snes": (".sfc", ".smc"),
    "md": (".md", ".gen", ".smd", ".32x"),  # .bin too, but only in an md folder: FOLDER_SYSTEMS
    "sms": (".sms",),
    "gg": (".gg",),
    # PlayStation discs by their index files only (.bin is the data track, and ambiguous)
    "psx": (".cue", ".chd", ".pbp", ".m3u"),
    # Nintendo 64: big-endian .z64, byte-swapped .v64, little-endian .n64; the core reads all three
    "n64": (".z64", ".n64", ".v64"),
}
# Folders that name a system for files whose extension cannot (Mega Drive dumps are mostly plain .bin, which a
# PlayStation data track also is): the games folders under /data/roms, with the names other frontends use.
FOLDER_SYSTEMS = {"md": "md", "genesis": "md", "megadrive": "md", "mega drive": "md"}
FOLDER_EXTENSIONS = {"md": (".bin",)}
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
    "sp-cobalt-screen": "gba",
    "sp-onyx-screen": "gba",
    "sp-platinum-screen": "gba",
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


def _system_of_folder(rom, name):
    """The system whose games folder holds rom, when name's extension is one that folder claims (a plain .bin)."""
    suffix = Path(name).suffix.lower()
    for folder in rom.parents:
        system = FOLDER_SYSTEMS.get(folder.name.lower())
        if system and suffix in FOLDER_EXTENSIONS.get(system, ()):
            return system
    return None


def system_of(rom):
    """'gba', 'gbc', 'gb', 'md', ..., or 'nds'. A .zip is the system of the game inside it."""
    rom = Path(rom)
    system = _system_of_name(rom.name) or _system_of_folder(rom, rom.name)
    if system:
        return system
    if rom.suffix.lower() == ".zip":
        try:
            with zipfile.ZipFile(rom) as archive:
                for name in archive.namelist():
                    system = _system_of_name(name) or _system_of_folder(rom, name)
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


def shader_for(rom, own, default, system_choice=-1):
    """The shader rom plays with: its own choice (-1 for Default), else its system's default
    (System defaults, system_choice), else the system's own screen, else the default from Settings;
    never another system's screen (then Off)."""
    if own >= 0 and shader_fits(rom, own): return own
    if system_choice >= 0 and shader_fits(rom, system_choice): return system_choice
    chosen = SYSTEM_SHADERS.get(system_of(rom), default)
    return chosen if shader_fits(rom, chosen) else 0


def engine_for(rom, chosen=None):
    """The engine to run rom with: PortMaster for ports, RG DS Play for other systems, DSCore for DS games.

    chosen (a stored engine choice) no longer matters: DSCore is the only DS engine."""
    system = system_of(rom)
    return PORT_CORE if system == "ports" else PLAY_CORE if system != "nds" else DS_CORE


def _path(data, rom):
    return Path(data) / "library" / game_key(rom) / "play.ini"


def frames_folder(data, rom):
    """Where the owner's picture frames for this game's system live."""
    return Path(data) / "frames" / system_of(rom)


def frame_fits(data, system, frame):
    """A built-in style for the system, or a picture frame (frame.png) in the system's folder."""
    systems = FRAMES.get(frame)
    if systems is not None and (systems == "" or system in systems.split(",")): return True
    if not frame or len(frame) > 64 or "/" in frame or frame.startswith("."): return False
    return (Path(data) / "frames" / system / (frame + ".png")).is_file()


def frame_known(data, rom, frame):
    return frame_fits(data, system_of(rom), frame)


# What a system's games share until a game says otherwise: userdata/system-defaults/<system>.ini
# (Settings > Library > System defaults). The same keys as a game's play.ini, plus `filter` (a shader id).
# A game's own play.ini holds a key only where the game differs from this.
BUILT_IN = {"fit": "sharp", "second": "companion", "frame": "off", "palette": PALETTES[0], "colour": CORRECTIONS[0]}


def defaults_path(data, system):
    return Path(data) / "system-defaults" / (system + ".ini")


def system_defaults(data, system):
    """The system's own defaults, only the keys that are set and valid (see BUILT_IN for the rest)."""
    fields = read_fields(defaults_path(data, system))
    chosen = {}
    if fields.get("fit") in FITS: chosen["fit"] = fields["fit"]
    if fields.get("second") in SECONDS: chosen["second"] = fields["second"]
    if fields.get("frame") and frame_fits(data, system, fields["frame"]): chosen["frame"] = fields["frame"]
    if fields.get("palette") in PALETTES: chosen["palette"] = fields["palette"]
    if fields.get("colour") in CORRECTIONS: chosen["colour"] = fields["colour"]
    filter_text = fields.get("filter", "").strip()
    if filter_text.isdigit(): chosen["filter"] = int(filter_text)
    return chosen


def view_defaults(data, system):
    """The view a game of `system` has when its play.ini says nothing."""
    return {**BUILT_IN, **{key: value for key, value in system_defaults(data, system).items() if key in BUILT_IN}}


def view(data, rom):
    fields = read_fields(_path(data, rom))
    system = system_of(rom)
    shown = view_defaults(data, system)
    if fields.get("fit") in FITS: shown["fit"] = fields["fit"]
    if fields.get("second") in SECONDS: shown["second"] = fields["second"]
    if fields.get("frame") and frame_fits(data, system, fields["frame"]): shown["frame"] = fields["frame"]
    if fields.get("palette") in PALETTES: shown["palette"] = fields["palette"]
    if fields.get("colour") in CORRECTIONS: shown["colour"] = fields["colour"]
    return shown


def _keep_differences(data, rom, fields, **chosen):
    """Store a game's key only where it differs from its system's default: picking the default again means
    following it."""
    base = view_defaults(data, system_of(rom))
    fields["schema"] = 1
    for key, value in chosen.items():
        if value == base[key]: fields.pop(key, None)
        else: fields[key] = value


def save_view(data, rom, fit, second, frame="off"):
    """fit and second as indexes, frame by name (the menu's play-graphics:<fit>:<second>:<frame>)."""
    path = _path(data, rom)
    fields = read_fields(path)
    _keep_differences(data, rom, fields, fit=FITS[fit], second=SECONDS[second], frame=frame)
    path.parent.mkdir(parents=True, exist_ok=True)
    write_fields(path, fields)


def save_colour(data, rom, palette, correction):
    """palette and correction as indexes (the menu's play-color:<palette>:<correction>)."""
    path = _path(data, rom)
    fields = read_fields(path)
    _keep_differences(data, rom, fields, palette=PALETTES[palette], colour=CORRECTIONS[correction])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_fields(path, fields)
