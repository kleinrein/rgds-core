#!/usr/bin/env python3
"""Create a private SD test package, with all three SimpleOS emulator adapters."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def put(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

def package():
    binary = ROOT / "build/device/rgds-launcher"
    if not binary.is_file():
        raise ValueError("Run make build-device first.")
    header = binary.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
        raise ValueError("The device launcher must be an AArch64 ELF, not a desktop binary.")
    upstream = ROOT / "simpleos-source/simpleos/system"
    if not (upstream / "bin/dsperate").is_file() or not (upstream / "nnddss_payload/nnddss").is_file():
        raise ValueError("The supplied SimpleOS DSperate and NNDDSS files are required for this private test package.")
    scratch = ROOT / "build/package"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    app = scratch / "app"
    put(binary, app / "bin/rgds-launcher")
    for folder in ("runtime", "themes", "assets"):
        for source in sorted((ROOT / folder).rglob("*")):
            if source.is_file() and "__pycache__" not in source.parts:
                put(source, app / source.relative_to(ROOT))
    put(upstream / "bin/dsperate", app / "emulators/dsperate/dsperate")
    for name in ("dsperate.ini", "nnddss.ini"):
        put(upstream / name, app / "emulators" / name)
    for source in sorted((upstream / "dsperate").glob("*")):
        if source.is_file():
            put(source, app / "emulators/dsperate" / source.name)
    for source in sorted((upstream / "nnddss_payload").rglob("*")):
        if source.is_file():
            put(source, app / "emulators/nnddss" / source.relative_to(upstream / "nnddss_payload"))
    for source in sorted((upstream / "shaders").glob("*")):
        if source.is_file():
            put(source, app / "emulators/shaders" / source.name)
    put(ROOT / "THIRD_PARTY.md", app / "THIRD_PARTY.md")
    files = {str(p.relative_to(app)): sha(p) for p in sorted(app.rglob("*")) if p.is_file()}
    version = (ROOT / "VERSION").read_text().strip()
    # Bootstrap changes also yield a different build identity.
    salt = {p.name: sha(p) for p in sorted((ROOT / "packaging").glob("*.sh"))}
    digest = hashlib.sha256(json.dumps({"files": files, "bootstrap": salt}, sort_keys=True).encode()).hexdigest()[:12]
    build_id = version + "-" + digest
    manifest = {"schema": 1, "project": "rgds", "version": version, "build": build_id,
                "target": "aarch64-linux-gnu", "minimum_glibc": "2.17", "private_development_package": True,
                "cores": ["drastic", "dsperate", "nnddss"], "files": files}
    (app / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    root = scratch / "card/rgds"
    release = root / "releases" / build_id
    release.parent.mkdir(parents=True)
    app.rename(release)
    put(ROOT / "packaging/main.sh", root / "main.sh")
    put(ROOT / "runtime/inventory.sh", root / "inventory.sh")
    (root / "state").mkdir()
    (root / "state/active").write_text(build_id + "\n")
    for source in sorted((ROOT / "packaging").glob("*.sh")):
        if source.name != "main.sh":
            put(source, scratch / "card/Roms/APPS" / source.name)
    for name in ("README.md", "docs/DEVICE_SETUP.md", "docs/DEVELOPMENT.md", "docs/SCREENSCRAPER.md", "THIRD_PARTY.md"):
        put(ROOT / name, root / "docs" / Path(name).name)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    archive = dist / ("RGDS-" + build_id + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for source in sorted((scratch / "card").rglob("*")):
            if source.is_file():
                info = zipfile.ZipInfo(str(source.relative_to(scratch / "card")), (2026, 10, 5, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                executable = source.suffix == ".sh" or source.name in ("rgds-launcher", "dsperate", "nnddss")
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                output.writestr(info, source.read_bytes())
    archive.with_suffix(".zip.sha256").write_text(sha(archive) + "  " + archive.name + "\n")
    (dist / "latest.txt").write_text(archive.name + "\n")
    print(str(archive))
    print("Private test package: DraStic adapter + supplied DSperate/NNDDSS. Hardware validation remains pending.")
    return archive

if __name__ == "__main__":
    try:
        package()
    except (OSError, ValueError) as error:
        print("RGDS packaging: " + str(error), file=sys.stderr)
        sys.exit(1)
