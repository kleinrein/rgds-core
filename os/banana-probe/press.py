#!/usr/bin/env python3
"""Press buttons on a real evdev device by writing events into its node (the kernel hands them to every reader).

  press.py DEVICE DELAY STEP CODE...   after DELAY s, press each key CODE (hex or decimal), one every STEP s
"""
import struct
import sys
import time

EV_SYN, EV_KEY = 0, 1


def emit(f, kind, code, value):
    now = time.time()
    f.write(struct.pack("llHHi", int(now), int(now % 1 * 1e6), kind, code, value))


def main():
    device, delay, step = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    codes = [int(c, 0) for c in sys.argv[4:]]
    time.sleep(delay)
    with open(device, "wb", buffering=0) as f:
        for code in codes:
            emit(f, EV_KEY, code, 1)
            emit(f, EV_SYN, 0, 0)
            time.sleep(0.06)
            emit(f, EV_KEY, code, 0)
            emit(f, EV_SYN, 0, 0)
            time.sleep(max(0.0, step - 0.06))


main()
