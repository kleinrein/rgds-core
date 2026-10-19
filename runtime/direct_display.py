"""Opt-in direct display for DSCore: stock Weston stops while the game runs.

Stock Weston GL-composites both panels on every frame (about 30% of a core,
plus Mali time the 2x renderer needs). DSCore can instead present straight to
KMS through SDL's KMSDRM driver; in the Mario Kart ink scene at 2x that cut
frame work from 16.2 to 13.2 ms. Weston has no DRM lease, and its atomic mode
cannot scan out DSCore's GLES buffers, so the only way is to stop it.

Weston must be back before the launcher reopens its windows, and before stock
gets its frontend back. A marker in /tmp records that RGDS stopped it, so the
session guardian (and the next session) can restart it after any crash.
"""
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from records import process_identity

INIT_SCRIPT = Path('/etc/init.d/S49weston')
SOCKET = Path('/var/run/wayland-0')
MARKER = Path('/tmp/rgds-weston-stopped.json')
LOCK = Path('/tmp/rgds-weston.lock')
# Stock init starts Weston with this environment (see /proc/1/environ).
INIT_ENVIRONMENT = {'HOME': '/', 'TERM': 'linux', 'PATH': '/usr/sbin:/usr/bin:/sbin:/bin'}


def available():
    """Stock Anbernic Linux only: an independent install owns its own display."""
    from linux_platform import independent
    return not independent() and INIT_SCRIPT.is_file() and os.access(INIT_SCRIPT, os.X_OK)


def _processes(names, proc=Path('/proc')):
    found = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():continue
        try:
            if (entry / 'comm').read_text().strip() in names and \
                    (entry / 'stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z':
                found.append(int(entry.name))
        except (OSError, IndexError):continue
    return found


def weston_running():
    return bool(_processes({'weston'}))


def ready():
    # The desktop shell is Weston's own first client: once it runs, a new
    # client gets the same fullscreen placement as at boot.
    return SOCKET.is_socket() and bool(_processes({'weston'})) and bool(_processes({'weston-desktop-'}))


class _Locked:
    """One stop/start at a time across the runner, guardian and next session."""
    def __enter__(self):
        self.stream = LOCK.open('a')
        fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX)
        return self

    def __exit__(self, *_):
        fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        self.stream.close()


class _Quiet:
    """A SIGTERM must not leave Weston half started; the guardian is the backstop."""
    def __enter__(self):
        self.previous = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})

    def __exit__(self, *_):
        signal.pthread_sigmask(signal.SIG_SETMASK, self.previous)


def _wait(predicate, timeout):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() > deadline:return False
        time.sleep(.01)
    return True


def stop(timeout=3):
    """Stop stock Weston and wait until its DRM master is released. Returns seconds."""
    began = time.monotonic()
    with _Locked(), _Quiet():
        # Marker first: a crash right after the stop must still bring it back.
        MARKER.write_text(json.dumps(dict(pid=os.getpid(), start=process_identity(os.getpid()), stopped=time.time())) + '\n')
        subprocess.run([str(INIT_SCRIPT), 'stop'], env=INIT_ENVIRONMENT, cwd='/', stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5, check=False)
        if not _wait(lambda:not weston_running(), timeout):
            for pid in _processes({'weston'}):
                try:os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:pass
            if not _wait(lambda:not weston_running(), 1):
                raise OSError('Weston did not stop')
    return time.monotonic() - began


def _start(timeout):
    if not weston_running():
        # The init script backgrounds "weston --warm-up | tee /var/log/weston.log".
        # Its own session keeps both out of the launcher's process group, which
        # the guardian stops at the end of a session.
        script = subprocess.Popen([str(INIT_SCRIPT), 'start'], env=INIT_ENVIRONMENT, cwd='/', stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:script.wait(timeout=5)
        except subprocess.TimeoutExpired:pass
    if not _wait(ready, timeout):
        raise OSError('Weston did not come back within %d seconds' % timeout)
    MARKER.unlink(missing_ok=True)


def start(timeout=10):
    """Start stock Weston and wait for its socket and desktop shell. Returns seconds."""
    began = time.monotonic()
    with _Locked(), _Quiet():
        _start(timeout)
    return time.monotonic() - began


def _owner_alive():
    try:owner = json.loads(MARKER.read_text())
    except (OSError, ValueError):return False
    return isinstance(owner, dict) and bool(owner.get('start')) and process_identity(owner.get('pid')) == owner['start']


def recover(timeout=10):
    """Restart Weston if an RGDS game stopped it and its runner is gone."""
    if not MARKER.exists():return None
    with _Locked(), _Quiet():
        # A live runner still owns the display (its game is playing on KMS).
        if not MARKER.exists() or _owner_alive():return None
        began = time.monotonic()
        _start(timeout)
        return time.monotonic() - began


class DirectDisplay:
    """Per game runner: Weston is stopped for a KMS engine and restored once."""
    def __init__(self, data):
        self.data = Path(data)
        self.stopped = False
        self.timings = {}
        self.gpu_resets = 0   # engine restarts after a GPU reset at start (emulators._run_once)

    def prepare(self, wanted):
        """Return the display the engine will really use ('kms' or 'wayland')."""
        if wanted == 'kms' and not self.stopped:
            try:
                self.timings['weston_stop_seconds'] = round(stop(), 3)
                self.stopped = True
                print('Direct display: Weston stopped in %.2f s; the engine presents on KMS' % self.timings['weston_stop_seconds'], flush=True)
            except (OSError, subprocess.SubprocessError) as error:
                print('Direct display unavailable, keeping Weston: ' + str(error), flush=True)
                self.restore()
                return 'wayland'
        elif wanted != 'kms' and self.stopped:
            self.restore()
        return 'kms' if self.stopped else 'wayland'

    def restore(self):
        if not self.stopped and not MARKER.exists():return
        exited = time.monotonic()
        try:
            self.timings['weston_start_seconds'] = round(start(), 3)
            self.stopped = False
        finally:
            self.timings['restored_monotonic'] = time.monotonic()
            self.timings['engine_exit_monotonic'] = exited
            try:
                path = self.data / 'session/direct-display.json'
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(self.timings) + '\n')
            except OSError:pass
        print('Direct display: Weston back in %.2f s' % self.timings['weston_start_seconds'], flush=True)


def use_wayland(env, wayland):
    """Undo a KMS launch plan when Weston could not be stopped."""
    for key, value in wayland.items():
        if value is None:env.pop(key, None)
        else:env[key] = value
