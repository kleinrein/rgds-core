#!/usr/bin/env python3
"""Nearby probe link test: UDP round trips, TCP throughput, broadcast delivery between two handhelds.

  link_test.py server [seconds]        echo on 49400/udp, sink on 49401/tcp, broadcasts 5/s on 49402
  link_test.py client HOST             prints one line of key=value results
"""
import os, socket, struct, sys, time, threading, statistics

UDP, TCP, BCAST = 49400, 49401, 49402


def server(seconds):
    end = time.time() + seconds
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("0.0.0.0", UDP))
    u.settimeout(0.5)
    t = socket.socket()
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(("0.0.0.0", TCP))
    t.listen(2)
    t.settimeout(0.5)
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    def echo():
        while time.time() < end:
            try:
                d, a = u.recvfrom(2048)
                u.sendto(d, a)
            except socket.timeout:
                pass

    def sink():
        while time.time() < end:
            try:
                c, _ = t.accept()
            except socket.timeout:
                continue
            c.settimeout(2)
            try:
                while c.recv(65536):
                    pass
            except OSError:
                pass
            c.close()

    def beacon():
        seq = 0
        while time.time() < end:
            b.sendto(struct.pack("!4sI", b"RGNB", seq), (os.environ.get("BCAST_ADDR", "255.255.255.255"), BCAST))
            seq += 1
            time.sleep(0.2)

    ts = [threading.Thread(target=f, daemon=True) for f in (echo, sink, beacon)]
    for x in ts:
        x.start()
    for x in ts:
        x.join()


def client(host):
    out = {}
    # broadcast listener runs during the whole test
    bs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bs.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    bs.bind(("", BCAST))
    bs.settimeout(0.5)
    seen = set()
    stop = threading.Event()

    def listen():
        while not stop.is_set():
            try:
                d, _ = bs.recvfrom(64)
                if d[:4] == b"RGNB":
                    seen.add(struct.unpack("!I", d[4:8])[0])
            except socket.timeout:
                pass

    lt = threading.Thread(target=listen, daemon=True)
    lt.start()
    # UDP round trips: 500 at 20 ms
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(0.3)
    rtts, lost = [], 0
    for i in range(500):
        s = time.perf_counter()
        u.sendto(struct.pack("!I", i) + b"x" * 60, (host, UDP))
        try:
            while True:
                d, _ = u.recvfrom(2048)
                if struct.unpack("!I", d[:4])[0] == i:
                    rtts.append((time.perf_counter() - s) * 1000)
                    break
        except socket.timeout:
            lost += 1
        time.sleep(0.02)
    if rtts:
        q = sorted(rtts)
        out.update(rtt_p50=round(q[len(q) // 2], 2), rtt_p95=round(q[int(len(q) * .95)], 2),
                   rtt_p99=round(q[int(len(q) * .99)], 2), rtt_max=round(q[-1], 2),
                   rtt_jitter=round(statistics.pstdev(rtts), 2))
    out["udp_lost"] = lost
    # TCP throughput: 8 s
    try:
        c = socket.create_connection((host, TCP), timeout=3)
        buf = b"\0" * 65536
        n, s = 0, time.time()
        while time.time() - s < 8:
            n += c.send(buf)
        c.close()
        out["tcp_mbit"] = round(n * 8 / (time.time() - s) / 1e6, 1)
    except OSError as e:
        out["tcp_err"] = str(e)
    stop.set()
    lt.join()
    if seen:
        span = max(seen) - min(seen) + 1
        out["bcast_got"] = f"{len(seen)}/{span}"
    else:
        out["bcast_got"] = "0"
    print(" ".join(f"{k}={v}" for k, v in out.items()), flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "server":
        server(float(sys.argv[2]) if len(sys.argv) > 2 else 120)
    else:
        client(sys.argv[2])
