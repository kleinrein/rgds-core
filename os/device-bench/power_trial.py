#!/usr/bin/env python3
"""power_trial.py OUT MODE:SYSTEM:CORE:ROM ...: what a power profile really does to a running game, per system.

Each run sets the mode, starts the game the way the launcher does (emulators.py launch under the session's CPU mask, its
environment), samples clocks, governors, temperature and battery current once a second, then quits it and reports the
medians. On a copy of the data, under a hold (session_hold.sh), so the owner's files stay out of it.

  DATA=/data/rgds/thermal-userdata SECS=30 MAXTEMP=55000 power_trial.py /run/rgds/dsab/p.txt 3:ds:rgds-experimental:rom.nds
"""
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

D = Path('/run/rgds/dsab')
A = Path('/usr/share/rgds/releases') / Path('/usr/share/rgds/active').read_text().strip()
RUNTIME = Path(os.environ.get('RUNTIME', A / 'runtime'))  # a patched copy, to try a change before it is in an image
sys.path.insert(0, str(RUNTIME))
from system_controls import Hardware  # noqa: E402

DATA = Path(os.environ.get('DATA', '/data/rgds/thermal-userdata'))
SECS = int(os.environ.get('SECS', '30'))
MAXTEMP = int(os.environ.get('MAXTEMP', '55000'))
CPU = Path('/sys/devices/system/cpu/cpufreq/policy0')
GPU = Path('/sys/class/devfreq/fde60000.gpu')
ZONE = Path('/sys/class/thermal/thermal_zone0')
BATTERY = Path('/sys/class/power_supply/battery')


def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return ''


def sample():
    return dict(
        cpu=int(read(CPU / 'scaling_cur_freq') or 0) // 1000,
        cpu_max=int(read(CPU / 'scaling_max_freq') or 0) // 1000,
        cpu_min=int(read(CPU / 'scaling_min_freq') or 0) // 1000,
        cpu_gov=read(CPU / 'scaling_governor'),
        gpu=int(read(GPU / 'cur_freq') or 0) // 1000000,
        gpu_max=int(read(GPU / 'max_freq') or 0) // 1000000,
        gpu_gov=read(GPU / 'governor'),
        temp=int(read(ZONE / 'temp') or 0) // 100 / 10,
        trip=int(read(ZONE / 'trip_point_0_temp') or 0) // 1000,
        ma=int(read(BATTERY / 'current_avg') or 0) // 1000
    )


def main():
    if not Path('/run/rgds/dsab-hold').exists() or subprocess.run(
        ['pidof', 'rgds-launcher'], stdout=subprocess.DEVNULL
    ).returncode == 0:
        sys.exit('no hold: refusing')
    out = open(sys.argv[1], 'a', buffering=1)
    hardware = Hardware(DATA)
    env = dict(line.split('=', 1) for line in (D / 'launcher.env').read_text().splitlines() if '=' in line)
    env['RGDS_FORCE_AUTOLOAD'] = '1'  # the same scene every run: the state copied in
    env.update(item.split('=', 1) for item in os.environ.get('EXTRA', '').split())
    control = DATA / 'session/emulator.control'
    for run in sys.argv[2:]:
        mode, system, core, rom = run.split(':', 3)
        for _ in range(150):
            if int(read(ZONE / 'temp') or 0) <= MAXTEMP: break
            time.sleep(2)
        hardware.power(int(mode))
        idle = sample()
        # The bridge writes these while a game presents: gone first, so that a run reads only its own.
        stats = DATA / 'logs' / f'{"rgds-experimental" if core == "rgds-experimental" else "rgds-play"}-performance.ini'
        stats.unlink(missing_ok=True)
        log = open(D / f'p-{mode}-{system}.out', 'w')
        child = subprocess.Popen(
            [
                'taskset', '-c', '0-2', 'python3', '-B',
                str(RUNTIME / 'emulators.py'), '--app',
                str(A), '--data',
                str(DATA), '--core', core, '--rom', rom, 'launch'
            ],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT
        )
        time.sleep(8)
        presses = D / f'p-{mode}-{system}.presses'
        if env.get('DS_CADENCE_LOG'):
            # Press to completed flip (game_latency.py, press_to_flip.py), for DSCore.
            subprocess.run(
                [
                    'python3', '-B',
                    str(D / 'game_latency.py'), env['RGDS_LATENCY_TRACE'], '/dev/input/event7', '20', '304',
                    str(presses)
                ],
                stdout=subprocess.DEVNULL
            )
        rows = []
        for _ in range(SECS):
            if child.poll() is not None: break
            rows.append(sample())
            time.sleep(1)
        alive = child.poll() is None
        control.write_text(f'{int(time.time())} quit\n')
        try:
            child.wait(25)
        except subprocess.TimeoutExpired:
            child.terminate()
            alive = 'hung at quit'
            try:
                child.wait(10)
            except subprocess.TimeoutExpired:
                child.kill()
        after = sample()
        fields = dict(line.split('=', 1) for line in read(stats).splitlines() if '=' in line)
        # The 3DS engine draws through its own path and reports its frame rate in its log instead.
        engine_log = read(DATA / 'logs' / f'{"rgds-experimental" if core == "rgds-experimental" else "rgds-play"}.log')
        reported = [float(v) for v in re.findall(r'\| ([0-9.]+) FPS', engine_log) if float(v) > 1][2:]
        if reported:
            fields['average_fps'] = f'{statistics.median(reported):.1f} (lowest {min(reported):.1f})'
        for line in re.findall(r'^pace: display-locked.*$', engine_log, re.M):
            print(f'mode {mode} {system} {line[:150]}', file=out)
        if env.get('DS_CADENCE_LOG'):
            flip = subprocess.run(
                ['python3', '-B', str(D / 'press_to_flip.py'),
                 str(presses), env['DS_CADENCE_LOG']],
                capture_output=True,
                text=True
            ).stdout
            print(
                f'mode {mode} {system} ' + ' / '.join(l.strip() for l in flip.splitlines() if 'flip completed' in l),
                file=out
            )
        if not rows:
            print(f'{mode} {system}: exited early, code {child.returncode}', file=out)
            continue
        median = lambda key: statistics.median(r[key] for r in rows)
        print(
            f"mode {mode} {system:5} fps {fields.get('average_fps', '?')} game {fields.get('game_fps', '-')} p95 {fields.get('p95_ms', '?')} slow {fields.get('over_20ms', '?')}/{fields.get('frames', '?')} cpu {median('cpu'):.0f}/{rows[-1]['cpu_max']} min {rows[-1]['cpu_min']} {rows[-1]['cpu_gov']:11} "
            f"gpu {median('gpu'):.0f}/{rows[-1]['gpu_max']} {rows[-1]['gpu_gov']:15} temp {rows[0]['temp']}->{rows[-1]['temp']} "
            f"trip {rows[-1]['trip']} current {statistics.mean(r['ma'] for r in rows):.0f} mA "
            f"| idle before cpu_gov {idle['cpu_gov']} gpu_gov {idle['gpu_gov']} | after cpu_gov {after['cpu_gov']} gpu_gov {after['gpu_gov']} ok={alive}",
            file=out
        )
    hardware.power(int(os.environ.get('LEAVE', '0')))
    print('### done', file=out)


if __name__ == '__main__':
    main()
