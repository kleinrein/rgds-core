# Where each thread of a running process is over 6 s, sampled every 2 ms: running, or asleep in which syscall and
# kernel wait (python3 dc_states.py PID). Tells GPU waits (dma_fence_default_wait) from computing.
import collections
import os
import sys
import time

pid = sys.argv[1]
tids = {t: open(f"/proc/{pid}/task/{t}/comm").read().strip() for t in os.listdir(f"/proc/{pid}/task")}
counts = {t: collections.Counter() for t in tids}
end, n = time.time() + 6, 0
while time.time() < end:
    for t in tids:
        try:
            state = open(f"/proc/{pid}/task/{t}/stat").read().rsplit(")", 1)[1].split()[0]
            wchan = open(f"/proc/{pid}/task/{t}/wchan").read().strip()
        except OSError:
            continue
        counts[t]["running" if state == "R" else f"waiting {wchan}"] += 1
    n += 1
    time.sleep(0.002)
for t, c in counts.items():
    print(tids[t], ", ".join(f"{k} {v * 100 // n}%" for k, v in c.most_common(4)))
