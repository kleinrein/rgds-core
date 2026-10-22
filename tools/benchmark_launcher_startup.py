#!/usr/bin/env python3
"""Measure fresh host launcher processes to their first RenderPresent ACK.

Uses preview + SDL dummy, temporary userdata, and an immutable binary copy.
This is a launcher benchmark with warm filesystem caches, not power-on timing.
Default --frames 1 also makes native exit counters describe the first frame;
--seconds exercises startup completion while keeping the same first-frame ACK.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def summary(samples):
    return dict(samples=samples, median=statistics.median(samples), minimum=min(samples), maximum=max(samples))


def benchmark(binary, asset_root=ROOT, iterations=16, seconds=None, frames=1, mode='all', font='all'):
    binary = Path(binary).resolve()
    asset_root = Path(asset_root).resolve()
    if not binary.is_file() or iterations < 1 or (seconds is not None and seconds < 1) or frames < 1:
        raise ValueError('Need a host binary, positive iterations and a positive run duration')
    cases = []
    for warning in (False, True):
        if mode != 'all' and (mode == 'warning') != warning: continue
        for face in ('rounded', 'pixel'):
            if font != 'all' and font != face: continue
            cases.append((warning, face))
    if not cases: raise ValueError('Unknown startup mode or font')
    result = dict(
        schema=1,
        captured_utc=datetime.now(timezone.utc).isoformat(),
        scope='fresh host process to first RenderPresent ACK; SDL dummy; warm filesystem',
        excludes=['Python boot/session dispatch', 'real display/input drivers', 'power-on boot'],
        host=dict(system=platform.system(), machine=platform.machine()),
        binary=str(binary),
        asset_root=str(asset_root),
        iterations=iterations,
        run=dict(seconds=seconds, frames=None if seconds is not None else frames),
        native_counter_scope='first frame' if seconds is None and frames == 1 else 'complete measured session',
        cases={}
    )
    with tempfile.TemporaryDirectory(prefix='rgds-launcher-startup-') as directory:
        directory = Path(directory)
        snapshot = directory / 'launcher'
        shutil.copy2(binary, snapshot)
        result['binary_sha256'] = hashlib.sha256(snapshot.read_bytes()).hexdigest()
        for warning, face in cases:
            acknowledgements = []
            finished = []
            reports = []
            errors = []
            for index in range(iterations):
                data = directory / (('warning' if warning else 'home') + '-' + face + '-' + str(index))
                data.mkdir()
                report = data / 'counters.json'
                reader, writer = socket.socketpair()
                reader.settimeout(10)
                command = [
                    str(snapshot), '--preview', '--boot-warning' if warning else '--boot-startup', '--font', face,
                    '--app',
                    str(asset_root), '--data',
                    str(data), '--preferences',
                    str(data / 'preferences.ini'), '--theme',
                    str(asset_root / 'themes/default/light.ini'), '--cache-report',
                    str(report), '--boot-ready-fd',
                    str(writer.fileno())
                ]
                command += ['--seconds', str(seconds)] if seconds is not None else ['--frames', str(frames)]
                child = None
                try:
                    began = time.perf_counter()
                    child = subprocess.Popen(
                        command,
                        cwd=asset_root,
                        env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
                        pass_fds=(writer.fileno(),),
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE
                    )
                    writer.close()
                    ack = reader.recv(1)
                    presented = time.perf_counter()
                    output, error = child.communicate(timeout=(seconds or 0) + 10)
                    ended = time.perf_counter()
                    if ack != b'1' or child.returncode:
                        raise ValueError(
                            'Launcher did not acknowledge first presentation: ' +
                            repr((ack, child.returncode, output.decode(), error.decode()))
                        )
                    # EOF confirms this descriptor was a one-shot presentation ACK.
                    if reader.recv(1) != b'': raise ValueError('Unexpected additional boot ACK')
                    acknowledgements.append(round((presented - began) * 1000, 6))
                    finished.append(round((ended - began) * 1000, 6))
                    reports.append(json.loads(report.read_text()))
                    if error: errors.append(error.decode())
                finally:
                    reader.close()
                    writer.close()
                    if child is not None and child.poll() is None:
                        child.terminate()
                        try:
                            child.communicate(timeout=1)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.communicate()
            case = ('warning' if warning else 'home') + '-' + face
            result['cases'][case] = dict(
                first_frame_ms=summary(acknowledgements),
                process_total_ms=summary(finished),
                first_present_acks=len(acknowledgements),
                native_reports=reports,
                stderr=errors
            )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/host/rgds-launcher')
    parser.add_argument('--asset-root', type=Path, default=ROOT)
    parser.add_argument('--iterations', type=int, default=16)
    duration = parser.add_mutually_exclusive_group()
    duration.add_argument('--frames', type=int, default=1)
    duration.add_argument('--seconds', type=int)
    parser.add_argument('--mode', choices=('all', 'home', 'warning'), default='all')
    parser.add_argument('--font', choices=('all', 'rounded', 'pixel'), default='all')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = benchmark(
            args.binary, args.asset_root, args.iterations, args.seconds, args.frames, args.mode, args.font
        )
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, 'benchmark_launcher_startup: ' + str(error) + '\n')
    serialized = json.dumps(result, indent=2, sort_keys=True) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized)
    print(serialized, end='')


if __name__ == '__main__': main()
