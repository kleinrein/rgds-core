"""Bounded lid and power key polling for the game runner; no worker thread or hardware writes."""
import os
import time
from pathlib import Path

from audio import EVENT
from fields import read_fields
from system_controls import Hardware

KEY_POWER = 116


def power_key_node(sysfs=Path('/sys/class/input'), devices=Path('/dev/input')):
    for entry in sorted(Path(sysfs).glob('event*')):
        try:
            if (entry / 'device/name').read_text().strip() == 'rk805 pwrkey':
                node = Path(devices) / entry.name
                return node if node.exists() else None
        except OSError:
            continue
    return None


class PowerKey:
    """A short press asks for sleep, a two-second hold for a clean power off; src/platform/power_key.hpp is the same
    contract for the launcher. Sleep waits for the release, so a hold never sleeps first, and holding the key to boot
    leaves a release with no press. The press that wakes the handheld arrives with the resume, so after_sleep discards
    it and ignores the key for a moment."""
    HOLD_LIMIT = 2
    WAKE_QUIET = 1

    def __init__(self, node=None):
        self.fd = -1
        self.armed = False
        self.wanted = None
        self.pressed_at = 0
        self.ignore_until = float('-inf')
        node = node or power_key_node()
        if node is None: return
        try:
            self.fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError:
            pass

    def feed(self, value, now):
        if value == 1:
            self.armed = now >= self.ignore_until
            self.pressed_at = now
        elif value == 0:
            if self.armed: self.wanted = 'sleep' if now - self.pressed_at < self.HOLD_LIMIT else 'poweroff'
            self.armed = False

    def tick(self, now):
        if self.armed and now - self.pressed_at >= self.HOLD_LIMIT:
            self.wanted = 'poweroff'
            self.armed = False

    def take(self, ready):
        # A request made while not ready (a save finishing) is kept until it is.
        if not ready: return None
        press, self.wanted = self.wanted, None
        return press

    def read(self):
        try:
            return os.read(self.fd, EVENT.size * 32)
        except OSError:
            return b''

    def poll(self, ready=True):
        if self.fd >= 0:
            now = time.monotonic()
            while True:
                payload = self.read()
                if not payload: break
                for offset in range(0, len(payload) - EVENT.size + 1, EVENT.size):
                    _sec, _usec, kind, code, value = EVENT.unpack_from(payload, offset)
                    if kind == 1 and code == KEY_POWER: self.feed(value, now)
                    elif kind == 0 and code == 3: self.armed = False  # SYN_DROPPED: an edge was lost
            self.tick(now)
        return self.take(ready)

    def after_sleep(self):
        if self.fd >= 0:
            while self.read():
                pass
        self.armed = False
        self.wanted = None
        self.ignore_until = time.monotonic() + self.WAKE_QUIET

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


class LidSleep:
    def __init__(self, data, hardware=None):
        self.preferences = Path(data) / 'system.ini'
        self.hardware = hardware if hardware is not None else Hardware(data)
        self.polled = float('-inf')
        self.candidate = None
        self.samples = 0
        self.handled = False
        self.generation = None

    def poll(self, ready=True):
        now = time.monotonic()
        if now - self.polled < .05:
            return False
        self.polled = now
        closed = self.hardware.lid_closed()
        generation = getattr(self.hardware, 'lid_generation', None)
        if closed is not None and isinstance(generation, int) and self.generation != generation:
            self.generation = generation
            self.handled = False
            self.candidate, self.samples = None, 0
        if closed is None:
            self.candidate = None
            self.samples = 0
            return False
        if self.candidate != closed:
            self.candidate, self.samples = closed, 1
        else:
            self.samples = min(2, self.samples + 1)
        if self.samples < 2:
            return False
        if not closed:
            self.handled = False
            return False
        if not ready or self.handled:
            return False
        # New settings default on; an existing explicit Off remains respected.
        fields = read_fields(self.preferences)
        if fields.get('schema') == '1' and fields.get('lid') == '0':
            return False
        self.handled = True
        return True
