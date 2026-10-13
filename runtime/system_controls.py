#!/usr/bin/env python3
"""Session-scoped RG DS hardware controls. Never writes stock configuration files.

CPU/devfreq ranges come exclusively from the driver's advertised frequencies.
Every persistent hardware change is journalled before writing so the session
guardian can return stock to its original settings, even after launcher SIGKILL.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import subprocess
import shutil
import fcntl
import functools
import threading
import time
import uuid
from contextlib import contextmanager

from audio import Volume

SLEEP_HOOK=Path('/usr/lib/systemd/system-sleep/90-rgds-resume')


def suspend_linux(command, runtime=Path('/run/rgds'), clock=time.monotonic, sleep=time.sleep):
    """Wait for this request's post-resume hook, not just logind's enqueue ACK."""
    from stock_handoff import clean_environment
    runtime=Path(runtime);request=runtime/'suspend-request';state=runtime/'suspend-state'
    if not runtime.is_dir():raise ValueError('OS sleep acknowledgement is unavailable')
    token=uuid.uuid4().hex
    pending=request.with_suffix('.pending');pending.write_text(token+'\n');pending.replace(request)
    try:
        subprocess.run(command,env=clean_environment(),cwd='/',stdin=subprocess.DEVNULL,check=True)
        deadline=clock()+15;started=False
        while True:
            try:fields=dict(line.split('=',1) for line in state.read_text().splitlines() if '=' in line)
            except OSError:fields={}
            if fields.get('token')==token:
                if fields.get('phase')=='post':return
                if fields.get('phase')=='pre':started=True
            if not started and clock()>=deadline:
                raise ValueError('The OS did not acknowledge suspend; check its sleep log')
            # Monotonic time stops while suspended. Once pre is observed, do
            # not bound the owner's sleep duration or restore controls early.
            sleep(.1)
    finally:
        try:
            if request.read_text().strip()==token:request.unlink()
        except FileNotFoundError:pass


def battery_status(sysfs):
    """Keep charge level, active charging and an attached supply distinct."""
    result=dict(battery=-1,charging=0,external_power=0)
    battery=None;online=[]
    for folder in sorted((Path(sysfs)/'class/power_supply').glob('*')):
        try: kind=(folder/'type').read_text().strip()
        except OSError: continue
        if kind=='Battery' and battery is None: battery=folder
        elif kind in ('Mains','Wireless') or kind.startswith('USB'):
            try: value=(folder/'online').read_text().strip()
            except OSError: continue
            if value in ('0','1'): online.append(value=='1')
    if battery is None:return result
    try: status=(battery/'status').read_text().strip()
    except OSError: status='Unknown'
    plugged=any(online) if online else status in ('Charging','Full')
    result.update(charging=int(status=='Charging' and plugged),external_power=int(plugged))
    try:
        capacity=int((battery/'capacity').read_text().strip())
        if 0<=capacity<=100:result['battery']=capacity
    except (OSError,ValueError):
        # Some power-supply drivers expose counters rather than capacity.
        for name in ('charge','energy'):
            try:
                now=int((battery/(name+'_now')).read_text().strip())
                full=int((battery/(name+'_full')).read_text().strip())
                if full>0 and 0<=now<=full:result['battery']=round(now*100/full);break
            except (OSError,ValueError):pass
    return result


def serialized(method):
    @functools.wraps(method)
    def run(self, *args, **kwargs):
        with self.transaction():
            return method(self, *args, **kwargs)
    return run


class Hardware:
    def __init__(self, data, sysfs=Path("/sys")):
        self.sysfs = Path(sysfs)
        self.journal = Path(data) / "session/system-hardware.json"
        self.lid_generation = None
        self._mutex = threading.RLock()
        self._transaction_depth = 0

    @contextmanager
    def transaction(self):
        # Helpers run in separate processes, including the recovery guardian.
        # Keep one inode: unlinking a lock could give two writers different locks.
        with self._mutex:
            if self._transaction_depth:
                yield
                return
            self.journal.parent.mkdir(parents=True, exist_ok=True)
            with self.journal.with_suffix(".lock").open("a") as lock:
                deadline = time.monotonic() + 2
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if time.monotonic() >= deadline:
                            raise ValueError("Hardware change is still in progress; try again shortly.")
                        time.sleep(.02)
                self._transaction_depth = 1
                try:
                    yield
                finally:
                    self._transaction_depth = 0
                    fcntl.flock(lock, fcntl.LOCK_UN)

    @staticmethod
    def suspend_helper():
        from linux_platform import independent
        if independent():
            return shutil.which("systemctl", path="/usr/bin:/bin")
        # pm-utils runs the installed vendor hooks around kernel suspend.
        # A raw /sys/power/state write silently skips those hooks.
        return shutil.which("pm-suspend", path="/usr/sbin:/usr/bin:/sbin:/bin")

    @staticmethod
    def read(path):
        return path.read_text().strip()

    @staticmethod
    def writable(path):
        return path.is_file() and os.access(path, os.W_OK)

    def backlights(self):
        result = []
        for folder in sorted((self.sysfs / "class/backlight").glob("*")):
            try:
                maximum = int(self.read(folder / "max_brightness"))
                if maximum > 0 and self.writable(folder / "brightness"):
                    result.append((folder / "brightness", maximum))
            except (OSError, ValueError):
                continue
        return result

    def clocks(self):
        cpu = self.sysfs / "devices/system/cpu"
        folders = sorted((cpu / "cpufreq").glob("policy*")) or sorted(cpu.glob("cpu*/cpufreq"))
        folders += [p for p in sorted((self.sysfs / "class/devfreq").glob("*"))
                    if "gpu" in p.name or p.name == "dmc"]
        result, seen = [], set()
        for folder in folders:
            if folder.resolve() in seen:
                continue
            seen.add(folder.resolve())
            cpu_policy = "cpufreq" in folder.parts
            prefix = "scaling_" if cpu_policy else ""
            frequencies_file = "scaling_available_frequencies" if cpu_policy else "available_frequencies"
            governors_file = "scaling_available_governors" if cpu_policy else "available_governors"
            minimum = folder / (prefix + "min_freq")
            maximum = folder / (prefix + "max_freq")
            governor = folder / (prefix + "governor")
            try:
                frequencies = sorted(set(int(v) for v in self.read(folder / frequencies_file).split()))
                governors = self.read(folder / governors_file).split()
                if not frequencies or frequencies[0] <= 0 or not governors:
                    continue
                if not all(self.writable(p) for p in (minimum, maximum, governor)):
                    continue
                # Only offer profiles when both a dynamic and performance governor exist.
                dynamic = next((g for g in ("schedutil", "ondemand", "simple_ondemand", "conservative")
                                if g in governors), None)
                if dynamic and "performance" in governors:
                    result.append((minimum, maximum, governor, frequencies, dynamic, cpu_policy))
            except (OSError, ValueError):
                continue
        return result

    def sleep_supported(self):
        path = self.sysfs / "power/state"
        try:
            from linux_platform import independent
            if independent() and self.sysfs==Path('/sys') and not (SLEEP_HOOK.is_file() and os.access(SLEEP_HOOK,os.X_OK)):
                return False
            return bool(self.suspend_helper()) and self.writable(path) and "mem" in self.read(path).split()
        except OSError:
            return False

    def lid_supported(self):
        from linux_platform import independent
        if independent():
            device=os.environ.get('RGDS_LID_DEV');wake=os.environ.get('RGDS_LID_WAKEUP')
            return bool(device and wake and str(Path(wake)).startswith(str(self.sysfs)+'/')
                        and Path(device).exists() and self.writable(Path(wake)) and self.sleep_supported())
        from lid_sensor import read_state
        wake = self.sysfs / "devices/platform/anbernic_misc/power/wakeup"
        return self.sleep_supported() and read_state() is not None and self.writable(wake)

    def lid_closed(self):
        """None means unreadable/unknown, never an open edge."""
        from linux_platform import independent
        self.lid_generation = None
        try:
            if independent():
                path = os.environ.get('RGDS_LID_DEV')
                if not path:
                    return None
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                try:
                    bits = bytearray(8)
                    fcntl.ioctl(fd, 0x8008451b, bits)  # EVIOCGSW(8), SW_LID = 0
                    return bool(bits[0] & 1)
                finally:
                    os.close(fd)
            from lid_sensor import read_snapshot
            value, self.lid_generation = read_snapshot()
            return bool(value) if value in (0, 1) else None
        except (OSError, UnicodeError):
            return None

    def status(self):
        result = {"brightness_supported": int(bool(self.backlights())),
                  "power_supported": int(any(c[-1] for c in self.clocks())),
                  "sleep_supported": int(self.sleep_supported()), "lid_supported": int(self.lid_supported()),
                  "brightness": 60, "battery": -1, "charging": 0}
        result.update(volume_supported=0, volume=0)
        result["poweroff_supported"] = int(self.sysfs == Path("/sys") and bool(shutil.which("poweroff")))
        if self.sysfs == Path("/sys"):
            try:
                result["volume"] = Volume(self.journal.parent.parent).status()
                result["volume_supported"] = 1
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
        levels = []
        for path, maximum in self.backlights():
            try:
                levels.append(round(int(self.read(path)) * 100 / maximum))
            except (OSError, ValueError):
                pass
        if levels:
            result["brightness"] = max(5, min(100, round(sum(levels) / len(levels))))
        result.update(battery_status(self.sysfs))
        return result

    def saved(self):
        if not self.journal.exists():
            return {}
        values = json.loads(self.journal.read_text())
        if not isinstance(values, dict):
            raise ValueError("Invalid hardware recovery journal")
        if "schema" in values:
            if values.get("schema") != 2 or not isinstance(values.get("values"), dict) or \
                    not (values.get("boot_id") is None or isinstance(values.get("boot_id"), str)):
                raise ValueError("Invalid hardware recovery journal")
            boot = self.boot_id()
            if boot and values.get("boot_id") and values["boot_id"] != boot:
                # A hard reset already reset sysfs. Replaying a previous boot's
                # clocks/wake/brightness would overwrite fresh vendor defaults.
                return {}
            values = values["values"]
        for name, value in values.items():
            path = Path(name)
            if path.is_absolute() or ".." in path.parts or not isinstance(value, str):
                raise ValueError("Invalid hardware recovery entry")
            if path.name not in ("brightness", "scaling_min_freq", "scaling_max_freq", "scaling_governor",
                                 "min_freq", "max_freq", "governor", "wakeup"):
                raise ValueError("Unknown hardware recovery control")
        return values

    @staticmethod
    def boot_id():
        try:return Path("/proc/sys/kernel/random/boot_id").read_text().strip() or None
        except OSError:return None

    def save_journal(self, values):
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        pending = self.journal.with_suffix(".tmp")
        with pending.open("w") as file:
            json.dump(dict(schema=2, boot_id=self.boot_id(), values=values), file)
            file.flush()
            os.fsync(file.fileno())
        pending.replace(self.journal)
        descriptor = os.open(self.journal.parent, os.O_RDONLY)
        try:os.fsync(descriptor)
        finally:os.close(descriptor)

    def restore_values(self, values):
        errors = []
        # Lower minimums first so restoring an earlier maximum is always legal.
        for name, value in values.items():
            if Path(name).name in ("min_freq", "scaling_min_freq"):
                path = self.sysfs / name
                try:
                    path.write_text(str(min(int(value), int(self.read(path)))) + "\n")
                except (OSError, ValueError) as error:
                    errors.append(f"{name}: {error}")
        for minimums in (False, True):
            for name, value in values.items():
                if (Path(name).name in ("min_freq", "scaling_min_freq")) == minimums:
                    try:
                        (self.sysfs / name).write_text(value + "\n")
                    except OSError as error:
                        errors.append(f"{name}: {error}")
        if errors:
            raise OSError("Hardware recovery incomplete: " + "; ".join(errors))

    @serialized
    def change(self, writes):
        if not writes or not all(self.writable(path) for path, _ in writes):
            raise ValueError("Hardware control unavailable")
        previous = {str(path.relative_to(self.sysfs)): self.read(path) for path, _ in writes}
        saved = self.saved()
        for name, value in previous.items():
            saved.setdefault(name, value)
        self.save_journal(saved)
        try:
            for path, value in writes:
                path.write_text(str(value) + "\n")
        except OSError:
            self.restore_values(previous)
            raise

    def brightness(self, percent):
        if not 5 <= percent <= 100:
            raise ValueError("Brightness must be 5–100 percent")
        self.change([(path, max(1, (maximum * percent + 50) // 100)) for path, maximum in self.backlights()])

    def volume(self, percent=None):
        Volume(self.journal.parent.parent).apply(percent)

    @serialized
    def power(self, mode):
        if mode not in range(4):
            raise ValueError("Invalid power mode")
        if mode == 0:
            self.restore_values({k: v for k, v in self.saved().items()
                                 if Path(k).name in ("scaling_min_freq", "scaling_max_freq", "scaling_governor",
                                                    "min_freq", "max_freq", "governor")})
            return
        clocks = self.clocks()
        if not any(c[-1] for c in clocks):
            raise ValueError("Power profiles unavailable")
        writes = []
        for minimum, maximum, governor, frequencies, dynamic, _ in clocks:
            index = (len(frequencies) - 1) // 2 if mode == 1 else max(0, len(frequencies) - 2) if mode == 2 else len(frequencies) - 1
            # Keep a low floor for dynamic modes. Performance holds the advertised ceiling.
            writes += [(minimum, frequencies[0]), (maximum, frequencies[index]),
                       (minimum, frequencies[index] if mode == 3 else frequencies[0]),
                       (governor, "performance" if mode == 3 else dynamic)]
        self.change(writes)

    @serialized
    def lid(self, enabled):
        if enabled:
            if not self.lid_supported():
                raise ValueError("Lid sleep unavailable")
            from linux_platform import independent
            path=Path(os.environ['RGDS_LID_WAKEUP']) if independent() else self.sysfs / "devices/platform/anbernic_misc/power/wakeup"
            self.change([(path, "enabled")])
        else:
            self.restore_values({k: v for k, v in self.saved().items() if Path(k).name == "wakeup"})

    @serialized
    def sleep_lid(self, wake_sound=True, close_sound=True):
        # Recheck after queued controls finish: opening the lid cancels a pending
        # request. Enable and journal the wake source before invoking stock hooks.
        if self.lid_closed() is not True:
            return False
        from fields import write_fields
        def record(state, **fields):
            print('Lid sleep: ' + state, flush=True)
            try:
                write_fields(self.journal.parent.parent / 'logs/lid-sleep.ini',
                             dict(state=state, time=time.time(), **fields), durable=False)
            except OSError:
                pass  # Diagnostics must never prevent suspend or recovery.
        try:
            self.lid(True)
            if self.lid_closed() is not True:
                record('cancelled-lid-open')
                return False
            record('helper-requested')
            if not self.sleep(lid=True, wake_sound=wake_sound, close_sound=close_sound):
                record('cancelled-lid-open')
                return False
            record('helper-returned', mode=getattr(self, 'sleep_mode', 'deep'))
            return True
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            record('failed', error=str(error))
            raise

    def wait_closed(self, deadline):
        """True after the grace period, False on open, None if position is lost."""
        while True:
            closed = self.lid_closed()
            if closed is not True:return closed
            remaining = deadline - time.monotonic()
            if remaining <= 0:return True
            time.sleep(min(.02, remaining))

    @serialized
    def sleep(self, lid=False, wake_sound=True, close_sound=True):
        from linux_platform import independent, system_command
        if not self.sleep_supported():
            raise ValueError("System sleep helper unavailable" if independent() else "Stock system sleep helper unavailable")
        from stock_handoff import clean_environment
        command = system_command("suspend") if independent() else [self.suspend_helper()]
        volume = Volume(self.journal.parent.parent)
        def sync_volume():
            try:
                volume.apply()
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                # An unavailable codec must not prevent sleep or hide a failed
                # suspend helper. Leave the saved level intact for a later retry.
                print(f"Sleep volume restore: {error}", file=sys.stderr, flush=True)
        data = self.journal.parent.parent
        from fields import write_fields
        timings = {}
        def mark(phase):
            timings[phase] = time.time()
            try:write_fields(data / 'logs/sleep-timing.ini', timings, durable=False)
            except OSError:pass
        self.sleep_mode = 'cancelled'
        # A brief close is a screen-off pause. Do not carry brightness=0 into
        # kernel suspend: pwm-backlight would restore that zero during resume,
        # keeping the displays dark until userspace and Wi-Fi have resumed.
        panels = {str(path.relative_to(self.sysfs)): self.read(path) for path, _ in self.backlights()} if lid else {}
        started = time.monotonic()
        mark('requested')
        try:
            if panels:self.change([(self.sysfs / name, 0) for name in panels])
            mark('screen_off')
            sync_volume()
            from sleep_sounds import play
            if close_sound:play(data, 'lid-close')
            mark('close_cue_finished')
            held = self.wait_closed(started + 5) if lid else True
            if held is None:return False
            if held:
                # Hand the normal brightness back to the driver before deep
                # sleep, so it can light the panels in its own resume phase.
                if panels:self.restore_values(panels)
                mark('deep_sleep_requested')
                self.sleep_mode = 'deep'
                # Preserve every vendor hook and never use raw kernel fallback.
                try:
                    if independent():suspend_linux(command)
                    else:subprocess.run(command, env=clean_environment(), cwd="/",
                                        stdin=subprocess.DEVNULL, check=True)
                finally:
                    mark('deep_helper_returned')
                    sync_volume()
            else:
                self.sleep_mode = 'quick'
                mark('quick_open')
        finally:
            if panels:self.restore_values(panels)
            mark('screen_restored')
        # Native menus play their cached wake cue after repainting, so its full
        # duration no longer blocks navigation. Games retain this shared owner.
        if wake_sound:play(self.journal.parent.parent, 'lid-open')
        return True

    @serialized
    def restore(self):
        self.restore_values(self.saved())
        self.journal.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "brightness", "volume", "power", "lid", "sleep", "sleep_lid", "restore"))
    parser.add_argument("value", nargs="?", type=int)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--menu-wake", action="store_true")
    parser.add_argument("--menu-close", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("Hardware controls require Linux")
    hardware = Hardware(args.data)
    if args.command == "status":
        for key, value in hardware.status().items():
            print(f"{key}={value}")
    elif args.command in ("brightness", "volume", "power", "lid"):
        if args.value is None and args.command != "volume":
            raise ValueError("Control value missing")
        if args.command == "lid" and args.value not in (0, 1):
            raise ValueError("Invalid lid setting")
        getattr(hardware, args.command)(args.value)
    elif args.command in ("sleep", "sleep_lid"):
        if not getattr(hardware, args.command)(wake_sound=not args.menu_wake, close_sound=not args.menu_close):
            sys.exit(2)  # A cancelled close is not a successful wake cue.
    else:
        getattr(hardware, args.command)()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("System control: " + str(error), file=sys.stderr)
        sys.exit(1)
