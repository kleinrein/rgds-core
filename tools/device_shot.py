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


def compose(top, bottom, style='float', layout='stack', bg='auto', scale=2):
    u = scale
    big = lambda a: np.repeat(np.repeat(a, u, 0), u, 1) if u > 1 else a
    top, bottom = big(top), big(bottom)
    if style == 'plain':
        return np.concatenate([top, bottom], axis=0 if layout == 'stack' else 1)
    th, tw, bh, bw = top.shape[0], top.shape[1], bottom.shape[0], bottom.shape[1]
    device = style == 'device'
    pad, hinge, gap = (48 * u, 64 * u, 40 * u)
    inner = pad if device else 0
    between = hinge if device else gap
    # Group geometry (the body for "device", the two screens for "float"), screens placed inside it.
    if layout == 'stack':
        gw, gh = max(tw, bw) + 2 * inner, th + between + bh + 2 * inner
        spots = [(inner, (gw - tw) // 2), (inner + th + between, (gw - bw) // 2)]
    else:
        gw, gh = tw + between + bw + 2 * inner, max(th, bh) + 2 * inner
        spots = [((gh - th) // 2, inner), ((gh - bh) // 2, inner + tw + between)]
    margin = 110 * u
    height, width = gh + 2 * margin, gw + 2 * margin
    canvas = np.zeros((height, width, 4), np.float32)
    transparent = bg == 'transparent'
    if transparent:
        pass
    else:
        if bg == 'auto':
            base = vertical(height, width, (0.075, 0.09, 0.14), (0.02, 0.025, 0.045))
        else:
            base = np.broadcast_to(np.array(hex_colour(bg), np.float32), (height, width, 3))
        yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
        edge = np.hypot((xx - width / 2) / (width / 2), (yy - height / 2) / (height / 2))
        canvas[..., :3] = base * (1 - 0.35 * np.clip(edge - 0.35, 0, 1)**1.5)[..., None]
        canvas[..., 3] = 1
    sr = 10 * u if device else 16 * u
    masks = [rounded(s.shape[0], s.shape[1], sr) for s in (top, bottom)]
    screens = [top.astype(np.float32) / 255, bottom.astype(np.float32) / 255]
    if device:
        yy, xx = np.mgrid[0:th, 0:tw].astype(np.float32)
        sheen = 0.07 * np.clip(1 - (xx / tw * 0.6 + yy / th * 0.4) / 0.55, 0, 1)**2
        screens = [np.clip(s + sheen[:s.shape[0], :s.shape[1], None], 0, 1) for s in screens]
    # Bias lighting: a heavily blurred copy of the panels, screen-blended into the backdrop.
    if not transparent:
        light = np.zeros((height, width, 3), np.float32)
        for (y, x), s, m in zip(spots, screens, masks):
            light[margin + y:margin + y + s.shape[0], margin + x:margin + x + s.shape[1]] = s * m[..., None]
        light = np.clip(soften(light, 58 * u) * 1.6, 0, 1)**0.85
        canvas[..., :3] = 1 - (1 - canvas[..., :3]) * (1 - light * 0.95)
    # Shadow under the whole group.
    shape = np.zeros((height, width), np.float32)
    if device:
        shape[margin:margin + gh, margin:margin + gw] = rounded(gh, gw, 60 * u)
    else:
        for (y, x), m in zip(spots, masks):
            shape[margin + y:margin + y + m.shape[0], margin + x:margin + x + m.shape[1]] = m
    shadow = soften(np.roll(shape, 26 * u, axis=0), 30 * u) * 0.7
    canvas[..., :3] *= (1 - shadow)[..., None]
    canvas[..., 3] = shadow + canvas[..., 3] * (1 - shadow)
    if device:
        body = vertical(gh, gw, (0.19, 0.21, 0.27), (0.085, 0.095, 0.125))
        mask = rounded(gh, gw, 60 * u)
        over(canvas, margin, margin, body, mask)
        rim = np.clip(mask - np.pad(rounded(gh - 4 * u, gw - 4 * u, 58 * u), 2 * u), 0, 1)
        fade = np.linspace(0.5, 0.08, gh, dtype=np.float32)[:, None]
        over(canvas, margin, margin, np.ones((gh, gw, 3), np.float32), rim * fade)
        # Hinge: a speaker slot with its highlight, flanked by dots.
        cy, cx = (margin + inner + th + hinge // 2,
                  margin + gw // 2) if layout == 'stack' else (margin + gh // 2, margin + inner + tw + hinge // 2)
        long_, short = 200 * u, 9 * u
        slot = rounded(short, long_, short / 2) if layout == 'stack' else rounded(long_, short, short / 2)
        sy, sx = cy - slot.shape[0] // 2, cx - slot.shape[1] // 2
        over(canvas, sy + int(1.5 * u), sx, np.ones((*slot.shape, 3), np.float32), slot * 0.10)
        over(canvas, sy, sx, np.full((*slot.shape, 3), 0.02, np.float32), slot)
        for side in (-1, 1):
            for lane in (-1, 0, 1):
                d = rounded(6 * u, 6 * u, 3 * u)
                dy, dx = (cy + lane * 12 * u,
                          cx + side * 190 * u) if layout == 'stack' else (cy + side * 190 * u, cx + lane * 12 * u)
                over(canvas, dy - 3 * u, dx - 3 * u, np.full((6 * u, 6 * u, 3), 0.02, np.float32), d)
        glass = 12 * u
        for (y, x), s in zip(spots, screens):  # black glass border around each panel
            frame = rounded(s.shape[0] + 2 * glass, s.shape[1] + 2 * glass, sr + glass)
            over(canvas, margin + y - glass, margin + x - glass, np.full((*frame.shape, 3), 0.012, np.float32), frame)
    for (y, x), s, m in zip(spots, screens, masks):
        over(canvas, margin + y, margin + x, s, m)
        if not device:  # hairline so the panel reads against a dark backdrop
            hair = np.clip(m - np.pad(rounded(m.shape[0] - 2 * u, m.shape[1] - 2 * u, sr - u), u), 0, 1)
            over(canvas, margin + y, margin + x, np.ones((*m.shape, 3), np.float32), hair * 0.14)
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
        help='float: rounded panels, shadow and bias lighting; device: handheld shell; plain: bare pixels'
    )
    parser.add_argument('--layout', choices=('stack', 'side'), default='stack', help='top over bottom, or side by side')
    parser.add_argument('--bg', default='auto', help="auto, transparent, or a #RRGGBB colour")
    parser.add_argument(
        '--scale', type=int, choices=(1, 2, 3, 4), default=2, help='integer pixel multiplier (default 2)'
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
    write_png(out, compose(top, bottom, args.style, args.layout, args.bg, args.scale))
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
