#!/usr/bin/env python3
"""Screenshot the RG DS over SSH and stitch both screens into one picture.

The launcher (or, while a game runs, the emulator bridge) writes both 640x480 panels as BMPs when asked through a
file in the session folder, so this needs nothing on the device but SSH and its Python. One connection asks, waits
and streams both panels back; the stitched PNG is composed here with numpy only (no Pillow, no ImageMagick).

    make shot                         # DEVICE defaults to rgds.local
    tools/device_shot.py --style device --layout side --open
    tools/device_shot.py --from top.bmp bottom.bmp -o out.png    # restyle saved panels, no device needed
"""
import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import time
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DATA_DIRS = ('/data/rgds/userdata', '/mnt/mmc/rgds/userdata')

# Runs on the device (python3 over stdin). Asks for a capture and streams
# b'RGDS' + mode + u32 top length + u32 bottom length + the two BMP files.
REMOTE = r'''
import fcntl, json, os, struct, sys, time
mode, wake, timeout = sys.argv[1], sys.argv[2] == '1', float(sys.argv[3])
data = next((d for d in sys.argv[4:] if os.path.isdir(d + '/session')), None)
if data is None:
    sys.exit('no session folder under ' + ', '.join(sys.argv[4:]))
session = data + '/session/'
if mode == 'auto':
    mode = 'launcher'
    try:
        pid = json.load(open(session + 'core.json'))['pid']
        if os.path.isdir('/proc/%d' % pid):
            mode = 'game'
    except (OSError, ValueError, KeyError, TypeError):
        pass
if mode == 'game':
    request, top, bottom = (session + 'emulator.control.capture', session + 'emulator.control.capture-top.bmp',
                            session + 'emulator.control.capture-bottom.bmp')
else:
    request, top, bottom = session + 'capture-request', session + 'ui-snapshot-top.bmp', session + 'ui-snapshot-bottom.bmp'

def stamp(path):
    try:
        return os.stat(path).st_mtime_ns
    except OSError:
        return None

def tap(code=44):  # KEY_Z: ignored by the launcher outside text fields, but it wakes the idle event wait
    try:
        fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, 0x40045564, 0x01)
        fcntl.ioctl(fd, 0x40045565, code)
        os.write(fd, struct.pack('80sHHHHi', b'rgds-shot', 3, 1, 1, 1, 0) + bytes(1024))
        fcntl.ioctl(fd, 0x5501)
        time.sleep(0.6)
        for value in (1, 0):
            os.write(fd, struct.pack('qqHHi', 0, 0, 1, code, value))
            os.write(fd, struct.pack('qqHHi', 0, 0, 0, 0, 0))
            time.sleep(0.05)
        time.sleep(0.1)
        fcntl.ioctl(fd, 0x5502)
        os.close(fd)
    except OSError:
        pass

before = (stamp(top), stamp(bottom))
with open(request, 'w') as handle:
    handle.write('shot')
begin, woke = time.time(), False
while True:
    if not os.path.exists(request) and stamp(top) != before[0] and stamp(bottom) != before[1]:
        break
    if time.time() - begin > timeout:
        try:
            os.unlink(request)
        except OSError:
            pass
        sys.exit('timed out waiting for the %s to write both panels' % ('emulator bridge' if mode == 'game' else 'launcher'))
    if wake and mode == 'launcher' and not woke and time.time() - begin > 0.8:
        woke = True
        tap()
    time.sleep(0.05)
time.sleep(0.05)
top_bytes, bottom_bytes = open(top, 'rb').read(), open(bottom, 'rb').read()
out = sys.stdout.buffer
out.write(b'RGDS' + mode[:1].encode() + struct.pack('>II', len(top_bytes), len(bottom_bytes)) + top_bytes + bottom_bytes)
out.flush()
'''


def fetch(device, data_dirs, mode, wake, timeout):
    command = 'python3 - ' + ' '.join(
        shlex.quote(str(part)) for part in (mode, '1' if wake else '0', timeout, *data_dirs)
    )
    result = subprocess.run(
        [
            'ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o',
            'LogLevel=ERROR', '-o', 'ConnectTimeout=8', 'root@' + device if '@' not in device else device, command
        ],
        input=REMOTE.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    if result.returncode:
        raise SystemExit(
            'device: ' + (result.stderr.decode(errors='replace').strip() or 'ssh exit %d' % result.returncode)
        )
    blob = result.stdout
    if blob[:4] != b'RGDS' or len(blob) < 13:
        raise SystemExit('device sent an unexpected reply (%d bytes)' % len(blob))
    top_length, bottom_length = struct.unpack('>II', blob[5:13])
    if len(blob) != 13 + top_length + bottom_length:
        raise SystemExit('the capture arrived truncated')
    return ({b'l': 'launcher', b'g': 'game'}[blob[4:5]], blob[13:13 + top_length], blob[13 + top_length:])


def read_bmp(data):
    """Decode an uncompressed 24/32-bit BMP (bottom-up or top-down) to an HxWx3 uint8 array."""
    if data[:2] != b'BM':
        raise ValueError('not a BMP')
    offset, header = struct.unpack_from('<II', data, 10)
    width, height, _planes, depth, compression = struct.unpack_from('<iiHHI', data, 18)
    if depth not in (24, 32) or compression not in (0, 3):
        raise ValueError('unsupported BMP (%d-bit, compression %d)' % (depth, compression))
    rows, step = abs(height), (width * depth // 8 + 3) // 4 * 4
    raw = np.frombuffer(data, np.uint8, rows * step, offset).reshape(rows, step)[:, :width * depth // 8]
    pixels = raw.reshape(rows, width, depth // 8)[..., 2::-1]  # BGR(A) -> RGB
    return np.ascontiguousarray(pixels if height < 0 else pixels[::-1])


def write_png(path, pixels):
    """Write an HxWx3/4 uint8 array as a PNG using the Sub filter, which suits flat UI and gradients."""
    rows, width, channels = pixels.shape
    sub = pixels.copy()
    sub[:, 1:] = pixels[:, 1:] - pixels[:, :-1]
    body = np.concatenate([np.ones((rows, 1), np.uint8), sub.reshape(rows, width * channels)], axis=1)

    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))

    header = struct.pack('>IIBBBBB', width, rows, 8, 2 if channels == 3 else 6, 0, 0, 0)
    Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(body.tobytes(), 4)) +
        chunk(b'IEND', b'')
    )


# --- compositing -----------------------------------------------------------------------------------------------


def box_blur(a, radius):
    for axis in (0, 1):
        length = a.shape[axis]
        pad = [(0, 0)] * a.ndim
        pad[axis] = (radius + 1, radius)
        total = np.cumsum(np.pad(a, pad, mode='edge'), axis=axis, dtype=np.float64)
        lead = [slice(None)] * a.ndim
        lag = [slice(None)] * a.ndim
        lead[axis], lag[axis] = slice(2 * radius + 1, 2 * radius + 1 + length), slice(0, length)
        a = ((total[tuple(lead)] - total[tuple(lag)]) / (2 * radius + 1)).astype(np.float32)
    return a


def resize_linear(a, rows, cols):
    for axis, size in ((0, rows), (1, cols)):
        old = a.shape[axis]
        at = np.clip((np.arange(size) + 0.5) * old / size - 0.5, 0, old - 1)
        low = np.floor(at).astype(int)
        high = np.minimum(low + 1, old - 1)
        weight = (at - low).reshape([-1 if i == axis else 1 for i in range(a.ndim)]).astype(np.float32)
        a = np.take(a, low, axis) * (1 - weight) + np.take(a, high, axis) * weight
    return a


def soften(a, sigma, factor=4):
    """Gaussian-ish blur (three box passes) done at reduced resolution; plenty for glows and shadows."""
    rows, cols = a.shape[:2]
    pad = [(0, -rows % factor), (0, -cols % factor)] + [(0, 0)] * (a.ndim - 2)
    small = np.pad(a, pad).reshape(
        (rows + pad[0][1]) // factor, factor, (cols + pad[1][1]) // factor, factor, *a.shape[2:]
    )
    small = small.mean(axis=(1, 3)).astype(np.float32)
    for _ in range(3):
        small = box_blur(small, max(1, int(sigma / factor)))
    return resize_linear(small, rows + pad[0][1], cols + pad[1][1])[:rows, :cols]


def rounded(rows, cols, radius):
    """Anti-aliased rounded-rectangle coverage from a signed distance."""
    y = np.abs(np.arange(rows)[:, None] + 0.5 - rows / 2) - (rows / 2 - radius)
    x = np.abs(np.arange(cols)[None, :] + 0.5 - cols / 2) - (cols / 2 - radius)
    distance = np.hypot(np.maximum(x, 0), np.maximum(y, 0)) + np.minimum(np.maximum(x, y), 0) - radius
    return np.clip(0.5 - distance, 0, 1).astype(np.float32)


def over(canvas, y, x, rgb, alpha):
    """Premultiplied source-over of a straight-colour layer."""
    rows, cols = alpha.shape
    region = canvas[y:y + rows, x:x + cols]
    keep = 1 - alpha
    region[..., :3] = rgb * alpha[..., None] + region[..., :3] * keep[..., None]
    region[..., 3] = alpha + region[..., 3] * keep


def vertical(rows, cols, top, bottom):
    ramp = np.linspace(0, 1, rows, dtype=np.float32)[:, None, None]
    return np.broadcast_to(
        np.array(top, np.float32) * (1 - ramp) + np.array(bottom, np.float32) * ramp, (rows, cols, 3)
    )


def hex_colour(text):
    text = text.lstrip('#')
    if len(text) != 6:
        raise ValueError('colour must be #RRGGBB')
    return tuple(int(text[i:i + 2], 16) / 255 for i in (0, 2, 4))


SHELLS = {
    # The lid's outer shell colour (its rim shows around the face when open); the base is always dark.
    'red': ((0.96, 0.20, 0.18), (0.60, 0.06, 0.07)),
    'black': ((0.19, 0.19, 0.20), (0.09, 0.09, 0.10)),
    'white': ((0.98, 0.98, 0.99), (0.80, 0.81, 0.84)),
    'turquoise': ((0.36, 0.86, 0.86), (0.10, 0.58, 0.61)),
}
GLASS = np.array((0.014, 0.016, 0.021), np.float32)
INK = np.array((0.50, 0.51, 0.55), np.float32)
GLYPHS = {
    'A': ('01110', '10001', '10001', '11111', '10001', '10001', '10001'),
    'B': ('11110', '10001', '10001', '11110', '10001', '10001', '11110'),
    'X': ('10001', '10001', '01010', '00100', '01010', '10001', '10001'),
    'Y': ('10001', '10001', '01010', '00100', '00100', '00100', '00100'),
}


def rounded4(rows, cols, tl, tr, br, bl):
    """Rounded rectangle with a radius per corner."""
    y = np.arange(rows)[:, None] + 0.5 - rows / 2
    x = np.arange(cols)[None, :] + 0.5 - cols / 2
    r = np.where(y < 0, np.where(x < 0, tl, tr), np.where(x < 0, bl, br)).astype(np.float32)
    qx, qy = np.abs(x) - (cols / 2 - r), np.abs(y) - (rows / 2 - r)
    distance = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r
    return np.clip(0.5 - distance, 0, 1).astype(np.float32)


def disc(diameter):
    return rounded(diameter, diameter, diameter / 2)


def lit_edges(mask, edge, rising):
    """Coverage of the edges facing the light (top) and away from it (bottom), `edge` pixels deep."""
    below = np.roll(mask, edge, 0)
    below[:edge] = 0
    above = np.roll(mask, -edge, 0)
    above[-edge:] = 0
    lit, dark = np.clip(mask - below, 0, 1), np.clip(mask - above, 0, 1)
    return (lit, dark) if rising else (dark, lit)


def raised(canvas, y, x, mask, top, bottom, m, depth=1.0, shadow=0.45, light=0.38):
    """Paste a lit shape: soft drop shadow, vertical gradient, a bright top edge and a dark bottom edge."""
    rows, cols = mask.shape
    edge = max(1, int(round(0.3 * m * depth)))
    if shadow:
        pad = int(3 * m)
        cast = soften(np.roll(np.pad(mask, pad), int(0.6 * m * depth), 0), 0.8 * m * depth + 1, 1)
        over(canvas, y - pad, x - pad, np.zeros(3, np.float32), cast * shadow)
    over(canvas, y, x, vertical(rows, cols, top, bottom), mask)
    lit, dark = lit_edges(mask, edge, True)
    over(canvas, y, x, np.ones(3, np.float32), lit * light)
    over(canvas, y, x, np.zeros(3, np.float32), dark * light * 0.8)


def sunken(canvas, y, x, mask, top, bottom, m, strength=0.5):
    """Paste a recess: a fill, a dark upper rim (in shadow) and a bright lower rim."""
    rows, cols = mask.shape
    over(canvas, y, x, vertical(rows, cols, top, bottom), mask)
    edge = max(1, int(round(0.25 * m)))
    dark, lit = lit_edges(mask, edge, False)
    over(canvas, y, x, np.zeros(3, np.float32), dark * strength)
    over(canvas, y, x, np.ones(3, np.float32), lit * strength * 0.30)


def glyph(char, m, height_mm=3.0):
    cell = max(2, int(round(height_mm * m / 7)))
    rows = np.array([[c == '1' for c in line] for line in GLYPHS[char]], np.float32)
    return box_blur(np.pad(np.kron(rows, np.ones((cell, cell), np.float32)), 2), 1)


def dome(diameter):
    """A glossy black stick cap: a soft falloff to the rim and a specular spot up and to the left."""
    n = diameter
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    ry, rx = (yy + 0.5 - n / 2) / (n / 2), (xx + 0.5 - n / 2) / (n / 2)
    shade = 0.03 + 0.17 * np.clip(1 - (rx * rx + ry * ry), 0, 1)**0.8
    spot = 0.45 * np.exp(-(((rx + 0.32)**2 + (ry + 0.38)**2) / 0.05))
    return np.repeat(np.clip(shade + spot, 0, 1)[..., None], 3, axis=2).astype(np.float32)


def barrel(rows, cols):
    """The hinge cylinder: black, with a soft band of light along its upper third."""
    t = np.linspace(0, 1, rows, dtype=np.float32)[:, None]
    level = 0.045 + 0.17 * np.exp(-((t - 0.26) / 0.17)**2) + 0.03 * (1 - t)
    return np.broadcast_to(level[..., None], (rows, cols, 3))


def rg_ds_parts(m):
    """Silhouette pieces of the open handheld in group pixels: (y, x, mask) for lid, hinge barrel and base."""
    P = lambda mm: int(round(mm * m))
    return [
        (0, 0, rounded4(P(80), P(160), P(6), P(6), P(0.8), P(0.8))),
        (P(81.3), P(0.5), rounded(P(9), P(159), P(4.5))),
        (P(90.5), 0, rounded4(P(81.5), P(160), P(0.8), P(0.8), P(4.2), P(4.2))),
    ]


def rg_ds_spots(m):
    P = lambda mm: int(round(mm * m))
    return [(P(8.7), P(36)), (P(95.0), P(36))]  # 88 x 66 mm panels in the 90 x 69 / 90 x 70.6 mm wells


def draw_rg_ds(canvas, margin, m, shell):
    """The open RG DS, 160 x 172 mm: a shell-coloured lid with a grey face, black hinge and base. Panels go on top."""
    P = lambda mm: int(round(mm * m))
    at = lambda y, x: (margin + P(y), margin + P(x))
    mid = lambda y, x, mask: (margin + P(y) - mask.shape[0] // 2, margin + P(x) - mask.shape[1] // 2)
    lid, hinge, base = (part[2] for part in rg_ds_parts(m))
    rim = SHELLS[shell]
    key = ((0.215, 0.218, 0.232), (0.125, 0.128, 0.140))
    dark_key = tuple(c * 0.8 for c in key[1])
    # Lid: shell colour, a black lip, then the grey face with a recessed screen.
    raised(canvas, *at(0, 0), lid, *rim, m, depth=1.4, shadow=0, light=0.5)
    over(canvas, *at(2.3, 2.3), np.full(3, 0.03, np.float32), rounded4(P(80 - 2.3), P(155.4), P(4), P(4), 0, 0))
    face = rounded4(P(80 - 3.7), P(152.6), P(3), P(3), 0, 0)
    sunken(canvas, *at(3.7, 3.7), face, (0.30, 0.30, 0.315), (0.215, 0.215, 0.225), m, 0.6)
    sheen = 0.05 * np.clip(
        1 -
        (np.arange(face.shape[1])[None, :] / face.shape[1] * 0.7 + np.arange(face.shape[0])[:, None] / face.shape[0]) /
        0.95, 0, 1
    )**2
    over(canvas, *at(3.7, 3.7), np.ones(3, np.float32), sheen.astype(np.float32) * face)
    screen_well = rounded(P(69), P(90), P(1.4))
    over(canvas, *at(6.9, 34.7), np.full(3, 0.28, np.float32), rounded(P(69.6), P(90.6), P(1.6)) * 0.45)
    sunken(canvas, *at(7.2, 35), screen_well, (0.105, 0.106, 0.116), (0.066, 0.067, 0.075), m, 0.5)
    for cx in (19.3, 140.7):  # speaker: seven dots in a hexagon
        for dy, dx in ((-3.9, -2.3), (-3.9, 2.3), (0, -4.6), (0, 0), (0, 4.6), (3.9, -2.3), (3.9, 2.3)):
            dot = disc(max(2, P(1.5)))
            over(canvas, *mid(42.6 + dy, cx + dx, dot), np.full(3, 0.035, np.float32), dot)
    for cx, cy, strength in ((19.3, 9.8, 1), (19.3, 71, 1), (140.7, 9.8, 0.4), (140.7, 71, 0.4)):  # rubber stops
        stop = rounded(P(3.8), P(3.8), P(1.0))
        over(canvas, *mid(cy, cx, stop), np.full(3, 0.36, np.float32), stop * 0.35 * strength)
    # Base: black, a deep pane around the screen, the lip under the screen.
    raised(canvas, *at(90.5, 0), base, (0.168, 0.171, 0.184), (0.100, 0.103, 0.113), m, depth=1.4, shadow=0, light=0.4)
    pane = rounded(P(77.1), P(93.8), P(3))
    sunken(canvas, *at(91.4, 33.1), pane, (0.012, 0.013, 0.016), (0.012, 0.013, 0.016), m, 0.35)
    over(canvas, *at(92.7, 35), np.full(3, 0.075, np.float32), rounded(P(70.6), P(90), P(1.5)))
    # D-pad: two crossed bars with a glossy dome in the middle.
    arm, span = P(7.3), P(21.5)
    cross = np.maximum(
        np.pad(rounded(arm, span, P(1.8)), (((span - arm) // 2, span - arm - (span - arm) // 2), (0, 0))),
        np.pad(rounded(span, arm, P(1.8)), ((0, 0), ((span - arm) // 2, span - arm - (span - arm) // 2)))
    )
    raised(canvas, *mid(90.5 + 17.4, 17.4, cross), cross, *key, m, depth=1.1, shadow=0.5, light=0.45)
    cap = disc(P(4.4))
    over(canvas, *mid(90.5 + 17.4, 17.4, cap), dome(cap.shape[0]), cap)
    # Face buttons: X on top, B below, Y left, A right; flat caps with faint print.
    face_button = disc(P(8.1))
    for char, dx, dy in (('X', 0, -8.2), ('B', 0, 8.2), ('Y', -8.2, 0), ('A', 8.2, 0)):
        raised(
            canvas,
            *mid(90.5 + 17.4 + dy, 142.7 + dx, face_button),
            face_button,
            *key,
            m,
            depth=0.7,
            shadow=0.45,
            light=0.4
        )
        g = glyph(char, m)
        over(canvas, *mid(90.5 + 17.4 + dy, 142.7 + dx, g), INK, g * 0.55)
    # Sticks: a black well, a bevelled grey ring, a dark groove, a glossy dome.
    for cx in (17.4, 142.5):
        cy = 90.5 + 48.7
        well = disc(P(24.8))
        sunken(canvas, *mid(cy, cx, well), well, (0.004, 0.004, 0.006), (0.012, 0.012, 0.015), m, 0.8)
        ring = disc(P(18.6))
        raised(
            canvas,
            *mid(cy, cx, ring),
            ring, (0.34, 0.345, 0.36), (0.16, 0.162, 0.175),
            m,
            depth=0.8,
            shadow=0.0,
            light=0.5
        )
        groove = disc(P(15.6))
        over(canvas, *mid(cy, cx, groove), np.full(3, 0.05, np.float32), groove)
        stick = disc(P(13))
        over(canvas, *mid(cy, cx + 0.3, stick), dome(stick.shape[0]), stick)
    # Four small keys (R/menu left, select/start right).
    for cx in (10.8, 23.5, 136.2, 148.8):
        small = disc(P(8.6))
        raised(canvas, *mid(90.5 + 69.6, cx, small), small, *key, m, depth=0.6, shadow=0.35, light=0.35)
        inner = disc(P(5.6))
        sunken(canvas, *mid(90.5 + 69.6, cx, inner), inner, dark_key, key[1], m, 0.25)
    # Green status lights at the right edge.
    glow = np.zeros((P(8), P(8)), np.float32)
    for cy in (60.3, 65.7):
        bar = rounded(max(2, P(1.5)), P(3.8), P(0.7))
        light = np.zeros((P(8), P(12)), np.float32)
        light[P(4) - bar.shape[0] // 2:P(4) - bar.shape[0] // 2 + bar.shape[0], P(4):P(4) + bar.shape[1]] = bar
        halo = soften(light, 1.2 * m, 1)
        over(
            canvas, margin + P(90.5 + cy) - P(4), margin + P(156) - P(4), np.array((0.25, 0.95, 0.25), np.float32),
            np.clip(halo * 1.4, 0, 1) * 0.45
        )
        over(
            canvas, margin + P(90.5 + cy) - P(4), margin + P(156) - P(4), np.array((0.55, 1.0, 0.5), np.float32), light
        )
    # Hinge: the lid's latch plate and lugs, then the black barrel with its seams and microphone.
    lug = rounded(max(2, P(1.0)), P(6.0), P(0.4))
    plate = rounded(max(2, P(1.0)), P(57.4), P(0.45))
    over(
        canvas, *at(79.5, 51.3), vertical(plate.shape[0], plate.shape[1], (0.50, 0.50, 0.52), (0.20, 0.20, 0.22)),
        plate * 0.9
    )
    for x in (8.4, 145.6):
        over(
            canvas, *at(79.6, x), vertical(lug.shape[0], lug.shape[1], (0.40, 0.40, 0.42), (0.16, 0.16, 0.18)),
            lug * 0.9
        )
    raised(canvas, *at(81.3, 0.5), hinge, (0.20, 0.20, 0.215), (0.02, 0.02, 0.025), m, depth=1.0, shadow=0.35)
    over(canvas, *at(81.3, 0.5), barrel(hinge.shape[0], hinge.shape[1]), hinge * 0.96)
    for x in (45, 115):
        seam = rounded(P(7.6), max(1, P(0.25)), 0.5)
        over(canvas, *at(82, x), np.zeros(3, np.float32), seam * 0.7)
    mic = disc(max(2, P(0.9)))
    over(canvas, *mid(86, 80, mic), np.zeros(3, np.float32), mic)


def compose(top, bottom, style='float', layout='stack', bg='auto', scale=2, shell='red'):
    u = scale
    big = lambda a: np.repeat(np.repeat(a, u, 0), u, 1) if u > 1 else a
    top, bottom = big(top), big(bottom)
    if style == 'plain':
        return np.concatenate([top, bottom], axis=0 if layout == 'stack' else 1)
    th, tw, bh, bw = top.shape[0], top.shape[1], bottom.shape[0], bottom.shape[1]
    device = style == 'device'
    gap = 40 * u
    if device:  # a clamshell only opens one way: top over bottom, 88 mm wide panels in a 160 mm body
        m = tw / 88
        parts = rg_ds_parts(m)
        gw, gh = int(round(160 * m)), int(round(172 * m))
        spots = rg_ds_spots(m)
    elif layout == 'stack':
        gw, gh = max(tw, bw), th + gap + bh
        spots = [(0, (gw - tw) // 2), (th + gap, (gw - bw) // 2)]
    else:
        gw, gh = tw + gap + bw, max(th, bh)
        spots = [((gh - th) // 2, 0), ((gh - bh) // 2, tw + gap)]
    margin = 110 * u
    height, width = gh + 2 * margin, gw + 2 * margin
    canvas = np.zeros((height, width, 4), np.float32)
    transparent = bg == 'transparent'
    if not transparent:
        if bg == 'auto':
            base = vertical(height, width, (0.075, 0.09, 0.14), (0.02, 0.025, 0.045))
        else:
            base = np.broadcast_to(np.array(hex_colour(bg), np.float32), (height, width, 3))
        yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
        edge = np.hypot((xx - width / 2) / (width / 2), (yy - height / 2) / (height / 2))
        canvas[..., :3] = base * (1 - 0.35 * np.clip(edge - 0.35, 0, 1)**1.5)[..., None]
        canvas[..., 3] = 1
    sr = int(1.5 * m) if device else 16 * u
    masks = [rounded(s.shape[0], s.shape[1], sr) for s in (top, bottom)]
    screens = [top.astype(np.float32) / 255, bottom.astype(np.float32) / 255]
    if device:
        yy, xx = np.mgrid[0:th, 0:tw].astype(np.float32)
        sheen = 0.07 * np.clip(1 - (xx / tw * 0.6 + yy / th * 0.4) / 0.55, 0, 1)**2
        screens = [np.clip(s + sheen[:s.shape[0], :s.shape[1], None], 0, 1) for s in screens]
    # Bias lighting: a heavily blurred copy of the panels, screen-blended into the backdrop.
    if not transparent:
        light = np.zeros((height, width, 3), np.float32)
        for (y, x), s, mk in zip(spots, screens, masks):
            light[margin + y:margin + y + s.shape[0], margin + x:margin + x + s.shape[1]] = s * mk[..., None]
        light = np.clip(soften(light, 58 * u) * 1.6, 0, 1)**0.85
        canvas[..., :3] = 1 - (1 - canvas[..., :3]) * (1 - light * 0.95)
    # Shadow under the whole group.
    shape = np.zeros((height, width), np.float32)
    pieces = [(y, x, mk) for y, x, mk in parts] if device else [(y, x, mk) for (y, x), mk in zip(spots, masks)]
    for y, x, mk in pieces:
        region = shape[margin + y:margin + y + mk.shape[0], margin + x:margin + x + mk.shape[1]]
        np.maximum(region, mk, out=region)
    shadow = soften(np.roll(shape, 26 * u, axis=0), 30 * u) * 0.7
    canvas[..., :3] *= (1 - shadow)[..., None]
    canvas[..., 3] = shadow + canvas[..., 3] * (1 - shadow)
    if device:
        draw_rg_ds(canvas, margin, m, shell)
    for (y, x), s, mk in zip(spots, screens, masks):
        over(canvas, margin + y, margin + x, s, mk)
        if not device:  # hairline so the panel reads against a dark backdrop
            hair = np.clip(mk - np.pad(rounded(mk.shape[0] - 2 * u, mk.shape[1] - 2 * u, sr - u), u), 0, 1)
            over(canvas, margin + y, margin + x, np.ones((*mk.shape, 3), np.float32), hair * 0.14)
    alpha = canvas[..., 3:]
    if transparent:
        rgb = np.where(alpha > 0, canvas[..., :3] / np.maximum(alpha, 1e-6), 0)
        return np.concatenate([np.clip(rgb * 255 + 0.5, 0, 255),
                               np.clip(alpha * 255 + 0.5, 0, 255)], axis=2).astype(np.uint8)
    dither = np.random.default_rng(0).random((height, width, 1), dtype=np.float32) - 0.5
    return np.clip(canvas[..., :3] * 255 + 0.5 + dither, 0, 255).astype(np.uint8)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0], formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        '-d',
        '--device',
        default=os.environ.get('DEVICE') or 'rgds.local',
        help='SSH address (default: $DEVICE or rgds.local)'
    )
    parser.add_argument('-o', '--out', help='output PNG (default: build/shots/rgds-<time>.png)')
    parser.add_argument(
        '--source',
        choices=('auto', 'launcher', 'game'),
        default='auto',
        help='who writes the panels: auto picks the emulator bridge while a game runs'
    )
    parser.add_argument(
        '--style',
        choices=('float', 'device', 'plain'),
        default='float',
        help=
        'float: rounded panels, shadow and bias lighting; device: an open RG DS (top over bottom only); plain: bare pixels'
    )
    parser.add_argument('--layout', choices=('stack', 'side'), default='stack', help='top over bottom, or side by side')
    parser.add_argument('--bg', default='auto', help="auto, transparent, or a #RRGGBB colour")
    parser.add_argument(
        '--scale',
        type=int,
        choices=(1, 2, 3, 4),
        help='integer pixel multiplier (default 2, or 1 for the device style: its shell is 160 mm of 8 px)'
    )
    parser.add_argument(
        '--shell',
        choices=tuple(SHELLS),
        default='red',
        help='device style lid colour: red, black, white or turquoise (the base is always dark)'
    )
    parser.add_argument('--raw', action='store_true', help='also keep each panel as its own PNG')
    parser.add_argument('--open', action='store_true', help='open the result (macOS)')
    parser.add_argument('--copy', action='store_true', help='copy the result to the clipboard (macOS), ready to paste')
    parser.add_argument('--no-wake', action='store_true', help='do not tap a key to wake an idle launcher')
    parser.add_argument('--timeout', type=float, default=8, help='seconds to wait for the panels')
    parser.add_argument('--data', action='append', help='userdata folder on the device (default: the usual locations)')
    parser.add_argument(
        '--from',
        dest='local',
        nargs=2,
        metavar=('TOP', 'BOTTOM'),
        help='compose two saved BMP panels instead of asking the device'
    )
    args = parser.parse_args()
    if args.local:
        top_bytes, bottom_bytes = (Path(p).read_bytes() for p in args.local)
        source = 'saved'
    else:
        source, top_bytes, bottom_bytes = fetch(
            args.device, args.data or DATA_DIRS, args.source, not args.no_wake, args.timeout
        )
    top, bottom = read_bmp(top_bytes), read_bmp(bottom_bytes)
    out = Path(args.out) if args.out else ROOT / 'build/shots' / time.strftime('rgds-%Y%m%d-%H%M%S.png')
    out.parent.mkdir(parents=True, exist_ok=True)
    started = time.time()
    write_png(
        out,
        compose(
            top, bottom, args.style, args.layout, args.bg, args.scale or (1 if args.style == 'device' else 2),
            args.shell
        )
    )
    if args.raw:
        write_png(out.with_name(out.stem + '-top.png'), top)
        write_png(out.with_name(out.stem + '-bottom.png'), bottom)
    print('%s (%s, %d KiB, composed in %.1fs)' % (out, source, out.stat().st_size // 1024, time.time() - started))
    if args.copy and sys.platform == 'darwin':
        subprocess.run(
            ['osascript', '-e',
             'set the clipboard to (read (POSIX file "%s") as «class PNGf»)' % out.resolve()],
            check=False
        )
    if args.open and sys.platform == 'darwin':
        subprocess.run(['open', str(out)], check=False)


if __name__ == '__main__':
    main()
