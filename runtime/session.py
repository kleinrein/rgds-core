#!/usr/bin/env python3
"""Own a temporary launcher session and recover the identified stock frontends."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import tempfile
import select
import socket

sys.dont_write_bytecode = True
from engines import CORES
from system_controls import Hardware
from records import process_identity, read_record, write_record
from stock_handoff import (
    acquire_stock, release_stock_flag, restore_stock, begin_session, end_session, consume_exit, execute_system_action,
    clean_environment
)

FRONTENDS = {"dmenu.bin", "muos3.bin", "muos2.bin", "MainUI"}
LOCK = Path("/tmp/rgds-session.lock")
STOCK_STOP = Path("/tmp/stopAPP.ini")
PROTECTED_SERVICES = {"networkmanager", "wpa_supplicant", "weston", "dbus-daemon", "dbus-broker"}
SYSTEM_EXIT_ACK = 76  # Private launcher acknowledgement, never ordinary Back.
SYSTEM_DISPATCH_ACK = 77  # Guardian completed an orderly system request.
# Bytecode for the runtime, in RAM. RGDS never writes bytecode next to the
# release's sources (they must match the manifest, and the card is FAT), so
# without it every game launch compiled emulators.py's imports again: 0.84 s
# against 0.10 s for a bare interpreter on the A55. precompile() fills it at
# session start; Python checks each file against its source's mtime and size
# before using it, and nothing else writes there.
PYCACHE = "/tmp/rgds-pycache"


class GuardianMessages:
    """Two newline-framed messages from the session parent through its private pipe."""
    def __init__(self):
        self.pending = b""
        self.owner = None
        self.action = None

    def feed(self, chunk):
        self.pending += chunk
        if len(self.pending) > 4096:
            raise ValueError("Invalid launcher ownership message")
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            value = json.loads(line)
            if self.owner is None:
                if (
                    not isinstance(value, dict) or set(value) != {"pid", "start"} or type(value["pid"]) is not int
                    or value["pid"] <= 1 or not isinstance(value["start"], str) or not value["start"].isdigit()
                ):
                    raise ValueError("Invalid launcher owner")
                self.owner = value
            elif self.action is None and isinstance(value, dict) and set(value) == {
                "action"
            } and value["action"] in ("poweroff", "reboot"):
                self.action = value["action"]
            else:
                raise ValueError("Invalid guardian action")


def finish_handoff(records, lease, action, data, standalone=False):
    """Do not relaunch the stock frontend between RGDS exit and init shutdown."""
    if action is None:
        restore(records, lease)
        return 0
    try:
        if not standalone and lease is None:
            lease = acquire_stock({"pid": os.getpid(), "start": process_identity(os.getpid())})
        if standalone: execute_system_action(action, data, standalone=True)
        else: execute_system_action(action, data)
    except BaseException:
        # A refused request returns the device to stock; no forced fallback.
        restore(records, lease)
        raise
    # Keep our stop flag until /tmp is cleared on boot, as the stock UI does.
    return SYSTEM_DISPATCH_ACK


class RecoveryPending(ValueError):
    pass


def discover_frontends(proc=Path("/proc"), include_stopped=False):
    records = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            name = (entry / "comm").read_text().strip()
            # Stock APPS starts a separate Now Playing surface. It otherwise
            # remains behind our windows and reappears during display handoff.
            overlay = name == "subscreen.dge" and (entry /
                                                   "exe").resolve() == Path("/mnt/vendor/subscreen/subscreen.dge")
            if name not in FRONTENDS and not overlay:
                continue
            state = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            if state[0] == "Z" or (not include_stopped and state[0] in ("T", "t")):
                continue
            record = {"pid": int(entry.name), "start": state[19]}
            records.append(record)
        except (OSError, IndexError):
            continue
    return records


def frontend_alive(record):
    if not record["start"] or process_identity(record["pid"]) != record["start"]: return False
    try:
        return (Path("/proc") / str(record["pid"]) / "stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def restore(records, lease=None):
    # The stock supervisor owns frontend startup and its environment.
    # `records` identify what we stopped; they never become spawn arguments.
    restore_stock(lease)


def stop_frontends(records):
    for record in records:
        if not frontend_alive(record): continue
        try:
            os.kill(record["pid"], signal.SIGTERM)
        except ProcessLookupError:
            continue
        for _ in range(20):
            if not frontend_alive(record): break
            time.sleep(0.05)
        else:
            if frontend_alive(record):
                try:
                    os.kill(record["pid"], signal.SIGKILL)
                except ProcessLookupError:
                    pass


def environment(app, *, standalone=False):
    env = clean_environment()
    # Diagnostic runs opt in before the session starts. Keep this allowlist
    # narrow: stale RGDS controls and paths must still be removed, and the
    # stock frontend's clean_environment() must never inherit our diagnostics.
    for key in (
        "RGDS_LAUNCH_PROFILE", "RGDS_MELONDS_PROFILE", "RGDS_MELONDS_PRESENT_EVERY_FRAME",
        "RGDS_MELONDS_POSIX_SEMAPHORE"
    ):
        if os.environ.get(key) == "1":
            env[key] = "1"
    if standalone:
        from linux_platform import environment as linux_environment
        return linux_environment(env)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = PYCACHE
    env.pop("LD_PRELOAD", None)
    env.pop("SDL_VIDEODRIVER", None)
    env["SDL_TOUCH_MOUSE_EVENTS"] = "0"
    env["SDL_MOUSE_TOUCH_EVENTS"] = "0"
    env["SDL_VIDEO_DISPLAY_PRIORITY"] = "DSI-1,DSI-2"
    from device_profile import stock_environment
    stock_environment(env)
    from shared_audio import configure as configure_audio
    configure_audio(app, env)
    print(
        'Device profile: ' + env['RGDS_DEVICE_MODEL'] + '; touch=' + env.get('RGDS_TOUCH_DEV', 'auto') +
        '; controller=' + env.get('NNDDSS_KEYS_DEV', 'unresolved'),
        flush=True
    )
    existing = env.get("XDG_RUNTIME_DIR")
    directories = ([Path(existing)]
                   if existing else []) + [Path(p) for p in ("/run/user/0", "/var/run", "/run", "/tmp")]
    for directory in directories:
        for path in sorted(directory.glob("wayland-*")):
            if path.is_socket():
                env["XDG_RUNTIME_DIR"] = str(directory)
                env["WAYLAND_DISPLAY"] = path.name
                env["SDL_VIDEODRIVER"] = "wayland"
                break
        if env.get("SDL_VIDEODRIVER"):
            break
    if not env.get("SDL_VIDEODRIVER"):
        raise ValueError("No Weston Wayland socket found. Run the inventory from stock APPS first.")
    for directory in (
        Path("/mnt/vendor/deep/drastic_aarch64/libs"), Path("/mnt/vendor/deep/drastic64/libs"), Path("/usr/lib"),
        Path("/usr/lib/aarch64-linux-gnu"), Path("/lib/aarch64-linux-gnu")
    ):
        for name in ("libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"):
            if (directory / name).is_file():
                env["RGDS_SDL_LIBRARY"] = str(directory / name)
                env["LD_LIBRARY_PATH"] = str(directory) + ":" + env.get("LD_LIBRARY_PATH", "")
                return env
    raise ValueError("Vendor SDL2 library not found. The inventory will identify its path.")


def precompile(app, env):
    """Compile the runtime into PYCACHE in the background, at the lowest CPU priority."""
    try:
        subprocess.Popen(
            [sys.executable, "-m", "compileall", "-q", str(app / "runtime")],
            env=dict(env, PYTHONPYCACHEPREFIX=PYCACHE),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            preexec_fn=lambda: os.nice(19)
        )
    except OSError as error:
        print("RGDS bytecode cache: " + str(error), file=sys.stderr, flush=True)


def release(root, boot=False, receipt=None):
    from activate import read_pointer, verify_release
    build_id = read_pointer(root / "state/active")
    if not build_id:
        raise ValueError("No active development build. Copy the complete SD package.")
    app = root / "releases" / build_id
    if boot:
        from activate import verify_startup, verify_startup_receipt
        if receipt is None: verify_startup(app)
        else: verify_startup_receipt(app, receipt)
    else: verify_release(app)
    return app


def kernel_uptime():
    """Linux uptime includes suspend; it starts after the bootloader hands off."""
    clock = getattr(time, "CLOCK_BOOTTIME", None)
    if clock is not None:
        try:
            return time.clock_gettime(clock)
        except (OSError, ValueError):
            pass
    try:
        return float(Path("/proc/uptime").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return None


class BootWork:
    """The guardian owns work that must wait for the launcher's first frame."""
    def __init__(self, app, data, ready_fd, network_config=None, receipt=None, began=None):
        self.app = Path(app)
        self.data = Path(data)
        self.ready_fd = ready_fd
        self.network_config = network_config
        self.receipt = receipt or {}
        self.began = began if began is not None else time.monotonic()
        self.process = None
        # Deferred maintenance after the first frame, each at idle priority:
        # a daily re-hash of memoized startup files (activate.audit) and RGDS's
        # weekly SD trim in place of stock's boot/hourly fstrim (system_tuning).
        self.jobs = []
        self.pending_jobs = []
        # A failed new display/worker must not expose a previous boot as ready.
        for name in ("boot-startup.json", "boot-network.json"):
            try:
                (self.data / "session" / name).unlink(missing_ok=True)
            except OSError:
                pass

    def displayed(self):
        signal = os.read(self.ready_fd, 16)
        os.close(self.ready_fd)
        self.ready_fd = None
        if signal != b"1": return  # EOF is a failed/early launcher, never readiness.
        now = time.monotonic()
        timings = dict(
            schema=1,
            build=self.app.name,
            session_to_first_frame_seconds=round(now - self.began, 3),
            timing_source="guardian_first_present_ack"
        )
        uptime = kernel_uptime()
        if uptime is not None: timings["kernel_boot_to_first_frame_seconds"] = round(uptime, 3)
        entry = self.receipt.get("began")
        if isinstance(entry, (int, float)) and entry <= self.began:
            timings["hook_to_first_frame_seconds"] = round(now - entry, 3)
            timings["verify_seconds"] = self.receipt.get("verify_seconds")
        for name in ("dispatch_mode", "session_import_seconds"):
            if name in self.receipt: timings[name] = self.receipt[name]
        try:
            (self.data / "session/boot-startup.json").write_text(json.dumps(timings) + "\n")
        except OSError:
            pass
        runtime = self.app / "runtime"
        self.pending_jobs = [
            (
                now + 60, [
                    sys.executable, "-B",
                    str(runtime / "activate.py"), "audit", "--root",
                    str(self.app.parent.parent), "--build", self.app.name
                ]
            )
        ]
        if self.network_config is not None:
            self.pending_jobs.append((now + 180, [sys.executable, "-B", str(runtime / "system_tuning.py"), "maintain"]))
        if self.network_config is not None:
            try:
                self.process = subprocess.Popen(
                    [
                        sys.executable, "-B",
                        str(self.app / "runtime/boot_network.py"), "--deferred",
                        str(self.network_config), "--result",
                        str(self.data / "session/boot-network.json")
                    ],
                    env=clean_environment(),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True
                )
            except OSError as error:
                print("RGDS deferred network: " + str(error), file=sys.stderr, flush=True)

    def poll(self):
        if self.process is not None and self.process.poll() is not None: self.process = None
        self.jobs = [job for job in self.jobs if job.poll() is None]
        # One maintenance job at a time; each yields the card to the other.
        if self.pending_jobs and not self.jobs and time.monotonic() >= self.pending_jobs[0][0]:
            _due, command = self.pending_jobs.pop(0)
            try:
                self.jobs.append(
                    subprocess.Popen(
                        command,
                        env=clean_environment(),
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        start_new_session=True
                    )
                )
            except OSError as error:
                print("RGDS maintenance: " + str(error), file=sys.stderr, flush=True)

    def close(self):
        for job in self.jobs:
            if job.poll() is None:
                try:
                    job.kill()
                except ProcessLookupError:
                    pass
                try:
                    job.wait(timeout=.5)
                except subprocess.TimeoutExpired:
                    pass
        self.jobs = []
        self.pending_jobs = []
        if self.ready_fd is not None:
            os.close(self.ready_fd)
            self.ready_fd = None
        if self.process is None: return
        if self.process.poll() is None:
            try:
                self.process.terminate()
            except ProcessLookupError:
                pass
            try:
                self.process.wait(timeout=.5)
            except subprocess.TimeoutExpired:
                try:
                    self.process.kill()
                except ProcessLookupError:
                    pass
                try:
                    self.process.wait(timeout=.5)
                except subprocess.TimeoutExpired:
                    pass
        self.process = None


def read_startup_receipt(descriptor):
    with os.fdopen(descriptor, "rb") as stream:
        payload = stream.read(4 * 1024 * 1024 + 1)
    if len(payload) > 4 * 1024 * 1024: raise ValueError("Startup verification receipt is too large.")
    receipt = json.loads(payload)
    if not isinstance(receipt, dict): raise ValueError("Invalid startup verification receipt.")
    return receipt


def lock():
    try:
        LOCK.mkdir()
    except FileExistsError:
        owner = LOCK / "owner.json"
        try:
            record = read_record(owner)
            if record is None: raise ValueError("Session owner missing")
        except (OSError, ValueError):
            raise ValueError("Session lock has no readable owner. Reboot stock before trying again.")
        if record["start"] and process_identity(record["pid"]) == record["start"]:
            raise ValueError("An RGDS session is already running.")
        # The guardian may still own recovery after its parent has died. Do not
        # replace its lock or let it later resume stock during a new session.
        raise ValueError("Previous session recovery is still pending. Retry shortly; reboot stock if the lock remains.")
    write_record(LOCK / "owner.json", {"pid": os.getpid(), "start": process_identity(os.getpid())})


def unlock():
    (LOCK / "owner.json").unlink(missing_ok=True)
    try:
        LOCK.rmdir()
    except OSError:
        pass


def rom_roots(root, cards=(Path("/mnt/mmc"), Path("/mnt/SDCARD"))):
    """ROM storage stays on the cards even when RGDS runs from internal storage."""
    root = Path(root)
    candidates = [root.parent / "Roms/NDS", root / "games"]
    for card in cards:
        candidates.extend((Path(card) / "Roms/NDS", Path(card) / "rgds/games"))
    seen = set()
    result = []
    for path in candidates:
        identity = str(path.resolve())
        if identity not in seen:
            seen.add(identity)
            result.append(path)
    return result


def ensure_writable_data(data):
    """Fail before taking stock input/audio when userdata cannot be updated."""
    data = Path(data)
    pending = None
    try:
        data.mkdir(parents=True, exist_ok=True)
        descriptor, pending = tempfile.mkstemp(prefix=".rgds-write-check-", dir=data)
        os.close(descriptor)
    except OSError as error:
        raise ValueError(
            "RGDS userdata is not writable at " + str(data) +
            ". Use the writable internal test installation; the SD card may be mounted read-only."
        ) from error
    finally:
        if pending is not None:
            Path(pending).unlink(missing_ok=True)


def _read_small(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        return os.read(descriptor, 1024)
    finally:
        os.close(descriptor)


def group_members(owned, proc=Path("/proc")):
    # The guardian samples this every second for the whole session. Raw reads
    # keep a ~300-process scan to a few syscalls each instead of pathlib's
    # buffered text stack, which alone cost ~8% of a Cortex-A55 core at idle.
    members = []
    leader = owned["pid"]
    folder = os.fspath(proc)
    try:
        names = os.listdir(folder)
    except OSError:
        return members
    for name in names:
        if not name.isdigit(): continue
        try:
            state = _read_small(folder + "/" + name + "/stat").rsplit(b")", 1)[1].split()
            if state[0] == b"Z" or int(state[2]) != leader: continue
            protected = _read_small(folder + "/" + name + "/comm").decode(errors="replace"
                                                                          ).strip().lower() in PROTECTED_SERVICES
            if not protected:
                members.append({"pid": int(name), "start": state[19].decode()})
        except (OSError, IndexError, ValueError):
            continue
    members.sort(key=lambda record: record["pid"])
    return members


def owned_group_alive(owned, members):
    """A reused leader PID never grants ownership of its new process group."""
    current = process_identity(owned["pid"])
    if current is not None and current != owned["start"]: return False
    for record in members:
        try:
            if not protected_process(record) and process_identity(record["pid"]) == record["start"] and os.getpgid(
                record["pid"]
            ) == owned["pid"]:
                return True
        except ProcessLookupError:
            pass
    return False


def protected_process(record):
    try:
        return (Path("/proc") / str(record["pid"]) / "comm").read_text().strip().lower() in PROTECTED_SERVICES
    except OSError:
        return True


def stop_owned_group(owned, members, data, timeout=7):
    if not owned or not owned.get("start"): return
    if process_identity(owned["pid"]) == owned["start"]:
        members = group_members(owned)
    try:
        core = read_record(Path(data) / "session/core.json")
        if core and process_identity(core["pid"]) == core["start"] and os.getpgid(core["pid"]) == owned["pid"]:
            members.append(core)
    except (OSError, ValueError):
        pass
    if not owned_group_alive(owned, members): return
    # A surviving recorded member anchors this private group after its leader
    # exits, allowing us to include helpers born since the last guardian sample.
    members = list({(record["pid"], record["start"]): record for record in members + group_members(owned)}.values())

    def signal_members(signum):
        for record in reversed(members):
            if process_identity(record["pid"]) != record["start"]: continue
            if protected_process(record): continue
            try:
                os.kill(record["pid"], signum)
            except ProcessLookupError:
                pass

    signal_members(signal.SIGTERM)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        live = [record for record in members if frontend_alive(record)]
        if not owned_group_alive(owned, live): return
        time.sleep(.05)
    if owned_group_alive(owned, members):
        signal_members(signal.SIGKILL)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if not any(not protected_process(record) and frontend_alive(record) for record in members): return
            time.sleep(.05)
        raise RecoveryPending("An owned worker is still stopping; its session lock was retained.")


def recover_display(label):
    """Restart Weston when a direct-display game stopped it and then died."""
    try:
        from direct_display import recover
        seconds = recover()
        if seconds is not None: print(label + ": Weston restarted in %.2f s" % seconds, file=sys.stderr, flush=True)
        return True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(label + ": " + str(error), file=sys.stderr, flush=True)
        return False


def recover_hardware(app, data):
    """Bound recovery work; an unfinished recovery leaves its journal intact."""
    helper = subprocess.Popen(
        [sys.executable, "-B",
         str(app / "runtime/system_controls.py"), "restore", "--data",
         str(data)],
        env=clean_environment(),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        start_new_session=True
    )
    try:
        _output, error = helper.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(helper.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            helper.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            write_record(LOCK / "recovery.json", dict(pid=helper.pid, start=process_identity(helper.pid) or "0"))
            raise RecoveryPending("Hardware recovery is still stopping; its session lock was retained.")
        raise ValueError("Hardware recovery timed out; its journal was retained.")
    if helper.returncode:
        raise ValueError("Hardware recovery failed: " + error.decode(errors="replace")[-2048:].strip())


def main(argv=None, startup_receipt=None):
    began = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--dev", action="store_true", help="Use the writable live theme and remember menu selection")
    parser.add_argument("--boot", action="store_true", help="Run as the stock supervisor's foreground boot hook")
    parser.add_argument(
        "--standalone",
        action="store_true",
        help="Run under the independent Linux service, without stock frontend ownership"
    )
    parser.add_argument(
        "--rom-dir", type=Path, action="append", default=[], help="Explicit ROM directory for standalone Linux"
    )
    parser.add_argument(
        "--boot-warning", action="store_true", help="Show the launcher's optional touch-to-continue boot screen"
    )
    parser.add_argument("--startup-fd", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--boot-network-config", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--core", choices=CORES, help="Device validation: launch this engine under the normal recovery guardian"
    )
    parser.add_argument("--rom", type=Path)
    args = parser.parse_args(argv)
    if args.standalone:
        if args.boot_network_config is not None:
            raise ValueError("Standalone sessions cannot run the stock boot-network worker.")
        from stock_handoff import supervisors
        if discover_frontends(include_stopped=True) or supervisors():
            raise ValueError("A stock frontend or supervisor is active; standalone RGDS will not take it over.")
        args.boot = True
    elif args.rom_dir:
        raise ValueError("Explicit ROM directories require --standalone.")
    if (
        args.startup_fd is not None or args.boot_network_config is not None or startup_receipt is not None
    ) and not args.boot:
        raise ValueError("Private startup options require a managed boot session.")
    if startup_receipt is not None and args.startup_fd is not None:
        raise ValueError("Use only one startup verification receipt.")
    if args.startup_fd is not None and args.startup_fd < 3: raise ValueError("Invalid startup verification descriptor.")
    if sys.platform != "linux":
        raise ValueError("Device sessions run on Linux. Use make preview on the desktop.")
    root = args.root.resolve()
    receipt = startup_receipt if startup_receipt is not None else read_startup_receipt(
        args.startup_fd
    ) if args.startup_fd is not None else None
    app = release(root, boot=args.boot) if receipt is None else release(root, boot=args.boot, receipt=receipt)
    if not args.standalone: recover_display("RGDS display")
    env = environment(app, standalone=True) if args.standalone else environment(app)
    data = root / "userdata"
    if args.boot and not args.core and not args.probe:
        # A games card the kernel switched to read-only breaks every action at
        # once; check it now (the launcher explains, the check reboots).
        from card_repair import STARTED, read_only, start
        if read_only(data) and start(data)[0] == STARTED:
            return subprocess.call(
                [str(app / "bin/rgds-launcher"), "--app",
                 str(app), "--data",
                 str(data), "--card-repair"], env=env
            )
    ensure_writable_data(data)
    if args.boot and not os.environ.get('RGDS_UPDATE_TOKEN'):
        from quick_resume import consume
        consume(data)
    if args.standalone and not args.boot_warning:
        from fields import read_fields
        args.boot_warning = read_fields(data / 'boot.ini').get('warning', '0') == '1'
    lock()
    # Finish a previous hardware recovery before starting another session.
    try:
        Hardware(data).restore()
    except (OSError, ValueError):
        unlock()
        raise
    env["RGDS_SYSTEM_MANAGED"] = "1"
    if os.environ.get("RGDS_UPDATE_TOKEN"):
        env["RGDS_UPDATE_TOKEN"] = os.environ["RGDS_UPDATE_TOKEN"]
    owner = {"pid": os.getpid(), "start": process_identity(os.getpid())}
    lease = None
    read_fd = write_fd = ready_read = ready_write = None
    lid_sensor = None
    try:
        records = discover_frontends(include_stopped=args.boot)
        if args.boot and records:
            raise ValueError("Boot hook found an existing stock frontend; returning control to stock.")
        if not args.boot: lease = acquire_stock(owner, marker=STOCK_STOP)
        begin_session(data, owner)
        read_fd, write_fd = os.pipe()
        if args.boot and not args.core:
            reader, writer = socket.socketpair()
            ready_read, ready_write = reader.detach(), writer.detach()
        if not args.standalone:
            from lid_sensor import LidSensor
            lid_sensor = LidSensor()
            if lid_sensor.path: env["RGDS_LID_STATE"] = str(lid_sensor.path)
        guardian = os.fork()
    except BaseException:
        if lid_sensor: lid_sensor.close()
        for descriptor in (read_fd, write_fd, ready_read, ready_write):
            if descriptor is not None: os.close(descriptor)
        try:
            if lease: release_stock_flag(lease)
        finally:
            try:
                end_session(data, owner)
            finally:
                unlock()
        raise
    if guardian == 0:
        os.close(write_fd)
        if ready_write is not None: os.close(ready_write)
        boot_work = BootWork(
            app, data, ready_read, args.boot_network_config, receipt, began
        ) if ready_read is not None else None
        # Closing the parent's descriptor (including SIGKILL) releases the stock UI.
        # This guardian does not inherit the launcher's process group.
        os.setsid()

        def interrupted(_signum, _frame):
            raise InterruptedError("Session recovery requested")

        for signum in (signal.SIGINT, signal.SIGTERM):
            signal.signal(signum, interrupted)
        messages = GuardianMessages()
        dispatch_status = 0
        owned = None
        members = []
        recovery_ok = True
        recovery_pending = False
        members_at = 0
        try:
            while True:
                descriptors = [read_fd]
                if boot_work and boot_work.ready_fd is not None: descriptors.append(boot_work.ready_fd)
                # Hall edges should wake the guardian immediately, not wait
                # behind the housekeeping timer.
                if lid_sensor and lid_sensor.fd >= 0: descriptors.append(lid_sensor.fd)
                # Inputs, launcher messages and the readiness socket all wake
                # select directly. The timeout only paces the lid heartbeat
                # (published each second, stale after three) and member samples.
                ready = select.select(descriptors, [], [], .5)[0]
                if boot_work and boot_work.ready_fd in ready: boot_work.displayed()
                if boot_work: boot_work.poll()
                if lid_sensor: lid_sensor.poll()
                if read_fd in ready:
                    chunk = os.read(read_fd, 1024)
                    if not chunk:
                        if messages.pending: raise ValueError("Incomplete guardian message")
                        break
                    messages.feed(chunk)
                    owned = messages.owner
                if time.monotonic() >= members_at and owned and owned.get("start") and process_identity(
                    owned["pid"]
                ) == owned["start"]:
                    members = group_members(owned)
                    members_at = time.monotonic() + 1
        except (OSError, ValueError):
            recovery_ok = False
        finally:
            os.close(read_fd)
            if lid_sensor: lid_sensor.close()
            # A real stock shutdown must not wait on a guardian ignoring TERM.
            for signum in (signal.SIGINT, signal.SIGTERM):
                signal.signal(signum, signal.SIG_DFL)
            if boot_work: boot_work.close()
            # Stop our orphaned process group before giving stock its input back.
            try:
                stop_owned_group(owned, members, data)
            except RecoveryPending as error:
                recovery_ok = False
                recovery_pending = True
                print("RGDS worker recovery: " + str(error), file=sys.stderr, flush=True)
            except (OSError, ValueError, KeyError):
                recovery_ok = False
            # A direct-display game stops Weston; it must be back before stock
            # takes the frontend again. A still-stopping engine may hold DRM
            # master, so then the next session retries instead.
            if not recovery_pending and not args.standalone and not recover_display("RGDS display recovery"):
                recovery_ok = False
            # Workers must finish first so an in-flight enable cannot restart
            # the separately owned SMB group after this final stop.
            try:
                from file_sharing import Sharing
                Sharing(data).stop()
            except (OSError, ValueError) as error:
                recovery_ok = False
                print("RGDS file sharing recovery: " + str(error), file=sys.stderr, flush=True)
            if not recovery_pending:
                try:
                    recover_hardware(app, data)
                except RecoveryPending as error:
                    recovery_ok = False
                    recovery_pending = True
                    print("RGDS hardware recovery: " + str(error), file=sys.stderr, flush=True)
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    # Preserve the journal for the next launch to retry recovery.
                    recovery_ok = False
                    print("RGDS hardware recovery: " + str(error), file=sys.stderr, flush=True)
            try:
                dispatch_status = finish_handoff(
                    records, lease, messages.action if recovery_ok else None, data, args.standalone
                )
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                recovery_ok = False
                print("RGDS stock recovery: " + str(error), file=sys.stderr, flush=True)
            finally:
                if not recovery_pending: unlock()
            os._exit(dispatch_status if recovery_ok else 1)
    os.close(read_fd)
    # Only the guardian retains the input FD and owns the temporary state.
    if lid_sensor and lid_sensor.fd >= 0: os.close(lid_sensor.fd)
    if ready_read is not None: os.close(ready_read)
    child = None

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    terminal_action = None
    result = 143
    guardian_ok = False
    try:
        if not args.boot: stop_frontends(records)
        if args.core:
            if not args.rom or not args.rom.is_file(): raise ValueError("--core needs an existing --rom")
            command = [
                "python3", "-B",
                str(app / "runtime/emulators.py"), "launch", "--app",
                str(app), "--data",
                str(data), "--core", args.core, "--rom",
                str(args.rom)
            ]
        else:
            command = [str(app / "bin/rgds-launcher"), "--app", str(app), "--data", str(data)]
            if args.standalone and env.get('RGDS_TOP_DISPLAY') and env.get('RGDS_BOTTOM_DISPLAY'):
                command += ['--top-display', env['RGDS_TOP_DISPLAY'], '--bottom-display', env['RGDS_BOTTOM_DISPLAY']]
            if args.boot: command.append("--boot-startup")
            if args.boot_warning: command.append("--boot-warning")
            if ready_write is not None: command += ["--boot-ready-fd", str(ready_write)]
        if args.dev and not args.core:
            live = data / "dev/theme.ini"
            live.parent.mkdir(parents=True, exist_ok=True)
            if not live.exists():
                live.write_bytes((app / "themes/default/light.ini").read_bytes())
            command += ["--theme", str(live), "--watch-assets", "--state", str(data / "dev/menu-state.txt")]
        if args.core:
            pass
        elif args.probe:
            command += ["--probe", "--seconds", "90"]
        else:
            command += ["--allow-launch"]
            for path in (args.rom_dir or [Path("/userdata/roms/nds"),
                                          Path("/userdata/roms/dsi")]) if args.standalone else rom_roots(root):
                command += ["--rom-dir", str(path)]
        log_path = data / "session.log"
        with log_path.open("a", buffering=1) as log:
            log.write("\nRGDS session: " + app.name + "\n")
            child = subprocess.Popen(
                command,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                pass_fds=(ready_write,) if ready_write is not None else ()
            )
            if not args.standalone: precompile(app, env)
            if ready_write is not None:
                os.close(ready_write)
                ready_write = None
            os.write(write_fd, (json.dumps({"pid": child.pid, "start": process_identity(child.pid)}) + "\n").encode())
            write_record(data / "session/launcher.json", {"pid": child.pid, "start": process_identity(child.pid)})
            result = child.wait()
            if result and result != SYSTEM_EXIT_ACK:
                print("RGDS returned an error. Read " + str(log_path))
    except KeyboardInterrupt:
        result = 143
    finally:
        if ready_write is not None: os.close(ready_write)
        if child and child.poll() is None:
            try:
                os.kill(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=7)
            except subprocess.TimeoutExpired:
                try:
                    os.kill(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    result = 1
                    print(
                        "RGDS launcher is still stopping; recovery owns its remaining workers.",
                        file=sys.stderr,
                        flush=True
                    )
        (data / "session/launcher.json").unlink(missing_ok=True)
        try:
            requested = consume_exit(data, owner)
            if result == SYSTEM_EXIT_ACK: terminal_action = requested
            elif requested: print("RGDS discarded an unacknowledged system request.", file=sys.stderr, flush=True)
        except (OSError, ValueError) as error:
            print("RGDS exit request: " + str(error), file=sys.stderr, flush=True)
        if terminal_action:
            # Recovery and init dispatch both belong to the guardian. Keep the
            # stop flag held throughout, instead of briefly launching stock UI.
            for signum in (signal.SIGINT, signal.SIGTERM):
                signal.signal(signum, signal.SIG_DFL)
            os.write(write_fd, (json.dumps({"action": terminal_action}) + "\n").encode())
        os.close(write_fd)
        _pid, guardian_status = os.waitpid(guardian, 0)
        guardian_ok = os.WIFEXITED(guardian_status) and os.WEXITSTATUS(guardian_status) == (
            SYSTEM_DISPATCH_ACK if terminal_action else 0
        )
        end_session(data, owner)
    if terminal_action:
        if not guardian_ok: raise ValueError("Stock recovery did not finish; system action was not dispatched.")
        for signum in (signal.SIGINT, signal.SIGTERM):
            signal.signal(signum, signal.SIG_DFL)
        result = 0
    elif result == SYSTEM_EXIT_ACK:
        raise ValueError("The launcher acknowledged a system action without a committed request.")
    return result if guardian_ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("RGDS session: " + str(error), file=sys.stderr)
        sys.exit(1)
