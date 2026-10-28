"""Idle dimming and the sleep timer while a game runs (the launcher does the same for the menu, system_settings.hpp).

IdleWatch reads every input device without grabbing it and keeps the time of the last real input: a button, the
D-pad, a touch, a stick pushed past a quarter of its travel (stick noise does not count). PanelDim lowers every
backlight to a share of its level straight through sysfs and puts back exactly what it found; nothing is saved.
The choices come from system.ini (dim, game_sleep), written by the launcher's Power page."""
import fcntl
import os
from pathlib import Path
import select
import struct
import time

from audio import EVENT
from fields import read_fields

# the launcher's dim_seconds / game_sleep_minutes (settings.hpp) and their defaults
DIM_SECONDS = (0, 30, 60, 120, 300)
DIM_DEFAULT = 2
GAME_SLEEP_MINUTES = (0, 10, 20, 30, 60)
DIM_PERCENT = 25
EV_KEY, EV_ABS = 1, 3
ABS_HAT0X, ABS_HAT3Y = 0x10, 0x17
ABS_MT_SLOT = 0x2f
ABSINFO = struct.Struct('6i')


def eviocgabs(code):
    return (2 << 30) | (ABSINFO.size << 16) | (ord('E') << 8) | (0x40 + code)


def timers(data):
    """(dim seconds, game sleep seconds) from system.ini; 0 is never."""
    fields = read_fields(Path(data) / 'system.ini')

    def pick(key, table, default):
        try:
            index = int(fields.get(key, default))
        except ValueError:
            index = default
        return table[index] if 0 <= index < len(table) else table[default]

    return pick('dim', DIM_SECONDS, DIM_DEFAULT), pick('game_sleep', GAME_SLEEP_MINUTES, 0) * 60


class IdleWatch:
    def __init__(self, directory=Path('/dev/input'), clock=time.monotonic):
        self.clock = clock
        self.last = clock()
        self.devices = {}  # fd -> {abs code: (centre, quarter of the travel)}
        self.held = {}
        for path in sorted(Path(directory).glob('event*')):
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError:
                continue
            sticks = {}
            for code in range(0, 6):  # ABS_X .. ABS_RZ
                info = bytearray(ABSINFO.size)
                try:
                    fcntl.ioctl(fd, eviocgabs(code), info)
                except OSError:
                    continue
                _value, low, high = ABSINFO.unpack(info)[:3]
                if high > low: sticks[code] = ((low + high) / 2, (high - low) / 4)
            self.devices[fd] = sticks

    def close(self):
        for fd in self.devices:
            os.close(fd)
        self.devices = {}

    def touch(self):
        """Count now as input (a wake from sleep, a menu that opened)."""
        self.last = self.clock()

    def counts(self, fd, kind, code, value):
        if kind == EV_KEY: return True
        if kind != EV_ABS: return False
        if code >= ABS_MT_SLOT: return True  # a finger on a touchscreen (which also sends BTN_TOUCH)
        if ABS_HAT0X <= code <= ABS_HAT3Y: return value != 0
        stick = self.devices.get(fd, {}).get(code)
        if stick is None: return False
        centre, quarter = stick
        return abs(value - centre) > quarter

    def idle(self):
        """Seconds since the last input."""
        if self.devices:
            try:
                readable, _, _ = select.select(list(self.devices), [], [], 0)
            except (OSError, ValueError):
                readable = []
            for fd in readable:
                try:
                    data = os.read(fd, EVENT.size * 64)
                except BlockingIOError:
                    continue
                except OSError:
                    self.devices.pop(fd, None)
                    os.close(fd)
                    continue
                for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    _sec, _usec, kind, code, value = EVENT.unpack_from(data, offset)
                    if self.counts(fd, kind, code, value):
                        self.last = self.clock()
        return self.clock() - self.last


class PanelDim:
    def __init__(self, sysfs=Path('/sys')):
        self.root = Path(sysfs) / 'class/backlight'
        self.levels = {}

    @property
    def dimmed(self):
        return bool(self.levels)

    def dim(self, percent=DIM_PERCENT):
        if self.levels: return
        for node in sorted(self.root.glob('*/brightness')):
            try:
                value = int(node.read_text())
                if value <= 1: continue
                node.write_text(str(max(1, value * percent // 100)))
                self.levels[node] = value
            except (OSError, ValueError):
                continue

    def restore(self):
        for node, value in self.levels.items():
            try:
                node.write_text(str(value))
            except OSError:
                pass
        self.levels = {}
