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


# Persistent hash memo for the hot startup/launch paths. FAT assigns inode
# numbers per mount, so the cache keys on the on-disk metadata only: mode, size,
# modification and change time. It is bound to the exact manifest bytes and
# lives outside the hashed tree. Installation, activation and OTA always hash in
# full; a deferred low-priority audit re-hashes memoized files (see `audit`).
VERIFY_CACHE_SCHEMA = 1
AUDIT_INTERVAL_SECONDS = 24 * 3600


def portable_stamp(stamp):
    return stamp[2:] if stamp else None


def verify_cache_path(app):
    """Only an installed <root>/releases/<build> layout has a memo location."""
    app = Path(app)
    if app.parent.name != "releases" or not (app.parent.parent / "userdata").is_dir():
        return None
    return app.parent.parent / "userdata/cache/verified" / (app.name + ".json")


def load_verify_cache(app, manifest_digest):
    if verify_cache_path(app) is None:return {}
    try:
        record = json.loads(verify_cache_path(app).read_text())
    except (OSError, ValueError):
        return {}
    if not isinstance(record, dict) or record.get("schema") != VERIFY_CACHE_SCHEMA or record.get("manifest") != manifest_digest or not isinstance(record.get("files"), dict):
        return {}
    return record


def save_verify_cache(app, manifest_digest, files, audited=None):
    path = verify_cache_path(app)
    if path is None:return
    previous = load_verify_cache(app, manifest_digest)
    merged = dict(previous.get("files", {}))
    merged.update(files)
    record = dict(schema=VERIFY_CACHE_SCHEMA, manifest=manifest_digest, files=merged,
                  audited=previous.get("audited", 0) if audited is None else audited)
    pending = path.with_name(path.name + ".%d.tmp" % os.getpid())
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        pending.write_text(json.dumps(record, separators=(",", ":")))
        os.replace(pending, path)
    except OSError:
        # The memo is an optimization; a read-only card just hashes next time.
        try:pending.unlink()
        except OSError:pass


def forget_verify_cache(app):
    path = verify_cache_path(app)
    if path is None:return
    try:path.unlink()
    except OSError:pass


def release_inventory(app, stamped=None):
    """Walk once without resolving every payload's ancestors or following links.

    Files for which `stamped(name)` is false are classified from the directory
    entry type alone; a cold FAT card otherwise spends seconds on one stat per
    payload file that startup never opens.
    """
    inventory = {}
    pending = [(Path(app), "")]
    while pending:
        folder, prefix = pending.pop()
        with os.scandir(folder) as entries:
            for entry in entries:
                name = prefix + entry.name
                if stamped is not None and name != "manifest.json" and not stamped(name):
                    if entry.is_dir(follow_symlinks=False):
                        pending.append((Path(entry.path), name + "/"))
                    elif entry.is_file(follow_symlinks=False):
                        inventory[name] = None
                    else:
                        raise ValueError("Missing or unsafe release file: " + name)
                    continue
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISDIR(info.st_mode):
                    pending.append((Path(entry.path), name + "/"))
                elif stat.S_ISREG(info.st_mode):
                    inventory[name] = file_stamp(info)
                else:
                    raise ValueError("Missing or unsafe release file: " + name)
    return inventory


def verify_release(app, selected=None, receipt=False, cached=False):
    app = Path(app)
    if app.is_symlink() or (app / "manifest.json").is_symlink():
        raise ValueError("Unsafe release directory or manifest.")
    manifest_stamp = file_stamp((app / "manifest.json").stat())
    manifest_bytes = (app / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
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
    inventory = release_inventory(app, selected if cached else None)
    if inventory.get("manifest.json") != manifest_stamp:
        raise ValueError("Manifest changed during verification.")
    cached = cached and verify_cache_path(app) is not None
    manifest_digest = hashlib.sha256(manifest_bytes).hexdigest() if cached else None
    memo = load_verify_cache(app, manifest_digest).get("files", {}) if cached else {}
    learned = {}
    actual = set(inventory) - {"manifest.json"}
    missing = set(files) - actual
    if missing:
        raise ValueError("Missing or unsafe release file: " + sorted(missing)[0])
    if actual != set(files):
        raise ValueError("Unlisted files in release.")
    for name, digest in files.items():
        source = app / name
        if selected is None or selected(name):
            stamp = portable_stamp(inventory[name])
            if cached and memo.get(name) == [stamp, digest]:
                continue
            hasher = hashlib.sha256()
            with source.open("rb") as stream:
                while True:
                    chunk = stream.read(1024 * 1024)
                    if not chunk:break
                    hasher.update(chunk)
                unchanged = file_stamp(os.fstat(stream.fileno())) == inventory[name]
            if not unchanged or hasher.hexdigest() != digest:
                if cached:forget_verify_cache(app)
                raise ValueError("Release file failed SHA-256: " + name)
            if cached:learned[name] = [stamp, digest]
    if learned:save_verify_cache(app, manifest_digest, learned)
    with (app / "bin/rgds-launcher").open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("Launcher is not a Linux AArch64 ELF.")
    if receipt:
        return dict(schema=1, app=str(app.absolute()), files=inventory)
    return manifest

def startup_file(name):
    return name == "bin/rgds-launcher" or name.startswith("runtime/") or name.startswith("themes/") or name.startswith("assets/icons/") or name.startswith("assets/status/") or name.startswith("assets/brand/") or name.startswith("assets/locales/") or name == "assets/shaders/catalog.tsv" or (name.startswith("assets/fonts/rounded-") and name.endswith(".rgf"))


def verify_startup(app, receipt=False):
    """Hash the executable/startup code before display; defer engine payloads."""
    return verify_release(app, startup_file, receipt=receipt, cached=True)


def verify_startup_receipt(app, receipt):
    """Reuse this boot's hashes only while every inspected file is unchanged."""
    app = Path(app)
    if not isinstance(receipt, dict) or receipt.get("schema") != 1 or receipt.get("app") != str(app.absolute()) or not isinstance(receipt.get("files"), dict):
        raise ValueError("Invalid startup verification receipt.")
    if app.is_symlink() or release_inventory(app, startup_file) != receipt["files"]:
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
    return verify_release(app, dependencies, cached=True)


def background_priority():
    """Lowest CPU priority and idle I/O class, so an audit never competes with play."""
    try:os.nice(19)
    except OSError:pass
    if sys.platform != "linux":return
    try:
        import ctypes, platform
        number = {"aarch64": 30, "arm64": 30, "x86_64": 251}.get(platform.machine())
        if number is not None:
            # ioprio_set(IOPRIO_WHO_PROCESS, self, IOPRIO_CLASS_IDLE << 13)
            ctypes.CDLL(None, use_errno=True).syscall(number, 1, 0, 3 << 13)
    except (OSError, AttributeError):pass


def audit(app, force=False, clock=time.time):
    """Re-hash memoized files at idle priority, at most once a day.

    Startup trusts unchanged metadata; this catches silent card corruption
    later without delaying the first frame or a game launch.
    """
    app = Path(app)
    try:manifest_bytes = (app / "manifest.json").read_bytes()
    except OSError:return dict(state="unavailable")
    digest = hashlib.sha256(manifest_bytes).hexdigest()
    record = load_verify_cache(app, digest)
    if not record or (not force and clock() - record.get("audited", 0) < AUDIT_INTERVAL_SECONDS):
        return dict(state="skipped")
    names = sorted(record["files"])
    try:
        verify_release(app, lambda name: name in names)
    except (OSError, ValueError) as error:
        forget_verify_cache(app)
        return dict(state="failed", error=str(error))
    save_verify_cache(app, digest, {}, audited=int(clock()))
    return dict(state="verified", files=len(names))


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
    parser.add_argument("action", choices=("activate", "rollback", "verify", "audit"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build")
    args = parser.parse_args()
    if args.action == "rollback":
        print("Activated " + rollback(args.root))
    elif args.action == "activate":
        if not args.build or not BUILD_ID.fullmatch(args.build):
            parser.error("--build must be a valid build ID")
        print("Activated " + activate(args.root, args.build))
    elif args.action == "audit":
        build = args.build or read_pointer(args.root / "state/active")
        if not build:
            raise ValueError("No active build")
        background_priority()
        result = audit(args.root / "releases" / build)
        print(json.dumps(result))
        if result["state"] == "failed":sys.exit(1)
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
