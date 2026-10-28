#!/usr/bin/env python3
"""Per-frame stage timings from the launcher's UiTrace record (src/platform/ui_trace.hpp), sampled every 0.2 ms.

  trace.py PATH SECONDS    prints one line per frame: total ms, then ms spent in each stage
"""
import mmap
import os
import struct
import sys
import time

STAGES = {
    1: "loop",
    2: "top-draw",
    3: "top-flush",
    4: "top-present",
    5: "bottom-draw",
    6: "bottom-flush",
    7: "bottom-present",
    8: "done",
    9: "playing",
    10: "assets",
    11: "events"
}
RECORD = struct.Struct("<IIIIQQII")


def main():
    path, seconds = sys.argv[1], float(sys.argv[2])
    end = time.monotonic() + seconds
    while not os.path.exists(path) or os.path.getsize(path) < RECORD.size:
        if time.monotonic() > end:
            sys.exit(f"{path}: no trace (is RGDS_SESSION_PLATFORM=independent set?)")
        time.sleep(0.05)
    with open(path, "rb") as f:
        view = mmap.mmap(f.fileno(), RECORD.size, prot=mmap.PROT_READ)
    last, entered, frame = None, None, {}
    first_count = count = None
    frames = []
    while time.monotonic() < end:
        seq, _, _, stage, count, _, _, _ = RECORD.unpack(view[:RECORD.size])
        if first_count is None and not seq % 2:
            first_count = count
        if seq % 2 or stage == last:
            time.sleep(0.0002)
            continue
        now = time.monotonic() * 1000
        if last is not None:
            frame[STAGES.get(last, str(last))] = frame.get(STAGES.get(last, str(last)), 0) + now - entered
        if stage == 8:
            frames.append(frame)
            frame = {}
        last, entered = stage, now
    for f in frames:
        busy = sum(v for k, v in f.items() if k not in ("loop", "done", "events"))
        print(f"{busy:7.1f} " + " ".join(f"{k}={v:.1f}" for k, v in sorted(f.items(), key=lambda kv: -kv[1])))
    if frames:
        busy = sorted(sum(v for k, v in f.items() if k not in ("loop", "done", "events")) for f in frames)
        print(
            f"rendered={count - first_count} sampled={len(frames)} median_busy={busy[len(busy) // 2]:.1f}ms p90={busy[int(len(busy) * .9)]:.1f}ms"
        )


main()
