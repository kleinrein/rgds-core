#!/usr/bin/env python3
"""Build RG DS Play (play/): libmgba at a pinned commit, then the player.

  python3 tools/build_play.py host [--fetch]     build/play/host: rgds-play, play-shot, play-tests
  python3 tools/build_play.py device [--fetch]   build/play/device: rgds-play for the RG DS (aarch64, glibc 2.17)

The device build needs `make build-engines` once (it prepares the SDL link SDK).
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_engines as base  # noqa: E402  (find_tool, toolchain)

MGBA_URL = "https://github.com/mgba-emu/mgba.git"
MGBA_COMMIT = "143adf5aa2a60c6036810b0cd72445e13d74b0e6"  # mGBA master, 23 August 2026
MGBA_SOURCE = ROOT / ".cache/mgba-source"
SDL_SDK = ROOT / "build/engine-cross/sdl-sdk"

# A full libmgba with every frontend and optional library off. Not LIBMGBA_ONLY:
# that also turns zlib off, and .zip ROMs need it. The GB core stays on: Game
# Boy and Game Boy Color games play in the same player.
MGBA_OPTIONS = {
    "LIBMGBA_ONLY": "OFF",
    "BUILD_STATIC": "ON",
    "BUILD_SHARED": "OFF",
    "CMAKE_BUILD_TYPE": "Release",
    "BUILD_LTO": "OFF",
    "BUILD_SDL": "OFF",
    "BUILD_QT": "OFF",
    "BUILD_LIBRETRO": "OFF",
    "BUILD_GL": "OFF",
    "BUILD_GLES2": "OFF",
    "BUILD_GLES3": "OFF",
    "BUILD_PERF": "OFF",
    "BUILD_TEST": "OFF",
    "BUILD_SUITE": "OFF",
    "BUILD_CINEMA": "OFF",
    "BUILD_ROM_TEST": "OFF",
    "BUILD_UPDATER": "OFF",
    "BUILD_HEADLESS": "OFF",
    "BUILD_PYTHON": "OFF",
    "USE_EPOXY": "OFF",
    "ENABLE_DEBUGGERS": "OFF",
    "ENABLE_GDB_STUB": "OFF",
    "USE_EDITLINE": "OFF",
    "USE_SQLITE3": "OFF",
    "USE_ELF": "OFF",
    "USE_LZMA": "ON",
    "USE_ZLIB": "ON",
    "USE_PNG": "OFF",
    "USE_LIBZIP": "OFF",
    "USE_MINIZIP": "OFF",
    "USE_FFMPEG": "OFF",
    "USE_LUA": "OFF",
    "USE_JSON_C": "OFF",
    "USE_DISCORD_RPC": "OFF",
    "USE_FREETYPE": "OFF",
    "M_CORE_GB": "ON",
    "M_CORE_GBA": "ON",
    "ENABLE_SCRIPTING": "OFF",
    "DISABLE_DEPS": "OFF",
    "CMAKE_DISABLE_FIND_PACKAGE_ZLIB": "ON",
    "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "ON"
}

CORE_SOURCES = [
    "core/mgba_core.cpp", "core/libretro_core.cpp", "companion/profile.cpp", "companion/map_capture.cpp",
    "companion/companion.cpp", "companion/party.cpp", "companion/party_gb.cpp", "companion/view.cpp",
    "companion/automap.cpp", "companion/mosaic.cpp", "companion/grid.cpp", "companion/trace.cpp", "companion/board.cpp",
    "companion/battle_gb.cpp", "companion/dock.cpp", "ui/canvas.cpp", "ui/screens.cpp", "ui/frame.cpp", "ui/colour.cpp",
    "ui/png.cpp", "ui/look.cpp", "ui/kit.cpp", "ui/words.cpp", "app/storage.cpp", "app/session.cpp"
]
C_SOURCES = ["core/mgba_split.c", "core/mgba_gb.c"]  # mGBA's internal headers are C
PROGRAMS = {
    "rgds-play": ("frontend/main.cpp", True),
    "play-shot": ("tools/play_shot.cpp", False),
    "play-tests": ("../tests/play_tests.cpp", False),
    "play-lint": ("tools/play_lint.cpp", False),  # the companion rules (docs/COMPANION.md)
}


def run(command, **kwargs):
    print("+ " + shlex.join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def ensure_mgba(fetch):
    if not (MGBA_SOURCE / ".git").is_dir():
        if not fetch:
            raise SystemExit("mGBA source missing: run with --fetch")
        run(["git", "clone", "-q", MGBA_URL, MGBA_SOURCE])
    head = subprocess.run(["git", "-C", MGBA_SOURCE, "rev-parse", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    if head != MGBA_COMMIT:
        if not fetch and head:
            raise SystemExit(f"mGBA is at {head[:12]}, expected {MGBA_COMMIT[:12]}: run with --fetch")
        run(["git", "-C", MGBA_SOURCE, "fetch", "-q", "origin"])
        run(["git", "-C", MGBA_SOURCE, "checkout", "-q", MGBA_COMMIT])
    restore_zconf()
    dirty = subprocess.run(["git", "-C", MGBA_SOURCE, "status", "--porcelain"], capture_output=True, text=True).stdout
    if dirty.strip():
        raise SystemExit("The mGBA checkout has local changes; the player builds from the pinned commit as is")


def restore_zconf():
    """zlib's CMake renames zconf.h in the source tree when it builds out of tree; put it back."""
    moved = MGBA_SOURCE / "src/third-party/zlib/zconf.h.included"
    if moved.exists():
        moved.unlink()
        run(["git", "-C", MGBA_SOURCE, "checkout", "-q", "src/third-party/zlib/zconf.h"])


def build_mgba(target, out, env):
    cmake, ninja = base.find_tool("cmake"), base.find_tool("ninja")
    build = out / "mgba"
    options = dict(MGBA_OPTIONS)
    command = [cmake, "-S", MGBA_SOURCE, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + str(ninja)]
    if target == "device":
        chain = base.toolchain(out / "toolchain", base.find_tool("zig"), MGBA_SOURCE, MGBA_SOURCE)
        command.append("-DCMAKE_TOOLCHAIN_FILE=" + str(chain))
        options["CMAKE_C_FLAGS"] = "-mcpu=cortex_a55"
        # mGBA also asks pkg-config, which would find the build machine's zlib:
        # the handheld gets mGBA's bundled copy, linked in statically. (The Mac
        # keeps its own zlib; the bundled one no longer builds against its SDK.)
        options["PKG_CONFIG_EXECUTABLE"] = "/usr/bin/false"
    command += ["-D" + k + "=" + v for k, v in sorted(options.items())]
    run(command, env=env, stdout=subprocess.DEVNULL)
    targets = ["mgba"] + (["zlibstatic"] if "zlibstatic" in (build / "build.ninja").read_text() else [])
    for name in targets:
        run([cmake, "--build", build, "--target", name, "--parallel", str(os.cpu_count() or 4)], env=env)
    restore_zconf()
    ninja_file = (build / "build.ninja").read_text()
    defines = next(line for line in ninja_file.splitlines() if line.startswith("  DEFINES = "))
    return build, shlex.split(defines.split("=", 1)[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=("host", "device"))
    parser.add_argument("--fetch", action="store_true", help="clone or update mGBA to the pinned commit")
    parser.add_argument("--only", choices=sorted(PROGRAMS), action="append", help="build just these programs")
    args = parser.parse_args()
    ensure_mgba(args.fetch)
    out = ROOT / "build/play" / args.target
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / ".cache/zig")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / ".cache/zig-play")
    mgba, defines = build_mgba(args.target, out, env)
    zlib = mgba / "zlib/libz.a"

    if args.target == "device":
        if not (SDL_SDK / "include/SDL2/SDL.h").is_file():
            raise SystemExit("SDL link SDK missing: run `make build-engines` first")
        compiler = [base.find_tool("zig"), "c++", "-target", base.TARGET, "-mcpu=cortex_a55", "-O2", "-flto=thin"]
        c_compiler = [base.find_tool("zig"), "cc", "-target", base.TARGET, "-mcpu=cortex_a55", "-O2"]
        sdl = ["-I" + str(SDL_SDK / "include"), "-L" + str(SDL_SDK / "lib"), "-lSDL2"]
        system_libs = ["-lpthread", "-lm", "-ldl", "-Wl,--strip-all"]
        programs = ["rgds-play", "play-shot"]
    else:
        compiler = [os.environ.get("CXX", "clang++"), "-O2", "-g"]
        c_compiler = [os.environ.get("CC", "clang"), "-O2", "-g"]
        sdl_cflags = subprocess.run(["sdl2-config", "--cflags"], capture_output=True, text=True)
        sdl_libs = subprocess.run(["sdl2-config", "--libs"], capture_output=True, text=True)
        include = [f for f in shlex.split(sdl_cflags.stdout) if f.startswith("-I")]
        # Our sources include <SDL2/SDL.h>: add the parent of the SDL2 include directory.
        sdl = include + ["-I" + str(Path(f[2:]).parent) for f in include] + shlex.split(sdl_libs.stdout)
        system_libs = ["-lz"
                       ] + (["-framework", "CoreFoundation"] if sys.platform == "darwin" else ["-lpthread", "-ldl"])
        programs = list(PROGRAMS)
    if args.only:
        programs = [p for p in programs if p in args.only]
    common = compiler + [
        "-std=c++17", "-Wall", "-Wextra", "-Werror", *defines, "-I" + str(MGBA_SOURCE / "include"),
        "-I" + str(mgba / "include"), "-I" + str(ROOT / "play/src"), "-I" + str(ROOT / "src")
    ]
    # The device's bundled zlib, for picture frames (ui/png.cpp) and mGBA's own headers (crc32.h).
    zlib_include = ["-I" + str(mgba / "zlib"), "-I" +
                    str(MGBA_SOURCE / "src/third-party/zlib")] if zlib.exists() else []
    common += zlib_include
    sources = [ROOT / "play/src" / s for s in CORE_SOURCES]
    for c_source in C_SOURCES:
        obj = out / (Path(c_source).stem + ".o")
        run(
            c_compiler + [
                "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", *defines,
                "-I" + str(MGBA_SOURCE / "include"), "-I" + str(mgba / "include"), "-I" + str(ROOT / "play/src"),
                *zlib_include, "-c", ROOT / "play/src" / c_source, "-o", obj
            ],
            env=env
        )
        sources.append(obj)
    for name in programs:
        main_source, needs_sdl = PROGRAMS[name]
        target = out / name
        candidate = out / ("." + name + ".new")
        extra = []
        if name == "rgds-play" and args.target == "device":
            # The RG DS bridge (preloaded) finds the live graphics setter with dlsym.
            extra = ["-rdynamic"]  # keeps rgds_play_set_graphics in the dynamic symbol table
        if name == "play-tests":
            extra = [
                "-DPLAY_PROFILES=\"%s\"" % (ROOT / "play/profiles"),
                "-DPLAY_FONTS=\"%s\"" % (ROOT / "assets/fonts")
            ]
        run(
            common + extra + [*sources, ROOT / "play/src" / main_source, mgba / "libmgba.a"] +
            ([zlib] if zlib.exists() else []) + (sdl if needs_sdl else []) + system_libs + ["-o", candidate],
            env=env
        )
        os.replace(candidate, target)
        print("built", target.relative_to(ROOT))
    if args.target == "device" and "rgds-play" in programs:
        stage_payload(out)


# ---- The release payload: emulators/rgds-play in a package, like the other engines.
# The launcher headers the player shares (its appearance settings, theme
# colours and Pixel classic text, so the second screen follows Theme & display;
# the calibrated touch reader for the lower panel).
SHARED_HEADERS = [
    "src/launcher/settings.hpp", "src/launcher/model.hpp", "src/launcher/shaders.hpp", "src/launcher/view_motion.hpp",
    "src/launcher/accent.hpp", "src/launcher/pixel_text.hpp", "src/platform/shader_catalog.hpp",
    "src/platform/touch.hpp"
]
PAYLOAD_SOURCES = [
    "play", *SHARED_HEADERS, "tools/build_play.py", "tools/build_engines.py", "tools/elf_abi.py", "LICENSE"
]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def archive_source(output):
    """Corresponding source: RG DS Play as built and the pinned mGBA tree (MPL-2.0)."""
    entries = []
    for name in PAYLOAD_SOURCES:
        path = ROOT / name
        files = [path] if path.is_file() else sorted(
            p for p in path.rglob("*") if p.is_file() and "__pycache__" not in p.parts
        )
        entries += [("rgds/" + f.relative_to(ROOT).as_posix(), f) for f in files]
    listed = subprocess.run(["git", "-C", MGBA_SOURCE, "ls-files", "-z"], capture_output=True, check=True).stdout
    entries += [
        ("mgba/" + n, MGBA_SOURCE / n) for n in sorted(listed.decode().split("\0")) if n and (MGBA_SOURCE / n).is_file()
    ]
    with output.open("wb") as stream, gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w") as tar:
            for name, path in entries:
                info = tarfile.TarInfo(name)
                data = path.read_bytes()
                info.size, info.mtime, info.mode = len(data), 0, 0o644
                tar.addfile(info, io.BytesIO(data))


def stage_payload(out):
    payload = out / "payload"
    staging = out / ".payload.new"
    if staging.exists():
        shutil.rmtree(staging)
    (staging / "profiles").mkdir(parents=True)
    shutil.copy2(out / "rgds-play", staging / "rgds-play")
    for system in sorted(p for p in (ROOT / "play/profiles").iterdir() if p.is_dir()):
        (staging / "profiles" / system.name).mkdir()
        for profile in sorted(system.glob("*.ini")):
            shutil.copy2(profile, staging / "profiles" / system.name / profile.name)
    shutil.copy2(ROOT / "LICENSE", staging / "COPYING")
    shutil.copy2(MGBA_SOURCE / "LICENSE", staging / "COPYING.mgba")
    archive_source(staging / "source.tar.gz")
    sys.path.insert(0, str(ROOT / "tools"))
    from elf_abi import inspect_elf
    files = {p.relative_to(staging).as_posix(): digest(p) for p in sorted(staging.rglob("*")) if p.is_file()}
    record = {
        "schema": 1,
        "engine": inspect_elf(staging / "rgds-play", "RG DS Play with pinned libmgba"),
        "mgba_commit": MGBA_COMMIT,
        "driver_sha256": digest(Path(__file__)),
        "files": files
    }
    (staging / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    validate_payload(staging)
    if payload.exists():
        shutil.rmtree(payload)
    staging.rename(payload)
    print("staged", payload.relative_to(ROOT), "(" + str(len(files)) + " files)")


def validate_payload(directory):
    """The staged emulators/rgds-play: every file as recorded, an AArch64 glibc-2.17 binary, its source inside."""
    directory = Path(directory)
    record = json.loads((directory / "build.json").read_text())
    if record.get("schema") != 1 or record.get("mgba_commit") != MGBA_COMMIT:
        raise ValueError("RG DS Play build record differs from the pinned mGBA; rebuild it")
    on_disk = {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file()} - {"build.json"}
    if on_disk != set(record.get("files", {})):
        raise ValueError("RG DS Play payload files differ from its build record")
    for name, checksum in record["files"].items():
        if digest(directory / name) != checksum:
            raise ValueError("RG DS Play payload changed after its build: " + name)
    engine = record.get("engine", {})
    if engine.get("machine") != "aarch64" or engine.get("interpreter") != "/lib/ld-linux-aarch64.so.1" or tuple(
        map(int,
            engine.get("glibc_minimum", "9.9").split("."))
    ) > (2, 17):
        raise ValueError("RG DS Play is not an AArch64 glibc-2.17 binary")
    with tarfile.open(directory / "source.tar.gz") as source:
        names = set(source.getnames())
        for needed in ("rgds/play/src/frontend/main.cpp", "rgds/tools/build_play.py", "mgba/LICENSE"):
            if needed not in names:
                raise ValueError("RG DS Play source archive is incomplete: " + needed)
        if source.extractfile("mgba/LICENSE").read() != (directory / "COPYING.mgba").read_bytes():
            raise ValueError("mGBA license mismatch")
    return record


if __name__ == "__main__":
    main()
