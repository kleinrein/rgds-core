#!/usr/bin/env python3
"""Drive a race for a benchmark recording: A held, a steer left or right every few seconds, on the joypad node.

  bench_record.py DEVICE SECONDS     e.g. bench_record.py /dev/input/event7 40
Runs beside a game started with --record (see pace_trial.sh); the engine writes one input record per frame.
"""
import struct
import sys
import time

EV_SYN, EV_KEY = 0, 1
BTN_SOUTH, BTN_EAST, BTN_DPAD_LEFT, BTN_DPAD_RIGHT = 304, 305, 0x222, 0x223


def emit(f, code, value):
    now = time.time()
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_KEY, code, value))
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), EV_SYN, 0, 0))


def main():
    device, seconds = sys.argv[1], float(sys.argv[2])
    end = time.monotonic() + seconds
    with open(device, "wb", buffering=0) as pad:
        emit(pad, BTN_EAST, 1)  # B: accelerate on the DS pad (BTN_EAST is the DS A here, see the bridge mapping)
        emit(pad, BTN_SOUTH, 1)
        side = BTN_DPAD_LEFT
        while time.monotonic() < end:
            emit(pad, side, 1)
            time.sleep(0.6)
            emit(pad, side, 0)
            side = BTN_DPAD_RIGHT if side == BTN_DPAD_LEFT else BTN_DPAD_LEFT
            time.sleep(2.4)
        emit(pad, BTN_SOUTH, 0)
        emit(pad, BTN_EAST, 0)


main()
