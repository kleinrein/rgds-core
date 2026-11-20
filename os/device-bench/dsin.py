#!/usr/bin/env python3
"""Write a DSCore input log (DSIN v2) from a tiny script: "FRAME BUTTONS" lines, buttons held from that frame on.
Buttons: A B SELECT START RIGHT LEFT UP DOWN R L X Y, or "-" for none; "FRAME touch X Y" presses the pen for 6 frames.
usage: dsin.py OUT.dsin TOTAL_FRAMES < script"""
import struct, sys
BITS = {n: i for i, n in enumerate("A B SELECT START RIGHT LEFT UP DOWN R L X Y".split())}
out, total = sys.argv[1], int(sys.argv[2])
events = []
for line in sys.stdin:
    parts = line.split()
    if not parts or parts[0].startswith("#"): continue
    frame = int(parts[0])
    if parts[1] == "touch": events.append((frame, ("touch", int(parts[2]), int(parts[3]))))
    else: events.append((frame, ("keys", 0 if parts[1] == "-" else sum(1 << BITS[k] for k in parts[1:]))))
events.sort()
buttons, pen = 0, None
with open(out, "wb") as f:
    f.write(b"DSIN" + struct.pack("<II", 2, total) + b"\0" * 4)
    i = 0
    for frame in range(total):
        while i < len(events) and events[i][0] <= frame:
            kind = events[i][1]
            if kind[0] == "keys": buttons = kind[1]
            else: pen = (kind[1], kind[2], frame + 6)
            i += 1
        down = pen is not None and frame < pen[2]
        x, y = (pen[0], pen[1]) if down else (0, 0)
        f.write(struct.pack("<HBBB", buttons, x, y, 1 if down else 0) + b"\0" * 3 + b"\0" * 8)
print(out, total, "frames")
