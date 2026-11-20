#!/usr/bin/env python3
"""Input-to-present latency of the running launcher: press a button on the real joypad node, then time how long
until the launcher's UiTrace record (src/platform/ui_trace.hpp) reports the next finished frame. Also reports the
frame rate while a direction is held. Both clocks are CLOCK_MONOTONIC.

  latency.py TRACE DEVICE [PRESSES]    e.g. latency.py /run/rgds-trace /dev/input/event7 20
"""
import mmap
import random
import struct
import sys
import time

RECORD = struct.Struct("<IIIIQQII")
EV_SYN, EV_KEY = 0, 1
RIGHT, LEFT = 0x223, 0x222
FRAME_DONE = 8


def read(view):
    while True:
        seq, _, _, stage, frames, ms, _, _ = RECORD.unpack(view[:RECORD.size])
        if not seq % 2:
            return stage, frames, ms


def emit(f, code, value):
    now = time.time()
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_KEY, code, value))
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_SYN, 0, 0))


def main():
    trace, device = sys.argv[1], sys.argv[2]
    presses = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    with open(trace, "rb") as t:
        view = mmap.mmap(t.fileno(), RECORD.size, prot=mmap.PROT_READ)
    results = []
    with open(device, "wb", buffering=0) as pad:
        for i in range(presses):
            time.sleep(random.uniform(0.4, 0.9))  # land anywhere in the idle wait
            _, before, _ = read(view)
            code = RIGHT if i % 2 == 0 else LEFT
            pressed = time.monotonic() * 1000
            emit(pad, code, 1)
            deadline = pressed + 1000
            while time.monotonic() * 1000 < deadline:
                stage, frames, ms = read(view)
                if frames > before and stage == FRAME_DONE:
                    results.append(ms - pressed)
                    break
                time.sleep(0.0002)
            else:
                results.append(None)
            emit(pad, code, 0)
        # held direction: frames per second over 2 s of repeat
        _, start_frames, _ = read(view)
        began = time.monotonic()
        emit(pad, RIGHT, 1)
        time.sleep(2)
        emit(pad, RIGHT, 0)
        _, end_frames, _ = read(view)
        held_fps = (end_frames - start_frames) / (time.monotonic() - began)
    seen = sorted(r for r in results if r is not None)
    missed = len(results) - len(seen)
    print("latency_ms " + " ".join(f"{r:.0f}" if r is not None else "miss" for r in results))
    if seen:
        print(f"median={seen[len(seen) // 2]:.0f}ms worst={seen[-1]:.0f}ms missed={missed} held_fps={held_fps:.1f}")


main()
