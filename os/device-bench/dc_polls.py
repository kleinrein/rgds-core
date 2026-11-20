# What a process's threads wait on in poll/ppoll, over 6 s (python3 dc_polls.py PID): for each sample of a thread
# asleep in poll, the files it polls (from the pollfd array in its memory, resolved through /proc/PID/fd) and the
# timeout. Answers "the main thread sits in poll 19% of the time: on what?".
import collections
import os
import struct
import sys
import time

pid = sys.argv[1]
POLL, PPOLL = 7, 73  # arm64: ppoll is 73; plain poll does not exist (7 is a placeholder that never matches)


def fd_name(fd):
    try:
        return os.readlink(f"/proc/{pid}/fd/{fd}")
    except OSError:
        return f"fd{fd}?"


mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
tids = {t: open(f"/proc/{pid}/task/{t}/comm").read().strip() for t in os.listdir(f"/proc/{pid}/task")}
seen = collections.Counter()
samples = collections.Counter()
end = time.time() + 6
while time.time() < end:
    for t in tids:
        try:
            fields = open(f"/proc/{pid}/task/{t}/syscall").read().split()
        except OSError:
            continue
        samples[t] += 1
        if not fields or fields[0] in ("running", "-1"): continue
        nr = int(fields[0])
        if nr != PPOLL: continue
        fds_ptr, nfds, timeout_ptr = int(fields[1], 16), int(fields[2], 16), int(fields[3], 16)
        try:
            mem.seek(fds_ptr)
            raw = mem.read(8 * min(nfds, 16))
            files = sorted({fd_name(struct.unpack_from("<ihh", raw, 8 * i)[0]) for i in range(len(raw) // 8)})
            timeout = "none"
            if timeout_ptr:
                mem.seek(timeout_ptr)
                sec, nsec = struct.unpack("<qq", mem.read(16))
                timeout = f"{sec * 1000 + nsec / 1e6:.1f} ms"
        except OSError:
            files, timeout = ["?"], "?"
        seen[(tids[t], ", ".join(files), timeout)] += 1
    time.sleep(0.002)
for (name, files, timeout), n in seen.most_common(10):
    t = next(k for k, v in tids.items() if v == name)
    print(f"{name}: {100 * n // max(samples[t], 1)}% in ppoll on [{files}] timeout {timeout}")
