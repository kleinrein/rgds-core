#!/usr/bin/env python3
"""Build a verified DSperate source tree with GCC 13.3 and upstream's PGO profile.

Upstream DSperate's releases are GCC 13.3 builds that apply the profile
committed under pgo/aarch64 (made by upstream's tools/pgo_refresh.sh from
real game scenes). The profile only applies for one compiler version and one
set of compile flags, which its MANIFEST fingerprints. Zig/Clang cannot use it.

Measured on the standard RG DS (Mario Kart DS race checkpoint, 2×40 s each,
13 October 2026): mean emulation time per frame 7.61/7.64 ms (Zig, no PGO) →
7.03/7.03 ms (GCC + PGO) for DSperate, 7.39 → 7.08 ms for RG DS Experimental.

The build runs in the project's Lima ARM64 VM (Ubuntu 24.04, GCC 13.3, glibc
2.39). The result needs glibc 2.38; stock RG DS Linux has 2.41. libstdc++ and
libgcc are linked statically; SDL2 still comes from the card at runtime.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LIMA = ROOT / ".cache/linux-builder/lima/bin/limactl"
LIMA_HOME = ROOT / ".cache/linux-builder/state"
VM = "rgds-image"
COMPILER = "gcc-pgo"
GCC_VERSION = "13.3.0"
GLIBC_CEILING = (2, 38)
# Flags the committed profile's MANIFEST fingerprint was made with: build
# type, no extra tune, JIT, NEON, no hardening. Feature switches (network,
# achievements, Vulkan, Wayland, headless) do not enter the fingerprint.
PGO_OPTIONS = {"CMAKE_BUILD_TYPE": "RelWithDebInfo", "DSPERATE_TUNE": "", "DSPERATE_PGO": "use",
               "DSPERATE_PGO_STRICT": "OFF", "DSPERATE_JIT": "ON", "DSPERATE_NEON": "ON", "DSPERATE_HARDEN": "OFF",
               # The RGDS bridge resolves exported engine symbols with dlsym.
               "CMAKE_EXE_LINKER_FLAGS": "-static-libstdc++ -static-libgcc -Wl,--export-dynamic",
               "CMAKE_SKIP_RPATH": "ON"}


def gcc_options(base):
    """Keep every feature switch of the Zig build; change only code generation."""
    options = {key: value for key, value in base.items() if key not in ("CMAKE_EXE_LINKER_FLAGS", "DSPERATE_TUNE", "DSPERATE_PGO", "CMAKE_BUILD_TYPE")}
    options.update(PGO_OPTIONS)
    return options


def available():
    return LIMA.is_file() and (LIMA_HOME / VM).is_dir()


def vm(script, capture=False):
    env = dict(os.environ, LIMA_HOME=str(LIMA_HOME))
    return subprocess.run([str(LIMA), "shell", VM, "--", "bash", "-c", script], env=env, check=True,
                          text=True, capture_output=capture)


def build(source, name, options, target="dsperate"):
    """Compile `source` (already verified and patched) in the VM.

    Returns the stripped executable's host path and a build record. The
    source tree is copied into VM-local storage; the PGO profile is the one
    tracked in that same verified tree, so it is part of the corresponding
    source archive the caller already ships.
    """
    source = Path(source).resolve()
    if not available():
        raise ValueError("The Lima ARM64 builder is missing; use --compiler zig or provision .cache/linux-builder")
    if not (source / "pgo/aarch64/MANIFEST").is_file():
        raise ValueError("The verified source has no aarch64 PGO profile")
    output = ROOT / "build/engine-gcc" / name
    output.mkdir(parents=True, exist_ok=True)
    binary = output / target
    binary.unlink(missing_ok=True)
    work = "$HOME/rgds-gcc/" + name
    defines = " ".join(shlex.quote("-D" + key + "=" + value) for key, value in sorted(options.items()))
    script = f"""set -euo pipefail
test "$(gcc -dumpfullversion)" = {GCC_VERSION}
rm -rf {work} && mkdir -p {work}
cp -a {shlex.quote(str(source))} {work}/src
cmake -S {work}/src -B {work}/build -G Ninja {defines} > {work}/configure.log
cmake --build {work}/build --target {target} -j4 > {work}/build.log 2>&1 || {{ tail -40 {work}/build.log; exit 1; }}
grep -c "control flow of function" {work}/build.log > {work}/mismatches || true
strip -o {shlex.quote(str(binary))} {work}/build/src/frontend/sdl/{target}
cat {work}/build/pgo-fingerprint
cat {work}/mismatches
gcc -dumpfullversion
cmake --version | head -1
"""
    lines = vm(script, capture=True).stdout.split()
    fingerprint, mismatches = lines[0], int(lines[1]) if lines[1].isdigit() else 0
    manifest = (source / "pgo/aarch64/MANIFEST").read_text()
    if "fingerprint " + fingerprint not in manifest:
        raise ValueError("GCC build flags no longer match the PGO profile fingerprint")
    if not binary.is_file():
        raise ValueError("GCC engine build produced no executable")
    record = {"compiler": COMPILER, "gcc": GCC_VERSION, "builder": "Lima " + VM + " (Ubuntu 24.04 ARM64)",
              "pgo_manifest_sha256": hashlib.sha256(manifest.encode()).hexdigest(), "pgo_fingerprint": fingerprint,
              "pgo_control_flow_mismatches": mismatches, "options": options,
              "builder_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    (output / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return binary, record


def glibc_allowed(abi, compiler):
    if not abi or not abi.get("glibc_minimum"):
        return False
    version = tuple(map(int, abi["glibc_minimum"].split(".")))
    return version <= (GLIBC_CEILING if compiler == COMPILER else (2, 17))
