"""Stream physical microphone levels to the launcher; never retain audio."""
import argparse
import array
import math
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

from records import read_record, process_identity


def levels(pcm):
    samples = array.array('h', pcm)
    if sys.byteorder != 'little':
        samples.byteswap()
    if not samples:
        return 0, 0, -60
    # Remove the ADC's DC offset so silence does not appear as a signal.
    mean = sum(samples) / len(samples)
    peak = max(abs(value - mean) for value in samples) / 32768
    rms = math.sqrt(sum((value - mean)**2 for value in samples) / len(samples)) / 32768
    db = max(-60.0, min(0.0, 20 * math.log10(max(rms, 1e-9))))
    peak_db = max(-60.0, min(0.0, 20 * math.log10(max(peak, 1e-9))))
    return round((db + 60) * 100 / 60), round((peak_db + 60) * 100 / 60), round(db)


def run(data):
    owner = read_record(Path(data) / 'session/core.json')
    if owner and process_identity(owner['pid']) == owner['start']:
        print('busy', flush=True)
        return
    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    parent = os.getppid()
    capture = None
    try:
        env = os.environ.copy()
        env.pop('LD_PRELOAD', None)
        env.pop('LD_LIBRARY_PATH', None)
        capture = subprocess.Popen(
            ['arecord', '-q', '-D', 'plughw:0,0', '-t', 'raw', '-f', 'S16_LE', '-r', '16000', '-c', '1'],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env
        )
        os.set_blocking(capture.stdout.fileno(), False)
        pending = b''
        last = time.monotonic()
        while not stopped and os.getppid() == parent:
            ready, _, _ = select.select([capture.stdout], [], [], .1)
            if ready:
                chunk = os.read(capture.stdout.fileno(), 4096)
                if not chunk:
                    raise OSError('Capture stopped')
                pending += chunk
                while len(pending) >= 1600:
                    print(*levels(pending[:1600]), flush=True)
                    pending = pending[1600:]
                    last = time.monotonic()
            if time.monotonic() - last > 3:
                raise OSError('No capture samples')
    except (OSError, ValueError):
        print('unavailable', flush=True)
    finally:
        if capture:
            capture.terminate()
            try:
                capture.wait(timeout=1)
            except subprocess.TimeoutExpired:
                capture.kill()
                capture.wait()
            capture.stdout.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    args = parser.parse_args()
    try:
        run(args.data)
    except (OSError, ValueError):
        print('unavailable', flush=True)
