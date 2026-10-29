"""Session-local mixing for the stock RK817's otherwise exclusive playback PCM."""
import ctypes
import os
from pathlib import Path
import signal
import sys
import time


def configure(app, env, stock=Path('/etc/asound.conf')):
    # Leave custom audio setups and non-stock platforms alone. This override
    # extends the known asym profile, preserving its microphone definition.
    if env.get('ALSA_CONFIG_PATH') or env.get('SDL_AUDIODRIVER', 'alsa') != 'alsa':
        return env
    home = Path(env.get('HOME', str(Path.home())))
    user_config = Path(env.get('XDG_CONFIG_HOME', str(home / '.config'))) / 'alsa/asoundrc'
    if (home / '.asoundrc').exists() or user_config.exists():
        return env
    config = Path(app) / 'runtime/alsa-rgds.conf'
    try:
        profile = Path(stock).read_text()
        known = all(
            value in profile for value in (
                'pcm.!default', 'type asym', 'playback.pcm', 'capture.pcm', 'slave.pcm "hw:0,0"',
                'name "Playback Path"', 'value "SPK"'
            )
        )
        if known and config.is_file():
            env['ALSA_CONFIG_PATH'] = str(config)
            env['RGDS_SHARED_AUDIO'] = '1'
    except OSError:
        pass
    return env


# alsa-lib guards the dmix stream with a SysV semaphore at ipc_key (+ uid):
# start, recovery after an underrun, open and close all take it for a moment.
# RG DS OS's RG DS Plus plays on its headphone jack through a second stream, the next key (/usr/libexec/rgds/audio).
MIXER_KEY = 1380402259
MIXER_KEYS = (MIXER_KEY, MIXER_KEY + 1)
_GETPID, _GETVAL = 11, 12


def mixer_holder(key=None):
    """Pid of the process inside the shared mixer's critical section.

    0 when the semaphore is free, None when there is no shared mixer (nobody
    has the stream open) or it cannot be read on this platform. Without a
    key, every RG DS stream's: a holder in any of them, else 0 if one is open."""
    if key is None:
        found = [mixer_holder(k) for k in MIXER_KEYS]
        return next((pid for pid in found if pid), 0 if 0 in found else None)
    if sys.platform != 'linux':
        return None
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        semget, semctl = libc.semget, libc.semctl
    except (OSError, AttributeError):
        return None
    semget.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    semget.restype = ctypes.c_int
    semctl.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    semctl.restype = ctypes.c_int
    semid = semget(key + os.getuid(), 0, 0)
    if semid < 0:
        return None
    value, pid = semctl(semid, 0, _GETVAL, 0), semctl(semid, 0, _GETPID, 0)
    if value < 0 or pid < 0:
        return None
    return pid if value > 0 else 0


def threads_stopped(pid):
    """True once every thread of pid is stopped (or the process is gone)."""
    try:
        tasks = list((Path('/proc') / str(pid) / 'task').iterdir())
    except OSError:
        return True
    for task in tasks:
        try:
            stat = (task / 'stat').read_text()
        except OSError:
            continue
        state = stat[stat.rfind(')') + 2:stat.rfind(')') + 3]
        if state not in ('T', 't', 'Z', 'X'):
            return False
    return True


def stop_outside_mixer(
    process,
    holder=mixer_holder,
    settled=threads_stopped,
    retries=50,
    delay=.002,
    sleep=time.sleep,
    clock=time.monotonic
):
    """SIGSTOP a process, but never while it holds the shared mixer semaphore.

    A client stopped inside alsa-lib's critical section keeps the semaphore,
    and the next client to open or start the stream (the pause menu's cue
    sink, the sleep owner's chime) blocks in semtimedop with no timeout until
    the holder runs again: the menu froze with the engine stopped underneath.
    Returns False when the process still held it after every retry."""
    for attempt in range(retries + 1):
        process.send_signal(signal.SIGSTOP)
        deadline = clock() + .1
        while not settled(process.pid) and clock() < deadline:
            sleep(.001)
        if holder() != process.pid:
            return True
        if attempt == retries:
            return False
        process.send_signal(signal.SIGCONT)
        sleep(delay)
    return False
