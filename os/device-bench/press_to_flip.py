#!/usr/bin/env python3
"""Button press to completed page flip, from game_latency.py's press file and DSCore's DS_CADENCE_LOG.

For each press: the frame whose input poll first saw it (seen_us falls right after that frame's start_ns), when that
frame was handed to the present (done_ns), and the first flip of the upper panel (0) swapped at or after that
hand-off. The flip's completion is the vblank that starts scanning the frame out; the panel's own scan adds up to
one refresh on top. Also prints the flips' period and how many frames each flip repeated (flips with no new swap).

  press_to_flip.py PRESSES CADENCE_LOG    e.g. press_to_flip.py /run/rgds/presses /run/rgds/cadence
"""
import sys


def median(values):
    return sorted(values)[len(values) // 2] if values else float("nan")


def main():
    presses = [tuple(int(x) for x in line.split()) for line in open(sys.argv[1]) if line.strip()]
    frames, flips = [], []  # (start_ns, done_ns); (at_ns, swap_ns)
    for line in open(sys.argv[2]):
        if line.startswith("#"): continue
        parts = line.split()
        if parts[0] == "f":
            if parts[1] == "0": flips.append((int(parts[3]), int(parts[4])))
        elif len(parts) >= 7:
            frames.append((int(parts[4]), int(parts[5])))
    flips.sort()
    rows = []
    for pressed_us, seen_us in presses:
        seen = seen_us * 1000
        frame = max((f for f in frames if f[0] <= seen), default=None)
        if not frame or not frame[1]: continue
        flip = next((fl for fl in flips if fl[1] and fl[1] >= frame[1]), None)
        if not flip: continue
        pressed = pressed_us * 1000
        rows.append(
            ((seen - pressed) / 1e6, (frame[1] - pressed) / 1e6, (flip[1] - pressed) / 1e6, (flip[0] - pressed) / 1e6)
        )
    if not rows: raise SystemExit("no press matched a frame and a flip")
    for name, i in (
        ("read by the engine", 0), ("frame handed to present", 1), ("its swap", 2), ("its flip completed", 3)
    ):
        values = [r[i] for r in rows]
        print(f"{name:26} median {median(values):6.1f} ms  worst {max(values):6.1f} ms")
    gaps = [(b[0] - a[0]) / 1e6 for a, b in zip(flips, flips[1:]) if b[0] > a[0]]
    waits = [(fl[0] - fl[1]) / 1e6 for fl in flips if fl[1]]
    repeats = sum(1 for a, b in zip(flips, flips[1:]) if b[1] and a[1] and b[1] == a[1])
    print(
        f"{len(rows)} presses; {len(flips)} flips, period median {median(gaps):.2f} ms, "
        f"swap-to-flip median {median(waits):.1f} ms, worst {max(waits):.1f} ms; {repeats} repeated"
    )


main()
