#!/usr/bin/env python3
"""Build RG DS Media (play/src/media): videos and books on both screens.

  python3 tools/build_media.py host [--only NAME]   build/media/host: rgds-media, media-tests

It links FFmpeg (8.x: avformat, avcodec, avfilter, swscale, swresample), libass, MuPDF and SDL2 from the system.
On a Mac: brew install ffmpeg libass mupdf sdl2. The handheld build is the Buildroot package rgds-media
(os/buildroot/package/rgds-media), against the image's own libraries.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "media/video.cpp", "media/book.cpp", "media/library.cpp", "media/gpu.cpp", "media/shared.cpp",
    "media/library_screen.cpp", "media/watch_screen.cpp", "media/read_screen.cpp", "ui/canvas.cpp", "ui/look.cpp",
    "ui/kit.cpp", "ui/words.cpp", "ui/png.cpp", "reader/pages.cpp", "third_party/stb_image.cpp"
]
PROGRAMS = {
    "rgds-media": "media/main.cpp",
    "media-tests": "../../tests/media_tests.cpp",
}
PKG = ["libavformat", "libavcodec", "libavfilter", "libswscale", "libswresample", "libavutil", "libass", "sdl2"]


def pkg(kind):
    return shlex.split(subprocess.check_output(["pkg-config", kind, *PKG], text=True))


def mupdf_flags():
    """MuPDF has no pkg-config file in Homebrew; Debian-style systems have one."""
    try:
        return (
            shlex.split(
                subprocess.check_output(["pkg-config", "--cflags", "mupdf"], text=True, stderr=subprocess.DEVNULL)
            ),
            shlex.split(
                subprocess.check_output(["pkg-config", "--libs", "mupdf"], text=True, stderr=subprocess.DEVNULL)
            )
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    for prefix in (Path("/opt/homebrew/opt/mupdf"), Path("/usr/local/opt/mupdf"), Path("/usr")):
        if (prefix / "include/mupdf/fitz.h").is_file():
            libs = ["-L" + str(prefix / "lib"), "-lmupdf"]
            if (prefix / "lib/libmupdf-third.dylib").exists() or (prefix / "lib/libmupdf-third.a").exists():
                libs.append("-lmupdf-third")
            return ["-I" + str(prefix / "include")], libs
    raise SystemExit("MuPDF not found: brew install mupdf")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=("host",))
    parser.add_argument("--only", choices=sorted(PROGRAMS), action="append")
    args = parser.parse_args()
    out = ROOT / "build/media" / args.target
    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX", "clang++")
    mu_cflags, mu_libs = mupdf_flags()
    sdl_inc = [f for f in pkg("--cflags") if f.startswith("-I") and f.endswith("SDL2")]
    cflags = [
        "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "play/src"),
        "-I" + str(ROOT / "src"), *pkg("--cflags"), *mu_cflags
    ]
    cflags += ["-I" + str(Path(f[2:]).parent) for f in sdl_inc]
    libs = pkg("--libs") + mu_libs + ["-lz", "-lpthread"]
    # Objects are rebuilt only when their source or a header in play/src/media or play/src/ui changed.
    headers = max(
        (
            p.stat().st_mtime for d in ("play/src/media", "play/src/ui", "play/src/reader")
            for p in (ROOT / d).glob("*.hpp")
        ),
        default=0
    )
    objects = []
    for source in SOURCES:
        src = ROOT / "play/src" / source
        obj = obj_dir / (source.replace("/", "_").replace(".cpp", ".o"))
        if not obj.exists() or obj.stat().st_mtime < max(src.stat().st_mtime, headers):
            extra = ["-Wno-unused-parameter", "-Wno-missing-field-initializers"] if "third_party" in source else []
            subprocess.run([cxx, *cflags, *extra, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    for name, main_source in PROGRAMS.items():
        if args.only and name not in args.only:
            continue
        defines = []
        if name == "media-tests":
            defines = ['-DMEDIA_FONTS="%s"' % (ROOT / "assets/fonts")]
        target = out / name
        tmp = out / ("." + name + ".new")
        subprocess.run(
            [cxx, *cflags, *defines, *objects,
             str(ROOT / "play/src" / main_source), *libs, "-o",
             str(tmp)], check=True
        )
        os.replace(tmp, target)
        print("built", target.relative_to(ROOT))


if __name__ == "__main__":
    main()
