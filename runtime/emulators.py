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
        return candidate.resolve() if candidate.is_file() and os.access(candidate, os.X_OK) and aarch64_binary(candidate) else None
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
            word = lambda offset: int.from_bytes(state[offset:offset+4], 'little')
            # The reviewed writer/reader uses v4 and a 128x96 HEAD thumbnail.
            # This verifies identity and header bounds, not full state loading.
            head_size = 52 + 128 * 96 * 2
            return (state[:4] == b'DSST' and word(4) == 4 and state[8:12] == b'HEAD'
                    and word(12) == head_size and size >= 16 + head_size
                    and (word(56), word(60)) == (128, 96)
                    and state[16:20] == header[12:16]
                    and int.from_bytes(state[20:28], 'little') == rom_identity)
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
        if pending.is_symlink() and os.readlink(pending)==target:pending.unlink()
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

def resolve_rom(rom, data):
    rom = rom.resolve(strict=True)
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


def base_environment():
    env = os.environ.copy()
    env.pop('RGDS_STATE_ATOMIC_REPLACE', None)
    # Each engine gets its own preload and HOME; never inherit another engine's hook.
    for key in tuple(env):
        if key.startswith(("SIMPLEOS_", "DSFLIP_", "DSHOOK_", "RAST", "RGDS_SD_")) or key in ("LD_PRELOAD", "LD_LIBRARY_PATH", "SDL_VIDEO_DOUBLE_BUFFER", "RGDS_FX_LIBRARY", "RGDS_LEGACY_ACTIVE", "RGDS_USE_LEGACY_DSPERATE", "RGDS_DISABLE_LEGACY_HOOKS", "RGDS_TOUCH_LIBRARY", "RGDS_CORE", "RGDS_CONTROL_FILE", "RGDS_STATE_DIR", "RGDS_STATE_SLOT", "RGDS_AUTOSTATE_FILE", "RGDS_AUTOLOAD", "RGDS_NATIVE_AUTOLOAD", "RGDS_THREADED_3D", "RGDS_AUTOSAVE", "RGDS_SHARED_MENU", "RGDS_MENU_LIVE", "RGDS_MENU_EXTERNAL", "RGDS_OVERLAY_FILE"):
            env.pop(key, None)
    env["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin"
    env["SDL_TOUCH_MOUSE_EVENTS"] = "0"
    env["SDL_MOUSE_TOUCH_EVENTS"] = "0"
    env['SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS'] = '1'
    if env.get("WAYLAND_DISPLAY"):
        env["SDL_VIDEODRIVER"] = "wayland"
    return env

def prepare_nnddss(app, data, binary):
    from nnddss_assets import install
    source = binary.parent
    root = data / "emulators" / "nnddss"
    # This loader locates BIOS/assets beside /proc/self/exe even with --root.
    # Keep the executable next to its writable runtime assets and private BIOS.
    install(app, root, binary)
    for folder in ("User/system", "User/savestates", "User/shaders", "User/config", "User/backup", "User/cheats", "User/input_record", "User/microphone", "User/slot2", "assets/bios", "assets/fonts", "assets/ui", "assets/shaders", "home", "tmp"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    # The loader can resolve lib/ relative to --root or its working directory.
    # Refresh these code dependencies while retaining User/config files.
    if (source / "lib").is_dir():
        for library in (source / "lib").iterdir():
            if library.is_file() and not library.is_symlink():
                link_component(library, root / "lib" / library.name)
    for name in ("gamecontrollerdb.txt", "tools/rgds_volume_helper.py"):
        original = source / name
        if original.is_file():
            copy_component(original, root / name)
    copy_missing(source / "gamecontrollerdb.txt" if (source / "gamecontrollerdb.txt").is_file() else None, root / "User/gamecontrollerdb.txt")
    for tree, destination in ((source / "assets", root / "assets"), (source / "ui", root / "assets/ui"),
                              (source / "shaders", root / "User/shaders"), (app / "emulators/shaders", root / "User/shaders")):
        if tree.is_dir():
            for path in tree.rglob("*"):
                if path.is_file() and not path.is_symlink():
                    copy_missing(path, destination / path.relative_to(tree))
    profile = app / "assets/emulator-profiles/nnddss.ini"
    if not profile.is_file(): profile = app / "emulators/nnddss.ini"
    copy_missing(profile if profile.is_file() else None, root / "assets/config.ini")
    copy_missing(root / "assets/config.ini" if (root / "assets/config.ini").is_file() else None, root / "config.ini")
    bindings = root / "assets/bindings.ini"
    if not bindings.exists():
        bindings.write_text("btn_a=b\nbtn_b=a\nbtn_x=y\nbtn_y=x\nbtn_l=leftshoulder\nbtn_r=rightshoulder\nbtn_start=start\nbtn_select=back\nbtn_up=dpup\nbtn_down=dpdown\nbtn_left=dpleft\nbtn_right=dpright\nbtn_screen=lefttrigger\nbtn_ffwd=none\n")
    # Optional databases are explicit user inputs. Never search another engine,
    # the stock OS, or a global font directory. No console firmware dump is copied.
    for name in ("game_database.xml", "usrcheat.dat"):
        candidate = data / 'bios/nnddss' / name
        found = candidate if candidate.is_file() else None
        copy_missing(found, root / "User" / name)
        if name == "game_database.xml": copy_missing(found, root / "assets" / name)
    return root

def dsperate_atomic_states(app, binary, verified_manifest):
    """Grant the bridge's early save ack only to the reviewed packaged writer."""
    if not isinstance(verified_manifest, dict) or binary.resolve() != (app/'emulators/dsperate/dsperate').resolve():
        return False
    files = verified_manifest.get('files', {})
    if 'emulators/dsperate/build.json' not in files:
        return False
    try:
        record = json.loads((app/'emulators/dsperate/build.json').read_text())
        return (record.get('kind') == 'source-built-dsperate'
                and record.get('source', {}).get('commit') == '1b76c355109c9f7576363ccc023927b3137d3c6f'
                and record.get('source', {}).get('patched_inputs_sha256') == 'c8f4a7243832dd609cca4b0b33fed3120fadbf0a3106c0f8b497981c91ec9129'
                and record.get('binary', {}).get('sha256') == files.get('emulators/dsperate/dsperate')
                and isinstance(files.get('emulators/dsperate/dsperate'), str))
    except (OSError, ValueError, AttributeError):
        return False


def launch_plan(core, app, data, rom, prepare=True, identity=None, fresh_start=False):
    if core not in CORES:
        raise ValueError("Unknown emulator: " + core)
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
        raise ValueError(core + " is missing. Install its verified application payload." if independent() else core + " is missing. Copy the supplied local emulator payload or verify stock DraStic.")
    env = base_environment()
    filter_choice = display_filter(data, identity or rom)
    tuning = game_options(data, identity or rom)
    if core == 'rgds-experimental':
        try: scale = int(env.get('RGDS_VK_SCALE', tuning['rgds_scale']))
        except ValueError: scale = 1
        scale = max(1, min(5, scale))
        env['RGDS_3D_SMOOTH'] = str(tuning['rgds_smooth'])
        env['RGDS_ACTIVE_SCALE'] = str(scale)
        candidate = binary.with_name('rgds-experimental-vulkan')
        driver = binary.parent/'lib/libvulkan_panfrost.so'
        live_graphics = candidate.is_file() and driver.is_file()
        if scale > 1 and not live_graphics:
            raise ValueError('RG DS high-resolution renderer is missing. Install the verified Vulkan payload or select Native.')
        if live_graphics:
            # Native starts without a Vulkan context; the same process can
            # create one when the user changes resolution during play.
            binary = candidate
            icd = data/'emulators/rgds-experimental/panvk.json'
            if prepare:
                icd.parent.mkdir(parents=True, exist_ok=True)
                content = json.dumps({'file_format_version':'1.0.0', 'ICD':{'library_path':str(driver), 'api_version':'1.3.0'}})+'\n'
                pending = icd.with_suffix('.pending'); pending.write_text(content); pending.replace(icd)
            env.update(RGDS_VK_SCALE=str(scale), DS_GPU3D='1' if scale > 1 else '0', VK_ICD_FILENAMES=str(icd),
                       PANVK_ATOM_STRIDE='56', PANVK_KBASE_DVFS='none')

    ds_input=read_fields(data/'library'/game_key(identity or rom)/'ds-input.ini')
    try:ds_language=int(ds_input.get('language','1'))
    except ValueError:ds_language=1
    ds_language=max(0,min(5,ds_language));ds_microphone=ds_input.get('microphone','1')!='0'
    ds_nickname=''.join(c for c in ds_input.get('nickname','RG DS Core' if core=='rgds-experimental' else 'DSperate') if c.isprintable())
    ds_nickname=ds_nickname.encode('utf-16-le')[:20].decode('utf-16-le',errors='ignore') or 'RG DS Core'
    env['RGDS_DS_LANGUAGE']=str(ds_language);env['RGDS_MICROPHONE']='1' if ds_microphone else '0'

    if not supported(core, filter_choice):
        print(f"{SHADERS[filter_choice]['name']} is unavailable in {core}; using Off.",file=sys.stderr)
        filter_choice=0
    env.update(RGDS_VOLUME_HELPER=str(app / "runtime/audio.py"), RGDS_VOLUME_DATA=str(data), RGDS_VOLUME_KEYS="1")
    env['RGDS_SHADER']=str(filter_choice)
    env['RGDS_FPS']='1' if env.get('RGDS_FPS')=='1' or read_fields(data/'performance.ini').get('fps')=='1' else '0'
    env['RGDS_SHADER_DIR']=str(app / 'emulators/shaders')
    global_controls,game_controls=control_paths(data,identity or rom)
    env['RGDS_CONTROLS_GLOBAL']=str(global_controls)
    env['RGDS_CONTROLS_FILE']=str(game_controls)
    env['RGDS_ENGINE_BINARY']=str(binary)
    env['RGDS_OWN_FF_HUD']='1' if core == 'rgds-experimental' else '0'
    env["TZ"] = read_fields(data / "system.ini").get("timezone", "Europe/Oslo")
    if backend == 'superdrastic':
        from superdrastic import launch_plan as super_plan
        return super_plan(app, data, binary, rom, env, prepare=prepare, identity=identity)
    command = [str(binary)]
    cwd = binary.parent
    hooks = app / "emulators/hooks"
    selected_hooks = select_hooks(core, hooks)
    if core == "drastic":
        home = drastic_home(app, data, binary.parent,mapped_controls=bool(selected_hooks.bridge)) if prepare else data / "emulators/drastic"
        if prepare:drastic_game_profile(home, rom, tuning)
        env["HOME"] = str(home)
        cwd = home
        from linux_platform import independent
        engine_libraries(env, private=(binary.parent / "libs",), stock_independent=independent())
        env["SDL_RENDER_DRIVER"] = "opengles2"
        touch_candidates = (binary.parent / "libs/libdrastouch.so",) if independent() else (binary.parent / "libs/libdrastouch.so", Path("/usr/lib/libdrastouch.so"))
        touch = next((p for p in touch_candidates if p.is_file()), None)
        if independent() and touch is None:
            raise ValueError("DraStic's verified private display/touch adapter is missing; reinstall its complete payload.")
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
            if os.environ.get("RGDS_FORCE_AUTOLOAD")=="1":auto_load=True
            if fresh_start:auto_load=False
            section_config(config, {"emu": {"autoload": str(auto_load).lower(), "autosave": str(auto_save).lower(), "frameskip":tuning['frameskip']},
                "paths": {"saves": str(root / "saves"), "states": str(root / "states")},
                "hotkeys": {"pause": "none", "pause.alt": "none", "save_state": "F5", "load_state": "F7", "volume_up":"none","volume_down":"none","mute":"none","fast_forward":"none","fast_forward_toggle":"Tab"},
                "padhotkeys":{"modifier":"none","pause":"none","quit":"none","save_state":"none","load_state":"none","volume_up":"none","volume_down":"none","fast_forward":"none","fast_forward_toggle":"none"},
                "keys":{name:"none" for name in ("a","b","x","y","l","r","start","select","up","down","left","right")},
                "pad":{"a":"a","b":"b","x":"x","y":"y","l":"leftshoulder","r":"rightshoulder","start":"start","select":"back","face_fix":"off"},
                "audio":{"volume":100,"driver":"alsa"},
                "video":{"sink":"renderer","linear":"false","lcd_grid":"0"}})
            for folder in ("saves", "states"): (root / folder).mkdir(parents=True, exist_ok=True)
            dsperate_input_profile(home,config,rom,{"user":{"language":ds_language,"nickname":ds_nickname},"audio":{"mic":str(ds_microphone).lower(),"mic_dev":"plughw:0,0","mic_gain":"1.0","mic_gate":"2.0"}})
        env["HOME"] = str(home)
        cwd = root
        env["XDG_CONFIG_HOME"] = str(home / ".config")
        engine_libraries(env, private=(binary.parent / "lib", binary.parent / "libs"), stock_independent=True)
        env["SDL_RENDER_DRIVER"] = "opengles2"
        env["SDL_GAMECONTROLLERCONFIG"] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
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
            top=env.get('RGDS_TOP_DISPLAY');bottom=env.get('RGDS_BOTTOM_DISPLAY')
            if top is not None and bottom is not None and top.isdigit() and bottom.isdigit() and top!=bottom:
                env['DS_DUAL_SCREENS']='upper='+top+',lower='+bottom
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
        env["SDL_GAMECONTROLLERCONFIG"] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        command += ["--rom", str(rom), "--save", str(root / "saves" / (key + ".sav")),
                    "--state", str(root / "states" / (key + ".dss"))]
    else:
        from linux_platform import independent
        if env.get('RGDS_INPUT_AMBIGUOUS')=='1':
            raise ValueError('Console input mapping is ambiguous; select the verified touch/controller devices before launching NNDDSS.')
        if env.get('RGDS_DEVICE_MODEL')=='rg-ds-plus' and (not env.get('NNDDSS_TOUCH_DEV') or not env.get('NNDDSS_KEYS_DEV')):
            raise ValueError('RG DS Plus input devices are unavailable; run RGDS Inventory and RGDS Probe before launching NNDDSS.')
        root = prepare_nnddss(app, data, binary) if prepare else data / "emulators/nnddss"
        if prepare:
            from nnddss_database import prepare as prepare_save_database
            prepare_save_database(app, data, root, rom)
        command[0] = str(root/'nnddss')
        cwd = root
        env["HOME"] = str(root / "home")
        env["TMPDIR"] = str(root / "tmp")
        env["XDG_CONFIG_HOME"] = str(root / "xdg-config")
        env["XDG_CACHE_HOME"] = str(root / "xdg-cache")
        # The supplied loader patches live code for DMA uploads; on this card
        # that path races during startup with an input/render hook installed.
        env['NNDDSS_FAST_UPLOAD']='0'
        if prepare:
            Path(env["XDG_CONFIG_HOME"]).mkdir(parents=True, exist_ok=True)
            Path(env["XDG_CACHE_HOME"]).mkdir(parents=True, exist_ok=True)
        engine_libraries(env, private=(root / "lib",), stock_independent=True)
        env["NNDDSS_DATA_ROOT"] = str(root)
        env['NNDDSS_CORE_DEFAULT'] = str(root / 'lib/libnnddss.so')
        env['NNDDSS_CORE_RENEGADE'] = str(root / 'lib/libnnddss_renegade_kid.so')
        env['NNDDSS_LANG'] = 'en'
        env['NNDDSS_GPU_MAX'] = '0'  # RGDS owns hardware power policy.
        # Retain the original card's fallback only outside the Plus profile.
        # Plus event numbering is unverified; the session discovers its nodes.
        from linux_platform import independent
        if not independent() and env.get('RGDS_DEVICE_MODEL')!='rg-ds-plus':
            env.setdefault("NNDDSS_TOUCH_DEV", "/dev/input/event1")
            env.setdefault("NNDDSS_KEYS_DEV", "/dev/input/event4")
        env["SDL_GAMECONTROLLERCONFIG"] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b1,b:b0,x:b2,y:b3,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        command += ["--root", str(root), "--rom", str(rom)]
        if prepare:
            for config in (root/"assets/config.ini",root/"config.ini"):
                # The bridge compiles live presets after the native game frame.
                # Keep the loader's startup-only effect off to avoid two passes.
                flat_config(config,{"fx_enable":0,"shader":"", "ir_3d_2x":tuning["hires"], "frameskip_auto":int(tuning["frameskip"]>0)})
    if selected_hooks.bridge:
        preload = [str(selected_hooks.bridge)]
        env["SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS"] = "0"
        if core == "drastic" and env.get("LD_PRELOAD"):
            preload.append(env["LD_PRELOAD"])
        env["LD_PRELOAD"] = ":".join(preload)
        auto_load, auto_save = policy(data)
        if os.environ.get("RGDS_FORCE_AUTOLOAD")=="1":auto_load=True
        if fresh_start:auto_load=False
        state_dir = data / ("emulators/drastic/savestates" if core=="drastic" else "emulators/nnddss/User/savestates" if core=="nnddss" else "emulators/melonds/states" if core=="melonds" else "emulators/" + core + "/states")
        auto = data / "library" / game_key(identity or rom) / ("auto-" + core + ".dss")
        env.update(RGDS_CORE="dsperate" if core in DSPERATE_CORES else core, RGDS_CONTROL_FILE=str(data / "session/emulator.control"), RGDS_STATE_DIR=str(state_dir),
                   RGDS_AUTOSTATE_FILE=str(auto), RGDS_AUTOSAVE="1" if auto_save else "0", RGDS_AUTOLOAD="0", RGDS_SHARED_MENU="1", RGDS_MENU_LIVE="0")
        slot_name = rom.stem + ("_1.dss" if core == "drastic" else "_8.dss")
        if core == "melonds":slot_name = game_key(identity or rom) + ".dss"
        if core in DSPERATE_CORES and prepare:
            with rom.open("rb") as stream:
                stream.seek(12);code = stream.read(4).decode("ascii", errors="replace")
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
                    for chunk in iter(lambda: stream.read(256 * 1024), b''): hasher.update(chunk)
                digest = hasher.hexdigest()[:16]
                backup = auto.parent / "manual-state-backups" / core / (destination.stem + "-" + digest + destination.suffix)
                copy_missing(destination, backup)
            shutil.copy2(auto, destination)
            env["RGDS_NATIVE_AUTOLOAD" if core=="melonds" else "RGDS_AUTOLOAD"]="1"
        if prepare and core=="nnddss":
            bindings=dict(btn_a='b',btn_b='a',btn_x='y',btn_y='x',btn_l='leftshoulder',btn_r='rightshoulder',btn_start='start',btn_select='back',btn_up='dpup',btn_down='dpdown',btn_left='dpleft',btn_right='dpright',btn_screen='lefttrigger',btn_ffwd='none')
            bindings.update({"key_"+name:"none" for name in ("a","b","x","y","l","r","start","select","up","down","left","right")})
            flat_config(root/'assets/bindings.ini',bindings)
            for folder in ("User/shaders", "assets/shaders"):
                for shader in (app / "emulators/shaders").glob("*"):copy_missing(shader, data / "emulators/nnddss" / folder / shader.name)
        if prepare and core=="drastic":
            for shader in (app / "emulators/shaders").glob("*"):copy_missing(shader, data / "emulators/drastic/shaders" / shader.name)
    return {"command": command, "cwd": str(cwd), "env": env}

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


def _run_once(core, app, data, rom, display_handoff=None, fresh_start=False):
    data.mkdir(parents=True, exist_ok=True)
    marker = data / "session/core.json"
    child = None
    volume = VolumeWatch(data)
    menu = None
    inputs = None
    lease = None
    engine_log = EngineLog(data, core, [], app)
    original_rom = Path(rom).resolve()
    accumulated, sessions = playtime(data, original_rom)
    began = None
    started = datetime.now(timezone.utc).isoformat()
    def stop(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        if os.statvfs(data).f_flag & os.ST_RDONLY:
            raise OSError(errno.EROFS, "Game storage is read-only. Start the writable internal RGDS installation; your saves were preserved.")
        # Serialize the launch marker with deployment's check+stop operation.
        # A newly requested launch cannot slip between that check and shutdown.
        with activity_lock(data):
            record = {"pid": os.getpid(), "start": process_identity(os.getpid()), "core": core}
            write_record(marker, record)
        from quick_resume import invalidate
        invalidate(data)
        rom = resolve_rom(rom, data)
        plan = launch_plan(core, app, data, rom, identity=original_rom, fresh_start=fresh_start)
        engine_log.command, engine_log.cwd = list(plan["command"]), plan["cwd"]
        plan['env']['RGDS_OVERLAY_FILE'] = f'/dev/shm/rgds-menu-{os.getpid()}-{time.monotonic_ns()}.frame'
        plan['env']['RGDS_FRAME_STATS'] = plan['env']['RGDS_OVERLAY_FILE'] + '.stats'
        print("Launching " + core + ": " + str(rom), flush=True)
        (data / "session/emulator.control").unlink(missing_ok=True)
        (data / "session/emulator.control.result").unlink(missing_ok=True)
        for suffix in ('.speed-request','.speed-state'):
            Path(str(data / 'session/emulator.control')+suffix).unlink(missing_ok=True)
        # Fail before opening audio if the mixer cannot be controlled.
        volume.start()
        plan["env"]["RGDS_VOLUME_KEYS"] = "1" if volume.descriptors else "0"
        inputs=MenuInput(data,original_rom)
        plan['env']['RGDS_MENU_EXTERNAL']='1' if inputs.descriptors else '0'
        super_backend = plan.get('backend') == 'superdrastic'
        if super_backend and not inputs.descriptors:
            raise ValueError('SuperDrastic needs readable RG DS menu controls. Choose Standard DraStic until input access is restored.')
        release_launcher_display(display_handoff)
        if super_backend:
            from superdrastic import VTLease
            lease = VTLease(); lease.acquire()
        child = subprocess.Popen(plan["command"], cwd=plan["cwd"], env=plan["env"],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        engine_log.start(child.stdout)
        began = time.monotonic()
        if super_backend:
            from superdrastic import Supervisor
            menu = Supervisor(child, app, data, core, original_rom, plan['env'], inputs=inputs)
        else:
            menu = GameMenu(child, app, data, core, original_rom, plan["env"],inputs=inputs)
        inputs=None
        checkpoint_at = began
        while child.poll() is None:
            menu.poll()
            if time.monotonic() - checkpoint_at >= 30:
                record_playtime(data, original_rom, accumulated + menu.played_seconds(), sessions+1, started)
                checkpoint_at = time.monotonic()
            time.sleep(.02)
        # Deliver the completed exit to the pause UI before closing its process.
        menu.poll()
        result = child.wait()
        if super_backend: result = menu.finish(result)
        if result:
            print(f"{core} exited with code {result}. Diagnostics: {engine_log.path}", file=sys.stderr, flush=True)
        if result==0:
            from quick_resume import record_exit
            try:record_exit(data,original_rom,core,datetime.fromisoformat(started).timestamp())
            except OSError as error:print('Quick resume was not armed: '+str(error),file=sys.stderr)
        switch = menu.completed_switch() if result == 0 else None
        if switch:
            print('Switching to ' + switch.core + ': ' + str(switch.rom), flush=True)
            return switch
        return (77 if getattr(menu,"reset_state",False) else 75) if result == 0 and menu.restart else result if result >= 0 else 128 - result
    except KeyboardInterrupt:
        if menu:menu.close()
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        return 143
    finally:
        if inputs:inputs.close()
        seconds = menu.played_seconds() if menu else int(time.monotonic()-began) if began is not None else 0
        if menu:
            try:menu.close()
            except OSError as error:print("Game-menu cleanup: "+str(error),file=sys.stderr)
        # A failed menu/IPC helper must not leave a live engine behind while the
        # launcher reopens and the activity marker is cleared.
        if child and child.poll() is None:
            child.terminate()
            try:child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill();child.wait()
        restore_error = None
        if lease:
            try: lease.close()
            except OSError as error:
                restore_error = error
                print('SuperDrastic display restoration failed: ' + str(error), file=sys.stderr)
        if engine_log:
            try: engine_log.finish(child.poll() if child else None, sys.exc_info()[1] or restore_error)
            except OSError as error: print("Could not save engine diagnostics: " + str(error), file=sys.stderr)
        try:volume.close()
        except OSError as error:print("Volume cleanup: "+str(error),file=sys.stderr)
        try:
            if began is not None:record_playtime(data, original_rom, accumulated + seconds, sessions+1, started)
        except OSError:
            print("Could not save playtime.", file=sys.stderr)
        finally:
            try:marker.unlink(missing_ok=True)
            except OSError as error:print("Could not clear game activity: " + str(error),file=sys.stderr)
        if restore_error and sys.exc_info()[0] is None:
            raise OSError('SuperDrastic could not restore the original display console; see the engine log.') from restore_error

def run(core, app, data, rom, display_handoff=None):
    fresh_start = False
    switching = False
    while True:
        previous_autoload = os.environ.get('RGDS_FORCE_AUTOLOAD')
        try:
            if switching:os.environ['RGDS_FORCE_AUTOLOAD'] = '1'
            result = _run_once(core, app, data, rom, display_handoff, fresh_start=fresh_start)
        finally:
            if previous_autoload is None:os.environ.pop('RGDS_FORCE_AUTOLOAD', None)
            else:os.environ['RGDS_FORCE_AUTOLOAD'] = previous_autoload
        display_handoff = None
        if isinstance(result, SwitchGame):
            core, rom = result.core, result.rom
            fresh_start = False
            switching = True
            continue
        if result not in (75,77):return result
        fresh_start = result == 77
        if fresh_start:continue
        options = game_options(data, rom)
        defaults = read_fields(Path(data)/'preferences.ini')
        selection = options['core'] if options['core'] >= 0 else int(defaults.get('core','1'))
        core = CORES[selection] if 0 <= selection < len(CORES) else core

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
