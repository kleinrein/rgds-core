#!/usr/bin/env python3
"""cadence_lock.py CADENCE...: from DS_CADENCE_LOG, locked against free frames: hand-off time, what happens from hand-off
to the upper panel's flip, and how long the runs of locked frames last."""
import bisect
import sys


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


for path in sys.argv[1:]:
    frames, flips = [], []
    for line in open(path, errors='replace'):
        p = line.split()
        if not p or p[0] == '#': continue
        if p[0] == 'f':
            if p[1] == '0' and len(p) >= 7 and int(p[4]):
                flips.append((int(p[4]), int(p[3]), int(p[6]), int(p[5])))  # swap, at, wait, tag
        elif len(p) >= 9 and int(p[5]):
            frames.append(dict(start=int(p[4]), done=int(p[5]), locked=int(p[7]), band=int(p[8])))
    flips.sort()
    swaps = [f[0] for f in flips]
    period = pct([(b[1] - a[1]) / 1e6 for a, b in zip(flips, flips[1:])], .5)
    rows = {0: [], 1: []}
    for f in frames:
        i = bisect.bisect_left(swaps, f['done'])
        if i >= len(flips): continue
        swap, at, wait, tag = flips[i]
        rows[f['locked']].append(
            dict(
                work=(f['done'] - f['start']) / 1e6,
                to_swap=(swap - f['done']) / 1e6,
                hold=wait / 1e6,
                lat=(at - swap) / 1e6,
                total=(at - f['start']) / 1e6,
                band=f['band'] / 1e6
            )
        )
    print(path.split('/')[-1], f'period {period:.2f}')
    for locked, name in ((1, 'locked'), (0, 'free')):
        r = rows[locked]
        if not r: continue
        print(
            f"  {name:7}{len(r):5} frames: hand-off {pct([x['work'] for x in r], .5):5.1f}/{pct([x['work'] for x in r], .95):5.1f}"
            f"  hand-off to swap {pct([x['to_swap'] for x in r], .5):5.1f}/{pct([x['to_swap'] for x in r], .95):5.1f}"
            f"  of it held {pct([x['hold'] for x in r], .5):5.1f}"
            f"  swap to flip {pct([x['lat'] for x in r], .05):5.1f}/{pct([x['lat'] for x in r], .5):5.1f}/{pct([x['lat'] for x in r], .95):5.1f}"
            f"  start to flip {pct([x['total'] for x in r], .5):5.1f}/{pct([x['total'] for x in r], .95):5.1f}  band {pct([x['band'] for x in r], .5):4.1f}"
        )
    # runs of locked frames
    runs, n = [], 0
    for f in frames:
        if f['locked']: n += 1
        elif n:
            runs.append(n)
            n = 0
    if n: runs.append(n)
    print('  locked runs:', runs[:20])
