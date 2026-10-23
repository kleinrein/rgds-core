"""Writable engine homes and native RGDS-managed engine settings."""
import configparser
import io
from engines import CORES
import os
from pathlib import Path
import shutil

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
        drastic_screen_swap=bounded(local, "drastic_screen_swap", 0, 0, 1),
        gengis_scale=bounded(local, "gengis_scale", 0, 2, 3),
        rgds_scale=bounded(local, "rgds_scale", 1, 1, 5),
        rgds_smooth=bounded(local, "rgds_smooth", 1, 0, 1),
        rgds_2d=bounded(local, "rgds_2d", 0, 0, 5),
        rgds_adaptive=bounded(local, "rgds_adaptive", 1, 0, 1),
        threaded=bounded(local, "threaded_3d", 1, 0, 1),
        hires=bounded(local, "hires_3d", 0, 0, 1),
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


def copy_missing(source, destination):
    if source.is_file() and not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def refresh(source, destination):
    if not source.is_file(): return
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and source.read_bytes() == destination.read_bytes(): return
    pending = destination.with_name(destination.name + ".pending")
    shutil.copy2(source, pending)
    pending.replace(destination)


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


def drastic_native_overrides(home, rom, fields, spaced=False):
    """Keep a native per-game layer consistent with the selected backend.

    DraStic loads config/drastic.cfg, then config/<ROM stem>.cfg on reset.
    Imported per-game settings must not override RGDS display/control choices.
    Only update an existing per-game file; preserve its unrelated options.
    """
    local = home / "config" / (rom.stem + ".cfg")
    if local != home / "config/drastic.cfg" and local.is_file():
        content = local.read_bytes()
        try:
            content.decode("utf-8")
        except UnicodeError:
            raise ValueError("DraStic has an unsupported binary per-game config: " + str(local))
        if content.startswith(b"DSCF") or b"\0" in content:
            raise ValueError("DraStic has an unsupported binary per-game config: " + str(local))
        managed = {
            key: value
            for key, value in fields.items() if key.startswith(('controls_a[', 'controls_b[')) or key in (
                'threaded_3d', 'hires_3d', 'frameskip_type', 'frameskip_value', 'fast_forward', 'savestate_number',
                'screen_wait_for_vsync', 'screen_swap'
            )
        }
        values = {key.strip(): value.strip() for key, value in read_fields(local).items()}
        values.update(managed)
        write_fields(local, {k + ' ': ' ' + str(v) for k, v in values.items()} if spaced else values)


def drastic_game_profile(home, rom, tuning):
    """Apply Standard DraStic tuning and screen order at both native layers."""
    config = home / "config/drastic.cfg"
    flat_config(
        config,
        dict(
            threaded_3d=tuning['threaded'],
            hires_3d=tuning['hires'],
            frameskip_type=int(tuning['frameskip'] > 0),
            frameskip_value=max(1, tuning['frameskip']),
            screen_swap=tuning['drastic_screen_swap']
        )
    )
    drastic_native_overrides(home, rom, read_fields(config))


def drastic_home(app, data, stock, mapped_controls=True):
    from linux_platform import independent
    isolated = independent()
    home = data / "emulators/drastic"
    for folder in ("config", "system", "savestates", "backup", "profiles", "cheats", "shaders"):
        destination = home / folder
        destination.mkdir(parents=True, exist_ok=True)
        source = stock / folder
        if source.is_dir() and not isolated:
            for file in source.rglob("*"):
                if file.is_file(): copy_missing(file, destination / file.relative_to(source))
    for name in ("game_database.xml", "usrcheat.dat", "drastic_logo_0.raw", "drastic_logo_1.raw"):
        copy_missing(stock / name, home / name)
    config = home / "config/drastic.cfg"
    owned = app / "assets/emulator-profiles/drastic.cfg"
    copy_missing(owned if owned.is_file() else app / "emulators/drastic.cfg", config)
    if not isolated: copy_missing(stock / "config/drastic.cfg", config)
    flat_config(
        config, {
            "controls_a[CONTROL_INDEX_MENU]": 65535,
            "controls_b[CONTROL_INDEX_MENU]": 65535,
            "controls_a[CONTROL_INDEX_START]": 1031,
            "controls_b[CONTROL_INDEX_START]": 1031,
            "controls_a[CONTROL_INDEX_SELECT]": 1030,
            "controls_b[CONTROL_INDEX_SELECT]": 1030,
            "controls_a[CONTROL_INDEX_SAVE_STATE]": 1040,
            "controls_b[CONTROL_INDEX_SAVE_STATE]": 1040,
            "controls_a[CONTROL_INDEX_LOAD_STATE]": 1041,
            "controls_b[CONTROL_INDEX_LOAD_STATE]": 1041,
            "controls_a[CONTROL_INDEX_QUIT]": 1042,
            "controls_b[CONTROL_INDEX_QUIT]": 1042,
            "controls_a[CONTROL_INDEX_FAST_FORWARD]": 1043,
            "controls_b[CONTROL_INDEX_FAST_FORWARD]": 1043,
            "fast_forward": 0,
            "savestate_number": 1,
            "screen_wait_for_vsync": 0,
            "screen_swap": 0
        }
    )
    # The dual-panel frontend owns placement. Imported native swap/orientation
    # shortcuts can overlap RGDS gameplay or fast-forward buttons and flip the
    # screens unexpectedly; disable both binding banks and reset the saved swap.
    flat_config(
        config, {
            f"controls_{bank}[CONTROL_INDEX_{action}]": 65535
            for bank in ("a", "b")
            for action in ("SWAP_SCREENS", "SWAP_ORIENTATION_A", "SWAP_ORIENTATION_B")
        }
    )
    # A fixed logical layout lets the owned bridge map any physical source,
    # including face buttons to D-pad directions, without touching saves.
    logical_buttons = {
        "A": 0,
        "B": 1,
        "X": 3,
        "Y": 2,
        "L": 4,
        "R": 5,
        "START": 7,
        "SELECT": 6,
        "UP": 20,
        "DOWN": 21,
        "LEFT": 22,
        "RIGHT": 23
    }
    if mapped_controls:
        flat_config(
            config, {
                f"controls_{bank}[CONTROL_INDEX_{name}]": 1024 + button
                for bank in ("a", "b")
                for name, button in logical_buttons.items()
            }
        )
    else:
        current = read_fields(config)
        original = {} if isolated else read_fields(stock / 'config/drastic.cfg')
        for bank in ("a", "b"):
            for name in ("UP", "DOWN", "LEFT", "RIGHT"):
                key = f"controls_{bank}[CONTROL_INDEX_{name}]"
                if key in original: current[key] = original[key]
                else: current.pop(key, None)
        write_fields(config, current)
    return home
