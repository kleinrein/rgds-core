#!/usr/bin/env python3
"""Import the owner's DraStic application for a private three-engine image.

Imports executable/display adapter and optional application data only. No OS
SDL/graphics libraries, BIOS dumps, saves, profiles or system scripts are copied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import tempfile

from elf_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]
KIND = "owner-provided-drastic-application"
REQUIRED = {"drastic", "libs/libdrastouch.so"}
OPTIONAL = {"game_database.xml", "usrcheat.dat", "drastic_logo_0.raw", "drastic_logo_1.raw"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate(directory):
    directory = Path(directory)
    if directory.is_symlink():
        raise ValueError("DraStic bundle must be a real directory")
    record = json.loads((directory / "application.json").read_text())
    if (not isinstance(record, dict) or record.get("schema") != 1 or record.get("kind") != KIND
            or record.get("private_only") is not True or not isinstance(record.get("files"), dict)
            or not REQUIRED <= set(record["files"]) <= REQUIRED | OPTIONAL):
        raise ValueError("Invalid private DraStic application record")
    actual = set()
    for path in directory.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError("Unsupported DraStic bundle node")
        if path.is_file() and path.name != "application.json":
            actual.add(path.relative_to(directory).as_posix())
    if actual != set(record["files"]):
        raise ValueError("DraStic application inventory mismatch")
    for name, checksum in record["files"].items():
        if not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-f]{64}", checksum) or digest(directory / name) != checksum:
            raise ValueError("DraStic application hash mismatch: " + name)
    for name in sorted(REQUIRED):
        info = inspect_elf(directory / name)
        if not info or info["machine"] != "aarch64":
            raise ValueError("DraStic requires Linux AArch64 application components")
    return record


def provision(source, output, touch_library=None):
    source, output = Path(source), Path(output)
    if source.is_symlink() or not source.is_dir() or output.exists() or output.is_symlink():
        raise ValueError("Need a real source directory and a fresh destination")
    binary = next((source / name for name in ("drastic", "drastic_oga") if (source / name).is_file()), None)
    adapter = Path(touch_library) if touch_library else source / "libs/libdrastouch.so"
    if binary is None or not adapter.is_file():
        raise ValueError("Need the owner's AArch64 DraStic executable and libdrastouch.so")
    components = {"drastic": binary, "libs/libdrastouch.so": adapter}
    components.update({name: source / name for name in OPTIONAL if (source / name).is_file()})
    for name, path in components.items():
        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise ValueError("Only regular application files may be imported")
        if name in REQUIRED:
            info = inspect_elf(path)
            if not info or info["machine"] != "aarch64":
                raise ValueError("DraStic requires Linux AArch64 application components")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".drastic-", dir=output.parent) as temporary:
        stage = Path(temporary) / "application";stage.mkdir()
        hashes = {name: digest(path) for name, path in components.items()}
        for name, path in components.items():
            target = stage / name;target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target);target.chmod(0o755 if name in REQUIRED else 0o644)
            if digest(target) != hashes[name] or digest(path) != hashes[name]:
                raise ValueError("DraStic source changed during import")
        record = dict(schema=1, kind=KIND, private_only=True, files=hashes,
                      source="Owner's existing DraStic application; binary components, not a source rebuild",
                      excluded=["BIOS/firmware dumps", "OS libraries/scripts", "user saves/configuration"],
                      redistribution_provenance="Unresolved; private testing only")
        (stage / "application.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        validate(stage);stage.rename(output)
    print("Verified private DraStic application: " + str(output))
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--touch-library", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "vendor/drastic")
    args = parser.parse_args()
    try:
        provision(args.source, args.output, args.touch_library)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
