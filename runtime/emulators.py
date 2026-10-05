#!/usr/bin/env python3
"""Private development adapters for the three engines in the supplied SimpleOS package."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
import zipfile

sys.dont_write_bytecode = True
from library import playtime, record_playtime
from records import read_record, write_record

CORES = ("drastic", "dsperate", "nnddss")
STOCK_DIRS = (Path("/mnt/vendor/deep/drastic_aarch64"), Path("/mnt/vendor/deep/drastic64"))

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
    override = os.environ.get(core.upper() + "_BIN")
    candidates = [Path(override)] if override else []
    candidates += [app / "emulators" / core / core]
    if core == "drastic":
        candidates += [d / name for d in STOCK_DIRS for name in ("drastic", "drastic_oga")]
    elif core == "dsperate":
        candidates += [Path("/mnt/mmc/simpleos/system/bin/dsperate"), Path("/mnt/SDCARD/simpleos/system/bin/dsperate")]
    else:
        candidates += [Path("/mnt/mmc/nnddss/nnddss"), Path("/mnt/SDCARD/nnddss/nnddss"), Path("/mnt/vendor/deep/nnddss/nnddss")]
    return next((p.resolve() for p in candidates if p.is_file() and os.access(p, os.X_OK)), None)

def stock_file(names):
    for root in STOCK_DIRS:
        for directory in (root / "system", root):
            for name in names:
                path = directory / name
                if path.is_file() and path.stat().st_size:
                    return path
    return None

def copy_missing(source, destination):
    if not source or destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)

def copy_component(source, destination):
    """Refresh code files in a core's writable root without touching its saves."""
    if destination.is_file() and hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(destination.read_bytes()).digest():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".partial")
    shutil.copyfile(source, temporary)
    temporary.chmod(source.stat().st_mode & 0o777)
    temporary.replace(destination)

def resolve_rom(rom, data):
    rom = rom.resolve(strict=True)
    if rom.suffix.lower() in (".nds", ".dsi"):
        return rom
    if rom.suffix.lower() != ".zip":
        raise ValueError("Choose an .nds, .dsi, or .zip game.")
    digest = hashlib.sha256()
    with rom.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    with zipfile.ZipFile(rom) as archive:
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
            return output
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix(output.suffix + ".partial")
        try:
            with archive.open(member) as source, temporary.open("wb") as target:
                shutil.copyfileobj(source, target)
                target.flush()
                os.fsync(target.fileno())
            temporary.replace(output)
        finally:
            if temporary.exists():
                temporary.unlink()
        return output

def base_environment():
    env = os.environ.copy()
    # Each engine gets its own preload and HOME; never inherit another engine's hook.
    for key in ("LD_PRELOAD", "SIMPLEOS_NNDDSS", "SIMPLEOS_OSD_ONLY", "SDL_VIDEO_DOUBLE_BUFFER"):
        env.pop(key, None)
    env["SDL_TOUCH_MOUSE_EVENTS"] = "0"
    env["SDL_MOUSE_TOUCH_EVENTS"] = "0"
    if env.get("WAYLAND_DISPLAY"):
        env["SDL_VIDEODRIVER"] = "wayland"
    return env

def prepare_nnddss(app, data, binary):
    source = binary.parent
    root = data / "emulators" / "nnddss"
    for folder in ("User/system", "User/savestates", "User/shaders", "assets/bios", "assets/fonts", "assets/ui", "assets/shaders", "home", "tmp"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    # The loader can resolve lib/ relative to --root or its working directory.
    # Refresh these code dependencies while retaining User/config files.
    if (source / "lib").is_dir():
        for library in (source / "lib").iterdir():
            if library.is_file() and not library.is_symlink():
                copy_component(library, root / "lib" / library.name)
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
    copy_missing(app / "emulators/nnddss.ini" if (app / "emulators/nnddss.ini").is_file() else None, root / "assets/config.ini")
    copy_missing(root / "assets/config.ini" if (root / "assets/config.ini").is_file() else None, root / "config.ini")
    bindings = root / "assets/bindings.ini"
    if not bindings.exists():
        bindings.write_text("btn_a=b\nbtn_b=a\nbtn_x=y\nbtn_y=x\nbtn_l=leftshoulder\nbtn_r=rightshoulder\nbtn_start=start\nbtn_select=back\nbtn_up=dpup\nbtn_down=dpdown\nbtn_left=dpleft\nbtn_right=dpright\nbtn_screen=lefttrigger\nbtn_ffwd=none\n")
    bios = {
        "drastic_bios_arm7.bin": ("drastic_bios_arm7.bin", "nds_bios_arm7.bin", "bios7.bin"),
        "drastic_bios_arm9.bin": ("drastic_bios_arm9.bin", "nds_bios_arm9.bin", "bios9.bin"),
        "nds_firmware.bin": ("nds_firmware.bin", "nds_firmware_modified.bin", "firmware.bin"),
    }
    for name, names in bios.items():
        found = stock_file(names)
        copy_missing(found, root / "assets/bios" / name)
        copy_missing(found, root / "User/system" / name)
        if name.startswith("drastic_bios"):
            copy_missing(found, root / "User/system" / name.replace("drastic_", "nds_"))
    for name in ("drastic_bios_arm7.bin", "drastic_bios_arm9.bin"):
        if not (root / "assets/bios" / name).is_file():
            raise ValueError("NNDDSS needs stock DS BIOS files. Inventory the stock DraStic installation first.")
    font_roots = (Path("/usr/share/fonts"), Path("/mnt/vendor/bin/ebook/resources/fonts"))
    fonts = [p for directory in font_roots if directory.is_dir() for p in directory.rglob("*.ttf") if p.is_file()]
    preferred = next((p for p in fonts if p.name == "DejaVuSans.ttf"), fonts[0] if fonts else None)
    cjk = next((p for p in fonts if "SourceHanSans" in p.name or "NotoSansCJK" in p.name), preferred)
    copy_missing(preferred, root / "assets/fonts/orbitron.ttf")
    copy_missing(cjk, root / "assets/fonts/SourceHanSansSC-Normal.ttf")
    return root

def launch_plan(core, app, data, rom, prepare=True):
    if core not in CORES:
        raise ValueError("Unknown emulator: " + core)
    binary = find_binary(core, app)
    if not binary:
        raise ValueError(core + " is missing. Copy the supplied local emulator payload or verify stock DraStic.")
    env = base_environment()
    command = [str(binary)]
    cwd = binary.parent
    if core == "drastic":
        # Keep the stock configuration/menu. SimpleOS's joystick-hook remaps are not
        # valid without its compiled in-game hook, so we do not apply those remaps.
        env["HOME"] = str(binary.parent)
        env["LD_LIBRARY_PATH"] = str(binary.parent / "libs") + ":" + env.get("LD_LIBRARY_PATH", "")
        touch = next((p for p in (binary.parent / "libs/libdrastouch.so", Path("/usr/lib/libdrastouch.so")) if p.is_file()), None)
        if touch:
            env["LD_PRELOAD"] = str(touch)
        command.append(str(rom))
    elif core == "dsperate":
        home = data / "emulators/dsperate/home"
        config = data / "emulators/dsperate/config.ini"
        if prepare:
            (home / ".config/dsperate/games").mkdir(parents=True, exist_ok=True)
            copy_missing(app / "emulators/dsperate.ini" if (app / "emulators/dsperate.ini").is_file() else None, config)
        env["HOME"] = str(home)
        cwd = data / "emulators/dsperate"
        env["XDG_CONFIG_HOME"] = str(home / ".config")
        for directory in STOCK_DIRS:
            if (directory / "libs").is_dir():
                env["LD_LIBRARY_PATH"] = str(directory / "libs") + ":" + env.get("LD_LIBRARY_PATH", "")
                break
        env["SDL_GAMECONTROLLERCONFIG"] = "1900b655010000000100000000010000,ANBERNIC-rk3568-keys,a:b0,b:b1,x:b3,y:b2,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,"
        command += [str(rom), "--dual-window", "--fullscreen", "--config", str(config)]
        for switch, names in (("--bios7", ("nds_bios_arm7.bin", "drastic_bios_arm7.bin", "bios7.bin")),
                              ("--bios9", ("nds_bios_arm9.bin", "drastic_bios_arm9.bin", "bios9.bin")),
                              ("--firmware", ("nds_firmware.bin", "firmware.bin"))):
            found = stock_file(names)
            if found:
                command += [switch, str(found)]
    else:
        root = prepare_nnddss(app, data, binary) if prepare else data / "emulators/nnddss"
        cwd = root
        env["HOME"] = str(root / "home")
        env["TMPDIR"] = str(root / "tmp")
        env["XDG_CONFIG_HOME"] = str(root / "xdg-config")
        env["XDG_CACHE_HOME"] = str(root / "xdg-cache")
        if prepare:
            Path(env["XDG_CONFIG_HOME"]).mkdir(parents=True, exist_ok=True)
            Path(env["XDG_CACHE_HOME"]).mkdir(parents=True, exist_ok=True)
        env["LD_LIBRARY_PATH"] = str(binary.parent / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
        # The event numbers are explicit overrides until the device probe verifies them.
        env.setdefault("NNDDSS_TOUCH_DEV", "/dev/input/event1")
        env.setdefault("NNDDSS_KEYS_DEV", "/dev/input/event3")
        command += ["--root", str(root), "--rom", str(rom)]
    return {"command": command, "cwd": str(cwd), "env": env}

def process_identity(pid):
    try:
        content = Path("/proc") / str(pid) / "stat"
        # comm can contain spaces and parentheses; fields after its last ')' are fixed.
        return content.read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None

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

def run(core, app, data, rom):
    data.mkdir(parents=True, exist_ok=True)
    marker = data / "session/core.json"
    child = None
    original_rom = Path(rom).resolve()
    accumulated, sessions = playtime(data, original_rom)
    began = None
    started = datetime.now(timezone.utc).isoformat()
    def stop(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        # Serialize the launch marker with deployment's check+stop operation.
        # A newly requested launch cannot slip between that check and shutdown.
        with activity_lock(data):
            record = {"pid": os.getpid(), "start": process_identity(os.getpid()), "core": core}
            write_record(marker, record)
        rom = resolve_rom(rom, data)
        plan = launch_plan(core, app, data, rom)
        print("Launching " + core + ": " + str(rom), flush=True)
        child = subprocess.Popen(plan["command"], cwd=plan["cwd"], env=plan["env"])
        began = time.monotonic()
        while True:
            try:result = child.wait(timeout=30);break
            except subprocess.TimeoutExpired:record_playtime(data, original_rom, accumulated + int(time.monotonic()-began), sessions+1, started)
        return result if result >= 0 else 128 - result
    except KeyboardInterrupt:
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        return 143
    finally:
        try:
            if began is not None:record_playtime(data, original_rom, accumulated + int(time.monotonic()-began), sessions+1, started)
        except OSError:
            print("Could not save playtime.", file=sys.stderr)
        finally:
            marker.unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("catalog", "launch", "plan"))
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("userdata"))
    parser.add_argument("--core", choices=CORES, default="drastic")
    parser.add_argument("--rom", type=Path)
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
        return run(args.core, args.app.resolve(), args.data.resolve(), args.rom)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print("RGDS emulator: " + str(error), file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
