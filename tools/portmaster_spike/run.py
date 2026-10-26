"""PortMaster Phase 0 spike runner (docs/PORTMASTER_PLAN.md). Runs on the handheld.

python3 -B run.py <script.sh> <key> [<key2>]
Pauses rgds-launcher, runs the port in a private mount namespace and its own
cgroup2 leaf, snapshots both panels with scanout, injects keys through a uinput
keyboard, simulates the in-game menu (cgroup freeze + fullscreen SDL windows on
every display), then thaws, kills the tree with cgroup.kill, checks nothing is
left, and resumes the launcher. Everything it creates lives in /tmp/pmspike.
"""
import ctypes, ctypes.util, fcntl, json, os, signal, struct, subprocess, sys, time
from pathlib import Path

sys.dont_write_bytecode = True
RGDS = Path("/mnt/mmc/rgds")
app = RGDS / "releases" / (RGDS / "state/active").read_text().strip()
sys.path.insert(0, str(app / "runtime"))
import session  # read only: environment()
from shared_audio import stop_outside_mixer

OUT = Path("/tmp/pmspike")
OUT.mkdir(exist_ok=True)
CG = OUT / "cg"
SCAN = "/mnt/mmc/portmaster/.spike/scanout"
script = Path(sys.argv[1])
keys = [int(k) for k in sys.argv[2:]] or [28]
report = {"script": str(script)}
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
CLONE_NEWNS, MS_REC, MS_PRIVATE = 0x00020000, 0x4000, 1 << 18


def log(*parts):
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


class Pid:
    def __init__(self, pid): self.pid = pid
    def send_signal(self, sig): os.kill(self.pid, sig)


def procs():
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                yield int(entry.name), (entry / "comm").read_text().strip()
            except OSError:
                pass


def scan(name):
    out = subprocess.run([SCAN, str(OUT / name)], capture_output=True, text=True, timeout=10).stdout
    return {crtc: OUT / f"{name}-crtc{crtc}-plane{plane}.bmp"
            for crtc, plane in ((l.split()[3], l.split()[1]) for l in out.splitlines() if l.startswith("plane"))}


def differs(a, b):
    x, y = Path(a).read_bytes()[54:], Path(b).read_bytes()[54:]
    step = 4 * 97  # sample about 3,000 pixels
    changed = sum(x[i:i + 3] != y[i:i + 3] for i in range(0, min(len(x), len(y)), step))
    return round(changed / (min(len(x), len(y)) // step), 3)


class Keyboard:
    def __init__(self):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, 0x40045564, 1)  # UI_SET_EVBIT EV_KEY
        for code in range(1, 120):
            fcntl.ioctl(self.fd, 0x40045565, code)  # UI_SET_KEYBIT
        dev = struct.pack("80sHHHHI", b"rgds-pm-spike", 3, 1, 1, 1, 0) + bytes(4 * 64 * 4)
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, 0x5501)  # UI_DEV_CREATE
        time.sleep(1.0)

    def tap(self, code):
        for value in (1, 0):
            os.write(self.fd, struct.pack("llHHi", 0, 0, 1, code, value))
            os.write(self.fd, struct.pack("llHHi", 0, 0, 0, 0, 0))
            time.sleep(0.12)

    def close(self):
        fcntl.ioctl(self.fd, 0x5502)
        os.close(self.fd)


class Mouse:
    def __init__(self):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, 0x40045564, 1)  # EV_KEY
        fcntl.ioctl(self.fd, 0x40045564, 2)  # EV_REL
        fcntl.ioctl(self.fd, 0x40045565, 0x110)  # BTN_LEFT
        fcntl.ioctl(self.fd, 0x40045566, 0)  # UI_SET_RELBIT REL_X
        fcntl.ioctl(self.fd, 0x40045566, 1)  # REL_Y
        os.write(self.fd, struct.pack("80sHHHHI", b"rgds-pm-spike-mouse", 3, 1, 2, 1, 0) + bytes(4 * 64 * 4))
        fcntl.ioctl(self.fd, 0x5501)
        time.sleep(1.0)

    def send(self, kind, code, value):
        os.write(self.fd, struct.pack("llHHi", 0, 0, kind, code, value))
        os.write(self.fd, struct.pack("llHHi", 0, 0, 0, 0, 0))

    def move(self, dx, dy):
        for _ in range(20):
            self.send(2, 0, dx // 20)
            self.send(2, 1, dy // 20)
            time.sleep(0.01)

    def click(self):
        self.send(1, 0x110, 1)
        time.sleep(0.08)
        self.send(1, 0x110, 0)

    def close(self):
        fcntl.ioctl(self.fd, 0x5502)
        os.close(self.fd)


COMPANION = r'''
import ctypes, signal, sys, time
sdl = ctypes.CDLL("libSDL2-2.0.so.0")
sdl.SDL_Init(0x20)
sdl.SDL_CreateWindow.restype = ctypes.c_void_p
sdl.SDL_CreateWindow.argtypes = [ctypes.c_char_p] + [ctypes.c_int] * 4 + [ctypes.c_uint32]
sdl.SDL_GetWindowSurface.restype = ctypes.c_void_p
sdl.SDL_GetWindowSurface.argtypes = [ctypes.c_void_p]
sdl.SDL_FillRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
sdl.SDL_UpdateWindowSurface.argtypes = [ctypes.c_void_p]
pos = 0x2FFF0000 | int(sys.argv[1])
w = sdl.SDL_CreateWindow(b"spike companion", pos, pos, 640, 480, 0x1001 | 0x4)
state = {"menu": False, "run": True}
signal.signal(signal.SIGUSR1, lambda *_: state.update(menu=not state["menu"]))
signal.signal(signal.SIGTERM, lambda *_: state.update(run=False))
event = ctypes.create_string_buffer(64)
while state["run"]:
    while sdl.SDL_PollEvent(event): pass
    surface = sdl.SDL_GetWindowSurface(w)
    if surface:
        sdl.SDL_FillRect(surface, None, 0x00E03030 if state["menu"] else 0x002060C0)
        sdl.SDL_UpdateWindowSurface(w)
    time.sleep(0.05)
sdl.SDL_Quit()
'''


MENU = r'''
import ctypes, time
sdl = ctypes.CDLL("libSDL2-2.0.so.0")
sdl.SDL_Init(0x20)
sdl.SDL_GetDisplayName.restype = ctypes.c_char_p
sdl.SDL_CreateWindow.restype = ctypes.c_void_p
sdl.SDL_CreateWindow.argtypes = [ctypes.c_char_p] + [ctypes.c_int] * 4 + [ctypes.c_uint32]
sdl.SDL_GetWindowSurface.restype = ctypes.c_void_p
sdl.SDL_GetWindowSurface.argtypes = [ctypes.c_void_p]
sdl.SDL_FillRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
sdl.SDL_UpdateWindowSurface.argtypes = [ctypes.c_void_p]
sdl.SDL_DestroyWindow.argtypes = [ctypes.c_void_p]
n = sdl.SDL_GetNumVideoDisplays()
print("displays", [sdl.SDL_GetDisplayName(i).decode() for i in range(n)], flush=True)
wins = []
for i in range(n):
    pos = 0x2FFF0000 | i
    w = sdl.SDL_CreateWindow(b"spike menu %d" % i, pos, pos, 640, 480, 0x1001 | 0x4)
    wins.append(w)
end = time.monotonic() + 2.5
event = ctypes.create_string_buffer(64)
while time.monotonic() < end:
    while sdl.SDL_PollEvent(event): pass
    for i, w in enumerate(wins):
        surface = sdl.SDL_GetWindowSurface(w)
        if surface:
            sdl.SDL_FillRect(surface, None, 0x00C02040 if i == 0 else 0x002040C0)
            sdl.SDL_UpdateWindowSurface(w)
    time.sleep(0.03)
for w in wins: sdl.SDL_DestroyWindow(w)
sdl.SDL_Quit()
'''


def outside_squashfs():
    return [l.split()[4] for l in Path("/proc/self/mountinfo").read_text().splitlines() if " squashfs " in l]


def preexec():
    os.setsid()
    if libc.unshare(CLONE_NEWNS) != 0:
        raise OSError(ctypes.get_errno(), "unshare")
    if libc.mount(None, b"/", None, MS_REC | MS_PRIVATE, None) != 0:
        raise OSError(ctypes.get_errno(), "mount private")
    (CG / "port/cgroup.procs").write_text(str(os.getpid()))


paused, child, mounted_cg, kb, companion = [], None, False, None, None
try:
    busy = [n for _, n in procs() if n.startswith(("dsperate", "rgds-experiment", "drastic", "melon", "retroarch", "rgds-play", "play-shot"))]
    if busy:
        sys.exit("A game is running: %s" % busy)
    CG.mkdir(exist_ok=True)
    if not (CG / "cgroup.procs").exists():
        subprocess.run(["mount", "-t", "cgroup2", "none", str(CG)], check=True)
        mounted_cg = True
    (CG / "port").mkdir(exist_ok=True)
    report["cgroup_controllers"] = (CG / "cgroup.controllers").read_text().strip()
    base = scan("0-home")
    for pid, name in procs():
        if name == "rgds-launcher":
            if not stop_outside_mixer(Pid(pid)):
                raise RuntimeError("launcher kept the mixer")
            paused.append(pid)
    env = session.environment(app)
    env.update(HOME="/mnt/mmc/portmaster/home", XDG_DATA_HOME="/mnt/mmc/portmaster", SDL_AUDIODRIVER="alsa",
               SDL_VIDEO_WAYLAND_WMCLASS="org.rgds.top", SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1")
    env.pop("PYTHONPYCACHEPREFIX", None)
    report["env_display"] = {k: env.get(k) for k in ("SDL_VIDEODRIVER", "WAYLAND_DISPLAY", "SDL_VIDEO_DISPLAY_PRIORITY", "ALSA_CONFIG_PATH")}
    kb = Keyboard()
    if os.environ.get("SPIKE_COMPANION") == "1":
        companion = subprocess.Popen(["python3", "-B", "-c", COMPANION, os.environ.get("SPIKE_DISPLAY", "1")], env=env)
        time.sleep(2.0)
        report["companion_bottom_vs_home"] = differs(base["90"], scan("0-companion")["90"])
        report["companion_top_vs_home"] = differs(base["130"], scan("0-companion")["130"])
    log("starting", script)
    with open(OUT / "port.log", "w") as out:
        child = subprocess.Popen(["bash", str(script)], cwd=str(script.parent), env=env, stdout=out, stderr=subprocess.STDOUT,
                                 preexec_fn=preexec)
    time.sleep(10)
    report["alive_after_10s"] = child.poll() is None
    members = (CG / "port/cgroup.procs").read_text().split()
    report["tree"] = sorted({(Path(f"/proc/{p}/comm").read_text().strip() if Path(f"/proc/{p}").exists() else "?") for p in members})
    inner = [l.split()[4] for p in members for l in Path(f"/proc/{p}/mountinfo").read_text().splitlines() if " squashfs " in l] if members else []
    report["squashfs_inside"] = sorted(set(inner))
    report["squashfs_outside"] = outside_squashfs()
    a = scan("1-running")
    report["panels_vs_home"] = {crtc: differs(base[crtc], a[crtc]) for crtc in a if crtc in base}
    if keys[0]:
        kb.tap(keys[0])
    time.sleep(2.5)
    b = scan("2-after-key")
    report["key_before_menu_changed_top"] = differs(a["130"], b["130"])
    # The menu: freeze the whole tree, put fullscreen windows on every display, close them, thaw.
    (CG / "port/cgroup.freeze").write_text("1")
    time.sleep(0.3)
    report["frozen"] = "frozen 1" in (CG / "port/cgroup.events").read_text()
    if companion:
        companion.send_signal(signal.SIGUSR1)  # the companion draws "the menu" in its own window
        time.sleep(1.0)
        report["companion_menu_bottom_changed"] = differs(a["90"], scan("3-companion-menu")["90"])
        companion.send_signal(signal.SIGUSR1)
    elif os.environ.get("SPIKE_NOMENU") != "1":
        menu = subprocess.run(["python3", "-B", "-c", MENU], env=env, capture_output=True, text=True, timeout=20)
        report["sdl_displays"] = menu.stdout.strip()
    f1 = scan("3-frozen")
    time.sleep(1.0)
    f2 = scan("4-frozen-later")
    report["frozen_top_still"] = differs(f1["130"], f2["130"])
    (CG / "port/cgroup.freeze").write_text("0")
    time.sleep(1.5)
    if os.environ.get("SPIKE_SWITCH"):
        # desktop-shell's window switcher: Super+Tab (n times), focus lands on release of Super.
        os.write(kb.fd, struct.pack("llHHi", 0, 0, 1, 125, 1)); os.write(kb.fd, struct.pack("llHHi", 0, 0, 0, 0, 0))
        for _ in range(int(os.environ["SPIKE_SWITCH"])):
            kb.tap(15)
        os.write(kb.fd, struct.pack("llHHi", 0, 0, 1, 125, 0)); os.write(kb.fd, struct.pack("llHHi", 0, 0, 0, 0, 0))
        time.sleep(0.5)
    if os.environ.get("SPIKE_REFOCUS") == "1":
        mouse = Mouse()
        mouse.move(-4000, -4000)
        mouse.move(int(os.environ.get("SPIKE_X", "320")), 240)
        mouse.click()
        time.sleep(0.3)
        mouse.close()
    c = scan("5-thawed")
    for key in keys[1:] or keys:
        kb.tap(key)
        time.sleep(0.6)
    time.sleep(2.0)
    d = scan("6-after-key2")
    report["key_after_menu_changed_top"] = differs(c["130"], d["130"])
    report["alive_before_kill"] = child.poll() is None
    (CG / "port/cgroup.kill").write_text("1")
    for _ in range(50):
        if not (CG / "port/cgroup.procs").read_text().split():
            break
        time.sleep(0.1)
    child.wait(5)
    report["left_after_kill"] = (CG / "port/cgroup.procs").read_text().split()
    report["squashfs_outside_after"] = outside_squashfs()
except Exception as error:
    report["error"] = repr(error)
finally:
    if child and child.poll() is None:
        try:
            (CG / "port/cgroup.kill").write_text("1")
        except OSError:
            os.killpg(child.pid, signal.SIGKILL)
    if kb:
        kb.close()
    if companion:
        companion.terminate()
        companion.wait(5)
    for pid in paused:
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            pass
    report["resumed"] = paused
    time.sleep(0.5)
    try:
        (CG / "port").rmdir()
    except OSError as error:
        report["cg_rmdir"] = repr(error)
    if mounted_cg:
        subprocess.run(["umount", str(CG)])
    print(json.dumps(report, indent=1))
