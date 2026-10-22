#!/usr/bin/env python3
"""Build pinned melonDS for Linux ARM64: core probe and optional SDL engine.

The original frontend uses upstream FreeBIOS and generated firmware, without Qt.
No handheld connection or deployment occurs here.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile

try:
    from .build_engines import digest, find_tool, tool_record
    from .elf_abi import inspect_elf
except ImportError:
    from build_engines import digest, find_tool, tool_record
    from elf_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = "https://github.com/melonDS-emu/melonDS"
COMMIT = "906e9ebb27da8c6a715cd7abab4abfe8a8d29427"
TREE = "60c6724f8695bdbe5e21c4367b18293f7f811fc2"
INPUTS_SHA256 = "17d56c66414d0d5d26fb1842b03772190931abc5bf54314684c67f223124f2da"
TARGET = "aarch64-linux-gnu.2.17"
FRONTEND = ROOT / "src/experimental/melonds"
OPTIONS = {
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
    "BUILD_QT_SDL": "OFF",
    "ENABLE_JIT": "ON",
    "ENABLE_LTO_RELEASE": "ON",
    "ENABLE_OGLRENDERER": "OFF",
    "ENABLE_GDBSTUB": "OFF",
    "TEAKRA_BUILD_TOOLS": "OFF",
    "TEAKRA_BUILD_UNIT_TESTS": "OFF",
    "CMAKE_SKIP_RPATH": "ON",
    "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "ON",
}


def run(command, env=None):
    print(shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def source_inputs(source, archived=False):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()

    if source.is_symlink(): raise ValueError("Source directory must not be a symlink")
    if archived:
        paths = list(source.rglob('*'))
        if any(p.is_symlink() for p in paths): raise ValueError('Archived source contains symlinks')
        names = [p.relative_to(source).as_posix() for p in paths if p.is_file()]
    else:
        if git("rev-parse", "HEAD") != COMMIT or git("rev-parse", "HEAD^{tree}") != TREE:
            raise ValueError("melonDS source must match the pinned commit and tree")
        if git("status", "--porcelain", "--untracked-files=all"):
            raise ValueError("melonDS source must be an unmodified checkout")
        names = subprocess.check_output(["git", "-C", str(source), "ls-files", "-z"]).decode().split("\0")
    files = {}
    for name in sorted(filter(None, names)):
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Unsupported melonDS source file: " + name)
        files[name] = digest(path)
    fingerprint = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if fingerprint != INPUTS_SHA256:
        raise ValueError("melonDS source bytes differ from the pin")
    return files


def ensure_source(source, fetch, archived=False):
    if not source.exists() and fetch:
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "init", source])
        run(["git", "-C", source, "remote", "add", "origin", UPSTREAM + ".git"])
        run(["git", "-C", source, "fetch", "--depth", "1", "origin", COMMIT])
        run(["git", "-C", source, "checkout", "--detach", COMMIT])
    if not source.is_dir():
        raise ValueError("melonDS source missing; use --fetch")
    return source_inputs(source, archived)


def write_toolchain(directory, zig, source):
    directory.mkdir(parents=True, exist_ok=True)
    for name, command in (("cc", "cc"), ("cxx", "c++"), ("ar", "ar"), ("ranlib", "ranlib")):
        args = [str(zig), command]
        if name in ("cc", "cxx"):
            args += ["-target", TARGET]
        path = directory / name
        path.write_text("#!/bin/sh\nexec " + shlex.join(args) + ' "$@"\n')
        path.chmod(0o755)
    path = directory / "linux-aarch64.cmake"
    lines = ['set(CMAKE_SYSTEM_NAME Linux)', 'set(CMAKE_SYSTEM_PROCESSOR aarch64)']
    for language, wrapper in (("C", "cc"), ("CXX", "cxx"), ("ASM", "cc")):
        lines.append(f'set(CMAKE_{language}_COMPILER "{directory / wrapper}")')
    for key, wrapper in (
        ("AR", "ar"), ("RANLIB", "ranlib"), ("C_COMPILER_AR", "ar"), ("CXX_COMPILER_AR", "ar"),
        ("C_COMPILER_RANLIB", "ranlib"), ("CXX_COMPILER_RANLIB", "ranlib")
    ):
        lines.append(f'set(CMAKE_{key} "{directory / wrapper}")')
    flags = f"-mtune=cortex_a55 -ffile-prefix-map={source}=/src/melonDS -ffile-prefix-map={ROOT}=/rgds"
    for language in ("C", "CXX", "ASM"):
        lines.append(f'set(CMAKE_{language}_FLAGS_INIT "{flags}")')
    for kind, mode in (("PROGRAM", "NEVER"), ("LIBRARY", "ONLY"), ("INCLUDE", "ONLY"), ("PACKAGE", "ONLY")):
        lines.append(f'set(CMAKE_FIND_ROOT_PATH_MODE_{kind} {mode})')
    path.write_text("\n".join(lines) + "\n")
    return path


def validate_build(build):
    cache = (build / "CMakeCache.txt").read_text()
    for key, expected in (("ENABLE_JIT", "ON"), ("ENABLE_LTO_RELEASE", "ON"), ("BUILD_QT_SDL", "OFF")):
        if f"{key}:BOOL={expected}\n" not in cache:
            raise ValueError("melonDS build did not enable requested option: " + key)
    entries = json.loads((build / "compile_commands.json").read_text())
    jit = [entry for entry in entries if "/ARMJIT_A64/" in entry["file"]]
    if len(jit) < 5 or not all("-DJIT_ENABLED" in entry["command"] for entry in jit):
        raise ValueError("ARM64 JIT was not compiled")
    if not all("-flto" in entry["command"] for entry in entries if entry["file"].endswith(".cpp")):
        raise ValueError("Release LTO was not compiled")
    abi = inspect_elf(build / "rgds-melonds-probe", "pinned melonDS core with original RGDS test frontend")
    if not abi or abi["machine"] != "aarch64" or abi["interpreter"] != "/lib/ld-linux-aarch64.so.1":
        raise ValueError("melonDS probe is not a Linux ARM64 executable")
    if not abi["glibc_minimum"] or tuple(map(int, abi["glibc_minimum"].split("."))) > (2, 17):
        raise ValueError("melonDS probe exceeds the intended GLIBC 2.17 baseline")
    return abi


def validate_engine(directory):
    directory = Path(directory)
    record = json.loads((directory / 'build.json').read_text())
    abi = inspect_elf(directory / 'melonds', 'Pinned melonDS with original RGDS SDL frontend')
    if record.get('engine') != abi or abi['machine'] != 'aarch64' or abi['interpreter'] != '/lib/ld-linux-aarch64.so.1':
        raise ValueError('melonDS engine differs from its build record')
    if tuple(map(int, abi['glibc_minimum'].split('.'))) > (2, 17):
        raise ValueError('melonDS engine exceeds GLIBC 2.17')
    if record.get('driver_sha256') != digest(Path(__file__)) or record.get('frontend', {}).get('inputs') != {
        p.name: digest(p)
        for p in FRONTEND.glob('*') if p.is_file()
    }:
        raise ValueError('melonDS sources changed; rebuild the engine')
    if record.get('options') != OPTIONS or record.get(
        'source', {}
    ).get('commit') != COMMIT or record['source'].get('inputs_sha256') != INPUTS_SHA256:
        raise ValueError('melonDS source/options differ from the pin')
    if record['source'].get('archive_sha256') != digest(directory / 'source.tar.gz'):
        raise ValueError('melonDS corresponding source changed')
    # The frontend and upstream source must be present in the archive, not just
    # a matching hash copied into a record by mistake.
    with tarfile.open(directory / 'source.tar.gz') as source:
        for name, checksum in record['frontend']['inputs'].items():
            data = source.extractfile('rgds/src/experimental/melonds/' + name).read()
            if hashlib.sha256(data).hexdigest() != checksum: raise ValueError('Incomplete frontend source archive')
        if source.extractfile('melonDS/LICENSE').read() != (directory / 'COPYING').read_bytes():
            raise ValueError('melonDS license mismatch')
    validate_build(directory)
    return record


def archive_source(source, files, output):
    extras = [
        Path(__file__), ROOT / "tools/probe_melonds.py", ROOT / "tools/probe_melonds_frontend.py",
        ROOT / "tools/build_engines.py", ROOT / "tools/elf_abi.py", *sorted(FRONTEND.glob("*"))
    ]
    entries = [("melonDS/" + name, source / name) for name in files]
    entries += [("rgds/" + path.relative_to(ROOT).as_posix(), path) for path in extras]
    with output.open("wb") as stream, gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w") as tar:
            for name, path in entries:
                data = path.read_bytes()
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = path.stat().st_mode & 0o777
                tar.addfile(member, io.BytesIO(data))


def build(args):
    source, directory = args.source.resolve(), args.build.resolve()
    files = ensure_source(source, args.fetch, args.archived_source)
    if args.frontend and not (args.sdl_sdk.resolve() / 'lib/libSDL2.so').is_file():
        raise ValueError('Build the SDL link SDK first with make build-engines; it is never packaged')
    frontend_inputs = {path.name: digest(path) for path in FRONTEND.glob("*") if path.is_file()}
    tools = {name: find_tool(name, getattr(args, name)) for name in ("zig", "cmake", "ninja")}
    chain = write_toolchain(directory / "toolchain", tools["zig"], source)
    env = os.environ.copy()
    env.update(ZIG_GLOBAL_CACHE_DIR=str(ROOT / ".cache/zig"), ZIG_LOCAL_CACHE_DIR=str(ROOT / ".cache/zig-melonds"))
    for name in (
        "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPPFLAGS", "PKG_CONFIG_PATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
        "LIBRARY_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"
    ):
        env.pop(name, None)
    run(
        [
            tools["cmake"], "-S", FRONTEND, "-B", directory, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" +
            str(tools["ninja"]), "-DCMAKE_TOOLCHAIN_FILE=" + str(chain), "-DMELONDS_SOURCE=" + str(source),
            "-DRGDS_SDL_SDK=" + (str(args.sdl_sdk.resolve()) if args.frontend else ""),
            *["-D" + k + "=" + v for k, v in sorted(OPTIONS.items())]
        ], env
    )
    run(
        [
            tools["cmake"], "--build", directory, "--target", "rgds-melonds-probe",
            *(["melonds"] if args.frontend else []), "--parallel", args.jobs
        ], env
    )
    abi = validate_build(directory)
    if source_inputs(source, args.archived_source) != files or frontend_inputs != {
        p.name: digest(p)
        for p in FRONTEND.glob("*") if p.is_file()
    }:
        raise ValueError("Source changed during the melonDS build")
    archive_source(source, files, directory / "source.tar.gz")
    (directory / "COPYING").write_bytes((source / "LICENSE").read_bytes())
    record = {
        "schema": 1,
        "kind": "experimental-melonds-core-probe",
        "target": TARGET,
        "source": {
            "url": UPSTREAM,
            "commit": COMMIT,
            "tree": TREE,
            "inputs_sha256": INPUTS_SHA256,
            "archive_sha256": digest(directory / "source.tar.gz"),
            "unmodified": True
        },
        "frontend": {
            "test_only": True,
            "inputs": frontend_inputs
        },
        "binary": abi,
        "tools": {
            name: tool_record(path)
            for name, path in tools.items()
        },
        "options": OPTIONS,
        "driver_sha256": digest(Path(__file__)),
        "selectable_engine": False,
        "device_tested": False,
        "runtime_tested": False,
    }
    if args.frontend:
        engine = inspect_elf(directory / 'melonds', 'Pinned melonDS with original RGDS SDL frontend')
        if engine['machine'] != 'aarch64' or tuple(map(int, engine['glibc_minimum'].split('.'))) > (2, 17):
            raise ValueError('Invalid melonDS frontend ABI')
        record['engine'] = engine
        record['frontend']['test_only'] = False
    (directory / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    print("Built melonDS engine and core probe" if args.frontend else "Built melonDS core probe", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument(
        "--archived-source", action="store_true", help="Verify exact archived upstream bytes without Git metadata"
    )
    parser.add_argument("--frontend", action="store_true", help="Build the playable dual-screen SDL frontend too")
    parser.add_argument("--sdl-sdk", type=Path, default=ROOT / "build/engine-cross/sdl-sdk")
    parser.add_argument("--source", type=Path, default=ROOT / ".cache/melonds-source")
    parser.add_argument("--build", type=Path, default=ROOT / "build/melonds-experiment")
    parser.add_argument("--jobs", type=int, default=4)
    for name in ("zig", "cmake", "ninja"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("jobs must be between 1 and 8")
    build(args)


if __name__ == "__main__":
    main()
