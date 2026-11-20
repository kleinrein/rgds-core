#!/usr/bin/env python3
"""Print a slice of DSCore's DS_CADENCE_LOG frame by frame, with each frame's swap and page flip (upper panel).

  cadence_slice.py CADENCE_LOG FROM_S TO_S
"""
import sys

rows, flips = [], []
for line in open(sys.argv[1]):
    if line.startswith("#"): continue
    p = line.split()
    if p[0] == "f":
        if p[1] == "0": flips.append((int(p[3]), int(p[4]), int(p[5]), int(p[6])))  # at, swap, tag, wait
    else: rows.append((int(p[0]), int(p[4]), int(p[5]), int(p[6]), p[7], float(p[8])))
flips.sort(key=lambda f: f[1])
t0 = rows[0][1]
lo, hi = float(sys.argv[2]), float(sys.argv[3])
print("frame  start_ms  emu  idle | done->swap swap->flip held tag | locked band")
for n, s, d, nx, locked, band in rows:
    if not lo <= (s - t0) / 1e9 < hi: continue
    fl = next((f for f in flips if f[1] >= d), None)
    swap = (fl[1] - d) / 1e6 if fl else -1
    print(
        f"{n:5} {(s - t0) / 1e6:9.1f} {(d - s) / 1e6:5.1f} {(nx - d) / 1e6:5.1f} | {swap:5.1f} "
        f"{(fl[0] - fl[1]) / 1e6 if fl else -1:5.1f} {fl[3] / 1e6 if fl else -1:5.1f} {fl[2] if fl else -1} | {locked} {band / 1e6:.1f}"
    )
