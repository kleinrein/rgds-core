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


class Hardware:
    def __init__(self, data, sysfs=Path("/sys")):
        self.sysfs = Path(sysfs)
        self.journal = Path(data) / "session/system-hardware.json"

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
            return self.writable(path) and "mem" in self.read(path).split()
        except OSError:
            return False

    def lid_supported(self):
        hall = self.sysfs / "class/anbernic_misc/hallkey"
        wake = self.sysfs / "devices/platform/anbernic_misc/power/wakeup"
        try:
            return self.sleep_supported() and self.read(hall) in ("0", "1") and self.writable(wake)
        except OSError:
            return False

    def status(self):
        result = {"brightness_supported": int(bool(self.backlights())),
                  "power_supported": int(any(c[-1] for c in self.clocks())),
                  "sleep_supported": int(self.sleep_supported()), "lid_supported": int(self.lid_supported()),
                  "brightness": 60, "battery": -1, "charging": 0}
        levels = []
        for path, maximum in self.backlights():
            try:
                levels.append(round(int(self.read(path)) * 100 / maximum))
            except (OSError, ValueError):
                pass
        if levels:
            result["brightness"] = max(5, min(100, round(sum(levels) / len(levels))))
        for folder in sorted((self.sysfs / "class/power_supply").glob("*")):
            try:
                if self.read(folder / "type") != "Battery":
                    continue
                capacity = int(self.read(folder / "capacity"))
                if 0 <= capacity <= 100:
                    result["battery"] = capacity
                result["charging"] = int(self.read(folder / "status") in ("Charging", "Full"))
                break
            except (OSError, ValueError):
                continue
        return result

    def saved(self):
        if not self.journal.exists():
            return {}
        values = json.loads(self.journal.read_text())
        if not isinstance(values, dict):
            raise ValueError("Invalid hardware recovery journal")
        for name, value in values.items():
            path = Path(name)
            if path.is_absolute() or ".." in path.parts or not isinstance(value, str):
                raise ValueError("Invalid hardware recovery entry")
            if path.name not in ("brightness", "scaling_min_freq", "scaling_max_freq", "scaling_governor",
                                 "min_freq", "max_freq", "governor", "wakeup"):
                raise ValueError("Unknown hardware recovery control")
        return values

    def save_journal(self, values):
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        pending = self.journal.with_suffix(".tmp")
        with pending.open("w") as file:
            json.dump(values, file)
            file.flush()
            os.fsync(file.fileno())
        pending.replace(self.journal)

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

    def lid(self, enabled):
        if enabled:
            if not self.lid_supported():
                raise ValueError("Lid sleep unavailable")
            self.change([(self.sysfs / "devices/platform/anbernic_misc/power/wakeup", "enabled")])
        else:
            self.restore_values({k: v for k, v in self.saved().items() if Path(k).name == "wakeup"})

    def sleep(self):
        if not self.sleep_supported():
            raise ValueError("System sleep unavailable")
        # Retain the stock-selected mem variant and wake sources; no fake blanking.
        levels = {str(p.relative_to(self.sysfs)): self.read(p) for p, _ in self.backlights()}
        try:
            if hasattr(os, "sync"):
                os.sync()
            if levels:
                self.change([(self.sysfs / name, "0") for name in levels])
            (self.sysfs / "power/state").write_text("mem\n")
        finally:
            self.restore_values(levels)

    def restore(self):
        self.restore_values(self.saved())
        self.journal.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "brightness", "power", "lid", "sleep", "restore"))
    parser.add_argument("value", nargs="?", type=int)
    parser.add_argument("--data", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("Hardware controls require Linux")
    hardware = Hardware(args.data)
    if args.command == "status":
        for key, value in hardware.status().items():
            print(f"{key}={value}")
    elif args.command in ("brightness", "power", "lid"):
        if args.value is None:
            raise ValueError("Control value missing")
        if args.command == "lid" and args.value not in (0, 1):
            raise ValueError("Invalid lid setting")
        getattr(hardware, args.command)(args.value)
    else:
        getattr(hardware, args.command)()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print("System control: " + str(error), file=sys.stderr)
        sys.exit(1)
