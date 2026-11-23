# GPU busy time of one process, per engine, from Panfrost's fdinfo (python3 dc_gpu.py NAME DELAY SECONDS).
# Needs /sys/bus/platform/drivers/panfrost/fde60000.gpu/profiling = 1 (the run script sets it), else the engine
# counters stay absent. Waits DELAY s for a process called NAME, then prints, once a second, how many ms of each
# engine's time that process used in that second (fragment = pixels, vertex-tiler = geometry + binning), and the total.
import os, sys, time

name, delay, seconds = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])


def find():
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                if open(f"/proc/{p}/comm").read().strip() == name: return p
            except OSError:
                pass


def snap(pid):
    out, seen = {}, set()
    for fd in os.listdir(f"/proc/{pid}/fdinfo"):
        try:
            text = open(f"/proc/{pid}/fdinfo/{fd}").read()
        except OSError:
            continue
        if "drm-driver:\tpanfrost" not in text: continue
        fields = dict(l.split(":", 1) for l in text.splitlines() if ":" in l)
        client = fields.get("drm-client-id", "").strip()
        if client in seen: continue  # dup'ed fds of one client
        seen.add(client)
        for k, v in fields.items():
            if k.startswith(("drm-engine-", "drm-cycles-")):
                out[k] = out.get(k, 0) + int(v.split()[0])
    return out


time.sleep(delay)
pid = find()
if not pid: raise SystemExit(f"no {name}")
prev, t0 = snap(pid), time.monotonic()
totals = {}
for _ in range(seconds):
    time.sleep(1)
    cur, t1 = snap(pid), time.monotonic()
    d = {k: cur[k] - prev.get(k, 0) for k in cur}
    span = t1 - t0
    frag = d.get("drm-engine-fragment", 0) / 1e6 / span
    vt = d.get("drm-engine-vertex-tiler", 0) / 1e6 / span
    print(f"gpu ms/s fragment {frag:6.1f} vertex-tiler {vt:6.1f} cycles-frag {d.get('drm-cycles-fragment', 0) / span / 1e6:.0f}M"
          f" cycles-vt {d.get('drm-cycles-vertex-tiler', 0) / span / 1e6:.0f}M", flush=True)
    for k, v in d.items(): totals[k] = totals.get(k, 0) + v
    prev, t0 = cur, t1
print("gpu total", " ".join(f"{k[4:]}={v / 1e6 / seconds:.1f}ms/s" if "engine" in k else f"{k[4:]}={v / 1e6 / seconds:.0f}M/s"
                            for k, v in sorted(totals.items())), flush=True)
