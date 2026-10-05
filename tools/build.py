#!/usr/bin/env python3
"""Build with installed SDL2 headers; the application loads SDL at runtime."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    target = sys.argv[1] if len(sys.argv) == 2 else ""
    if target not in ("host", "device"):
        raise SystemExit("Usage: tools/build.py host|device")
    if not shutil.which("sdl2-config"):
        raise SystemExit("SDL2 development headers required. Install SDL2 or provide sdl2-config on PATH.")
    includes = [flag for flag in shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True)) if flag.startswith("-I")]
    output = ROOT / "build" / target / "rgds-launcher"
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [os.environ.get("CXX", "clang++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
    env = os.environ.copy()
    if target == "device":
        if not shutil.which("zig"):
            raise SystemExit("Zig is required for the initial AArch64 cross build.")
        # Zig supplies its static libc++ runtime itself.
        cmd = ["zig", "c++", "-target", "aarch64-linux-gnu.2.17", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-ldl"]
        env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / ".cache" / "zig")
        env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / ".cache" / "zig-local")
    elif sys.platform != "darwin":
        cmd += ["-ldl"]
    # A failed compilation must leave the running/last successful build intact.
    fd, candidate = tempfile.mkstemp(prefix=".launcher-", dir=output.parent)
    os.close(fd)
    cmd += includes + ["-Isrc", "src/launcher/main.cpp", "-o", candidate]
    print(f"Building {target}: {output.relative_to(ROOT)}", flush=True)
    try:
        subprocess.run(cmd, cwd=ROOT, env=env, check=True)
        os.replace(candidate, output)
    finally:
        Path(candidate).unlink(missing_ok=True)
    print("Device ABI target: Linux AArch64 / glibc 2.17; vendor SDL2 compatibility still requires the card." if target == "device" else "Host build ready.")

if __name__ == "__main__":
    main()
