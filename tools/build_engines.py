#!/usr/bin/env python3
"""Cross-build the pinned DSperate engine without accessing or running a device.

SDL2 is built only as a link-time SDK. Its dummy-driver library MUST NOT be
installed on a card; the engine uses the card's existing SDL2 at runtime.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import tarfile
import urllib.request

try:
    from .elf_abi import inspect_elf
    from . import build_engines_gcc as gcc
except ImportError:
    from elf_abi import inspect_elf
    import build_engines_gcc as gcc

ROOT = Path(__file__).resolve().parents[1]
COMMIT = "1b76c355109c9f7576363ccc023927b3137d3c6f"
TREE = "e8781d083ba9cbcf55e056a02d2b2f87d1a5e14e"
SOURCE_INPUTS_SHA256 = "c9863153f5eb63c236af478f188a30879add65daf1385a0607c4231ddabe9fa2"
PATCHED_INPUTS_SHA256 = "c8f4a7243832dd609cca4b0b33fed3120fadbf0a3106c0f8b497981c91ec9129"
UPSTREAM = "https://github.com/beebono/DSperate"
SDL_VERSION = "2.32.10"
SDL_ARCHIVE = f"SDL2-{SDL_VERSION}.tar.gz"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/{SDL_ARCHIVE}"
SDL_SHA256 = "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
TARGET = "aarch64-linux-gnu.2.17"
KIND = "source-built-dsperate"
PATCH = ROOT / "tools/engine_patches/dsperate-clang-function-pointer.patch"
PATCH_SHA256 = "f89b546dcabf821733b9b2c13881150007e1dd11a45ccafd50858eecf38bc780"
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


def git(source, *arguments):
    return subprocess.check_output(["git", "-C", str(source), *arguments], text=True).strip()


def source_inputs(source):
    """Reject a mismatching checkout and fingerprint all compiled source bytes."""
    if source.is_symlink() or git(source, "rev-parse", "HEAD") != COMMIT:
        raise ValueError("DSperate source must be the pinned commit " + COMMIT)
    if git(source, "rev-parse", "HEAD^{tree}") != TREE:
        raise ValueError("DSperate source tree does not match its pin")
    if git(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("DSperate source has changes; use an unmodified pinned checkout")
    names = subprocess.check_output(["git", "-C", str(source), "ls-files", "-z"]).decode().split("\0")
    files = {}
    for name in sorted(filter(None, names)):
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Unsupported upstream source file: " + name)
        files[name] = digest(path)
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    result = hashlib.sha256(encoded).hexdigest()
    if result != SOURCE_INPUTS_SHA256:
        raise ValueError("Pinned DSperate source bytes do not match their SHA-256 fingerprint")
    return result, files


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


def tool_record(path):
    version = subprocess.check_output([str(path), "version" if path.name == "zig" else "--version"], text=True).strip()
    return {"name": path.name, "version": version.splitlines()[0], "sha256": digest(path)}


def prepare_build_source(source, destination, inputs):
    """Apply the recorded Clang portability patch to an isolated checkout."""
    if digest(PATCH) != PATCH_SHA256:
        raise ValueError("DSperate portability patch checksum mismatch")
    edits = {
        "src/core/gpu/render3d.cpp": [
            (b"reinterpret_cast<GatherNFn>(sh.gather4)", b"reinterpret_cast<GatherNFn>(const_cast<void*>(sh.gather4))")
        ],
        "src/core/gpu/kernels_neon.cpp": [
            (b"vst1_u32(dst + x, uint32x2_t{c, cs})", b"vst1_u32(dst + x, (uint32x2_t{c, cs}))"),
            (b"vst1_u32(dst + x, uint32x2_t{cd, c})", b"vst1_u32(dst + x, (uint32x2_t{cd, c}))")
        ]
    }
    endian = b"""/* RGDS: libslirp's stream helpers need these GLib byte-order macros. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define GUINT16_FROM_BE(v) ((guint16)(v))
#define GUINT32_FROM_BE(v) ((guint32)(v))
#else
#define GUINT16_FROM_BE(v) ((guint16)__builtin_bswap16((guint16)(v)))
#define GUINT32_FROM_BE(v) ((guint32)__builtin_bswap32((guint32)(v)))
#endif
#define GUINT16_TO_BE(v) GUINT16_FROM_BE(v)
#define GUINT32_TO_BE(v) GUINT32_FROM_BE(v)
#define GINT16_FROM_BE(v) ((gint16)GUINT16_FROM_BE(v))
#define GINT16_TO_BE(v) GINT16_FROM_BE(v)
#define GINT32_FROM_BE(v) ((gint32)GUINT32_FROM_BE(v))
#define GINT32_TO_BE(v) GINT32_FROM_BE(v)

"""
    marker = b"#if !defined(_WIN32)\n"
    edits["src/net/slirp/glib/glib.h"] = [(marker, endian + marker)]
    # Count completed emulated frames, including frames intentionally not drawn.
    # Export the accessor so the HUD can distinguish speed from refresh.
    edits["src/frontend/sdl/main.cpp"] = [
        (
            b"// Nearest tag and short commit hash.\n",
            b"#include <cstdint>\nstatic std::atomic<uint32_t> rgds_completed_frames{0};\n"
            b'extern "C" uint32_t rgds_emulated_frames() { return rgds_completed_frames.load(std::memory_order_relaxed); }\n\n'
            b"// Nearest tag and short commit hash.\n"
        ),
        (
            b"    nds.run_frame();\n#if DSPERATE_CHEEVOS\n",
            b"    nds.run_frame();\n    rgds_completed_frames.fetch_add(1, std::memory_order_relaxed);\n#if DSPERATE_CHEEVOS\n"
        )
    ]
    originals, patched = {}, {}
    expected = dict(inputs)
    for name, replacements in edits.items():
        originals[name] = (source / name).read_bytes()
        data = originals[name]
        for before, after in replacements:
            if data.count(before) != 1:
                raise ValueError("Pinned upstream code does not match portability patch: " + name)
            data = data.replace(before, after)
        patched[name] = data
        expected[name] = hashlib.sha256(data).hexdigest()
    if not destination.exists():
        run(["git", "clone", "--shared", "--no-hardlinks", source, destination])
        run(["git", "-C", destination, "checkout", "--detach", COMMIT])
        run(["git", "-C", destination, "apply", "--unidiff-zero", PATCH])
    if git(destination, "rev-parse", "HEAD") != COMMIT:
        raise ValueError("Isolated engine build checkout has the wrong commit")
    names = set(
        filter(None,
               subprocess.check_output(["git", "-C", str(destination), "ls-files", "-z"]).decode().split("\0"))
    )
    if names != set(expected):
        raise ValueError("Isolated engine build checkout has an unexpected source inventory")
    for name, data in patched.items():
        path = destination / name
        if not path.is_symlink() and path.is_file() and path.read_bytes() == originals[name]:
            path.write_bytes(data)
    for name, checksum in expected.items():
        path = destination / name
        if path.is_symlink() or not path.is_file() or digest(path) != checksum:
            raise ValueError("Isolated engine build source differs from the recorded patch: " + name)
    patched_hash = hashlib.sha256(json.dumps(expected, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if patched_hash != PATCHED_INPUTS_SHA256:
        raise ValueError("Patched DSperate source bytes do not match their SHA-256 fingerprint")
    return patched_hash


def source_archive(source, destination, names):
    with destination.open("wb") as stream:
        with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                entries = [("DSperate/" + name, source / name) for name in sorted(names)]
                # Keep the exact build driver and patch beside full upstream
                # source so recipients can reproduce this modified build.
                entries += [
                    ("DSperate/tools/build_engines.py", Path(__file__)),
                    ("DSperate/tools/build_engines_gcc.py", ROOT / "tools/build_engines_gcc.py"),
                    ("DSperate/tools/elf_abi.py", ROOT / "tools/elf_abi.py"),
                    ("DSperate/tools/engine_patches/" + PATCH.name, PATCH)
                ]
                for name, path in entries:
                    data = path.read_bytes()
                    item = tarfile.TarInfo(name)
                    item.size = len(data)
                    item.mode = path.stat().st_mode & 0o777
                    archive.addfile(item, io.BytesIO(data))


def ensure_sources(source, sdk, fetch=False):
    archive = sdk / SDL_ARCHIVE
    if not source.exists() and fetch:
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--no-checkout", UPSTREAM + ".git", source])
        run(["git", "-C", source, "checkout", "--detach", COMMIT])
    if not source.is_dir():
        raise ValueError("Pinned DSperate checkout missing; use --fetch or --source")
    if not archive.is_file() and fetch:
        sdk.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix(".pending")
        try:
            urllib.request.urlretrieve(SDL_URL, temporary)
            if digest(temporary) != SDL_SHA256:
                raise ValueError("SDL source archive checksum mismatch")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if not archive.is_file() or digest(archive) != SDL_SHA256:
        raise ValueError("Pinned SDL source archive missing or checksum mismatch: " + str(archive))
    # Extract every build from the verified archive, never trust an old edited
    # SDK checkout. This archive's link/special-file members are not needed.
    original = sdk / f"SDL2-{SDL_VERSION}"
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
        if original.exists():
            shutil.rmtree(original)
        tar.extractall(sdk, members=members)
    return original


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
        for path, label in ((source, "/src/DSperate"), (sdl_source, "/src/SDL2"), (ROOT, "/rgds"))
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


def build_engine(args):
    source, sdk, output = args.source.resolve(), args.sdk.resolve(), args.output.resolve()
    compiler = getattr(args, "compiler", "zig")
    tools = {
        name: find_tool(name, getattr(args, name))
        for name in ("zig", "cmake", "ninja")
    } if compiler == "zig" else {}
    sdl_source = ensure_sources(source, sdk, args.fetch)
    source_hash, inputs = source_inputs(source)
    env = os.environ.copy()
    env.update(ZIG_GLOBAL_CACHE_DIR=str(ROOT / ".cache/zig"), ZIG_LOCAL_CACHE_DIR=str(ROOT / ".cache/zig-engines"))
    # Clear host package environment so macOS SDL headers cannot be selected.
    for name in (
        "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPPFLAGS", "PKG_CONFIG_PATH", "SDL2_DIR", "CPATH", "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"
    ):
        env.pop(name, None)
    scratch = ROOT / "build/engine-cross"
    scratch.mkdir(parents=True, exist_ok=True)
    modified_source = scratch / "source"
    patched_hash = prepare_build_source(source, modified_source, inputs)
    if compiler == gcc.COMPILER:
        return publish_gcc_engine(source, modified_source, output, inputs, source_hash, patched_hash)
    chain = toolchain(scratch / "toolchain", tools["zig"], modified_source, sdl_source)
    sdl_build, sdl_install = scratch / "sdl", scratch / "sdl-sdk"
    sdl_options = {**SDL_OPTIONS, "CMAKE_INSTALL_PREFIX": str(sdl_install)}
    configure(tools["cmake"], tools["ninja"], sdl_source, sdl_build, chain, sdl_options, env)
    run([tools["cmake"], "--build", sdl_build, "--parallel", args.jobs], env=env)
    run([tools["cmake"], "--install", sdl_build], env=env)
    engine_build = scratch / "dsperate"
    options = {
        **ENGINE_OPTIONS, "SDL2_DIR": str(sdl_install / "lib/cmake/SDL2"),
        "CMAKE_FIND_ROOT_PATH": str(sdl_install),
        "CMAKE_PREFIX_PATH": str(sdl_install)
    }
    # The original compile attempt used the unpatched checkout. CMake caches
    # the source root, so a transition to the isolated tree needs a fresh cache.
    cache = engine_build / "CMakeCache.txt"
    if cache.is_file() and f"CMAKE_HOME_DIRECTORY:INTERNAL={modified_source}" not in cache.read_text():
        shutil.rmtree(engine_build)
    configure(tools["cmake"], tools["ninja"], modified_source, engine_build, chain, options, env)
    run([tools["cmake"], "--build", engine_build, "--target", "dsperate", "--parallel", args.jobs], env=env)
    candidate = engine_build / "src/frontend/sdl/dsperate"
    abi = inspect_elf(candidate, "pinned upstream DSperate built from source")
    if not abi or abi["machine"] != "aarch64" or not abi["glibc_minimum"] or tuple(
        map(int, abi["glibc_minimum"].split("."))
    ) > (2, 17):
        raise ValueError("Source engine failed Linux AArch64 / GLIBC 2.17 ABI validation")
    if source_inputs(source)[0] != source_hash:
        raise ValueError("Upstream source changed during the build")
    if prepare_build_source(source, modified_source, inputs) != patched_hash:
        raise ValueError("Patched build source changed during the build")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = Path(tempfile.mkdtemp(prefix=".dsperate-", dir=output.parent))
    try:
        shutil.copy2(candidate, pending / "dsperate")
        shutil.copy2(source / "LICENSE", pending / "LICENSE")
        source_archive(modified_source, pending / "source.tar.gz", inputs)
        (pending / "NOTICE.txt").write_text(
            "DSperate 3.0.0; GPL-3.0-or-later (see LICENSE and complete source.tar.gz).\n"
            f"Upstream: {UPSTREAM}\nCommit: {COMMIT}\n"
            "RGDS changes: Clang callback/vector-initializer portability; missing GLib byte-order macros for libslirp; exported completed-emulation-frame counter.\n"
            "Built for RGDS on Linux AArch64 / Cortex-A55 with JIT, NEON and LTO.\n"
            "The SDL2 library is a build-time SDK only; the card supplies runtime SDL2.\n"
            "Hardware test scope and results are recorded separately in RGDS documentation.\n"
        )
        commands = json.loads((engine_build / "compile_commands.json").read_text())
        compile_options = sorted(
            {
                entry["command"].split(" -o ")[0].replace(str(modified_source),
                                                          "/src/DSperate").replace(str(ROOT), "/rgds")
                for entry in commands
            }
        )
        sdl_lib = next((sdl_install / "lib").glob("libSDL2-2.0.so.*.*.*"))
        headers = {
            path.relative_to(sdl_install / "include").as_posix(): digest(path)
            for path in sorted((sdl_install / "include").rglob("*.h"))
        }
        record = {
            "schema": 1,
            "kind": KIND,
            "binary": abi,
            "source": {
                "url": UPSTREAM,
                "commit": COMMIT,
                "tree": TREE,
                "inputs_sha256": source_hash,
                "patched_inputs_sha256": patched_hash,
                "patches": [{
                    "name": PATCH.name,
                    "sha256": digest(PATCH)
                }],
                "archive_sha256": digest(pending / "source.tar.gz")
            },
            "tools": {
                name: tool_record(path)
                for name, path in tools.items()
            },
            "build_driver_sha256": digest(Path(__file__)),
            "target": TARGET,
            "options": ENGINE_OPTIONS,
            "compile_options": compile_options,
            "source_file_count": len(inputs),
            "device_tested": False,
            "sdl_link_sdk": {
                "version": SDL_VERSION,
                "url": SDL_URL,
                "source_archive_sha256": SDL_SHA256,
                "library_sha256": digest(sdl_lib),
                "headers_sha256": hashlib.sha256(json.dumps(headers, sort_keys=True).encode()).hexdigest(),
                "options": SDL_OPTIONS,
                "packaged": False,
                "runtime": "Existing card libSDL2-2.0.so.0; ABI symbols still need device validation"
            },
            "files": {
                name: digest(pending / name)
                for name in ("dsperate", "LICENSE", "NOTICE.txt", "source.tar.gz")
            },
        }
        (pending / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        validate_build(pending)
        publish_build(pending, output)
    finally:
        if pending.exists():
            shutil.rmtree(pending)
    return output


def publish_gcc_engine(source, modified_source, output, inputs, source_hash, patched_hash):
    """Same verified, patched tree; GCC 13.3 with upstream's committed PGO profile."""
    options = gcc.gcc_options(ENGINE_OPTIONS)
    candidate, gcc_record = gcc.build(modified_source, "dsperate", options)
    abi = inspect_elf(candidate, "pinned upstream DSperate built from source")
    if not abi or abi["machine"] != "aarch64" or not gcc.glibc_allowed(abi, gcc.COMPILER):
        raise ValueError("GCC engine failed Linux AArch64 / GLIBC 2.38 ABI validation")
    if source_inputs(source)[0] != source_hash or prepare_build_source(source, modified_source, inputs) != patched_hash:
        raise ValueError("Engine source changed during the GCC build")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = Path(tempfile.mkdtemp(prefix=".dsperate-", dir=output.parent))
    try:
        shutil.copy2(candidate, pending / "dsperate")
        shutil.copy2(source / "LICENSE", pending / "LICENSE")
        source_archive(modified_source, pending / "source.tar.gz", inputs)
        (pending / "NOTICE.txt").write_text(
            "DSperate 3.0.0; GPL-3.0-or-later (see LICENSE and complete source.tar.gz).\n"
            f"Upstream: {UPSTREAM}\nCommit: {COMMIT}\n"
            "RGDS changes: Clang callback/vector-initializer portability; missing GLib byte-order macros for libslirp; exported completed-emulation-frame counter.\n"
            "Built for RGDS on Linux AArch64 with GCC 13.3, upstream's committed PGO profile, JIT and NEON (tools/build_engines_gcc.py).\n"
            "libstdc++/libgcc are linked statically; the card supplies runtime SDL2.\n"
            "Hardware test scope and results are recorded separately in RGDS documentation.\n"
        )
        record = {
            "schema": 1,
            "kind": KIND,
            "binary": abi,
            "compiler": gcc.COMPILER,
            "source": {
                "url": UPSTREAM,
                "commit": COMMIT,
                "tree": TREE,
                "inputs_sha256": source_hash,
                "patched_inputs_sha256": patched_hash,
                "patches": [{
                    "name": PATCH.name,
                    "sha256": digest(PATCH)
                }],
                "archive_sha256": digest(pending / "source.tar.gz")
            },
            "tools": {
                "gcc": gcc_record
            },
            "build_driver_sha256": digest(Path(__file__)),
            "target": "aarch64-linux-gnu (glibc >= 2.38)",
            "options": options,
            "source_file_count": len(inputs),
            "device_tested": False,
            "files": {
                name: digest(pending / name)
                for name in ("dsperate", "LICENSE", "NOTICE.txt", "source.tar.gz")
            },
        }
        (pending / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        validate_build(pending)
        publish_build(pending, output)
    finally:
        if pending.exists():
            shutil.rmtree(pending)
    return output


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


def validate_build(directory):
    """Verify an exact source-built artifact before a packager imports it."""
    directory = Path(directory)
    record = json.loads((directory / "build.json").read_text())
    expected = {"dsperate", "LICENSE", "NOTICE.txt", "source.tar.gz"}
    if (
        not isinstance(record, dict) or type(record.get("schema")) is not int or record.get("schema") != 1
        or record.get("kind") != KIND or not isinstance(record.get("source"), dict)
        or not isinstance(record.get("binary"), dict) or not isinstance(record.get("files"), dict)
        or record.get("source", {}).get("commit") != COMMIT or record.get("source", {}).get("tree") != TREE
        or record.get("source", {}).get("inputs_sha256") != SOURCE_INPUTS_SHA256
        or record.get("source", {}).get("patched_inputs_sha256") != PATCHED_INPUTS_SHA256
        or record.get("source", {}).get("patches") != [{
            "name": PATCH.name,
            "sha256": PATCH_SHA256
        }] or set(record.get("files", {})) != expected
    ):
        raise ValueError("Invalid source-built DSperate metadata")
    if directory.is_symlink() or {path.name for path in directory.iterdir()} != expected | {"build.json"}:
        raise ValueError("Unexpected source-built DSperate file inventory")
    for name, checksum in record["files"].items():
        path = directory / name
        if (
            not isinstance(checksum, str) or not re.fullmatch("[0-9a-f]{64}", checksum) or path.is_symlink()
            or not path.is_file() or digest(path) != checksum
        ):
            raise ValueError("Source-built DSperate artifact hash mismatch: " + name)
    abi = inspect_elf(directory / "dsperate", record["binary"].get("source"))
    compiler = record.get("compiler", "zig")
    if (
        compiler not in ("zig", gcc.COMPILER) or not abi or abi != record["binary"] or abi["machine"] != "aarch64"
        or not gcc.glibc_allowed(abi, compiler) or abi.get("runpaths")
    ):
        raise ValueError("Source-built DSperate ELF metadata mismatch")
    if record["source"].get("archive_sha256") != record["files"]["source.tar.gz"]:
        raise ValueError("Source-built DSperate source archive metadata mismatch")
    validate_source_archive(directory / "source.tar.gz", record)
    return record


def validate_source_archive(path, record):
    """The archive must contain the actual pinned-and-patched source bytes."""
    files = {}
    builder = None
    with tarfile.open(path) as archive:
        names = set()
        for member in archive.getmembers():
            relative = Path(member.name)
            if (
                relative.is_absolute() or ".." in relative.parts or not member.isfile()
                or not member.name.startswith("DSperate/") or member.name in names
            ):
                raise ValueError("Unsafe or duplicate DSperate source archive entry")
            names.add(member.name)
            content = archive.extractfile(member).read()
            name = member.name[len("DSperate/"):]
            if name in (
                "tools/build_engines.py", "tools/build_engines_gcc.py", "tools/elf_abi.py",
                "tools/engine_patches/" + PATCH.name
            ):
                if name == "tools/build_engines.py":
                    builder = hashlib.sha256(content).hexdigest()
                elif name == "tools/engine_patches/" + PATCH.name and hashlib.sha256(content
                                                                                     ).hexdigest() != PATCH_SHA256:
                    raise ValueError("DSperate source archive portability patch mismatch")
            else:
                files[name] = hashlib.sha256(content).hexdigest()
    fingerprint = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if (
        fingerprint != PATCHED_INPUTS_SHA256 or builder != record.get("build_driver_sha256")
        or len(files) != record.get("source_file_count")
    ):
        raise ValueError("DSperate source archive does not match the built source fingerprint")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", nargs="?", choices=("dsperate",), default="dsperate")
    parser.add_argument("--source", type=Path, default=ROOT / ".cache/dsperate-source")
    parser.add_argument("--sdk", type=Path, default=ROOT / ".cache/engine-sdk")
    parser.add_argument("--output", type=Path, default=ROOT / "build/engines/dsperate")
    parser.add_argument("--fetch", action="store_true", help="Download pinned upstream sources if missing")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    for name in ("zig", "cmake", "ninja"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument(
        "--compiler",
        choices=("zig", gcc.COMPILER),
        default="zig",
        help="gcc-pgo: GCC 13.3 + upstream's PGO profile in the Lima ARM64 VM (~8%% less emulation time)"
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        print(build_engine(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
