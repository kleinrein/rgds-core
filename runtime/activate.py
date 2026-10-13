#!/usr/bin/env python3
"""Release verification and local activation primitives; signed OTA lives in ota.py."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import sys
import time

sys.dont_write_bytecode = True
from engines import CORES
from records import process_identity, read_record

BUILD_ID = re.compile(r"[a-z0-9][a-z0-9.-]{0,100}\Z")

def read_pointer(path):
    if not path.exists():
        return None
    value = path.read_text().strip()
    if not BUILD_ID.fullmatch(value):
        raise ValueError("Invalid build pointer: " + str(path))
    return value

def file_stamp(info):
    return [info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns]


def release_inventory(app):
    """Walk once without resolving every payload's ancestors or following links."""
    inventory = {}
    pending = [(Path(app), "")]
    while pending:
        folder, prefix = pending.pop()
        with os.scandir(folder) as entries:
            for entry in entries:
                name = prefix + entry.name
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISDIR(info.st_mode):
                    pending.append((Path(entry.path), name + "/"))
                elif stat.S_ISREG(info.st_mode):
                    inventory[name] = file_stamp(info)
                else:
                    raise ValueError("Missing or unsafe release file: " + name)
    return inventory


def verify_release(app, selected=None, receipt=False):
    app = Path(app)
    if app.is_symlink() or (app / "manifest.json").is_symlink():
        raise ValueError("Unsafe release directory or manifest.")
    manifest_stamp = file_stamp((app / "manifest.json").stat())
    manifest = json.loads((app / "manifest.json").read_text())
    if not isinstance(manifest, dict) or manifest.get("project") != "rgds" or manifest.get("schema") != 1 or manifest.get("target") != "aarch64-linux-gnu":
        raise ValueError("Wrong project, schema, or architecture.")
    if manifest.get("build") != app.name or not BUILD_ID.fullmatch(app.name):
        raise ValueError("Release directory does not match manifest.")
    files = manifest.get("files", {})
    if not isinstance(files, dict) or "bin/rgds-launcher" not in files:
        raise ValueError("Manifest has no launcher.")
    for name, digest in files.items():
        if not isinstance(name, str) or not name or not isinstance(digest, str):
            raise ValueError("Invalid manifest file entry.")
        path = Path(name)
        if path.is_absolute() or ".." in path.parts or "\\" in name or str(path) != name or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid manifest file entry.")
    inventory = release_inventory(app)
    if inventory.get("manifest.json") != manifest_stamp:
        raise ValueError("Manifest changed during verification.")
    actual = set(inventory) - {"manifest.json"}
    missing = set(files) - actual
    if missing:
        raise ValueError("Missing or unsafe release file: " + sorted(missing)[0])
    if actual != set(files):
        raise ValueError("Unlisted files in release.")
    for name, digest in files.items():
        source = app / name
        if selected is None or selected(name):
            hasher = hashlib.sha256()
            with source.open("rb") as stream:
                while True:
                    chunk = stream.read(1024 * 1024)
                    if not chunk:break
                    hasher.update(chunk)
                unchanged = file_stamp(os.fstat(stream.fileno())) == inventory[name]
            if not unchanged or hasher.hexdigest() != digest:
                raise ValueError("Release file failed SHA-256: " + name)
    with (app / "bin/rgds-launcher").open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("Launcher is not a Linux AArch64 ELF.")
    if receipt:
        return dict(schema=1, app=str(app.absolute()), files=inventory)
    return manifest

def verify_startup(app, receipt=False):
    """Hash the executable/startup code before display; defer engine payloads."""
    def startup(name):
        return name == "bin/rgds-launcher" or name.startswith("runtime/") or name.startswith("themes/") or name.startswith("assets/icons/") or name.startswith("assets/status/") or name.startswith("assets/brand/") or name.startswith("assets/locales/") or name == "assets/shaders/catalog.tsv" or (name.startswith("assets/fonts/rounded-") and name.endswith(".rgf"))
    return verify_release(app, startup, receipt=receipt)


def verify_startup_receipt(app, receipt):
    """Reuse this boot's hashes only while every inspected file is unchanged."""
    app = Path(app)
    if not isinstance(receipt, dict) or receipt.get("schema") != 1 or receipt.get("app") != str(app.absolute()) or not isinstance(receipt.get("files"), dict):
        raise ValueError("Invalid startup verification receipt.")
    if app.is_symlink() or release_inventory(app) != receipt["files"]:
        raise ValueError("Release changed after startup verification.")


def verify_engine(app, core, backend=None):
    if core not in CORES:
        raise ValueError("Unknown engine")
    def dependencies(name):
        # Corresponding engine source/build tools are distributed for rebuilds,
        # but are not loaded by an emulator. Full installation verification
        # covers them; do not rehash a large source archive on every game launch.
        if name in ("emulators/dsperate/source.tar.gz", "emulators/rgds-experimental/source.tar.gz", "emulators/rgds-experimental/vulkan-source.tar.gz", "emulators/rgds-experimental/panvk-source.tar.gz", "emulators/melonds/source.tar.gz", "emulators/superdrastic/source.tar.gz", "emulators/superdrastic/sdl/source.tar.gz") or name.startswith("emulators/dsperate/rgds-build/"):
            return False
        if core == 'drastic' and backend == 'superdrastic' and name.startswith(('emulators/superdrastic/', 'assets/fonts/source/')):
            return True
        if core == 'nnddss' and (name.startswith('assets/nnddss/') or name.startswith('assets/fonts/source/')):
            return True
        return name.startswith("runtime/") or name == "emulators/hooks/librgdsbridge.so" or name.startswith("emulators/" + core + "/") or name in ("emulators/" + core + ".ini", "emulators/" + core + ".cfg") or name.startswith("emulators/shaders/") or name.startswith("assets/emulator-profiles/")
    return verify_release(app, dependencies)


def write_pointer(path, value):
    if not BUILD_ID.fullmatch(value):
        raise ValueError("Invalid build ID.")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w") as stream:
        stream.write(value + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    # Best effort metadata flush; power-loss recovery on FAT still needs hardware testing.
    try:
        descriptor = os.open(str(path.parent), os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError:
        pass

def stop_session(data):
    with activity_lock(data):
        _stop_session_locked(data)


def activity_lock(data):
    from emulators import activity_lock as lock
    return lock(data)


def core_running(data):
    from emulators import core_running as running
    return running(data)


def _stop_session_locked(data):
    if core_running(data):
        raise ValueError("A game is running. Exit to the RGDS menu before deploying or rolling back.")
    marker = Path("/tmp/rgds-session.lock/owner.json")
    if marker.is_file():
        record = read_record(marker)
        if record is None: return
        pid = int(record["pid"])
        if record["start"] and process_identity(pid) == record["start"]:
            os.kill(pid, signal.SIGTERM)
            for _ in range(100):
                if not marker.exists():
                    return
                time.sleep(0.1)
            raise ValueError("Session did not stop. Leave this build active and inspect its log.")

def activate(root, build, stop=True):
    if not BUILD_ID.fullmatch(build):
        raise ValueError("Invalid build ID.")
    app = root / "releases" / build
    verify_release(app)
    if stop:
        stop_session(root / "userdata")
    current = read_pointer(root / "state/active")
    if current and current != build:
        write_pointer(root / "state/previous", current)
    write_pointer(root / "state/active", build)
    return build

def rollback(root, stop=True):
    from contextlib import nullcontext
    guard=None
    if (root/'update_guard.py').exists():
        from ota import guard_module
        guard=guard_module(root)
    with guard.update_lock(root) if guard else nullcontext():
        if (root/'state/update-pending.json').exists():raise ValueError('An update is already prepared. Install it or discard it first.')
        previous = read_pointer(root / "state/previous")
        if not previous:
            raise ValueError("No previous development build is available.")
        return activate(root, previous, stop=stop)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("activate", "rollback", "verify"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build")
    args = parser.parse_args()
    if args.action == "rollback":
        print("Activated " + rollback(args.root))
    elif args.action == "activate":
        if not args.build or not BUILD_ID.fullmatch(args.build):
            parser.error("--build must be a valid build ID")
        print("Activated " + activate(args.root, args.build))
    else:
        build = args.build or read_pointer(args.root / "state/active")
        if not build:
            raise ValueError("No active build")
        print(json.dumps(verify_release(args.root / "releases" / build), indent=2))

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print("RGDS activation: " + str(error), file=sys.stderr)
        sys.exit(1)
