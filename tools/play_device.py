#!/usr/bin/env python3
"""Hand-play RG DS Play on the handheld, before it is part of a release.

  python3 tools/play_device.py deploy [--device root@anbernic.local]
  python3 tools/play_device.py run "/mnt/mmc/Roms/GBA/Game.gba" [--device ...]
  python3 tools/play_device.py status | stop | log
  python3 tools/play_device.py install [--device ...]    RG DS Core with RG DS Play, as a test release
  python3 tools/play_device.py rollback [--device ...]   back to the installed release it was made from

Everything lives in /mnt/mmc/rgds-play (the player, profiles, fonts, saves in
data/). The installed RG DS Core release is only read: the runner borrows its
engine environment (Weston socket, sound setup, SDL) and its mixer-safe pause.
While a game runs the launcher (or the stock menu) is paused outside the sound
mixer's critical section, and resumed when the player exits, however it exits.
The player grabs the RG DS buttons, so nothing is queued for the launcher.
Home, or Select+Start held for a second, saves and quits.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REMOTE = "/mnt/mmc/rgds-play"

# Runs on the handheld (python3 -B). Kept here so the tool is one file.
RUNNER = r'''
import os, signal, subprocess, sys, time
from pathlib import Path
sys.dont_write_bytecode = True  # the release must stay byte-identical to its manifest
HERE = Path(__file__).resolve().parent
RGDS = Path("/mnt/mmc/rgds")
app = RGDS / "releases" / (RGDS / "state/active").read_text().strip()
sys.path.insert(0, str(app / "runtime"))
import session  # read only: environment() and FRONTENDS
from shared_audio import stop_outside_mixer

ENGINES = ("dsperate", "rgds-experiment", "drastic", "melonDS", "melonds", "retroarch", "play-shot", "rgds-play", "ppsspp")

def processes():
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit(): continue
        try:
            yield int(entry.name), (entry / "comm").read_text().strip(), (entry / "stat").read_text().rsplit(")", 1)[1].split()[0]
        except (OSError, IndexError):
            continue

class Pid:
    def __init__(self, pid): self.pid = pid
    def send_signal(self, sig): os.kill(self.pid, sig)

rom = Path(sys.argv[1])
if not rom.is_file(): sys.exit("No such game: %s" % rom)
busy = [(pid, name) for pid, name, state in processes() if any(name.startswith(e) for e in ENGINES)]
if busy: sys.exit("Another game or benchmark is running %s; try again when it is done." % busy)
pause = [pid for pid, name, state in processes() if (name == "rgds-launcher" or name in session.FRONTENDS) and state not in "TtZ"]
child, paused, result = None, [], 1

def stop(signum, frame): raise KeyboardInterrupt
for s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT): signal.signal(s, stop)
log = open(HERE / "rgds-play.log", "a", buffering=1)
log.write("\n== %s %s (pausing %s)\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), rom, pause))
try:
    for pid in pause:
        if not stop_outside_mixer(Pid(pid)): raise RuntimeError("pid %d kept the sound mixer; not started" % pid)
        paused.append(pid)
    env = session.environment(app)
    # Replay switches for a headless check (RGDS_PLAY_INPUT, _SHOTS, _QUIT_AT, ...)
    env.update({k: v for k, v in os.environ.items() if k.startswith("RGDS_PLAY_")})
    command = [str(HERE / "rgds-play"), "--rom", str(rom), "--data", str(HERE / "data"), "--fonts", str(HERE / "fonts")]
    for system in ("gba", "gb"):
        command += ["--profiles", str(HERE / "profiles" / system)]
    child = subprocess.Popen(command, env=env, stdout=log, stderr=log)
    result = child.wait()
except KeyboardInterrupt:
    log.write("runner: asked to stop\n")
except Exception as error:
    log.write("runner: %s\n" % error)
finally:
    if child and child.poll() is None:
        child.terminate()  # the player writes its save on SIGTERM
        try: child.wait(10)
        except subprocess.TimeoutExpired: child.kill()
    for pid in paused:
        try: os.kill(pid, signal.SIGCONT)
        except ProcessLookupError: pass
    log.write("runner: player exit %s; resumed %s\n" % (child.returncode if child else None, paused))
'''

# Runs on the handheld: builds releases/<base>-play<N> beside the active release
# (a full copy, never touching it), overlays what changed, writes its manifest,
# verifies it and activates it with the release's own activate module.
DERIVE = r'''
import hashlib, json, shutil, sys, tarfile
from pathlib import Path
sys.dont_write_bytecode = True
RGDS = Path("/mnt/mmc/rgds")
HERE = Path(__file__).resolve().parent
active = (RGDS / "state/active").read_text().strip()
sys.path.insert(0, str(RGDS / "releases" / active / "runtime"))
import activate
record = HERE / "derived.json"
if sys.argv[1] == "rollback":
    base = json.loads(record.read_text())["base"]
    print("Activated " + activate.activate(RGDS, base, stop=True))
    sys.exit(0)
plan = json.loads((HERE / "overlay.json").read_text())
if active != plan["active"]:
    sys.exit("The active release changed (" + active + "); run install again.")
build, base = plan["build"], plan["base"]
dest, tmp = RGDS / "releases" / build, RGDS / "releases" / (".tmp-" + build)
for old in (tmp, dest):
    if old.exists():
        shutil.rmtree(old)
# From the active release: what other work installed there (engines, say) is kept.
shutil.copytree(RGDS / "releases" / active, tmp, symlinks=True)
for name in plan["delete"]:
    (tmp / name).unlink()
with tarfile.open(HERE / "overlay.tar") as overlay:
    for member in overlay.getmembers():
        path = Path(member.name)
        if path.is_absolute() or ".." in path.parts or not member.isfile():
            sys.exit("Unsafe overlay entry: " + member.name)
        target = tmp / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.unlink(missing_ok=True)
        target.write_bytes(overlay.extractfile(member).read())
        target.chmod(member.mode)
manifest = json.loads((tmp / "manifest.json").read_text())
manifest["build"] = build
manifest["cores"] = sorted(set(manifest.get("cores", [])) | {"rgds-play"})
manifest["files"] = {
    p.relative_to(tmp).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in sorted(tmp.rglob("*")) if p.is_file() and p.relative_to(tmp).as_posix() != "manifest.json"
}
(tmp / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
tmp.rename(dest)
activate.verify_release(dest)
record.write_text(json.dumps({"base": base, "build": build}) + "\n")
print("Activated " + activate.activate(RGDS, build, stop=True) + " (installed release: " + base + ")")
# The test release this one replaces goes; a rollback returns to the installed release.
if active != base and active != build and "-play" in active:
    (RGDS / "state/previous.writing").write_text(base + "\n")
    (RGDS / "state/previous.writing").replace(RGDS / "state/previous")
    shutil.rmtree(RGDS / "releases" / active)
    print("Removed the previous test release " + active)
'''


def ssh(device, command, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", device, command], **kwargs)


def deploy(device):
    binary = ROOT / "build/play/device/rgds-play"
    if not binary.is_file():
        raise SystemExit("Build first: python3 tools/build_play.py device")
    staging = ROOT / "build/play/device-stage"
    if staging.exists():
        subprocess.run(["rm", "-rf", str(staging)], check=True)
    (staging / "fonts").mkdir(parents=True)
    subprocess.run(["cp", str(binary), str(staging / "rgds-play")], check=True)
    for system in ("gba", "gb"):
        (staging / "profiles" / system).mkdir(parents=True)
        for p in (ROOT / "play/profiles" / system).glob("*.ini"):
            (staging / "profiles" / system / p.name).write_bytes(p.read_bytes())
    for p in (ROOT / "assets/fonts").glob("rounded-*.rgf"):
        (staging / "fonts" / p.name).write_bytes(p.read_bytes())
    (staging / "run.py").write_text(RUNNER)
    ssh(device, "mkdir -p " + REMOTE, check=True)
    # Profiles and fonts are replaced whole; the player is swapped in by rename; data/ is never touched.
    subprocess.run(
        [
            "scp", "-q", "-o", "BatchMode=yes", "-r",
            str(staging / "profiles"),
            str(staging / "fonts"),
            str(staging / "run.py"), device + ":" + REMOTE + "/"
        ],
        check=True
    )
    subprocess.run(
        ["scp", "-q", "-o", "BatchMode=yes",
         str(staging / "rgds-play"), device + ":" + REMOTE + "/.rgds-play.new"],
        check=True
    )
    ssh(device, f"chmod 755 {REMOTE}/.rgds-play.new && mv {REMOTE}/.rgds-play.new {REMOTE}/rgds-play", check=True)
    print("deployed to " + device + ":" + REMOTE)


def install(device):
    """RG DS Core with RG DS Play, as the package would build it, beside the installed release."""
    import hashlib
    import io
    import json
    import tarfile
    sys.path.insert(0, str(ROOT / "tools"))
    from build_play import validate_payload
    sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    rgds = json.loads((ROOT / "build/device/build.json").read_text())
    for name, info in rgds["artifacts"].items():
        if sha(ROOT / "build/device" / name) != info["sha256"]:
            raise SystemExit("build/device changed after its build: run python3 tools/build.py device")
    payload = ROOT / "build/play/device/payload"
    play = validate_payload(payload)
    active = ssh(device, "cat /mnt/mmc/rgds/state/active", capture_output=True, text=True, check=True).stdout.strip()
    # A test release is rebuilt from the installed release it was made from.
    base = active.split("-play")[0] if "-play" in active else active
    # The new test release starts from the active one (keeping what other work installed there).
    manifest = json.loads(
        ssh(device, f"cat /mnt/mmc/rgds/releases/{active}/manifest.json", capture_output=True, check=True).stdout
    )["files"]
    # What package.py puts in a release, from this tree.
    wanted = {
        "bin/rgds-launcher": ROOT / "build/device/rgds-launcher",
        "emulators/hooks/librgdsbridge.so": ROOT / "build/device/librgdsbridge.so",
        "build/rgds.json": ROOT / "build/device/build.json",
        "emulators/drastic.cfg": ROOT / "assets/emulator-profiles/drastic.cfg",
        "emulators/dsperate.ini": ROOT / "assets/emulator-profiles/dsperate.ini",
    }
    for folder in ("runtime", "themes", "assets"):
        for source in sorted((ROOT / folder).rglob("*")):
            if source.is_file() and "__pycache__" not in source.parts and source.name != ".DS_Store" and not \
                    source.name.startswith("._"):
                wanted[source.relative_to(ROOT).as_posix()] = source
    for name in (*play["files"], "build.json"):
        wanted["emulators/rgds-play/" + name] = payload / name
    # The RGDS presets also go where the bridge reads shaders (package.py does the same).
    for source in sorted((ROOT / "assets/shaders").glob("*.dsd")):
        wanted["emulators/shaders/" + source.name] = source
    changed = {name: path for name, path in wanted.items() if manifest.get(name) != sha(path)}
    delete = sorted(
        name for name in manifest if name.split("/")[0] in ("runtime", "themes", "assets", "emulators")
        and name not in wanted and (not name.startswith("emulators/") or name.startswith("emulators/rgds-play/"))
    )
    build = base + "-play" + hashlib.sha256(json.dumps(sorted((n, sha(p))
                                                              for n, p in changed.items())).encode()).hexdigest()[:8]
    stage = ROOT / "build/play/install"
    stage.mkdir(parents=True, exist_ok=True)
    with tarfile.open(stage / "overlay.tar", "w") as tar:
        for name, path in sorted(changed.items()):
            data = path.read_bytes()
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o755 if os.access(path, os.X_OK) or name.endswith(".sh") else 0o644
            tar.addfile(info, io.BytesIO(data))
    (stage / "overlay.json").write_text(
        json.dumps({
            "active": active,
            "base": base,
            "build": build,
            "delete": delete
        }, indent=1)
    )
    (stage / "derive.py").write_text(DERIVE)
    print(f"{len(changed)} files changed, {len(delete)} removed; building {build} from {base}")
    ssh(device, "mkdir -p " + REMOTE, check=True)
    subprocess.run(
        [
            "scp", "-q", "-o", "BatchMode=yes", *(str(stage / n) for n in ("overlay.tar", "overlay.json", "derive.py")),
            device + ":" + REMOTE + "/"
        ],
        check=True
    )
    ssh(device, f"cd {REMOTE} && python3 -B derive.py install", check=True)
    start_session(device)


def start_session(device):
    """RG DS Core again, on whatever release is active now (as make run does)."""
    ssh(
        device, "mkdir -p /mnt/mmc/rgds/userdata; nohup sh /mnt/mmc/rgds/main.sh > /mnt/mmc/rgds/userdata/runner.log "
        "2>&1 < /dev/null &",
        check=True
    )
    print("RG DS Core is starting on the handheld")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("deploy", "run", "status", "stop", "log", "install", "rollback"))
    parser.add_argument("rom", nargs="?")
    parser.add_argument("--device", default="root@anbernic.local")
    args = parser.parse_args()
    if args.action == "deploy":
        deploy(args.device)
    elif args.action == "install":
        install(args.device)
    elif args.action == "rollback":
        ssh(args.device, f"cd {REMOTE} && python3 -B derive.py rollback", check=True)
        start_session(args.device)
    elif args.action == "run":
        if not args.rom:
            raise SystemExit("run needs the game's path on the handheld")
        # Detached, so it outlives this SSH connection; the runner resumes the launcher itself.
        ssh(
            args.device, f"cd {REMOTE} && setsid nohup python3 -B run.py {shlex.quote(args.rom)} "
            f">> {REMOTE}/runner.out 2>&1 < /dev/null &",
            check=True
        )
        print("started; `log` shows how it goes")
    elif args.action == "stop":
        ssh(
            args.device, f"for p in /proc/[0-9]*; do [ \"$(cat $p/comm 2>/dev/null)\" = rgds-play ] && "
            f"kill -TERM ${{p#/proc/}}; done; true",
            check=True
        )
    elif args.action == "status":
        ssh(
            args.device,
            "for p in /proc/[0-9]*; do n=$(cat $p/comm 2>/dev/null); case \"$n\" in rgds-play|rgds-launcher|"
            "dmenu.bin) echo \"${p#/proc/} $n $(cut -d' ' -f3 $p/stat)\";; esac; done"
        )
    else:
        ssh(args.device, f"tail -n 40 {REMOTE}/rgds-play.log; tail -n 5 {REMOTE}/runner.out 2>/dev/null")


if __name__ == "__main__":
    main()
