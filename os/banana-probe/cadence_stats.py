#!/usr/bin/env python3
"""Frame-time picture from DSCore's DS_CADENCE_LOG: emulation time per frame, frame intervals, and the stalls.

  cadence_stats.py CADENCE_LOG [SKIP_SECONDS]   (default: the first 5 s are left out)
"""
import sys


def pct(values, p):
    return sorted(values)[min(len(values) - 1, int(len(values) * p))] if values else float("nan")


def main():
    rows = []
    for line in open(sys.argv[1]):
        if line.startswith("#") or line.startswith("f "): continue
        parts = line.split()
        if len(parts) >= 7: rows.append((int(parts[4]), int(parts[5]), int(parts[6]), parts[3], parts[7]))
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    t0 = rows[0][0]
    rows = [r for r in rows if (r[0] - t0) / 1e9 >= skip]
    work = [(d - s) / 1e6 for s, d, n, p, l in rows if d]
    gaps = [(n - s) / 1e6 for s, d, n, p, l in rows if n]
    print(
        f"{len(rows)} frames; emulation ms p50 {pct(work, .5):.1f} p90 {pct(work, .9):.1f} p99 {pct(work, .99):.1f} "
        f"max {max(work):.1f}; interval ms p50 {pct(gaps, .5):.1f} p90 {pct(gaps, .9):.1f} p99 {pct(gaps, .99):.1f} "
        f"max {max(gaps):.1f}; locked {sum(1 for r in rows if r[4] == '1')}; not presented {sum(1 for r in rows if r[3] == '0')}"
    )
    # Stalls: runs of frames whose interval is over two refreshes, with their time into the run.
    stalls, run = [], []
    for s, d, n, p, l in rows:
        if n and (n - s) / 1e6 > 33.4: run.append((s, (n - s) / 1e6, (d - s) / 1e6 if d else 0))
        elif run:
            stalls.append(run)
            run = []
    if run: stalls.append(run)
    print(f"{len(stalls)} stalls (intervals over 33 ms), {sum(len(s) for s in stalls)} frames in them")
    for s in sorted(stalls, key=lambda s: -sum(f[1] for f in s))[:8]:
        print(
            f"  at {(s[0][0] - t0) / 1e9:6.1f} s: {len(s)} frames, {sum(f[1] for f in s):.0f} ms, "
            f"emulation " + " ".join(f"{f[2]:.0f}" for f in s[:10]) + (" ..." if len(s) > 10 else "")
        )
    # Per second: mean interval and the worst, to see the slow stretches.
    bins = {}
    for s, d, n, p, l in rows:
        if n: bins.setdefault(int((s - t0) / 1e9), []).append((n - s) / 1e6)
    print(
        "per second (mean/max interval ms): " +
        " ".join(f"{k}:{sum(v)/len(v):.0f}/{max(v):.0f}" for k, v in sorted(bins.items()))
    )


main()
