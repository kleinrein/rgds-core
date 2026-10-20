#!/usr/bin/env python3
"""Create a private SD package with RGDS integration and supplied emulator payloads."""
import hashlib
import fcntl
import json
from pathlib import Path
import shutil
import struct
import sys
import zipfile

if __package__:
    from .elf_abi import inspect_elf
    from .provision_engines import validate_bundle
    from .build import validate_build as validate_rgds_build
    from .build_engines import validate_build as validate_engine_build
    from .validate_shader_previews import validate_shader_previews
    from .build_freebios import validate as validate_freebios
    from .build_nnddss_database import validate as validate_nnddss_database
    from .build_melonds import validate_engine as validate_melonds
    from .build_rgds_experimental import validate_build as validate_experimental
    from .package_rgds_vulkan import validate_payload as validate_vulkan
    from .build_superdrastic import validate_build as validate_superdrastic
    from .build_samba import validate_build as validate_samba
else:
    from elf_abi import inspect_elf
    from provision_engines import validate_bundle
    from build import validate_build as validate_rgds_build
    from build_engines import validate_build as validate_engine_build
    from validate_shader_previews import validate_shader_previews
    from build_freebios import validate as validate_freebios
    from build_nnddss_database import validate as validate_nnddss_database
    from build_melonds import validate_engine as validate_melonds
    from build_rgds_experimental import validate_build as validate_experimental
    from package_rgds_vulkan import validate_payload as validate_vulkan
    from build_superdrastic import validate_build as validate_superdrastic
    from build_samba import validate_build as validate_samba

ROOT = Path(__file__).resolve().parents[1]

# Keep the documentation tree intact so relative links work in the SD package.
# Historical reports live below docs/archive, separate from current guides.
DOCUMENTS = (
    'README.md',
    'THIRD_PARTY.md',
    'src/experimental/rgds/README.md',
    *(path.relative_to(ROOT).as_posix() for path in sorted((ROOT / 'docs').rglob('*.md'))),
)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def put(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

def checked_record(path, validated):
    """A concurrently published build cannot replace a checked record on copy."""
    try:
        copied = json.loads(Path(path).read_text())
    except (OSError, ValueError) as error:
        raise ValueError("Copied build record is unreadable: " + str(path)) from error
    if copied != validated:
        raise ValueError("Build record changed while packaging: " + str(path))

def payload_source(name, source_build=None):
    if name.startswith('services/samba/'):
        return 'Pinned Samba SMB service with private runtime dependencies'
    if name == "bin/rgds-launcher" or name.endswith("/librgdsbridge.so"):
        return "RGDS source build"
    if name.startswith("emulators/dsperate/"):
        if source_build:
            return "DSperate source build at " + source_build["source"]["commit"]
        return "Supplied DSperate payload; upstream engine, supplied build"
    if name.startswith("emulators/rgds-experimental/"):
        return "RG DS experimental source fork of pinned DSperate"
    if name.startswith("emulators/melonds/"):
        return "Pinned melonDS source build with original RGDS SDL frontend"
    if name.startswith("emulators/superdrastic/sdl/"):
        return "Pinned SDL2 2.32.10 private runtime for SuperDrastic"
    if name.startswith("emulators/superdrastic/"):
        return "Pinned SuperDrastic source build with RGDS integration"
    if name.startswith("emulators/nnddss/"):
        return "Supplied NNDDSS payload; external engine and loader/shims"
    return "External engine runtime library"


def component_abi(app, files, source_build=None):
    result = {}
    for name in files:
        path = app / name
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        metadata = inspect_elf(path, source=payload_source(name, source_build))
        if metadata["machine"] != "aarch64":
            raise ValueError("A device payload has the wrong architecture: " + name)
        if metadata["sha256"] != files[name]:
            raise ValueError("Payload changed during inspection: " + name)
        result[name] = metadata
    return result


def glibc_floor(components):
    versions = ["2.17", *(entry["glibc_minimum"] for entry in components.values() if entry["glibc_minimum"])]
    return max(versions, key=lambda value: tuple(map(int, value.split("."))))


def package():
    lock = ROOT / ".cache/package.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    with lock.open("a") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        return _package()

def _package():
    binary = ROOT / "build/device/rgds-launcher"
    if not binary.is_file():
        raise ValueError("Run make build-device first.")
    header = binary.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
        raise ValueError("The device launcher must be an AArch64 ELF, not a desktop binary.")
    rgds_build = validate_rgds_build(binary.parent)
    engine_source = ROOT / "build/engines/dsperate"
    try: engine_build = validate_engine_build(engine_source)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError("Source-built DSperate rejected: " + str(error) + ". Run make build-engines.") from error
    experimental_source = ROOT / "build/engines/rgds-experimental"
    experimental_build = validate_experimental(experimental_source)
    vulkan_source = ROOT / "build/engines/rgds-vulkan"
    vulkan_build = validate_vulkan(vulkan_source)
    melon_source = ROOT / "build/melonds-experiment"
    melon_build = validate_melonds(melon_source)
    super_source = ROOT / 'build/superdrastic'
    super_build = validate_superdrastic(super_source)
    samba_source = ROOT / 'build/samba'
    samba_build = validate_samba(samba_source)
    upstream = ROOT / "vendor/engines"
    try: payloads = validate_bundle(upstream)
    except (OSError, ValueError) as error:
        raise ValueError("Engine bundle rejected: " + str(error) + ". Provision it with tools/provision_engines.py.") from error
    previews = validate_shader_previews(ROOT / "assets/shader-previews", upstream / "shaders", ROOT / "src/platform/game_shader.hpp")
    freebios = validate_freebios(ROOT / 'assets/nnddss/freebios')
    validate_nnddss_database(ROOT / 'assets/nnddss/save-types')
    scratch = ROOT / "build/package"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    app = scratch / "app"
    put(binary, app / "bin/rgds-launcher")
    put(ROOT / "build/device/librgdsbridge.so", app / "emulators/hooks/librgdsbridge.so")
    put(binary.parent / "build.json", app / "build/rgds.json")
    checked_record(app / "build/rgds.json", rgds_build)
    put(ROOT / "assets/emulator-profiles/drastic.cfg", app / "emulators/drastic.cfg")
    for folder in ("runtime", "themes", "assets"):
        for source in sorted((ROOT / folder).rglob("*")):
            if source.is_file() and "__pycache__" not in source.parts and source.name != ".DS_Store" and not source.name.startswith("._"):
                put(source, app / source.relative_to(ROOT))
    for name in (*sorted(samba_build['files']), 'build.json'):
        put(samba_source / name, app / 'services/samba' / name)
    checked_record(app / 'services/samba/build.json', samba_build)
    for name, expected in samba_build['files'].items():
        if sha(app / 'services/samba' / name) != expected:
            raise ValueError('SMB service changed while packaging: ' + name)
    put(ROOT / "assets/emulator-profiles/dsperate.ini", app / "emulators/dsperate.ini")
    put(ROOT / "assets/emulator-profiles/nnddss.ini", app / "emulators/nnddss.ini")
    for name in sorted(payloads["files"]):
        if name.startswith("dsperate/"): continue
        put(upstream / name, app / "emulators" / name)
    for source in sorted((ROOT / "assets/shaders").glob("*.dsd")):
        put(source, app / "emulators/shaders" / source.name)
    for name in (*sorted(engine_build["files"]), "build.json"):
        put(engine_source / name, app / "emulators/dsperate" / name)
    checked_record(app / "emulators/dsperate/build.json", engine_build)
    for name in ("tools/build_engines.py", "tools/elf_abi.py"):
        put(ROOT / name, app / "emulators/dsperate/rgds-build" / name)
    driver_hash = engine_build.get("build_driver_sha256")
    if driver_hash and sha(app / "emulators/dsperate/rgds-build/tools/build_engines.py") != driver_hash:
        raise ValueError("Source engine build driver changed since compilation; rebuild DSperate")
    patches = ROOT / "tools/engine_patches"
    for recorded_patch in engine_build.get("source", {}).get("patches", []):
        source = patches / recorded_patch['name']
        if source.is_file(): put(source, app / "emulators/dsperate/rgds-build" / source.relative_to(ROOT))
    for recorded_patch in engine_build.get("source", {}).get("patches", []):
        patched = app / "emulators/dsperate/rgds-build/tools/engine_patches" / recorded_patch["name"]
        if not patched.is_file() or sha(patched) != recorded_patch["sha256"]:
            raise ValueError("Source engine portability patch changed since compilation; rebuild DSperate")
    for name in (*sorted(experimental_build['files']), 'build.json'):
        put(experimental_source/name, app/'emulators/rgds-experimental'/name)
    checked_record(app/'emulators/rgds-experimental/build.json', experimental_build)
    for name in (*sorted(vulkan_build['files']), 'vulkan-build.json'):
        put(vulkan_source/name, app/'emulators/rgds-experimental'/name)
    checked_record(app/'emulators/rgds-experimental/vulkan-build.json', vulkan_build)
    for name, expected in vulkan_build['files'].items():
        if sha(app/'emulators/rgds-experimental'/name) != expected:
            raise ValueError('Vulkan payload changed while packaging: '+name)

    for name, expected in experimental_build['files'].items():
        if sha(app/'emulators/rgds-experimental'/name) != expected:
            raise ValueError('Experimental payload changed while packaging: ' + name)
    for name in ('melonds','source.tar.gz','COPYING','build.json'):
        put(melon_source/name,app/'emulators/melonds'/name)
    checked_record(app/'emulators/melonds/build.json',melon_build)
    if sha(app/'emulators/melonds/melonds')!=melon_build['engine']['sha256'] or sha(app/'emulators/melonds/source.tar.gz')!=melon_build['source']['archive_sha256']:
        raise ValueError('melonDS payload changed during packaging')
    for name in (*sorted(super_build['files']), 'build.json'):
        put(super_source/name, app/'emulators/superdrastic'/name)
    checked_record(app/'emulators/superdrastic/build.json', super_build)
    for name, expected in super_build['files'].items():
        if sha(app/'emulators/superdrastic'/name) != expected:
            raise ValueError('SuperDrastic payload changed during packaging: ' + name)
    put(ROOT / "runtime/nnddss_volume.py", app / "emulators/nnddss/tools/rgds_volume_helper.py")
    for name, expected in payloads["files"].items():
        if name.startswith("dsperate/"): continue
        if sha(app / "emulators" / name) != expected:
            raise ValueError("Engine payload changed while packaging: " + name)
    for name, expected in freebios['files'].items():
        if sha(app / 'assets/nnddss/freebios' / name) != expected:
            raise ValueError('Replacement NNDDSS BIOS changed while packaging: ' + name)
    validate_freebios(app / 'assets/nnddss/freebios')
    put(ROOT / 'tools/build_freebios.py', app / 'assets/nnddss/build_freebios.py')
    if sha(app / 'assets/nnddss/build_freebios.py') != freebios['driver_sha256']:
        raise ValueError('Replacement BIOS build driver changed while packaging')
    for name, expected in engine_build["files"].items():
        if sha(app / "emulators/dsperate" / name) != expected:
            raise ValueError("Source engine changed while packaging: " + name)
    for name, info in rgds_build["artifacts"].items():
        copied = app / ("bin/rgds-launcher" if name == "rgds-launcher" else "emulators/hooks/librgdsbridge.so")
        if sha(copied) != info["sha256"]: raise ValueError("RGDS artifact changed while packaging: " + name)
    packaged_payloads = dict(payloads, kind="rgds-composed-engine-bundle")
    packaged_payloads["components"] = dict(payloads["components"], dsperate="Pinned DSperate source build with corresponding source and build record", melonds="Pinned melonDS source build with RGDS SDL frontend and corresponding source", superdrastic="Optional pinned SuperDrastic backend with RGDS integration and corresponding source")
    packaged_payloads["components"]["rgds-experimental"] = "Experimental DSperate fork with isolated saves and corresponding source"
    packaged_payloads["files"] = {name: checksum for name, checksum in payloads["files"].items() if not name.startswith("dsperate/")}
    packaged_payloads["files"].update({"dsperate/" + name: checksum for name, checksum in engine_build["files"].items()})
    packaged_payloads["files"].update({"melonds/"+name:sha(app/"emulators/melonds"/name) for name in ("melonds","source.tar.gz","COPYING","build.json")})
    packaged_payloads['files'].update({'superdrastic/'+name:sha(app/'emulators/superdrastic'/name) for name in (*super_build['files'], 'build.json')})
    packaged_payloads["files"].update({"rgds-experimental/" + name: sha(app/"emulators/rgds-experimental"/name) for name in (*experimental_build["files"], "build.json")})
    packaged_payloads["files"].update({"rgds-experimental/"+name:sha(app/"emulators/rgds-experimental"/name) for name in (*vulkan_build["files"], "vulkan-build.json")})
    (app / "emulators/payloads.json").write_text(json.dumps(packaged_payloads, indent=2, sort_keys=True) + "\n")
    put(ROOT / "THIRD_PARTY.md", app / "THIRD_PARTY.md")
    files = {str(p.relative_to(app)): sha(p) for p in sorted(app.rglob("*")) if p.is_file()}
    abi = component_abi(app, files, engine_build)
    version = (ROOT / "VERSION").read_text().strip()
    # Bootstrap changes also yield a different build identity.
    salt = {p.name: sha(p) for p in sorted((ROOT / "packaging").glob("*.sh"))}
    salt.update({name:sha(ROOT/"packaging"/name) for name in ("update_guard.py","update-trust.json")})
    digest = hashlib.sha256(json.dumps({"files": files, "bootstrap": salt, "component_abi": abi}, sort_keys=True).encode()).hexdigest()[:12]
    build_id = version + "-" + digest
    manifest = {"schema": 1, "project": "rgds", "version": version, "build": build_id,
                "target": "aarch64-linux-gnu", "minimum_glibc": glibc_floor(abi),
                "launcher_minimum_glibc": "2.17", "component_abi": abi, "private_development_package": True,
                "source_builds": {"samba": "services/samba/build.json", "rgds-experimental": "emulators/rgds-experimental/build.json", "rgds": "build/rgds.json", "dsperate": "emulators/dsperate/build.json", "melonds": "emulators/melonds/build.json", "superdrastic": "emulators/superdrastic/build.json"},
                "shader_previews": {"manifest": "assets/shader-previews/manifest.json", "scenes": len(previews["scenes"]), "presets": len(previews["scenes"][0]["presets"])},
                "cores": ["drastic", "dsperate", "nnddss", "melonds", "rgds-experimental"], "optional_backends": {"drastic": ["standard", "superdrastic"]}, "files": files}
    (app / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    root = scratch / "card/rgds"
    release = root / "releases" / build_id
    release.parent.mkdir(parents=True)
    app.rename(release)
    put(ROOT / "packaging/main.sh", root / "main.sh")
    (root / "state").mkdir()
    put(ROOT/"packaging/update_guard.py",root/"update_guard.py")
    put(ROOT/"packaging/update-trust.json",root/"state/update-trust.json")
    (root / "state/active").write_text(build_id + "\n")
    for source in sorted((ROOT / "packaging").glob("*.sh")):
        if source.name != "main.sh":
            put(source, scratch / "card/Roms/APPS" / source.name)
    for name in DOCUMENTS:
        put(ROOT / name, root / "docs" / name)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    archive = dist / ("RGDS-" + build_id + ".zip")
    pending = archive.with_suffix(".zip.pending")
    with zipfile.ZipFile(pending, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for source in sorted((scratch / "card").rglob("*")):
            if source.is_file():
                info = zipfile.ZipInfo(str(source.relative_to(scratch / "card")), (2026, 10, 5, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                executable = source.suffix == ".sh" or source.name in ("rgds-launcher", "dsperate", "nnddss", "melonds", "rgds-experimental", "rgds-experimental-vulkan", "smbd", "smbpasswd", "smbclient", "samba-dcerpcd", "rpcd_classic", "rpcd_winreg", "rpcd_lsad")
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                output.writestr(info, source.read_bytes())
    pending.replace(archive)
    archive.with_suffix(".zip.sha256").write_text(sha(archive) + "  " + archive.name + "\n")
    pointer = dist / "latest.pending"
    pointer.write_text(archive.name + "\n");pointer.replace(dist / "latest.txt")
    print(str(archive))
    print("Private test package: source-built RGDS, DSperate, RG DS Experimental, melonDS and optional SuperDrastic; stock DraStic and supplied NNDDSS. Hardware validation remains pending.")
    return archive

if __name__ == "__main__":
    try:
        package()
    except (OSError, ValueError) as error:
        print("RGDS packaging: " + str(error), file=sys.stderr)
        sys.exit(1)
