#!/usr/bin/env python3
"""Own a temporary launcher session and restore only the stock processes we paused."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from emulators import process_identity
from system_controls import Hardware
from records import read_record, write_record

FRONTENDS = {"dmenu.bin", "muos3.bin", "muos2.bin", "MainUI"}
LOCK = Path("/tmp/rgds-session.lock")

def discover_frontends(proc=Path("/proc")):
    records = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip() not in FRONTENDS:
                continue
            state = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            if state[0] in ("T", "t", "Z"):
                continue
            records.append({"pid": int(entry.name), "start": state[19]})
        except (OSError, IndexError):
            continue
    return records

def restore(records):
    for record in records:
        if record["start"] and process_identity(record["pid"]) == record["start"]:
            try:
                os.kill(record["pid"], signal.SIGCONT)
            except ProcessLookupError:
                pass

def environment(app):
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.pop("LD_PRELOAD", None)
    env.pop("SDL_VIDEODRIVER", None)
    env["SDL_TOUCH_MOUSE_EVENTS"] = "0"
    env["SDL_MOUSE_TOUCH_EVENTS"] = "0"
    env["SDL_VIDEO_DISPLAY_PRIORITY"] = "DSI-1,DSI-2"
    existing = env.get("XDG_RUNTIME_DIR")
    directories = ([Path(existing)] if existing else []) + [Path(p) for p in ("/run/user/0", "/var/run", "/run", "/tmp")]
    for directory in directories:
        for path in sorted(directory.glob("wayland-*")):
            if path.is_socket():
                env["XDG_RUNTIME_DIR"] = str(directory)
                env["WAYLAND_DISPLAY"] = path.name
                env["SDL_VIDEODRIVER"] = "wayland"
                break
        if env.get("SDL_VIDEODRIVER"):
            break
    if not env.get("SDL_VIDEODRIVER"):
        raise ValueError("No Weston Wayland socket found. Run the inventory from stock APPS first.")
    for directory in (Path("/mnt/vendor/deep/drastic_aarch64/libs"), Path("/mnt/vendor/deep/drastic64/libs"), Path("/usr/lib")):
        for name in ("libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"):
            if (directory / name).is_file():
                env["RGDS_SDL_LIBRARY"] = str(directory / name)
                env["LD_LIBRARY_PATH"] = str(directory) + ":" + env.get("LD_LIBRARY_PATH", "")
                return env
    raise ValueError("Vendor SDL2 library not found. The inventory will identify its path.")

def release(root):
    from activate import read_pointer, verify_release
    build_id = read_pointer(root / "state/active")
    if not build_id:
        raise ValueError("No active development build. Copy the complete SD package.")
    app = root / "releases" / build_id
    verify_release(app)
    return app

def lock():
    try:
        LOCK.mkdir()
    except FileExistsError:
        owner = LOCK / "owner.json"
        try:
            record = read_record(owner)
            if record is None: raise ValueError("Session owner missing")
        except (OSError, ValueError):
            raise ValueError("Session lock has no readable owner. Reboot stock before trying again.")
        if record["start"] and process_identity(record["pid"]) == record["start"]:
            raise ValueError("An RGDS session is already running.")
        # The guardian may still own recovery after its parent has died. Do not
        # replace its lock or let it later resume stock during a new session.
        raise ValueError("Previous session recovery is still pending. Retry shortly; reboot stock if the lock remains.")
    write_record(LOCK / "owner.json", {"pid": os.getpid(), "start": process_identity(os.getpid())})

def unlock():
    (LOCK / "owner.json").unlink(missing_ok=True)
    try:
        LOCK.rmdir()
    except OSError:
        pass

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--dev", action="store_true", help="Use the writable live theme and remember menu selection")
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("Device sessions run on Linux. Use make preview on the desktop.")
    root = args.root.resolve()
    app = release(root)
    env = environment(app)
    data = root / "userdata"
    data.mkdir(parents=True, exist_ok=True)
    lock()
    # Finish a previous hardware recovery before starting another session.
    try:
        Hardware(data).restore()
    except (OSError, ValueError):
        unlock()
        raise
    env["RGDS_SYSTEM_MANAGED"] = "1"
    records = discover_frontends()
    read_fd, write_fd = os.pipe()
    guardian = os.fork()
    if guardian == 0:
        os.close(write_fd)
        # Closing the parent's descriptor (including SIGKILL) releases the stock UI.
        # This guardian does not inherit the launcher's process group.
        os.setsid()
        for signum in (signal.SIGINT, signal.SIGTERM):
            signal.signal(signum, signal.SIG_IGN)
        message = b""
        try:
            while True:
                chunk = os.read(read_fd, 1024)
                if not chunk:
                    break
                message += chunk
        finally:
            # Stop our orphaned process group before giving stock its input back.
            try:
                owned = json.loads(message.decode())
                if owned["start"] and process_identity(owned["pid"]) == owned["start"]:
                    os.killpg(owned["pid"], signal.SIGTERM)
                    for _ in range(70):
                        if process_identity(owned["pid"]) != owned["start"]:
                            break
                        time.sleep(0.1)
                    else:
                        os.killpg(owned["pid"], signal.SIGKILL)
            except (OSError, ValueError, KeyError):
                pass
            try:
                Hardware(data).restore()
            except (OSError, ValueError) as error:
                # Preserve the journal for the next launch to retry recovery.
                print("RGDS hardware recovery: " + str(error), file=sys.stderr, flush=True)
            restore(records)
            unlock()
            os._exit(0)
    os.close(read_fd)
    child = None
    def stop(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        for record in records:
            if record["start"] and process_identity(record["pid"]) == record["start"]:
                os.kill(record["pid"], signal.SIGSTOP)
        command = [str(app / "bin/rgds-launcher"), "--app", str(app), "--data", str(data),
                   "--theme", str(app / "themes/default/light.ini")]
        if args.dev:
            live = data / "dev/theme.ini"
            live.parent.mkdir(parents=True, exist_ok=True)
            if not live.exists():
                live.write_bytes((app / "themes/default/light.ini").read_bytes())
            command[command.index("--theme") + 1] = str(live)
            command += ["--watch-assets", "--state", str(data / "dev/menu-state.txt")]
        if args.probe:
            command += ["--probe", "--seconds", "90"]
        else:
            command += ["--allow-launch"]
            for path in (root.parent / "Roms/NDS", root / "games", root.parent / "simpleos/games"):
                command += ["--rom-dir", str(path)]
        log_path = data / "session.log"
        with log_path.open("a", buffering=1) as log:
            log.write("\nRGDS session: " + app.name + "\n")
            child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            os.write(write_fd, json.dumps({"pid": child.pid, "start": process_identity(child.pid)}).encode())
            write_record(data / "session/launcher.json", {"pid": child.pid, "start": process_identity(child.pid)})
            result = child.wait()
            if result:
                print("RGDS returned an error. Read " + str(log_path))
            return result
    except KeyboardInterrupt:
        return 143
    finally:
        if child and child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=7)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
        (data / "session/launcher.json").unlink(missing_ok=True)
        os.close(write_fd)
        os.waitpid(guardian, 0)

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("RGDS session: " + str(error), file=sys.stderr)
        sys.exit(1)
