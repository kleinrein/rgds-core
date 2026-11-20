#!/usr/bin/env python3
"""Button press to the page flip of the first frame that answers it, from game_latency.py's press file and a
DS_CADENCE_LOG written with DS_CADENCE_HASH=1 (a hash of each screen's picture per frame).

Needs a still scene whose picture changes only on the press (a menu: alternate Down/Up so the cursor moves every
time). For each press: the frame whose input poll first saw it (as press_to_flip.py), then the first frame from there
whose picture on SCREEN (0 top, 1 bottom, default both) differs from the frame before, and the first upper-panel flip
swapped at or after that frame's hand-off. "frames after reading" is how many emulated frames the game took to show
the press: what DS_FRAME_EDGE moves.

  press_to_change.py PRESSES CADENCE_LOG [SCREEN]    e.g. press_to_change.py /run/rgds/presses /run/rgds/cadence 1
"""
import sys


def median(values):
    return sorted(values)[len(values) // 2] if values else float("nan")


def main():
    presses = [tuple(int(x) for x in line.split()) for line in open(sys.argv[1]) if line.strip()]
    screens = (int(sys.argv[3]),) if len(sys.argv) > 3 else (0, 1)
    frames, flips = [], []  # (start_ns, done_ns, picture); (at_ns, swap_ns)
    for line in open(sys.argv[2]):
        if line.startswith("#"): continue
        parts = line.split()
        if parts[0] == "f":
            if parts[1] == "0": flips.append((int(parts[3]), int(parts[4])))
        elif len(parts) >= 11:
            frames.append((int(parts[4]), int(parts[5]), tuple(parts[9 + s] for s in screens)))
    if not frames: raise SystemExit("no hashed frames: was DS_CADENCE_HASH=1 set?")
    flips.sort()
    rows, unanswered = [], 0
    for pressed_us, seen_us in presses:
        seen, pressed = seen_us * 1000, pressed_us * 1000
        reading = max((i for i, f in enumerate(frames) if f[0] <= seen), default=None)
        if reading is None or reading == 0: continue
        answer = next(
            (i for i in range(reading, min(reading + 8, len(frames))) if frames[i][2] != frames[i - 1][2]), None
        )
        if answer is None or not frames[answer][1]:
            unanswered += 1
            continue
        flip = next((fl for fl in flips if fl[1] and fl[1] >= frames[answer][1]), None)
        if not flip: continue
        rows.append((answer - reading, (frames[answer][1] - pressed) / 1e6, (flip[0] - pressed) / 1e6))
    if not rows: raise SystemExit("no press matched a changed frame and a flip")
    after = [r[0] for r in rows]
    print("frames after reading       " + " ".join(f"{n}:{after.count(n)}" for n in sorted(set(after))))
    for name, i in (("answering frame handed", 1), ("its flip completed", 2)):
        values = [r[i] for r in rows]
        print(f"{name:26} median {median(values):6.1f} ms  worst {max(values):6.1f} ms")
    print(f"{len(rows)} presses answered within 8 frames, {unanswered} not")


main()
