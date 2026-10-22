"""Stock RG DS hall-key events, owned by the session recovery guardian.

The hallkey sysfs value is configuration, not position. Stock firmware emits
one-second KEY_POWER2 (110) close / KEY_PAUSE (111) open pulses on rk805 pwrkey.
Latch press edges; a release is not the opposite lid position. Share the latch
through a short-lived, atomic tmpfs record so helper processes can recheck it.
"""
import os
from pathlib import Path
import shutil
import struct
import tempfile
import time

from input_devices import bits

EVENT = struct.Struct('llHHi')


def discover(sysfs=Path('/sys/class/input'), devices=Path('/dev/input')):
    candidates = []
    for entry in sorted(Path(sysfs).glob('event*')):
        try:
            if (entry / 'device/name').read_text().strip() != 'rk805 pwrkey':
                continue
            keys = bits(entry / 'device/capabilities/key')
            node = Path(devices) / entry.name
            if all(keys & (1 << code) for code in (110, 111, 116)) and node.exists():
                candidates.append(node)
        except OSError:
            continue
    return candidates[0] if len(candidates) == 1 else None


def read_snapshot(path=None):
    """Read one atomic position/close-generation snapshot; accept legacy records."""
    path = path or os.environ.get('RGDS_LID_STATE')
    if not path:
        return None, None
    try:
        fields = Path(path).read_text().split()
        if len(fields) not in (2, 3): return None, None
        stamp, state = fields[:2]
        age = time.monotonic() - float(stamp)
        value = int(state)
        generation = int(fields[2]) if len(fields) == 3 and fields[2].isdigit() else None
        if len(fields) == 3 and (generation is None or generation >= 2**64): return None, None
        return (value, generation) if 0 <= age <= 3 and value in (-1, 0, 1) else (None, None)
    except (OSError, ValueError):
        return None, None


def read_state(path=None):
    """Return -1 for an active sensor with unknown position; None if unavailable."""
    return read_snapshot(path)[0]


class LidSensor:
    def __init__(self, node=None):
        self.fd = -1
        self.folder = None
        self.path = None
        self.closed = -1
        self.generation = 0
        self.published = float('-inf')
        self.pending = b''
        self.dropped = False
        node = node or discover()
        if node is None:
            return
        try:
            self.fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            self.folder = Path(tempfile.mkdtemp(prefix='rgds-lid-', dir='/tmp'))
            self.path = self.folder / 'state'
            self.publish()
        except OSError:
            self.close()

    def feed(self, payload):
        self.pending += payload
        changed = False
        while len(self.pending) >= EVENT.size:
            _, _, kind, code, value = EVENT.unpack(self.pending[:EVENT.size])
            self.pending = self.pending[EVENT.size:]
            if self.dropped:
                if kind == 0 and code == 0: self.dropped = False
                continue
            if kind == 0 and code == 3:  # SYN_DROPPED: never trust a lost edge.
                self.dropped = True
                self.closed = -1
                changed = True
            elif kind == 1 and value == 1 and code in (110, 111):
                if code == 110 and self.closed != 1: self.generation += 1
                self.closed = int(code == 110)
                changed = True
        return changed

    def publish(self):
        now = time.monotonic()
        pending = self.path.with_suffix('.pending')
        pending.write_text(f'{now:.9f} {self.closed} {self.generation}\n')
        pending.replace(self.path)
        self.published = now

    def poll(self):
        if self.fd < 0:
            return
        changed = False
        try:
            # Bound work if a faulty input node floods events.
            for _ in range(16):
                try:
                    payload = os.read(self.fd, EVENT.size * 32)
                except BlockingIOError:
                    break
                if not payload:
                    raise OSError('Lid input disconnected')
                changed = self.feed(payload) or changed
            if changed or time.monotonic() - self.published >= 1:
                self.publish()
        except OSError:
            self.close()

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1
        if self.folder:
            shutil.rmtree(self.folder, ignore_errors=True)
            self.folder = None
