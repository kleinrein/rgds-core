#!/usr/bin/env python3
"""Build the music engine (src/music/engine.cpp) for this computer: build/host/rgds-music, beside the host launcher.

It links FFmpeg and SDL2 from the system (pkg-config), unlike the launcher, which loads SDL at run time. The handheld's
engine is built by Buildroot against the image's own FFmpeg (os/buildroot/package/rgds-music), not here.

  build_music.py [host]
"""
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = ['libavformat', 'libavcodec', 'libswresample', 'libswscale', 'libavutil', 'sdl2']


def flags(kind):
    return shlex.split(subprocess.check_output(['pkg-config', kind, *LIBRARIES], text=True))


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else 'host'
    if target != 'host':
        raise SystemExit('The handheld build is the Buildroot package rgds-music (make image).')
    output = ROOT / 'build/host/rgds-music'
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        'c++', '-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I',
        str(ROOT / 'src'), *flags('--cflags'),
        str(ROOT / 'src/music/engine.cpp'), '-o',
        str(output), *flags('--libs'), '-lpthread'
    ]
    subprocess.run(command, check=True)
    print(output)


if __name__ == '__main__':
    main()
