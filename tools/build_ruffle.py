#!/usr/bin/env python3
"""Build the Flash core (play/ruffle, Ruffle behind libretro) for the RG DS on this machine.

    python3 tools/build_ruffle.py           build/ruffle/ruffle_libretro.so (aarch64, glibc 2.17, Cortex-A55)
    python3 tools/build_ruffle.py --host    the same for this computer (a Mac's .dylib), for play-shot

Cargo cross-compiles with zig as the linker, as tools/build.py builds the launcher, so neither Buildroot's Rust (too
old for Ruffle 0.8.0) nor the build VM's disk is needed: os/buildroot/package/libretro-ruffle installs the result.
The Rust version is play/ruffle/rust-toolchain.toml's (rustup installs it and the aarch64 target when missing).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CRATE = ROOT / "play/ruffle"
OUT = ROOT / "build/ruffle"
TARGET_DIR = ROOT / ".cache/ruffle-target"
TARGET = "aarch64-unknown-linux-gnu"
ZIG_TARGET = "aarch64-linux-gnu.2.17"


def toolchain():
    for line in (CRATE / "rust-toolchain.toml").read_text().splitlines():
        if line.strip().startswith("channel"):
            return line.split("=", 1)[1].strip().strip('"')
    raise SystemExit("play/ruffle/rust-toolchain.toml names no channel")


def linker_wrapper():
    """zig cc as Cargo's linker: Rust's arguments for a GNU target, with the ones zig's driver refuses dropped."""
    wrapper = ROOT / ".cache/ruffle-zig-cc"
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    wrapper.write_text(
        "#!/bin/sh\n"
        "# Written by tools/build_ruffle.py\n"
        "args=''\n"
        "for a in \"$@\"; do\n"
        "  case \"$a\" in\n"
        "    --target=*|-lgcc_s|-Wl,--as-needed|-Wl,-Bdynamic|-Wl,-Bstatic|-Wl,--fix-cortex-a53-843419|-Wl,--no-undefined-version) ;;\n"
        "    *) args=\"$args '$(printf %s \"$a\" | sed \"s/'/'\\\\\\\\''/g\")'\" ;;\n"
        "  esac\n"
        "done\n"
        f"eval exec zig cc -target {ZIG_TARGET} -mcpu=cortex_a55 -lunwind $args\n"
    )
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return wrapper


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", action="store_true", help="build for this computer instead of the handheld")
    args = parser.parse_args()
    channel = toolchain()
    env = os.environ.copy()
    env["CARGO_TARGET_DIR"] = str(TARGET_DIR)
    env["RUSTUP_TOOLCHAIN"] = channel
    if args.host:
        subprocess.run(["cargo", "build", "--release", "--lib"], cwd=CRATE, env=env, check=True)
        print(TARGET_DIR / "release")
        return 0
    if not shutil.which("zig"): raise SystemExit("zig is needed as the cross linker (brew install zig).")
    subprocess.run(["rustup", "target", "add", "--toolchain", channel, TARGET], check=True)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / ".cache/zig")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / ".cache/zig-local")
    env["CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER"] = str(linker_wrapper())
    env["RUSTFLAGS"] = "-C target-cpu=cortex-a55"
    subprocess.run(["cargo", "build", "--release", "--lib", "--target", TARGET], cwd=CRATE, env=env, check=True)
    built = TARGET_DIR / TARGET / "release/libruffle_libretro.so"
    OUT.mkdir(parents=True, exist_ok=True)
    result = OUT / "ruffle_libretro.so"
    shutil.copyfile(built, result)
    digest = hashlib.sha256(result.read_bytes()).hexdigest()
    sources = sorted(p for p in CRATE.rglob("*") if p.is_file() and "target" not in p.parts)
    record = {
        "toolchain": channel,
        "target": TARGET,
        "linker": "zig cc -target " + ZIG_TARGET,
        "sha256": digest,
        "size": result.stat().st_size,
        "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    }
    (OUT / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    print("built %s (%.1f MB, sha256 %s)" % (result.relative_to(ROOT), result.stat().st_size / 1e6, digest[:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
