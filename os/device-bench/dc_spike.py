# Dreamcast spike on RG DS OS (docs/DREAMCAST_PLAN.md "Spike"): one game through this checkout's runtime and player,
# with Flycast from the spike folder, on a userdata of its own (never the owner's /data/rgds/userdata).
#
#   sh session_hold.sh on
#   python3 dc_spike.py ROM SECONDS [FRESH=1] [VAR=VALUE ...]   (VAR=VALUE goes into the player's environment,
#                                                                 RGDS_PLAY_CORE_OPTIONS="flycast_x=y;..." included)
#   sh session_hold.sh off
#
# The spike folder (this file's) holds runtime/ (a copy of the checkout's), rgds-play (tools/build_play.py device) and
# flycast_libretro.so. Prints the player's log lines as they come, then the peak resident memory of the player.
import os
import subprocess
import sys
import time
from pathlib import Path

spike = Path(__file__).resolve().parent
release = Path("/usr/share/rgds/releases") / Path("/usr/share/rgds/active").read_text().strip()
sys.path.insert(0, str(spike / "runtime"))
os.environ.update(
    RGDS_SESSION_PLATFORM="independent",
    RGDS_INPUT_PROFILE="mainline-rg-ds",
    SDL_VIDEODRIVER="kmsdrm",
    SDL_KMSDRM_FIRST="DSI-2",
    RGDS_TOUCH_DEV="/dev/input/event2",
    HOME="/root",
    XDG_RUNTIME_DIR="/run/rgds"
)
rom, seconds = Path(sys.argv[1]), int(sys.argv[2])
extra = dict(kv.split("=", 1) for kv in sys.argv[3:])
fresh = extra.pop("FRESH", "") == "1"

# The image's cores beside the spike's Flycast, so the runtime finds every one where it looks. In RAM: /data is exFAT,
# which holds no symlinks.
cores = Path("/run/rgds/dc-spike-cores")
cores.mkdir(parents=True, exist_ok=True)
for core in Path("/usr/lib/libretro").glob("*_libretro.so"):
    if not (cores / core.name).is_symlink(): (cores / core.name).symlink_to(core)
# The spike's Flycast always, also where the image has one of its own (it does since 0.9.1-os8974f5f1: runs on
# 2026-11-20 measured the image's unpatched core by mistake until this replaced the link).
(cores / "flycast_libretro.so").unlink(missing_ok=True)
(cores / "flycast_libretro.so").symlink_to(spike / "flycast_libretro.so")
data = spike / "userdata"
(data / "session").mkdir(parents=True, exist_ok=True)

import play_options  # noqa: E402

play_options.LIBRETRO_DIR = cores
import emulators  # noqa: E402
import activate  # noqa: E402

# The spike runs its own player and core, so the release's file check says nothing about them; another session's
# bind mount over the release's rgds-play (2026-11-23) made it refuse every spike launch.
activate.verify_engine = lambda *args, **kwargs: None

if play_options.system_of(rom) != "dc": raise SystemExit(f"{rom} is not a Dreamcast game to this runtime")
plan = emulators.launch_plan("rgds-play", release, data, rom, fresh_start=fresh)
plan["command"][0] = str(spike / "rgds-play")
plan["env"].update(RGDS_PLAY_PROFILE="1", RGDS_RUMBLE="0")  # no vibration in test runs (VAR= RGDS_RUMBLE=N to feel it)
plan["env"].update(extra)
for name in [k for k, v in extra.items() if v == ""]:  # VAR= drops it: RGDS_CORE= LD_PRELOAD= runs without the bridge
    plan["env"].pop(name, None)
print("command", " ".join(plan["command"]), flush=True)
player = subprocess.Popen(plan["command"], cwd=plan["cwd"], env=plan["env"])
peak, started = 0, time.monotonic()
while player.poll() is None and time.monotonic() - started < seconds:
    try:
        status = Path(f"/proc/{player.pid}/status").read_text()
        peak = max(peak, int(status.split("VmHWM:")[1].split()[0]))
    except (OSError, IndexError, ValueError):
        pass
    time.sleep(0.5)
if player.poll() is None:
    player.terminate()
    try:
        player.wait(20)
    except subprocess.TimeoutExpired:
        player.kill()
print(f"EXIT {player.returncode} after {time.monotonic() - started:.0f} s, peak resident {peak // 1024} MB", flush=True)
