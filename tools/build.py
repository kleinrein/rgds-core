#!/usr/bin/env python3
"""Build with installed SDL2 headers; the application loads SDL at runtime."""
import os
import fcntl
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE_EXPORTS = {"SDL_Init", "SDL_IsGameController", "SDL_RenderCopy", "SDL_RenderCopyEx", "SDL_RenderPresent", "SDL_GL_SwapWindow",
                  "SDL_PollEvent", "SDL_OpenAudioDevice", "SDL_PauseAudioDevice", "SDL_JoystickGetButton",
                  "SDL_GameControllerGetButton", "SDL_GetKeyboardState", "SDL_PeepEvents",
                  "glTexImage2D", "glDrawArrays", "glDeleteTextures", "glDeleteProgram",
                  "glBufferData", "glBufferSubData", "glDeleteBuffers",
                  "read", "ioctl", "kill", "snd_pcm_open", "pthread_create", "pthread_cond_init", "rgds_present_canvas"}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_snapshot():
    paths = [ROOT / "tools/build.py", ROOT / "tools/bridge.exports"]
    paths += [path for path in (ROOT / "src").rglob("*") if path.suffix in (".cpp", ".hpp", ".h")]
    return {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(paths) if path.is_file()}


def validate_build(directory):
    directory = Path(directory)
    try: record = json.loads((directory / "build.json").read_text())
    except (OSError, ValueError) as error: raise ValueError("Missing/invalid RGDS build record; rebuild the device target") from error
    if record.get("schema") != 1 or record.get("target") != "aarch64-linux-gnu.2.17" or record.get("sources") != source_snapshot():
        raise ValueError("RGDS sources changed since the device build; rebuild before packaging")
    expected = {"rgds-launcher", "librgdsbridge.so"}
    if set(record.get("artifacts", {})) != expected:
        raise ValueError("Incomplete RGDS build record")
    for name, info in record["artifacts"].items():
        if digest(directory / name) != info.get("sha256"):
            raise ValueError("RGDS build changed after compilation: " + name)
    return record


def publish(candidates, record, directory):
    """Restore the previous pair if publication fails after the first rename."""
    pending = directory / ".build.json.pending"
    backups = {}
    try:
        pending.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        replacements = dict(candidates)
        replacements[directory / "build.json"] = pending
        for destination in replacements:
            if destination.exists():
                fd, name = tempfile.mkstemp(prefix=".rollback-", dir=directory); os.close(fd)
                backup = Path(name); backups[destination] = backup
                shutil.copy2(destination, backup)
            else: backups[destination] = None
        try:
            for destination, candidate in replacements.items(): os.replace(candidate, destination)
        except BaseException:
            for destination, backup in backups.items():
                if backup is None: destination.unlink(missing_ok=True)
                else: os.replace(backup, destination)
            raise
    finally:
        pending.unlink(missing_ok=True)
        for backup in backups.values():
            if backup is not None: backup.unlink(missing_ok=True)


def build(target):
    if not shutil.which("sdl2-config"):
        raise SystemExit("SDL2 development headers required. Install SDL2 or provide sdl2-config on PATH.")
    includes = [flag for flag in shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True)) if flag.startswith("-I")]
    output = ROOT / "build" / target / "rgds-launcher"
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [os.environ.get("CXX", "clang++"), "-std=c++17", "-O3", "-Wall", "-Wextra", "-Werror"]
    env = os.environ.copy()
    if target == "device":
        if not shutil.which("zig"):
            raise SystemExit("Zig is required for the initial AArch64 cross build.")
        # Zig supplies its static libc++ runtime itself.
        cmd = ["zig", "c++", "-target", "aarch64-linux-gnu.2.17", "-mcpu=cortex_a55", "-std=c++17", "-O3", "-flto", "-Wall", "-Wextra", "-Werror", "-Wl,--strip-all", "-ldl"]
        env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / ".cache" / "zig")
        env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / ".cache" / "zig-local")
    elif sys.platform != "darwin":
        cmd += ["-ldl"]
    # Build the pair before publishing either artifact. A bridge compilation
    # failure cannot replace the launcher with a mismatched new version.
    sources = source_snapshot()
    candidates = {}
    commands = {}
    print(f"Building {target}: {output.relative_to(ROOT)}", flush=True)
    try:
        jobs = [(output, "src/launcher/main.cpp", [])]
        if target == "device":
            jobs.append((output.parent / "librgdsbridge.so", "src/platform/emulator_bridge.cpp",
                         ["-shared", "-fPIC", "-Wl,--version-script=" + str(ROOT / "tools/bridge.exports")]))
        for destination, source, extra in jobs:
            fd, candidate = tempfile.mkstemp(prefix=".launcher-" if destination == output else ".bridge-", dir=output.parent)
            os.close(fd); candidates[destination] = Path(candidate)
            command = cmd + extra + includes + ["-Isrc", source, "-o", candidate]
            subprocess.run(command, cwd=ROOT, env=env, check=True)
            commands[destination.name] = command[:-1] + [destination.name]
        if source_snapshot() != sources:
            raise ValueError("Sources changed during compilation; rebuild once edits settle")
        record = {"schema": 1, "target": "aarch64-linux-gnu.2.17" if target == "device" else "host",
                  "sources": sources, "commands": commands, "artifacts": {}}
        if target == "device":
            if __package__: from .elf_abi import inspect_elf
            else: from elf_abi import inspect_elf
            for destination, candidate in candidates.items():
                info = inspect_elf(candidate, source="RGDS source build", symbols=destination.name.endswith(".so"))
                if info["machine"] != "aarch64" or info["glibc_minimum"] != "2.17":
                    raise ValueError("Unexpected device build ABI: " + destination.name)
                if "exports" in info and any(not name.startswith(("SDL_", "gl")) and name not in ("ioctl", "read", "kill", "snd_pcm_open", "pthread_create", "pthread_cond_init", "rgds_present_canvas") for name in info["exports"]):
                    raise ValueError("The bridge exports private runtime symbols")
                if destination.name.endswith(".so") and not BRIDGE_EXPORTS.issubset(info.get("exports", [])):
                    raise ValueError("The bridge is missing required emulator hooks")
                record["artifacts"][destination.name] = info
            record["compiler"] = subprocess.check_output(["zig", "version"], text=True).strip()
        else:
            record["artifacts"] = {path.name: {"sha256": digest(candidate)} for path, candidate in candidates.items()}
        publish(candidates, record, output.parent)
    finally:
        for candidate in candidates.values(): candidate.unlink(missing_ok=True)
    print("Device ABI target: Linux AArch64 / glibc 2.17; vendor SDL2 compatibility still requires the card." if target == "device" else "Host build ready.")


def main():
    target = sys.argv[1] if len(sys.argv) == 2 else ""
    if target not in ("host", "device"):
        raise SystemExit("Usage: tools/build.py host|device")
    lock = ROOT / ".cache/build.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    with lock.open("a") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        build(target)

if __name__ == "__main__":
    main()
