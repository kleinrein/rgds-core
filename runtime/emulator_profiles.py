"""Writable engine homes and native RGDS-managed engine settings."""
import configparser
import io
from engines import CORES
import os
from pathlib import Path

from library import read_fields, write_fields
from library import game_key
from shaders import FILTERS, SHADERS
from controls import LOGICAL

DS_INPUT_PROFILE = {
    "keys": {
        name: "none"
        for name in LOGICAL
    },
    "pad": dict(
        a="a",
        b="b",
        x="x",
        y="y",
        l="leftshoulder",
        r="rightshoulder",
        start="start",
        select="back",
        up="dpup",
        down="dpdown",
        left="dpleft",
        right="dpright",
        face_fix="off",
        stylus_button="rightstick",
        **{
            "stylus_button.alt": "none",
            "stylus_dpad": "none"
        }
    ),
    "hotkeys": {
        name: "none"
        for name in (
            "quit", "pause", "fast_forward", "fast_forward_toggle", "save_state", "load_state", "slot_next",
            "slot_prev", "volume_up", "volume_down", "mute", "layout_next", "layout_prev", "screen_swap",
            "pip_corner_next", "fullscreen", "screenshot", "lid", "mic", "fps", "modifier"
        )
    },
    "padhotkeys": {
        name: "none"
        for name in (
            "quit", "pause", "fast_forward", "fast_forward_toggle", "save_state", "load_state", "slot_next",
            "slot_prev", "volume_up", "volume_down", "mute", "layout_next", "layout_prev", "screen_swap",
            "pip_corner_next", "fullscreen", "screenshot", "lid", "mic", "fps", "modifier"
        )
    },
}
for section in ("hotkeys", "padhotkeys"):
    DS_INPUT_PROFILE[section].update(
        {name + ".alt": "none"
         for name in tuple(DS_INPUT_PROFILE[section]) if name != "modifier"}
    )
DS_INPUT_PROFILE["hotkeys"].update(save_state="F5", load_state="F7", fast_forward_toggle="Tab", mic="m", lid="l")
DS_INPUT_PROFILE["padhotkeys"]["mic"] = "leftstick"


def game_options(data, rom):
    defaults = read_fields(Path(data) / "preferences.ini")
    defaults.update(read_fields(Path(data) / "ui.ini"))
    local = read_fields(Path(data) / "library" / game_key(rom) / "game.ini")

    def bounded(fields, key, default, low, high):
        try:
            value = int(fields.get(key, default))
        except ValueError:
            return default
        return value if low <= value <= high else default

    # Direct display (DSCore on KMS, Weston stopped while playing): a game's
    # 0/1 wins over the global choice; -1 follows it. Off by default. It has
    # its own display.ini files: the launcher rewrites game.ini with only the
    # keys it knows (game_options.hpp). game.ini's rgds_direct still counts.
    direct = bounded(read_fields(Path(data) / "library" / game_key(rom) / "display.ini"), "direct", -1, -1, 1)
    if direct < 0: direct = bounded(local, "rgds_direct", -1, -1, 1)
    if direct < 0:
        direct = bounded(
            read_fields(Path(data) / "display.ini"), "direct", bounded(defaults, "rgds_direct", 0, 0, 1), 0, 1
        )
    return dict(
        core=bounded(local, "core", -1, -1,
                     len(CORES) - 1),
        rgds_direct=direct,
        filter=bounded(local, "filter", bounded(defaults, "filter", 0, 0,
                                                len(SHADERS) - 1), -1,
                       len(SHADERS) - 1),
        rgds_scale=bounded(local, "rgds_scale", 1, 1, 5),
        rgds_smooth=bounded(local, "rgds_smooth", 1, 0, 1),
        rgds_2d=bounded(local, "rgds_2d", 0, 0, 5),
        rgds_adaptive=bounded(local, "rgds_adaptive", 1, 0, 1),
        frameskip=bounded(local, "frameskip", 0, 0, 3)
    )


def display_filter(data, rom):
    choices = game_options(data, rom)
    if choices["filter"] >= 0: return choices["filter"]
    defaults = read_fields(Path(data) / "preferences.ini")
    defaults.update(read_fields(Path(data) / "ui.ini"))
    try:
        value = int(defaults.get("filter", "0"))
    except ValueError:
        return 0
    return value if 0 <= value < len(SHADERS) else 0


def flat_config(path, updates):
    fields = read_fields(path)
    fields = {k.strip(): v.strip() for k, v in fields.items()}
    fields.update({key: str(value) for key, value in updates.items()})
    write_fields(path, fields)


def section_config(path, updates):
    config = configparser.ConfigParser(strict=False, interpolation=None)
    config.optionxform = str
    if path.exists(): config.read(path)
    for section, values in updates.items():
        if not config.has_section(section): config.add_section(section)
        for key, value in values.items():
            config.set(section, key, str(value))
    stream = io.StringIO()
    config.write(stream)
    content = stream.getvalue()
    if path.is_file() and path.read_text() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + ".pending")
    try:
        pending.write_text(content)
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def dsperate_input_profile(home, config, rom, extra=None):
    """Pin owned input aliases after the native engine's per-game layers.

    The upstream engine merges these writable INIs after --config. Their
    unrelated tuning is retained; RGDS control profiles remain authoritative.
    """
    profile = {k: dict(v) for k, v in DS_INPUT_PROFILE.items()}
    profile.update(extra or {})
    section_config(config, profile)
    native = Path(home) / ".config/dsperate/games"
    paths = {native / (Path(rom).stem + ".ini")}
    if Path(rom).is_file():
        with Path(rom).open("rb") as stream:
            stream.seek(12)
            code = stream.read(4).decode("ascii", errors="replace")
        code = "".join(char if 32 <= ord(char) < 127 and char != "/" else "_" for char in code)
        paths.add(native / (code + ".ini"))
    for path in paths:
        if path.is_file(): section_config(path, profile)


def policy(data):
    fields = read_fields(Path(data) / "preferences.ini")
    return fields.get("auto_load", "1") != "0", fields.get("auto_save", "1") != "0"
