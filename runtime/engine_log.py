"""Drain engine output continuously into a bounded RAM tail, saved on exit."""
from collections import deque
from pathlib import Path
import select
import os
import threading
import time
import tempfile
import sys


class EngineLog:
    def __init__(self, data, core, command, cwd, limit=128 * 1024):
        self.path = Path(data) / "logs" / (core + ".log")
        self.core, self.command, self.cwd = core, list(command), str(cwd)
        self.limit = limit
        self.chunks = deque()
        self.size = self.dropped = 0
        self.lock = threading.Lock()
        self.stream = self.worker = None
        self.deadline = None
        self.wake_reader = self.wake_writer = None
        self.gpu_faults = 0  # "[KFAULT]" lines: PanVK GPU jobs the kernel terminated

    def append(self, chunk):
        with self.lock:
            self.gpu_faults += chunk.count(b"[KFAULT]")
            self.chunks.append(chunk)
            self.size += len(chunk)
            while self.size > self.limit:
                excess = self.size - self.limit
                first = self.chunks.popleft()
                removed = min(len(first), excess)
                if removed < len(first): self.chunks.appendleft(first[removed:])
                self.size -= removed
                self.dropped += removed

    def start(self, stream):
        self.stream = stream
        if stream is None: return
        self.wake_reader, self.wake_writer = os.pipe()
        self.worker = threading.Thread(target=self.drain, name="rgds-engine-log", daemon=True)
        try:
            self.worker.start()
        except Exception:
            os.close(self.wake_reader)
            os.close(self.wake_writer)
            self.wake_reader = self.wake_writer = self.worker = None
            raise

    def drain(self):
        descriptor = self.stream.fileno()
        try:
            while self.deadline is None or time.monotonic() < self.deadline:
                # A quiet engine needs no periodic wakeups. finish() wakes this
                # wait explicitly, including when a descendant holds stdout.
                timeout = None if self.deadline is None else max(0, self.deadline - time.monotonic())
                readable, _, _ = select.select([descriptor, self.wake_reader], [], [], timeout)
                if self.wake_reader in readable:
                    os.read(self.wake_reader, 1)
                    if descriptor not in readable: break
                if not readable:
                    if self.deadline is not None: break
                    continue
                chunk = os.read(descriptor, 64 * 1024)
                if not chunk: break
                self.append(chunk)
        except (OSError, ValueError) as error:
            self.append(("\nOutput capture: " + str(error) + "\n").encode())
        finally:
            os.close(self.wake_reader)

    def finish(self, returncode=None, error=None):
        # Descendants may inherit stdout after the engine has exited. An open
        # inherited writer must not prevent the launcher from returning home.
        self.deadline = time.monotonic() + .3
        if self.wake_writer is not None:
            try:
                os.write(self.wake_writer, b"x")
            except OSError:
                pass  # The engine may already have closed stdout.
        if self.worker: self.worker.join(timeout=1)
        if self.wake_writer is not None:
            os.close(self.wake_writer)
            self.wake_writer = None
        if self.stream and (not self.worker or not self.worker.is_alive()): self.stream.close()
        with self.lock:
            tail = b"".join(self.chunks).decode("utf-8", errors="replace")
            dropped = self.dropped
        header = (
            f"Engine: {self.core}\nWorking directory: {self.cwd}\n"
            f"Command: {self.command!r}\nExit code: {returncode}\n"
        )
        if error: header += "Runtime error: " + str(error) + "\n"
        if dropped: header += f"Earlier output discarded: {dropped} bytes\n"
        pending = self.path.with_suffix(".log.partial")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            pending.write_text(header + "\n" + tail)
            pending.replace(self.path)
        except OSError as failure:
            # Preserve the bounded crash tail even if a card remounts read-only
            # mid-game. This recovery file never becomes a save destination.
            with tempfile.NamedTemporaryFile(
                mode="w", prefix="rgds-" + self.core + "-", suffix=".log", delete=False
            ) as stream:
                stream.write(header + "Log storage error: " + str(failure) + "\n\n" + tail)
                self.path = Path(stream.name)
            print("Engine diagnostics recovered to " + str(self.path), file=sys.stderr, flush=True)
        finally:
            try:
                pending.unlink(missing_ok=True)
            except OSError:
                pass
        return self.path
