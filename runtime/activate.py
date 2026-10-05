#!/usr/bin/env python3
"""Development build validation and version-file activation; this is not the OTA updater."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

sys.dont_write_bytecode = True
from emulators import activity_lock, core_running, process_identity
from records import read_record

BUILD_ID = re.compile(r"[a-z0-9][a-z0-9.-]{0,100}\Z")

def read_pointer(path):
    if not path.exists():
        return None
    value = path.read_text().strip()
    if not BUILD_ID.fullmatch(value):
        raise ValueError("Invalid build pointer: " + str(path))
    return value

def verify_release(app):
    manifest = json.loads((app / "manifest.json").read_text())
    if manifest.get("project") != "rgds" or manifest.get("schema") != 1 or manifest.get("target") != "aarch64-linux-gnu":
        raise ValueError("Wrong project, schema, or architecture.")
    if manifest.get("build") != app.name or not BUILD_ID.fullmatch(app.name):
        raise ValueError("Release directory does not match manifest.")
    files = manifest.get("files", {})
    if not isinstance(files, dict) or "bin/rgds-launcher" not in files:
        raise ValueError("Manifest has no launcher.")
    for name, digest in files.items():
        path = Path(name)
        if path.is_absolute() or ".." in path.parts or "\\" in name or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid manifest file entry.")
        source = app / path
        if source.is_symlink() or not source.is_file() or app.resolve() not in source.resolve().parents:
            raise ValueError("Missing or unsafe release file: " + name)
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError("Release file failed SHA-256: " + name)
    actual = {str(p.relative_to(app)) for p in app.rglob("*") if p.is_file() and p != app / "manifest.json"}
    if actual != set(files):
        raise ValueError("Unlisted files in release.")
    header = (app / "bin/rgds-launcher").read_bytes()[:20]
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("Launcher is not a Linux AArch64 ELF.")
    return manifest

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
