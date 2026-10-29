#!/usr/bin/env python3
"""Shared cross-build pieces for the engines RGDS compiles: DSCore, its Vulkan renderer and RG DS Play.

The zig toolchain wrappers for Linux AArch64 (GLIBC 2.17), CMake configuration,
the SDL2 link SDK, hashing and ELF checks. The SDK is a link-time stand-in only:
its dummy-driver library is never installed; the handheld supplies SDL2 at run time.

DSCore (dscore/) is a fork of DSperate; UPSTREAM/COMMIT/TREE record the pinned
upstream it started from, which its corresponding-source notice must name.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

try:
    from .elf_abi import inspect_elf
    from . import build_engines_gcc as gcc
except ImportError:
    from elf_abi import inspect_elf
    import build_engines_gcc as gcc

ROOT = Path(__file__).resolve().parents[1]
# The DSperate commit dscore/ was forked from (dscore/FORK.md must name it).
UPSTREAM = "https://github.com/beebono/DSperate"
COMMIT = "1b76c355109c9f7576363ccc023927b3137d3c6f"
TREE = "e8781d083ba9cbcf55e056a02d2b2f87d1a5e14e"
SDL_VERSION = "2.32.10"
SDL_ARCHIVE = f"SDL2-{SDL_VERSION}.tar.gz"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/{SDL_ARCHIVE}"
SDL_SHA256 = "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
SDL_CACHE = ROOT / ".cache/engine-sdk"
SDL_SOURCE = SDL_CACHE / f"SDL2-{SDL_VERSION}"
SDL_SDK = ROOT / "build/engine-cross/sdl-sdk"
TARGET = "aarch64-linux-gnu.2.17"
# DSCore keeps upstream's CMake option names (DSPERATE_*).
ENGINE_OPTIONS = {
    "CMAKE_BUILD_TYPE": "Release",
    "DSPERATE_VERSION": "3.0.0",
    "DSPERATE_CHEEVOS_VERSION": "3.0.0",
    "DSPERATE_JIT": "ON",
    # Zig's cc wrapper uses underscore CPU identifiers for -mtune.
    "DSPERATE_NEON": "ON",
    "DSPERATE_TUNE": "-mtune=cortex_a55",
    "DSPERATE_PGO": "OFF",
    "DSPERATE_HARDEN": "OFF",
    "DSPERATE_TESTS": "OFF",
    "DSPERATE_HEADLESS": "OFF",
    "DSPERATE_SDL": "ON",
    "DSPERATE_WAYLAND": "OFF",
    "DSPERATE_VULKAN": "OFF",
    "DSPERATE_CHEEVOS": "ON",
    "DSPERATE_NET": "ON",
    "CMAKE_EXE_LINKER_FLAGS": "-Wl,--strip-all -Wl,--export-dynamic",
    "CMAKE_SKIP_RPATH": "ON",
    "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "ON",
}
# A real, source-built SDL library supplies its public symbols to the linker.
# Native platform dependencies are intentionally absent from this link SDK.
SDL_OPTIONS = {
    "CMAKE_BUILD_TYPE": "Release",
    "SDL_SHARED": "ON",
    "SDL_STATIC": "OFF",
    "SDL_TEST": "OFF",
    "SDL_TESTS": "OFF",
    "SDL2_DISABLE_SDL2MAIN": "ON",
    "SDL_DUMMYVIDEO": "ON",
    "SDL_DUMMYAUDIO": "ON"
}
for _option in (
    "ALSA", "JACK", "ESD", "PIPEWIRE", "PULSEAUDIO", "ARTS", "NAS", "SNDIO", "FUSIONSOUND", "LIBSAMPLERATE", "OSS",
    "X11", "WAYLAND", "RPI", "VIVANTE", "KMSDRM", "OPENGL", "OPENGLES", "VULKAN", "DBUS", "IBUS", "LIBUDEV", "HIDAPI",
    "OFFSCREEN", "CCACHE", "RPATH"
):
    SDL_OPTIONS["SDL_" + _option] = "OFF"
# Host variables that could pull the build machine's headers or libraries into a cross build.
HOST_VARIABLES = (
    "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPPFLAGS", "PKG_CONFIG_PATH", "SDL2_DIR", "CPATH", "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"
)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def run(command, *, env=None, capture=False):
    print(shlex.join(str(value) for value in command), flush=True)
    return subprocess.run(
        [str(value) for value in command], cwd=ROOT, env=env, check=True, text=True, capture_output=capture
    )


def find_tool(name, explicit=None):
    value = str(explicit) if explicit else shutil.which(name)
    if not value and name in ("cmake", "ninja"):
        # Android Studio's bundled CMake works as a host tool, not an Android
        # target toolchain. No Android libraries or headers enter this build.
        candidates = sorted((Path.home() / "Library/Android/sdk/cmake").glob("*/bin/" + name))
        value = str(candidates[-1]) if candidates else None
    if not value:
        raise ValueError(name + " is required on the host")
    path = Path(value).resolve()
    if not path.is_file():
        raise ValueError("Build tool missing: " + str(path))
    return path


def toolchain(directory, zig, source, sdl_source):
    directory.mkdir(parents=True, exist_ok=True)
    for name, language in (("cc", "cc"), ("cxx", "c++"), ("ar", "ar"), ("ranlib", "ranlib")):
        arguments = [str(zig), language]
        if name in ("cc", "cxx"):
            arguments += ["-target", TARGET]
        path = directory / name
        path.write_text("#!/bin/sh\nexec " + shlex.join(arguments) + ' "$@"\n')
        path.chmod(0o755)
    flags = " ".join(
        f"-ffile-prefix-map={path}={label}"
        for path, label in ((source, "/src/engine"), (sdl_source, "/src/SDL2"), (ROOT, "/rgds"))
    )
    path = directory / "linux-aarch64.cmake"
    path.write_text(
        "\n".join(
            [
                'set(CMAKE_SYSTEM_NAME Linux)',
                'set(CMAKE_SYSTEM_PROCESSOR aarch64)',
                f'set(CMAKE_C_COMPILER "{directory / "cc"}")',
                f'set(CMAKE_CXX_COMPILER "{directory / "cxx"}")',
                f'set(CMAKE_AR "{directory / "ar"}")',
                f'set(CMAKE_RANLIB "{directory / "ranlib"}")',
                f'set(CMAKE_C_COMPILER_AR "{directory / "ar"}")',
                f'set(CMAKE_CXX_COMPILER_AR "{directory / "ar"}")',
                f'set(CMAKE_C_COMPILER_RANLIB "{directory / "ranlib"}")',
                f'set(CMAKE_CXX_COMPILER_RANLIB "{directory / "ranlib"}")',
                f'set(CMAKE_C_FLAGS_INIT "{flags}")',
                f'set(CMAKE_CXX_FLAGS_INIT "{flags}")',
                'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)',
                'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)',
                'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)',
                'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)',
            ]
        ) + "\n"
    )
    return path


def configure(cmake, ninja, source, build, toolchain_path, options, env):
    run(
        [
            cmake, "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + str(ninja),
            "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain_path),
            *["-D" + key + "=" + value for key, value in sorted(options.items())]
        ],
        env=env
    )


def cross_environment(local_cache):
    """The process environment for a zig cross build, without the host's compiler search paths."""
    env = {key: value for key, value in os.environ.items() if key not in HOST_VARIABLES}
    env.update(ZIG_GLOBAL_CACHE_DIR=str(ROOT / ".cache/zig"), ZIG_LOCAL_CACHE_DIR=str(ROOT / ".cache" / local_cache))
    return env


def sdl_source(fetch=False):
    """Extract the pinned SDL source fresh from its verified archive (downloading it with fetch)."""
    archive = SDL_CACHE / SDL_ARCHIVE
    if not archive.is_file() and fetch:
        SDL_CACHE.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix(".pending")
        try:
            urllib.request.urlretrieve(SDL_URL, temporary)
            if digest(temporary) != SDL_SHA256:
                raise ValueError("SDL source archive checksum mismatch")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if not archive.is_file() or digest(archive) != SDL_SHA256:
        raise ValueError("Pinned SDL source archive missing or checksum mismatch (use --fetch): " + str(archive))
    # Never trust an old, possibly edited extraction. Link/special-file members are not needed.
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            relative = Path(member.name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Unsupported SDL source archive member: " + member.name)
            if member.issym() or member.islnk():
                continue  # Android project links are unused by this Linux SDK.
            if not (member.isfile() or member.isdir()):
                raise ValueError("Unsupported SDL source archive member: " + member.name)
            members.append(member)
        if SDL_SOURCE.exists():
            shutil.rmtree(SDL_SOURCE)
        tar.extractall(SDL_CACHE, members=members)
    return SDL_SOURCE


def sdl_sdk(fetch=False, jobs=4):
    """The SDL2 link SDK the zig builds link against, built once from the pinned source."""
    if (SDL_SDK / "lib/cmake/SDL2").is_dir() and (SDL_SDK / "include/SDL2/SDL.h").is_file():
        return SDL_SDK
    tools = {name: find_tool(name) for name in ("zig", "cmake", "ninja")}
    source = sdl_source(fetch)
    scratch = ROOT / "build/engine-cross"
    env = cross_environment("zig-sdl")
    chain = toolchain(scratch / "toolchain", tools["zig"], source, source)
    build = scratch / "sdl"
    configure(tools["cmake"], tools["ninja"], source, build, chain, {**SDL_OPTIONS, "CMAKE_INSTALL_PREFIX": str(SDL_SDK)}, env)
    run([tools["cmake"], "--build", build, "--parallel", jobs], env=env)
    run([tools["cmake"], "--install", build], env=env)
    return SDL_SDK


def publish_build(pending, output):
    """Publish the verified directory, restoring the last build on failure."""
    previous = output.with_name(output.name + ".previous")
    moved_previous = False
    try:
        if output.exists():
            if previous.exists():
                shutil.rmtree(previous)
            output.rename(previous)
            moved_previous = True
        pending.rename(output)
    except OSError:
        if moved_previous and previous.exists() and not output.exists():
            previous.rename(output)
        raise


def main():
    parser = argparse.ArgumentParser(description="Prepare the SDL2 link SDK for the zig engine builds.")
    parser.add_argument("--fetch", action="store_true", help="download the pinned SDL source archive if missing")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    print(sdl_sdk(args.fetch, args.jobs))


if __name__ == "__main__":
    main()
