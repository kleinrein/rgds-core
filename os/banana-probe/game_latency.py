#!/usr/bin/env python3
"""Button-to-present latency inside a running game, with the bridge's RGDS_LATENCY_TRACE (emulator_bridge.cpp).

Presses a button on the real joypad node at random moments and reads the trace: when the engine first read the
press, when the next frame was presented, and when the frame after that (the one that read the press) was.
All times are CLOCK_MONOTONIC. The panel's own scan-out (up to one refresh after a page flip) comes on top.

  game_latency.py TRACE DEVICE [PRESSES] [CODE] [PRESSES_OUT]    e.g. game_latency.py /run/rgds/latency /dev/input/event7 20 304
PRESSES_OUT gets one "pressed_us seen_us" line per traced press, for press_to_flip.py.
"""
import random
import struct
import sys
import time

EV_SYN, EV_KEY = 0, 1


def emit(f, code, value):
    now = time.time()
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_KEY, code, value))
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_SYN, 0, 0))


def lines(path):
    try:
        with open(path) as stream:
            return stream.read().splitlines()
    except OSError:
        return []


def main():
    trace, device = sys.argv[1], sys.argv[2]
    presses = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    code = int(sys.argv[4]) if len(sys.argv) > 4 else 304  # BTN_SOUTH: B
    out = open(sys.argv[5], "w") if len(sys.argv) > 5 else None
    rows = []
    with open(device, "wb", buffering=0) as pad:
        for _ in range(presses):
            time.sleep(random.uniform(0.35, 0.7))
            known = len(lines(trace))
            pressed = time.monotonic() * 1e6
            emit(pad, code, 1)
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline and len(lines(trace)) == known:
                time.sleep(0.002)
            emit(pad, code, 0)
            new = lines(trace)[known:]
            if new:
                seen, first, second = (int(x) for x in new[0].split())
                rows.append(((seen - pressed) / 1000, (first - pressed) / 1000, (second - pressed) / 1000))
                if out: out.write(f"{pressed:.0f} {seen}\n")
    if out: out.close()
    if not rows:
        raise SystemExit("no presses traced: is RGDS_LATENCY_TRACE set for the game?")

    def median(values):
        return sorted(values)[len(values) // 2]

    for name, index in (("read by the engine", 0), ("next present", 1), ("present of the reading frame", 2)):
        values = [row[index] for row in rows]
        print(f"{name:30} median {median(values):6.1f} ms  worst {max(values):6.1f} ms")
    print(f"{len(rows)} of {presses} presses traced")


main()
