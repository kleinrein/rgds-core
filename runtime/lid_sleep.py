"""Bounded lid polling for the game runner; no worker thread or hardware writes."""
import time
from pathlib import Path

from fields import read_fields
from system_controls import Hardware


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
