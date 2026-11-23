# Watch a real launch without touching it (python3 dc_watch.py SECONDS): every 5 s, the busiest processes and the
# game's threads (CPU %, allowed CPUs, policy), each Panfrost client's GPU time (fdinfo; needs the panfrost profiling
# knob on), CPU/GPU clocks and temperatures. For "fast in the script, slow when the owner plays" (2026-11-23).
import os, sys, time

GPU = "/sys/bus/platform/drivers/panfrost/fde60000.gpu"


def read(path, default=""):
    try:
        with open(path) as f: return f.read()
    except OSError:
        return default


def procs():
    out = {}
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        s = read(f"/proc/{p}/stat")
        if not s: continue
        f = s[s.rindex(")") + 2:].split()
        out[p] = (s[s.index("(") + 1:s.rindex(")")], int(f[11]) + int(f[12]))
    return out


def threads(pid):
    out = {}
    for t in os.listdir(f"/proc/{pid}/task") if os.path.isdir(f"/proc/{pid}/task") else []:
        s = read(f"/proc/{pid}/task/{t}/stat")
        if not s: continue
        f = s[s.rindex(")") + 2:].split()
        allowed = next((l.split()[1] for l in read(f"/proc/{pid}/task/{t}/status").splitlines()
                        if l.startswith("Cpus_allowed_list")), "?")
        out[t] = (s[s.index("(") + 1:s.rindex(")")], int(f[11]) + int(f[12]), f[36], f[38], allowed)
    return out


def gpu():
    out = {}
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            fds = os.listdir(f"/proc/{p}/fdinfo")
        except OSError:
            continue
        seen = set()
        for fd in fds:
            text = read(f"/proc/{p}/fdinfo/{fd}")
            if "drm-driver:\tpanfrost" not in text: continue
            fields = dict(l.split(":", 1) for l in text.splitlines() if ":" in l)
            client = fields.get("drm-client-id", "").strip()
            if client in seen: continue
            seen.add(client)
            ns = sum(int(v.split()[0]) for k, v in fields.items() if k.startswith("drm-engine-"))
            out[p] = out.get(p, 0) + ns
    return out


def game():
    for p in os.listdir("/proc"):
        if p.isdigit() and read(f"/proc/{p}/comm").strip() == "rgds-play": return p


seconds = int(sys.argv[1])
end = time.monotonic() + seconds
a, g, t0 = procs(), gpu(), time.monotonic()
pid = game()
ta = threads(pid) if pid else {}
while time.monotonic() < end:
    time.sleep(5)
    b, h, t1 = procs(), gpu(), time.monotonic()
    span = t1 - t0
    busy = sorted(((v[1] - a.get(k, (0, v[1]))[1]) * 100 / 100 / span, k, v[0]) for k, v in b.items())[::-1][:6]
    print(f"--- {time.strftime('%H:%M:%S')}", flush=True)
    print(" cpu%: " + ", ".join(f"{n}({k}) {c:.0f}" for c, k, n in busy if c >= 1), flush=True)
    print(" gpu ms/s: " + ", ".join(f"{b.get(k, ('?',))[0]}({k}) {(v - g.get(k, 0)) / 1e6 / span:.0f}"
                                    for k, v in h.items() if v - g.get(k, 0) > 1e6), flush=True)
    freqs = [read(f"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq").strip() for i in range(4)]
    temps = [read(f"/sys/class/thermal/thermal_zone{i}/temp").strip() for i in range(2)]
    print(f" cpu MHz {[int(x) // 1000 if x else 0 for x in freqs]} gov {read('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').strip()}"
          f" gpu {read('/sys/class/devfreq/fde60000.gpu/cur_freq').strip()} temps {temps}", flush=True)
    pid = pid or game()
    if pid:
        tb = threads(pid)
        rows = sorted(((v[1] - ta.get(k, (0, v[1]))[1]) / span, k, v) for k, v in tb.items())[::-1][:5]
        print(" game threads: " + "; ".join(f"{v[0]} {c:.0f}% cpu{v[2]} pol{v[3]} allowed {v[4]}" for c, k, v in rows),
              flush=True)
        ta = tb
    a, g, t0 = b, h, t1
