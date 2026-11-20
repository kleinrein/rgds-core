# CPU use per thread of a running process over 5 s, busiest first (python3 dc_threads.py PID); BusyBox has no top -H.
import os, sys, time

pid = sys.argv[1]


def snap():
    out = {}
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            s = open(f"/proc/{pid}/task/{t}/stat").read()
            comm = s[s.index("(") + 1:s.rindex(")")]
            f = s[s.rindex(")") + 2:].split()
            out[t] = (comm, int(f[11]) + int(f[12]), f[36])
        except OSError:
            pass
    return out


a = snap()
time.sleep(5)
b = snap()
for t, (comm, ticks, cpu) in sorted(b.items(), key=lambda kv: -(kv[1][1] - a.get(kv[0], ("", kv[1][1]))[1])):
    print(t, comm, f"{(ticks - a.get(t, (comm, ticks))[1]) * 100 // 500}%", "cpu" + cpu)
