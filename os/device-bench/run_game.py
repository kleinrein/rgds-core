# Hand-run a game on RG DS OS through the release's own launch_plan, with the session held (see pace_trial.sh).
# run_game.py ROM SECONDS [PRE:VAR=VALUE ...] [VAR=VALUE ...]: PRE: ones are set before launch_plan, the rest after.
import os, subprocess, sys
from pathlib import Path

release = Path("/usr/share/rgds/releases") / Path("/usr/share/rgds/active").read_text().strip()
sys.path.insert(0, str(release / "runtime"))
os.environ.update(
    RGDS_SESSION_PLATFORM="independent",
    RGDS_INPUT_PROFILE="mainline-rg-ds",
    SDL_VIDEODRIVER="kmsdrm",
    SDL_KMSDRM_FIRST="DSI-2",
    RGDS_TOUCH_DEV="/dev/input/event2",
    HOME="/root",
    XDG_RUNTIME_DIR="/run/rgds"
)
extra = dict(kv.split("=", 1) for kv in sys.argv[3:])
os.environ.update({k[4:]: v for k, v in extra.items() if k.startswith("PRE:")})
for f in ("emulator.control", "emulator.control.result"):
    try:
        os.unlink("/data/rgds/userdata/session/" + f)
    except OSError:
        pass
import emulators

plan = emulators.launch_plan(
    extra.pop("CORE", "rgds-experimental"),
    release,
    Path("/data/rgds/userdata"),
    Path(sys.argv[1]),
    fresh_start=extra.pop("FRESH", "") == "1"
)  # FRESH=1: from the title screen, no state
plan["env"].update({k: v for k, v in extra.items() if not k.startswith("PRE:")})
if "BIN" in extra: plan["command"][0] = extra.pop("BIN")  # another engine binary, e.g. one just built
plan["command"] += extra.pop("ARGS", "").split()  # extra engine arguments, e.g. ARGS="--frames 2400 --replay F"
print(
    "env", {k: plan["env"].get(k)
            for k in ("DS_DIRECT_PRESENT", "RGDS_VK_SCALE", "DS_PACE", "DS_ADAPTIVE")},
    flush=True
)
try:
    r = subprocess.run(plan["command"], cwd=plan["cwd"], env=plan["env"], timeout=int(sys.argv[2]))
    print("EXIT", r.returncode, flush=True)
except subprocess.TimeoutExpired:
    print("TIMEOUT", flush=True)
