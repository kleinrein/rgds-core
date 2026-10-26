"""Hand play of a PortMaster port from SSH (docs/PORTMASTER_PLAN.md, Phase 0). Runs on the handheld.

    ssh -t root@anbernic.local python3 -B /mnt/mmc/portmaster/.spike/play.py cats
    ssh -t root@anbernic.local python3 -B /mnt/mmc/portmaster/.spike/play.py apotris

Pauses rgds-launcher, runs the port the way RG DS Core will (session
environment, private mount namespace, its own cgroup2 leaf), handles the volume
keys with the release's own VolumeWatch, and ends the port on Home, on
Select+Start (gptokeyb's PortMaster exit), on Ctrl+C or when SSH drops. The
launcher is always resumed. Output goes to play.log beside this file.
"""
import ctypes, ctypes.util, os, select, signal, struct, subprocess, sys, time
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
RGDS = Path("/mnt/mmc/rgds")
DATA = RGDS / "userdata"
PORTS = Path("/mnt/mmc/portmaster/ports")
app = RGDS / "releases" / (RGDS / "state/active").read_text().strip()
sys.path.insert(0, str(app / "runtime"))
import session  # read only: environment()
from shared_audio import stop_outside_mixer
from audio import VolumeWatch

NAMES = {"apotris": "Apotris.sh", "cats": "Cats on Mars.sh", "catsonmars": "Cats on Mars.sh"}
HOME_CODES = (312, 354)  # the logo/Home key on the RG DS pad (game_menu.ANBERNIC_MENU_CODES)
WORK = Path("/tmp/rgds-pm-play")
CG = WORK / "cg"
EVENT = struct.Struct("llHHi")
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class Pid:
    def __init__(self, pid): self.pid = pid
    def send_signal(self, sig): os.kill(self.pid, sig)


def say(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


def procs():
    """Live processes; a port's processes still being reaped after a kill (Z, X) do not count."""
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                if (entry / "stat").read_text().rsplit(")", 1)[1].split()[0] in "ZX":
                    continue
                yield int(entry.name), (entry / "comm").read_text().strip()
            except (OSError, IndexError):
                pass


def preexec():
    os.setsid()
    if libc.unshare(0x00020000) != 0:  # CLONE_NEWNS: the port's mounts stay its own
        raise OSError(ctypes.get_errno(), "unshare")
    if libc.mount(None, b"/", None, 0x4000 | (1 << 18), None) != 0:  # MS_REC | MS_PRIVATE
        raise OSError(ctypes.get_errno(), "mount private")
    (CG / "port/cgroup.procs").write_text(str(os.getpid()))


def home_inputs():
    """Pad devices that have the Home key; read only, never grabbed (gptokeyb needs them)."""
    found = []
    for path in sorted(Path("/dev/input").glob("event*")):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError:
            continue
        bits = bytearray(96)
        try:
            import fcntl
            fcntl.ioctl(fd, 0x80604521, bits)  # EVIOCGBIT(EV_KEY, 96)
        except OSError:
            os.close(fd)
            continue
        if any(bits[c // 8] & (1 << (c % 8)) for c in HOME_CODES):
            found.append(fd)
        else:
            os.close(fd)
    return found


class Stop(Exception):
    pass


def stop(signum, frame):
    raise Stop(signal.Signals(signum).name)


def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: play.py apotris|cats|<script.sh in %s>" % PORTS)
    script = PORTS / NAMES.get(sys.argv[1].lower(), sys.argv[1])
    if not script.is_file():
        sys.exit("No such port script: %s" % script)
    busy = [n for _, n in procs() if n.startswith(("dsperate", "rgds-experiment", "drastic", "melon", "retroarch",
                                                    "rgds-play", "play-shot", "frt_", "Apotris", "gptokeyb"))]
    if busy:
        sys.exit("Something is already running: %s" % ", ".join(sorted(set(busy))))
    for s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(s, stop)
    paused, child, mounted, volume, inputs, reason = [], None, False, None, [], "the port exited"
    log = open(HERE / "play.log", "a", buffering=1)
    log.write("\n== %s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), script))
    try:
        WORK.mkdir(exist_ok=True)
        CG.mkdir(exist_ok=True)
        if not (CG / "cgroup.procs").exists():
            subprocess.run(["mount", "-t", "cgroup2", "none", str(CG)], check=True)
            mounted = True
        (CG / "port").mkdir(exist_ok=True)
        for pid, name in procs():
            if name == "rgds-launcher":
                if not stop_outside_mixer(Pid(pid)):
                    raise RuntimeError("the launcher kept the sound mixer; not started")
                paused.append(pid)
        env = session.environment(app)
        env.pop("PYTHONPYCACHEPREFIX", None)
        env.update(HOME="/mnt/mmc/portmaster/home", XDG_DATA_HOME="/mnt/mmc/portmaster", SDL_AUDIODRIVER="alsa",
                   SDL_VIDEO_WAYLAND_WMCLASS="org.rgds.top", SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1")
        volume = VolumeWatch(DATA)
        volume.start()
        inputs = home_inputs()
        child = subprocess.Popen(["bash", str(script)], cwd=str(script.parent), env=env, stdout=log,
                                 stderr=subprocess.STDOUT, preexec_fn=preexec)
        say("Playing %s. Home or Select+Start ends it; Ctrl+C here does too." % script.stem)
        while child.poll() is None:
            for fd in select.select(inputs, [], [], .25)[0]:
                data = os.read(fd, EVENT.size * 32)
                for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    _s, _u, kind, code, value = EVENT.unpack_from(data, offset)
                    if kind == 1 and code in HOME_CODES and value == 1:
                        raise Stop("Home")
    except Stop as why:
        reason = str(why)
    except Exception as error:
        reason = "error: %r" % (error,)
    finally:
        if child and child.poll() is None:
            try:
                (CG / "port/cgroup.kill").write_text("1")
            except OSError:
                os.killpg(child.pid, signal.SIGKILL)
        if child:
            try:
                child.wait(5)
            except subprocess.TimeoutExpired:
                pass
        for _ in range(50):
            try:
                if not (CG / "port/cgroup.procs").read_text().split():
                    break
            except OSError:
                break
            time.sleep(.1)
        if volume:
            volume.close()
        for fd in inputs:
            os.close(fd)
        for pid in paused:
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        try:
            (CG / "port").rmdir()
        except OSError:
            pass
        if mounted:
            subprocess.run(["umount", str(CG)])
        log.write("runner: ended (%s), exit %s, resumed %s\n" % (reason, child.returncode if child else None, paused))
        say("Ended (%s). The launcher is back." % reason)


if __name__ == "__main__":
    main()
