#!/usr/bin/env python3
"""RGDS engine adapters with isolated legacy compatibility where still needed."""
import argparse
import errno
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
import zipfile

sys.dont_write_bytecode = True
from library import playtime, record_playtime
from records import process_identity, read_record, write_record
from emulator_profiles import drastic_home, drastic_game_profile, section_config, flat_config, policy, game_options, display_filter, FILTERS, dsperate_input_profile
from library import read_fields, game_key
from audio import VolumeWatch
from game_menu import GameMenu, MenuInput, SwitchGame
from shaders import supported, SHADERS
from integration import select_hooks, engine_libraries
from engine_log import EngineLog
from hud_theme import hud_theme
from controls import paths as control_paths

from engines import CORES, DSPERATE_CORES

STOCK_DIRS = (Path("/mnt/vendor/deep/drastic_aarch64"), Path("/mnt/vendor/deep/drastic64"))


def aarch64_binary(path):
    """Reject stock ARM32 binaries before preloading the AArch64 integration."""
    with path.open("rb") as stream:
        header = stream.read(20)
    if not header.startswith(b"\x7fELF"):
        return True  # Host script fixtures and explicit development adapters.
    return len(header) == 20 and header[4:6] == b"\x02\x01" and header[18:20] == b"\xb7\x00"


@contextmanager
def activity_lock(data):
    directory = data / "session"
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "activity.lock").open("a") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def find_binary(core, app):
    from linux_platform import independent
    if independent():
        # Each engine is an explicit application payload. Never fall back to a
        # mounted vendor OS or a stale environment override on independent Linux.
        candidate = app / "emulators" / core / core
        if candidate.is_symlink():
            raise ValueError("Independent engine binaries must be regular packaged files")
        return candidate.resolve(
        ) if candidate.is_file() and os.access(candidate, os.X_OK) and aarch64_binary(candidate) else None
    override = os.environ.get(core.upper() + "_BIN")
    candidates = [Path(override)] if override else []
    candidates += [app / "emulators" / core / core]
    if core == "drastic":
        candidates += [d / name for d in STOCK_DIRS for name in ("drastic", "drastic_oga")]
    return next((p.resolve() for p in candidates if p.is_file() and os.access(p, os.X_OK) and aarch64_binary(p)), None)


def stock_file(names, extra=()):
    for root in (*extra, *STOCK_DIRS, Path("/oem/retro")):
        for directory in (root / "system", root):
            for name in names:
                path = directory / name
                if path.is_file() and path.stat().st_size:
                    return path
    return None


def copy_missing(source, destination):
    if not source or (destination.is_file() and destination.stat().st_size):
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_name(destination.name + ".partial")
    try:
        shutil.copyfile(source, pending)
        pending.replace(destination)
    finally:
        pending.unlink(missing_ok=True)


def dscore_cheat_database(data, rom, prepare=True):
    """The Action Replay database DSCore reads for this game, or None.

    A usrcheat.dat beside the game wins (the user's own). Otherwise the
    engine's cheats folder, seeded once from DraStic's copy on a stock card so
    cheats work out of the box; a file dropped there later is kept as is.
    """
    beside = Path(rom).parent / "usrcheat.dat"
    if beside.is_file() and beside.stat().st_size:
        return beside
    own = data / "emulators/rgds-experimental/cheats/usrcheat.dat"
    if prepare and not own.is_file():
        from linux_platform import independent
        sources = [data / "emulators/drastic/usrcheat.dat"]
        if not independent():
            sources += [directory / "usrcheat.dat" for directory in STOCK_DIRS]
        copy_missing(next((path for path in sources if path.is_file() and path.stat().st_size), None), own)
    return own if own.is_file() and own.stat().st_size else None


def same_file_content(source, destination):
    """Compare code/state bytes with bounded memory, even on a 1 GB handheld."""
    if not destination.is_file() or source.stat().st_size != destination.stat().st_size:
        return False
    with source.open("rb") as original, destination.open("rb") as installed:
        while True:
            chunk = original.read(256 * 1024)
            if chunk != installed.read(256 * 1024): return False
            if not chunk: return True


def dsperate_resume_state(data, rom, identity=None, core="dsperate"):
    """Choose the latest automatic state without rolling back a newer battery save."""
    root = data / 'emulators' / core
    candidates = [data / 'library' / game_key(identity or rom) / ('auto-' + core + '.dss')]
    if not rom.is_file():
        return None
    with rom.open('rb') as stream:
        header = stream.read(0x160)
        rom_size = os.fstat(stream.fileno()).st_size
    code = header[12:16].decode('ascii', errors='replace')
    if len(code) != 4 or not all(32 < ord(c) < 127 and c not in '/\\' for c in code):
        raise ValueError('DSperate game header has no valid game code')
    # Match pinned core nds.cpp::rom_identity without hashing the whole ROM.
    rom_identity = 1469598103934665603
    for byte in header:
        rom_identity = ((rom_identity ^ byte) * 1099511628211) & 0xffffffffffffffff
    rom_identity ^= rom_size
    candidates.append(root / 'states' / (code + '.auto.dss'))

    def matches(path):
        try:
            if not path.is_file():
                return False
            with path.open('rb') as stream:
                size = os.fstat(stream.fileno()).st_size
                state = stream.read(64)
            if len(state) != 64 or not 64 <= size <= 128 * 1024 * 1024:
                return False
            word = lambda offset: int.from_bytes(state[offset:offset + 4], 'little')
            # The reviewed writer/reader uses v4 and a 128x96 HEAD thumbnail.
            # This verifies identity and header bounds, not full state loading.
            head_size = 52 + 128 * 96 * 2
            return (
                state[:4] == b'DSST' and word(4) == 4 and state[8:12] == b'HEAD' and word(12) == head_size
                and size >= 16 + head_size and (word(56), word(60)) == (128, 96) and state[16:20] == header[12:16]
                and int.from_bytes(state[20:28], 'little') == rom_identity
            )
        except OSError:
            return False

    candidates = [path for path in candidates if matches(path)]
    if not candidates:
        return None
    # Canonical wins ties; a newer native autosave may include later game progress.
    state = max(candidates, key=lambda path: path.stat().st_mtime_ns)
    battery = root / 'saves' / (rom.stem + '.sav')
    if battery.is_file() and battery.stat().st_mtime_ns > state.stat().st_mtime_ns:
        return None
    return state


def copy_component(source, destination):
    """Refresh code files in a core's writable root without touching its saves."""
    if same_file_content(source, destination):
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".partial")
    try:
        shutil.copyfile(source, temporary)
        temporary.chmod(source.stat().st_mode & 0o777)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def link_component(source, destination):
    """Reuse verified release libraries, with a copy fallback on FAT/exFAT homes.

    The engine verifier checks release bytes before preparation. Only code in
    the owned lib directory is linked; saves/configuration remain writable.
    """
    source, destination = Path(source), Path(destination)
    if source.is_symlink() or not source.is_file():
        raise ValueError("Engine library source must be a regular release file")
    if destination.parent.is_symlink():
        raise ValueError("Engine library directory must not redirect outside its owned home")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # The release verifier already rejects links inside the release. Keep its
    # versioned absolute path without resolving every ancestor for every library.
    target = str(source.absolute())
    if destination.is_symlink() and os.readlink(destination) == target:
        return
    pending = destination.with_name(destination.name + ".link-pending")
    try:
        if pending.is_symlink() and os.readlink(pending) == target: pending.unlink()
        try:
            pending.symlink_to(target)
        except OSError as error:
            if error.errno not in (errno.EPERM, errno.ENOSYS, errno.EOPNOTSUPP, errno.ENOTSUP):
                raise
            copy_component(source, destination)
            return
        pending.replace(destination)
    finally:
        pending.unlink(missing_ok=True)


def resolve_rom(rom, data, core=None):
    rom = rom.resolve(strict=True)
    from play_options import PLAY_CORE, PORT_CORE, system_of
    if core == PORT_CORE:
        return rom  # a port's script, or the PortMaster entry
    if core == PLAY_CORE:
        if system_of(rom) == "nds": raise ValueError("RG DS Play opens Game Boy and Game Boy Advance games.")
        return rom  # it opens .zip archives itself
    if system_of(rom) != "nds": raise ValueError("This game is played with RG DS Play, not a DS engine.")
    if rom.suffix.lower() in (".nds", ".dsi"):
        return rom
    if rom.suffix.lower() != ".zip":
        raise ValueError("Choose an .nds, .dsi, or .zip game.")
    digest = hashlib.sha256()
    # Hash, validate and extract the same open file. Renaming/replacing the ZIP
    # cannot make the cache identity come from one file and its ROM from another.
    with rom.open("rb") as source_archive:
        before = os.fstat(source_archive.fileno())
        identity = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)

        def unchanged():
            if identity(os.fstat(source_archive.fileno())) != identity(before):
                raise ValueError("The game archive changed while being read. Try launching it again.")

        for chunk in iter(lambda: source_archive.read(1024 * 1024), b""):
            digest.update(chunk)
        unchanged()
        source_archive.seek(0)
        with zipfile.ZipFile(source_archive) as archive:
            games = []
            for info in archive.infolist():
                name = Path(info.filename)
                mode = info.external_attr >> 16
                if name.is_absolute() or ".." in name.parts or "\\" in info.filename or stat.S_ISLNK(mode):
                    raise ValueError("Archive contains an unsafe path or symlink.")
                if not info.is_dir() and name.suffix.lower() in (".nds", ".dsi"):
                    games.append(info)
            if len(games) != 1:
                raise ValueError("A ZIP must contain exactly one DS game.")
            member = games[0]
            if member.file_size > 1024 * 1024 * 1024:
                raise ValueError("Archived game exceeds the 1 GiB development limit.")
            output = data / "cache" / "roms" / digest.hexdigest() / Path(member.filename).name
            if output.is_file() and output.stat().st_size == member.file_size:
                unchanged()
                return output
            output.parent.mkdir(parents=True, exist_ok=True)
            temporary = output.with_suffix(output.suffix + ".partial")
            try:
                with archive.open(member) as source, temporary.open("wb") as target:
                    shutil.copyfileobj(source, target)
                    target.flush()
                    os.fsync(target.fileno())
                unchanged()
                temporary.replace(output)
            finally:
                if temporary.exists():
                    temporary.unlink()
            return output


def rumble_setting(data):
    """Game rumble (Settings -> Sound -> Vibration) as RGDS_RUMBLE: its strength, "0" when off.

    DSCore puts a Rumble Pak in the GBA slot only when it is on; the bridge drives the motor."""
    ui = read_fields(data / 'ui.ini')
    try:
        strength = max(10, min(100, int(ui.get('rumble_strength', '80'))))
    except ValueError:
        strength = 80
    return str(strength if ui.get('game_rumble', '1') != '0' else 0)


def base_environment():
    env = os.environ.copy()
    env.pop('RGDS_STATE_ATOMIC_REPLACE', None)
    # Each engine gets its own preload and HOME; never inherit another engine's hook.
    for key in tuple(env):
        if key.startswith(("DSFLIP_", "DSHOOK_", "RAST", "RGDS_SD_")) or key in (
            "LD_PRELOAD", "LD_LIBRARY_PATH", "SDL_VIDEO_DOUBLE_BUFFER", "RGDS_TOUCH_LIBRARY", "RGDS_CORE",
            "RGDS_CONTROL_FILE", "RGDS_STATE_DIR", "RGDS_STATE_SLOT", "RGDS_AUTOSTATE_FILE", "RGDS_AUTOLOAD",
            "RGDS_NATIVE_AUTOLOAD", "RGDS_THREADED_3D", "RGDS_AUTOSAVE", "RGDS_SHARED_MENU", "RGDS_MENU_LIVE",
            "RGDS_MENU_EXTERNAL", "RGDS_OVERLAY_FILE"
        ):
            env.pop(key, None)
    env["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin"
    env["SDL_TOUCH_MOUSE_EVENTS"] = "0"
    env["SDL_MOUSE_TOUCH_EVENTS"] = "0"
    env['SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS'] = '1'
    if env.get("WAYLAND_DISPLAY"):
        env["SDL_VIDEODRIVER"] = "wayland"
    return env


def dsperate_atomic_states(app, binary, verified_manifest):
    """Grant the bridge's early save ack only to the reviewed packaged writer."""
    if not isinstance(verified_manifest, dict) or binary.resolve() != (app / 'emulators/dsperate/dsperate').resolve():
        return False
    files = verified_manifest.get('files', {})
    if 'emulators/dsperate/build.json' not in files:
        return False
    try:
        record = json.loads((app / 'emulators/dsperate/build.json').read_text())
        return (
            record.get('kind') == 'source-built-dsperate'
            and record.get('source', {}).get('commit') == '1b76c355109c9f7576363ccc023927b3137d3c6f'
            and record.get('source', {}).get('patched_inputs_sha256')
            == 'c8f4a7243832dd609cca4b0b33fed3120fadbf0a3106c0f8b497981c91ec9129'
            and record.get('binary', {}).get('sha256') == files.get('emulators/dsperate/dsperate')
            and isinstance(files.get('emulators/dsperate/dsperate'), str)
        )
    except (OSError, ValueError, AttributeError):
        return False


def top_display(env):
    """The connector of the top panel (stock RG DS: DSI-1, see device_profile)."""
    return env.get('RGDS_TOP_DISPLAY', 'DSI-1')


def launch_plan(core, app, data, rom, prepare=True, identity=None, fresh_start=False):
    if core not in CORES:
        raise ValueError("Unknown emulator: " + core)
    if core == 'portmaster':
        # A PortMaster port: a script and its process tree, run by port_runner (no engine, no bridge).
        from portmaster import launch_plan as port_plan
        return port_plan(app, data, rom, base_environment(), prepare=prepare)
    backend = None
    if core == 'drastic':
        from superdrastic import backend_choice
        backend = backend_choice(data, identity or rom)
    verified_manifest = None
    if prepare and (app / "manifest.json").is_file():
        from activate import verify_engine
        if backend == 'superdrastic': verify_engine(app, core, backend=backend)
        else: verified_manifest = verify_engine(app, core)
    binary = find_binary(core, app)
    if not binary:
        from linux_platform import independent
        raise ValueError(
            core + " is missing. Install its verified application payload." if independent() else core +
            " is missing. Copy the supplied local emulator payload or verify stock DraStic."
        )
    env = base_environment()
    filter_choice = display_filter(data, identity or rom)
    if core == 'rgds-play':
        # A game on Default gets its system's screen (GBA screen for the GBA), and never a DS screen.
        from play_options import shader_for
        own = read_fields(data / 'library' / game_key(identity or rom) / 'game.ini').get('filter', '-1').strip()
        filter_choice = shader_for(rom, int(own) if own.lstrip('-').isdigit() else -1, filter_choice)
    tuning = game_options(data, identity or rom)
    display = 'wayland'
    if core == 'rgds-experimental':
        # Direct display: DSCore presents on KMS itself while Weston is stopped
        # (direct_display), skipping Weston's GL composite of every frame.
        if tuning['rgds_direct'] and env.get('WAYLAND_DISPLAY'):
            from direct_display import available
            if available(): display = 'kms'
        try:
            scale = int(env.get('RGDS_VK_SCALE', tuning['rgds_scale']))
        except ValueError:
            scale = 1
        scale = max(1, min(5, scale))
        env['RGDS_3D_SMOOTH'] = str(tuning['rgds_smooth'])
        env['RGDS_2D_FILTER'] = str(tuning['rgds_2d'])
        env['RGDS_ACTIVE_SCALE'] = str(scale)
        candidate = binary.with_name('rgds-experimental-vulkan')
        driver = binary.parent / 'lib/libvulkan_panfrost.so'
        live_graphics = scale > 1 and candidate.is_file() and driver.is_file()
        if scale > 1 and not live_graphics:
            raise ValueError(
                'DSCore high-resolution renderer is missing. Install the verified Vulkan payload or select Native.'
            )
        if live_graphics:
            # Native resolution runs the lean GCC + PGO binary: the Vulkan
            # build costs ~2.6 ms more per frame even with GPU 3D off (Mario
            # Kart race, 9.7 vs 7.1 ms). Crossing between native and 2x-5x
            # during play saves a checkpoint and restarts (game_menu).
            binary = candidate
            icd = data / 'emulators/rgds-experimental/panvk.json'
            if prepare:
                icd.parent.mkdir(parents=True, exist_ok=True)
                content = json.dumps(
                    {
                        'file_format_version': '1.0.0',
                        'ICD': {
                            'library_path': str(driver),
                            'api_version': '1.3.0'
                        }
                    }
                ) + '\n'
                pending = icd.with_suffix('.pending')
                pending.write_text(content)
                pending.replace(icd)
            # Tested on the standard RG DS (Mali-G52, kbase JM 11.47): 56-byte
            # atoms, and no context warmup. The driver's default 2 s sleep is
            # for a Mali-G57; here the first job completes without it, and
            # skipping it shortens every 2x-5x start and switch by seconds.
            env.update(
                RGDS_VK_SCALE=str(scale),
                DS_GPU3D='1' if scale > 1 else '0',
                VK_ICD_FILENAMES=str(icd),
                PANVK_ATOM_STRIDE='56',
                PANVK_KBASE_DVFS='none',
                PANVK_KBASE_WARMUP_MS='0'
            )
            # Adaptive 2x drops heavy 3D stretches to native to hold 60 fps;
            # "Always sharp" turns it off. DSCore reads it once per launch. An
            # explicit DS_ADAPTIVE (benchmark A/B runs) wins, like RGDS_VK_SCALE.
            env['DS_ADAPTIVE'] = env.get('DS_ADAPTIVE', str(tuning['rgds_adaptive']))
        # DSCore paces itself (config vsync = false). Stock APPS exports
        # SDL_RENDER_VSYNC=1, which overrides that and makes every panel's
        # swap wait for the display, on Weston and on KMS alike.
        env['SDL_RENDER_VSYNC'] = '0'

    ds_input = read_fields(data / 'library' / game_key(identity or rom) / 'ds-input.ini')
    try:
        ds_language = int(ds_input.get('language', '1'))
    except ValueError:
        ds_language = 1
    ds_language = max(0, min(5, ds_language))
    ds_microphone = ds_input.get('microphone', '1') != '0'
    ds_nickname = ''.join(
        c for c in ds_input.get('nickname', 'RG DS Core' if core == 'rgds-experimental' else 'DSperate')
        if c.isprintable()
    )
    ds_nickname = ds_nickname.encode('utf-16-le')[:20].decode('utf-16-le', errors='ignore') or 'RG DS Core'
    env['RGDS_DS_LANGUAGE'] = str(ds_language)
    env['RGDS_MICROPHONE'] = '1' if ds_microphone else '0'

    if not supported(core, filter_choice):
        print(f"{SHADERS[filter_choice]['name']} is unavailable in {core}; using Off.", file=sys.stderr)
        filter_choice = 0
    env.update(RGDS_VOLUME_HELPER=str(app / "runtime/audio.py"), RGDS_VOLUME_DATA=str(data), RGDS_VOLUME_KEYS="1")
    env['RGDS_SHADER'] = str(filter_choice)
    env['RGDS_FPS'] = '1' if env.get('RGDS_FPS') == '1' or read_fields(data /
                                                                       'performance.ini').get('fps') == '1' else '0'
    env['RGDS_SHADER_DIR'] = str(app / 'emulators/shaders')
    env['RGDS_RUMBLE'] = rumble_setting(data)
    global_controls, game_controls = control_paths(data, identity or rom)
    env['RGDS_CONTROLS_GLOBAL'] = str(global_controls)
    env['RGDS_CONTROLS_FILE'] = str(game_controls)
    env['RGDS_ENGINE_BINARY'] = str(binary)
    env['RGDS_OWN_FF_HUD'] = '1' if core == 'rgds-experimental' else '0'
    env["TZ"] = read_fields(data / "system.ini").get("timezone", "Europe/Oslo")
    if backend == 'superdrastic':
        from superdrastic import launch_plan as super_plan
        return super_plan(app, data, binary, rom, env, prepare=prepare, identity=identity)
    command = [str(binary)]
    cwd = binary.parent
    hooks = app / "emulators/hooks"
    selected_hooks = select_hooks(core, hooks)
    if core == "drastic":
        home = drastic_home(
            app, data, binary.parent, mapped_controls=bool(selected_hooks.bridge)
        ) if prepare else data / "emulators/drastic"
        if prepare: drastic_game_profile(home, rom, tuning)
        env["HOME"] = str(home)
        cwd = home
        from linux_platform import independent
        engine_libraries(env, private=(binary.parent / "libs",), stock_independent=independent())
        env["SDL_RENDER_DRIVER"] = "opengles2"
        touch_candidates = (binary.parent / "libs/libdrastouch.so",) if independent(
        ) else (binary.parent / "libs/libdrastouch.so", Path("/usr/lib/libdrastouch.so"))
        touch = next((p for p in touch_candidates if p.is_file()), None)
        if independent() and touch is None:
            raise ValueError(
                "DraStic's verified private display/touch adapter is missing; reinstall its complete payload."
            )
        if touch:
            env["LD_PRELOAD"] = str(touch)
            env['RGDS_TOUCH_LIBRARY'] = str(touch)
        command.append(str(rom))
    elif core in DSPERATE_CORES:
        if dsperate_atomic_states(app, binary, verified_manifest):
            env['RGDS_STATE_ATOMIC_REPLACE'] = '1'
        root = data / "emulators" / core
        home = root / "home"
        config = root / "config.ini"
        if prepare:
            (home / ".config/dsperate/games").mkdir(parents=True, exist_ok=True)
            profile = app / "assets/emulator-profiles/dsperate.ini"
            if not profile.is_file(): profile = app / "emulators/dsperate.ini"
            copy_missing(profile if profile.is_file() else None, config)
            auto_load, auto_save = policy(data)
            if os.environ.get("RGDS_FORCE_AUTOLOAD") == "1": auto_load = True
            if fresh_start: auto_load = False
            # An empty cheats path leaves DSCore its own lookup (beside the game, then its config folder).
            cheats = dscore_cheat_database(data, rom) if core == 'rgds-experimental' else None
            section_config(
                config, {
                    "emu": {
                        "autoload": str(auto_load).lower(),
                        "autosave": str(auto_save).lower(),
                        "frameskip": tuning['frameskip']
                    },
                    "paths": {
                        "saves": str(root / "saves"),
                        "states": str(root / "states"),
                        "cheats": str(cheats) if cheats else ""
                    },
                    "hotkeys": {
                        "pause": "none",
                        "pause.alt": "none",
                        "save_state": "F5",
                        "load_state": "F7",
                        "volume_up": "none",
                        "volume_down": "none",
                        "mute": "none",
                        "fast_forward": "none",
                        "fast_forward_toggle": "Tab"
                    },
                    "padhotkeys": {
                        "modifier": "none",
                        "pause": "none",
                        "quit": "none",
                        "save_state": "none",
                        "load_state": "none",
                        "volume_up": "none",
                        "volume_down": "none",
                        "fast_forward": "none",
                        "fast_forward_toggle": "none"
                    },
                    "keys": {
                        name: "none"
                        for name in ("a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right")
                    },
                    "pad": {
                        "a": "a",
                        "b": "b",
                        "x": "x",
                        "y": "y",
                        "l": "leftshoulder",
                        "r": "rightshoulder",
                        "start": "start",
                        "select": "back",
                        "face_fix": "off"
                    },
                    "audio": {
                        "volume": 100,
                        "driver": "alsa"
                    },
                    "video": {
                        "sink": "renderer",
                        "linear": "false",
                        "lcd_grid": "0"
                    }
                }
            )
            for folder in ("saves", "states"):
                (root / folder).mkdir(parents=True, exist_ok=True)
            dsperate_input_profile(
                home, config, rom, {
                    "user": {
                        "language": ds_language,
                        "nickname": ds_nickname
                    },
                    "audio": {
                        "mic": str(ds_microphone).lower(),
                        "mic_dev": "plughw:0,0",
                        "mic_gain": "1.0",
                        "mic_gate": "2.0"
                    }
                }
            )
        env["HOME"] = str(home)
        cwd = root
        env["XDG_CONFIG_HOME"] = str(home / ".config")
        engine_libraries(env, private=(binary.parent / "lib", binary.parent / "libs"), stock_independent=True)
        env["SDL_RENDER_DRIVER"] = "opengles2"
        env[
            "SDL_GAMECONTROLLERCONFIG"
        ] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        command += [str(rom), "--dual-window", "--fullscreen", "--config", str(config)]
        # The native automatic slot is named GAMECODE.auto.dss, whereas RGDS
        # checkpoints are keyed by game identity. Select explicitly so a stale
        # native slot cannot override disabled autoload or a newer battery save.
        command.append('--no-autoload')
        if prepare and auto_load:
            resume = dsperate_resume_state(data, rom, identity, core)
            if resume is not None: command += ['--load-state', str(resume)]
        from linux_platform import independent
        if independent():
            top = env.get('RGDS_TOP_DISPLAY')
            bottom = env.get('RGDS_BOTTOM_DISPLAY')
            if top is not None and bottom is not None and top.isdigit() and bottom.isdigit() and top != bottom:
                env['DS_DUAL_SCREENS'] = 'upper=' + top + ',lower=' + bottom
        # The source-built engine contains FreeBIOS and generates firmware.
        # Explicit empty CLI values also override stale native configuration
        # paths left by earlier releases that searched the stock installation.
        command += ["--bios7", "", "--bios9", "", "--firmware", ""]
    elif core == "melonds":
        root = data / "emulators/melonds"
        key = game_key(identity or rom)
        if prepare:
            for folder in ("home", "saves", "states"):
                (root / folder).mkdir(parents=True, exist_ok=True)
        env["HOME"] = str(root / "home")
        cwd = root
        engine_libraries(env, private=(binary.parent / "lib",), stock_independent=True)
        env["SDL_RENDER_DRIVER"] = "opengles2"
        env["RGDS_THREADED_3D"] = str(tuning["threaded"])
        env["RGDS_NATIVE_AUTOLOAD"] = "0"
        env[
            "SDL_GAMECONTROLLERCONFIG"
        ] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        command += [
            "--rom",
            str(rom), "--save",
            str(root / "saves" / (key + ".sav")), "--state",
            str(root / "states" / (key + ".dss"))
        ]
    elif core == "rgds-play":
        # RG DS Play: every system but the DS. Its saves, states and backups
        # live under userdata/emulators/rgds-play; the view is per game (play.ini).
        from play_options import view, frames_folder
        root = data / "emulators/rgds-play"
        if prepare:
            for folder in ("home", "states"):
                (root / folder).mkdir(parents=True, exist_ok=True)
        env["HOME"] = str(root / "home")
        cwd = root
        engine_libraries(env, private=(binary.parent / "lib",), stock_independent=True)
        env["SDL_RENDER_DRIVER"] = "opengles2"
        env[
            "SDL_GAMECONTROLLERCONFIG"
        ] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        shown = view(data, identity or rom)
        command += ["--rom", str(rom), "--data", str(root), "--fonts", str(app / "assets/fonts")]
        # The launcher's Theme & display (and language, time format) for the second screen.
        command += ["--preferences", str(data / "preferences.ini")]
        for system in ("gba", "gb"):
            command += ["--profiles", str(binary.parent / "profiles" / system)]
        command += ["--fit", shown["fit"], "--second", shown["second"], "--frame", shown["frame"]]
        command += ["--palette", shown["palette"], "--colour", shown["colour"]]
        command += ["--frames", str(frames_folder(data, rom))]
    else:
        # NNDDSS is retired: its supplied binaries are no longer shipped and a
        # saved choice falls back to DSCore before a launch gets here.
        raise ValueError('NNDDSS is retired; choose DSCore for this game.')
    if selected_hooks.bridge:
        preload = [str(selected_hooks.bridge)]
        env["SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS"] = "0"
        if core == "drastic" and env.get("LD_PRELOAD"):
            preload.append(env["LD_PRELOAD"])
        env["LD_PRELOAD"] = ":".join(preload)
        auto_load, auto_save = policy(data)
        if os.environ.get("RGDS_FORCE_AUTOLOAD") == "1": auto_load = True
        if fresh_start: auto_load = False
        state_dir = data / (
            "emulators/drastic/savestates" if core == "drastic" else
            "emulators/melonds/states" if core == "melonds" else "emulators/" + core + "/states"
        )
        auto = data / "library" / game_key(identity or rom) / ("auto-" + core + ".dss")
        env.update(
            RGDS_CORE="dsperate" if core in DSPERATE_CORES else core,
            RGDS_CONTROL_FILE=str(data / "session/emulator.control"),
            RGDS_STATE_DIR=str(state_dir),
            RGDS_AUTOSTATE_FILE=str(auto),
            RGDS_AUTOSAVE="1" if auto_save else "0",
            RGDS_AUTOLOAD="0",
            RGDS_SHARED_MENU="1",
            RGDS_MENU_LIVE="0"
        )
        slot_name = rom.stem + ("_1.dss" if core == "drastic" else "_8.dss")
        if core == "melonds": slot_name = game_key(identity or rom) + ".dss"
        if core in DSPERATE_CORES and prepare:
            with rom.open("rb") as stream:
                stream.seek(12)
                code = stream.read(4).decode("ascii", errors="replace")
            if len(code) != 4 or not all(32 < ord(c) < 127 and c not in "/\\" for c in code):
                raise ValueError("DSperate game header has no valid game code")
            slot_name = code + ".0.dss"
        env["RGDS_STATE_SLOT"] = str(state_dir / slot_name)
        newer_battery = False
        if prepare and core == 'drastic' and auto.is_file():
            battery = home / 'backup' / (rom.stem + '.dsv')
            newer_battery = battery.is_file() and battery.stat().st_mtime_ns > auto.stat().st_mtime_ns
        if prepare and core not in DSPERATE_CORES and auto_load and auto.is_file() and not newer_battery:
            destination = state_dir / slot_name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.is_file() and not same_file_content(destination, auto):
                # Loading the independent automatic snapshot uses an engine slot.
                # Preserve the previous manual state before placing it there.
                hasher = hashlib.sha256()
                with destination.open('rb') as stream:
                    for chunk in iter(lambda: stream.read(256 * 1024), b''):
                        hasher.update(chunk)
                digest = hasher.hexdigest()[:16]
                backup = auto.parent / "manual-state-backups" / core / (
                    destination.stem + "-" + digest + destination.suffix
                )
                copy_missing(destination, backup)
            shutil.copy2(auto, destination)
            # melonDS and RG DS Play load it before their first frame; the others get F7 from the bridge.
            env["RGDS_NATIVE_AUTOLOAD" if core in ("melonds", "rgds-play") else "RGDS_AUTOLOAD"] = "1"
        if prepare and core == "drastic":
            for shader in (app / "emulators/shaders").glob("*"):
                copy_missing(shader, data / "emulators/drastic/shaders" / shader.name)
    if core in DSPERATE_CORES:
        # A host sleep freezes the engine (SIGSTOP) and the game carries on where
        # it was on wake, like the pause menu. DSCore's own hinge pulse on resume
        # closed the emulated lid for five frames: a game that had not reached its
        # sleep by the open edge stayed asleep (panels off, saves refused).
        env['DS_LID_PULSE'] = '0'
    plan = {"command": command, "cwd": str(cwd), "env": env}
    if display == 'kms' and selected_hooks.bridge:
        # No compositor is left for a paused-menu window: the engine keeps
        # presenting (held still) and the bridge draws the shared menu frame.
        plan['display'] = 'kms'
        plan['wayland'] = {key: env.get(key) for key in ('WAYLAND_DISPLAY', 'SDL_VIDEODRIVER', 'RGDS_MENU_LIVE')}
        plan['wayland_command'] = list(command)
        env.pop('WAYLAND_DISPLAY', None)
        env.update(SDL_VIDEODRIVER='KMSDRM', RGDS_MENU_LIVE='1')
        # Direct KMS scanout for DSCore: its frames (the 2x compose included)
        # are copied into dma-buf buffers and page-flipped, with no GL job in
        # the frame's path; the bridge blends the menu/HUD, captures and times
        # frames through the engine's present hook. The renderer sink stays
        # for Weston (DS_RGDS_SINK overrides, for measurement; an RGDS_ key
        # would be dropped by the guardian's clean environment).
        # A screen filter is the bridge's GL pass over the renderer's game
        # draw (its SDL_RenderCopy hook); the direct present has no GPU
        # stage, so a chosen filter keeps the renderer sink on KMS. HD 2D
        # (rgds_2d=4, HD smooth 5) is a GPU pass in the same draw.
        gpu_pass = bool(filter_choice) or env.get('RGDS_2D_FILTER') in ('4', '5')
        if core in DSPERATE_CORES and prepare:
            sink = os.environ.get('DS_RGDS_SINK') or ('renderer' if gpu_pass else 'auto')
            section_config(config, {"video": {"sink": sink, "gpu_present": "false"}})
        if core in DSPERATE_CORES and gpu_pass:
            env['DS_DIRECT_PRESENT'] = '0'
        # SDL's KMSDRM display 0 is DSI-1, the stock top panel, but DSCore's
        # KMSDRM order opens the upper window on display 1 first. Opening the
        # display-0 window first coincided with a Mali GPU reset at start (5 of
        # 7 launches), so keep DSCore's order and swap the windows' contents:
        # the display-1 (bottom) window shows the DS bottom screen.
        if top_display(env) == 'DSI-1':
            plan['command'] = command + ['--screen', 'bottom']
    return plan


def core_running(data):
    marker = data / "session/core.json"
    if not marker.is_file():
        return False
    try:
        record = read_record(marker)
        if record is None: return False
        return bool(record["start"] and process_identity(int(record["pid"])) == record["start"])
    except (ValueError, KeyError, TypeError):
        # An unreadable marker is not permission to terminate a game.
        raise ValueError("Unreadable core activity record; inspect it before deploying.")


def release_launcher_display(channel):
    """Keep preparation visible; open the engine only after SDL releases both panels."""
    if channel is None:
        return
    channel.settimeout(10)
    channel.sendall(b"R")
    if channel.recv(1) != b"C":
        raise OSError("Launcher closed before display handoff")


def _run_once(core, app, data, rom, display_handoff=None, fresh_start=False, display=None):
    data.mkdir(parents=True, exist_ok=True)
    marker = data / "session/core.json"
    child = None
    volume = VolumeWatch(data)
    menu = None
    inputs = None
    lease = None
    engine_log = EngineLog(data, core, [], app)
    original_rom = Path(rom).resolve()
    # PortMaster itself is a tool, not a game: no playtime, so it never shows in Recents or Last played.
    counted = core != 'portmaster' or not original_rom.match('*/entry/PortMaster.sh')
    accumulated, sessions = playtime(data, original_rom)
    began = None
    started = datetime.now(timezone.utc).isoformat()

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        if os.statvfs(data).f_flag & os.ST_RDONLY:
            raise OSError(
                errno.EROFS,
                "Game storage is read-only. Start the writable internal RGDS installation; your saves were preserved."
            )
        # Serialize the launch marker with deployment's check+stop operation.
        # A newly requested launch cannot slip between that check and shutdown.
        with activity_lock(data):
            record = {"pid": os.getpid(), "start": process_identity(os.getpid()), "core": core}
            write_record(marker, record)
        from quick_resume import invalidate
        invalidate(data)
        rom = resolve_rom(rom, data, core)
        plan = launch_plan(core, app, data, rom, identity=original_rom, fresh_start=fresh_start)
        engine_log.command, engine_log.cwd = list(plan["command"]), plan["cwd"]
        plan['env']['RGDS_OVERLAY_FILE'] = f'/dev/shm/rgds-menu-{os.getpid()}-{time.monotonic_ns()}.frame'
        plan['env']['RGDS_FRAME_STATS'] = plan['env']['RGDS_OVERLAY_FILE'] + '.stats'
        if (app / 'assets/fonts/rounded-20.rgf').is_file():
            plan['env']['RGDS_FONT_DIR'] = str(app / 'assets/fonts')
        hud_colours = hud_theme(app, data)
        if hud_colours:
            plan['env']['RGDS_HUD_THEME'] = hud_colours
        print("Launching " + core + ": " + str(rom), flush=True)
        (data / "session/emulator.control").unlink(missing_ok=True)
        (data / "session/emulator.control.result").unlink(missing_ok=True)
        for suffix in ('.speed-request', '.speed-state'):
            Path(str(data / 'session/emulator.control') + suffix).unlink(missing_ok=True)
        # Fail before opening audio if the mixer cannot be controlled.
        volume.start()
        plan["env"]["RGDS_VOLUME_KEYS"] = "1" if volume.descriptors else "0"
        inputs = MenuInput(data, original_rom)
        plan['env']['RGDS_MENU_EXTERNAL'] = '1' if inputs.descriptors else '0'
        super_backend = plan.get('backend') == 'superdrastic'
        if super_backend and not inputs.descriptors:
            raise ValueError(
                'SuperDrastic needs readable RG DS menu controls. Choose Standard DraStic until input access is restored.'
            )
        release_launcher_display(display_handoff)
        if display is not None:
            wanted = plan.get('display', 'wayland')
            # Twice a GPU reset under a starting KMS engine: this game goes back to Weston.
            if wanted == 'kms' and display.gpu_resets >= 2: wanted = 'wayland'
            if display.prepare(wanted) != plan.get('display', 'wayland'):
                from direct_display import use_wayland
                use_wayland(plan['env'], plan['wayland'])
                plan['command'] = plan['wayland_command']
        if super_backend:
            from superdrastic import VTLease
            lease = VTLease()
            lease.acquire()
        preexec = None
        if core == 'portmaster':
            # The menu helper maps its windows before the port, so the port keeps keyboard focus.
            from port_runner import PortMenu, enter_port, prepare_binds
            menu = PortMenu(None, app, data, core, original_rom, plan['env'], inputs=inputs)
            inputs = None
            menu.start_companion()
            prepare_binds()
            preexec = enter_port
        child = subprocess.Popen(
            plan["command"],
            cwd=plan["cwd"],
            env=plan["env"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
            preexec_fn=preexec
        )
        engine_log.start(child.stdout)
        began = time.monotonic()
        if super_backend:
            from superdrastic import Supervisor
            menu = Supervisor(child, app, data, core, original_rom, plan['env'], inputs=inputs)
        elif core == 'portmaster':
            menu.attach(child)
        else:
            menu = GameMenu(child, app, data, core, original_rom, plan["env"], inputs=inputs)
        inputs = None
        checkpoint_at = began
        gpu_reset = False
        while child.poll() is None:
            menu.poll()
            # The Mali driver can reset the GPU under an engine starting on KMS
            # ("Job has been on the GPU for too long"); PanVK's 3D then stays
            # broken for the whole process. Start the engine again instead.
            if (
                display is not None and plan['env'].get('SDL_VIDEODRIVER') == 'KMSDRM' and engine_log.gpu_faults
                and time.monotonic() - began < 30
            ):
                print('Direct display: GPU jobs failed while the engine started; restarting it', flush=True)
                display.gpu_resets += 1
                gpu_reset = True
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                break
            if counted and time.monotonic() - checkpoint_at >= 30:
                record_playtime(data, original_rom, accumulated + menu.played_seconds(), sessions + 1, started)
                checkpoint_at = time.monotonic()
            time.sleep(.02)
        # Deliver the completed exit to the pause UI before closing its process.
        menu.poll()
        result = child.wait()
        if core == 'portmaster': result = menu.port_result(result)
        if gpu_reset: return 76  # the same engine again (run() keeps the display state)
        if super_backend: result = menu.finish(result)
        if result:
            print(f"{core} exited with code {result}. Diagnostics: {engine_log.path}", file=sys.stderr, flush=True)
        if result == 0 and core != 'portmaster':  # quick resume reopens engines, not ports
            from quick_resume import record_exit
            try:
                record_exit(data, original_rom, core, datetime.fromisoformat(started).timestamp())
            except OSError as error:
                print('Quick resume was not armed: ' + str(error), file=sys.stderr)
        switch = menu.completed_switch() if result == 0 else None
        if switch:
            print('Switching to ' + switch.core + ': ' + str(switch.rom), flush=True)
            return switch
        if result == 0 and menu.restart and getattr(menu, "restart_same_core", False): return 76
        return (
            77 if getattr(menu, "reset_state", False) else 75
        ) if result == 0 and menu.restart else result if result >= 0 else 128 - result
    except KeyboardInterrupt:
        if menu: menu.close()
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        return 143
    finally:
        if inputs: inputs.close()
        seconds = menu.played_seconds() if menu else int(time.monotonic() - began) if began is not None else 0
        if menu:
            try:
                menu.close()
            except OSError as error:
                print("Game-menu cleanup: " + str(error), file=sys.stderr)
        # A failed menu/IPC helper must not leave a live engine behind while the
        # launcher reopens and the activity marker is cleared.
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        restore_error = None
        if lease:
            try:
                lease.close()
            except OSError as error:
                restore_error = error
                print('SuperDrastic display restoration failed: ' + str(error), file=sys.stderr)
        if engine_log:
            try:
                engine_log.finish(child.poll() if child else None, sys.exc_info()[1] or restore_error)
            except OSError as error:
                print("Could not save engine diagnostics: " + str(error), file=sys.stderr)
        try:
            volume.close()
        except OSError as error:
            print("Volume cleanup: " + str(error), file=sys.stderr)
        try:
            if began is not None and counted:
                record_playtime(data, original_rom, accumulated + seconds, sessions + 1, started)
        except OSError:
            print("Could not save playtime.", file=sys.stderr)
        finally:
            try:
                marker.unlink(missing_ok=True)
            except OSError as error:
                print("Could not clear game activity: " + str(error), file=sys.stderr)
        if restore_error and sys.exc_info()[0] is None:
            raise OSError(
                'SuperDrastic could not restore the original display console; see the engine log.'
            ) from restore_error


def run(core, app, data, rom, display_handoff=None):
    from direct_display import DirectDisplay
    # Weston stays stopped across restarts and switches between KMS engines,
    # and is back before the launcher reopens, whichever way the game ends.
    display = DirectDisplay(data)
    try:
        return _run_games(core, app, data, rom, display_handoff, display)
    finally:
        display.restore()


def _run_games(core, app, data, rom, display_handoff, display):
    from play_options import engine_for
    fresh_start = False
    switching = False
    while True:
        previous_autoload = os.environ.get('RGDS_FORCE_AUTOLOAD')
        try:
            if switching: os.environ['RGDS_FORCE_AUTOLOAD'] = '1'
            result = _run_once(core, app, data, rom, display_handoff, fresh_start=fresh_start, display=display)
        finally:
            if previous_autoload is None: os.environ.pop('RGDS_FORCE_AUTOLOAD', None)
            else: os.environ['RGDS_FORCE_AUTOLOAD'] = previous_autoload
        display_handoff = None
        if isinstance(result, SwitchGame):
            core, rom = result.core, result.rom
            fresh_start = False
            switching = True
            continue
        if result not in (75, 76, 77): return result
        fresh_start = result == 77
        # 76: the same engine with another renderer (3D resolution change).
        if fresh_start or result == 76: continue
        options = game_options(data, rom)
        defaults = read_fields(Path(data) / 'preferences.ini')
        selection = options['core'] if options['core'] >= 0 else int(defaults.get('core', '4'))
        core = CORES[selection] if 0 <= selection < len(CORES) else core
        if core == 'nnddss': core = 'rgds-experimental'  # retired: a saved choice falls back to DSCore
        core = engine_for(rom, core)  # other systems always restart in RG DS Play


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("catalog", "launch", "plan"))
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("userdata"))
    parser.add_argument("--core", choices=CORES, default="dsperate")
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--display-handoff-fd", type=int, default=-1, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.action == "catalog":
            print(json.dumps({core: str(find_binary(core, args.app) or "") for core in CORES}, indent=2))
            return 0
        if args.rom is None:
            parser.error("--rom is required for launch/plan")
        if args.action == "plan":
            plan = launch_plan(args.core, args.app.resolve(), args.data.resolve(), args.rom.resolve(), prepare=False)
            # A dry run must not print private inherited environment variables.
            print(json.dumps({"command": plan["command"], "cwd": plan["cwd"]}, indent=2))
            return 0
        if args.display_handoff_fd >= 0:
            with socket.socket(fileno=args.display_handoff_fd) as channel:
                return run(args.core, args.app.resolve(), args.data.resolve(), args.rom, channel)
        return run(args.core, args.app.resolve(), args.data.resolve(), args.rom)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print("RGDS emulator: " + str(error), file=sys.stderr)
        return 78 if isinstance(error, OSError) and error.errno == errno.EROFS else 1


if __name__ == "__main__":
    sys.exit(main())
