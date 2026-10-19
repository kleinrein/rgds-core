"""Hold the GPU and memory clock floors while a GPU-rendered game runs.

The stock devfreq governor ramps the Mali from 200 MHz to 800 MHz only after
load appears, so every light-to-heavy scene change in a 2x-5x game starts a
few frames at a low clock. While such a game runs, the floor of every
non-CPU clock the hardware journal already manages (the GPU and the memory
controller) is raised to its ceiling, through the same journalled writes the
power profiles use, and put back when the game ends. A power profile that
caps the ceiling keeps its cap: the floor never exceeds the current maximum.
Opt out per device with `gpu_floor=0` in performance.ini, or per launch with
RGDS_GPU_FLOOR=0.
"""
from pathlib import Path

from system_controls import Hardware


class GameClocks:
    def __init__(self, data, sysfs=Path("/sys")):
        self.hardware = Hardware(data, sysfs)
        self.previous = []   # (minimum path, value before the hold)

    @staticmethod
    def wanted(environment, data):
        """True for a launch that renders 3D on the GPU and has not opted out."""
        if environment.get("RGDS_GPU_FLOOR") == "0" or environment.get("DS_GPU3D") != "1":
            return False
        try:
            scale = int(environment.get("RGDS_ACTIVE_SCALE", "1"))
        except ValueError:
            scale = 1
        if scale <= 1:
            return False
        from fields import read_fields
        return read_fields(Path(data) / "performance.ini").get("gpu_floor", "1") != "0"

    def hold(self):
        """Raise each non-CPU clock floor to its current ceiling; returns the clocks held."""
        writes, previous = [], []
        for minimum, maximum, _governor, frequencies, _dynamic, cpu_policy in self.hardware.clocks():
            if cpu_policy:
                continue
            try:
                floor, ceiling = int(self.hardware.read(minimum)), int(self.hardware.read(maximum))
            except (OSError, ValueError):
                continue
            target = min(frequencies[-1], ceiling)
            if floor >= target:
                continue
            writes.append((minimum, target))
            previous.append((minimum, str(floor)))
        if writes:
            self.hardware.change(writes)
            self.previous += previous
        return [path.parent.name for path, _ in writes]

    def release(self):
        """Put back the floors hold() raised; a no-op when nothing was held."""
        previous, self.previous = self.previous, []
        if previous:
            self.hardware.change(previous)
