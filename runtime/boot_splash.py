#!/usr/bin/env python3
"""Bounded early KMS animation. Runs before Weston; never takes its DRM master."""
import argparse
import ctypes as C
import fcntl
import json
import mmap
import os
from pathlib import Path
import signal
import struct
import time
import zlib

U32, U64 = C.c_uint32, C.c_uint64


def record(name, fields):
    return type(name, (C.Structure,), {'_fields_': fields})


Mode = record(
    'Mode', [('clock', U32)] + [
        (n, C.c_uint16) for n in (
            'hdisplay', 'hsync_start', 'hsync_end', 'htotal', 'hskew', 'vdisplay', 'vsync_start', 'vsync_end', 'vtotal',
            'vscan'
        )
    ] + [('vrefresh', U32), ('flags', U32), ('type', U32), ('name', C.c_char * 32)]
)
Resources = record(
    'Resources', [(n, U64) for n in ('fb_id_ptr', 'crtc_id_ptr', 'connector_id_ptr', 'encoder_id_ptr')] + [
        (n, U32) for n in (
            'count_fbs', 'count_crtcs', 'count_connectors', 'count_encoders', 'min_width', 'max_width', 'min_height',
            'max_height'
        )
    ]
)
Connector = record(
    'Connector', [(n, U64) for n in ('encoders_ptr', 'modes_ptr', 'props_ptr', 'prop_values_ptr')] + [
        (n, U32) for n in (
            'count_modes', 'count_props', 'count_encoders', 'encoder_id', 'connector_id', 'connector_type',
            'connector_type_id', 'connection', 'mm_width', 'mm_height', 'subpixel', 'pad'
        )
    ]
)
Encoder = record(
    'Encoder', [(n, U32) for n in ('encoder_id', 'encoder_type', 'crtc_id', 'possible_crtcs', 'possible_clones')]
)
Crtc = record(
    'Crtc', [('set_connectors_ptr', U64)] +
    [(n, U32)
     for n in ('count_connectors', 'crtc_id', 'fb_id', 'x', 'y', 'gamma_size', 'mode_valid')] + [('mode', Mode)]
)
Dumb = record('Dumb', [(n, U32) for n in ('height', 'width', 'bpp', 'flags', 'handle', 'pitch')] + [('size', U64)])
Mapping = record('Mapping', [('handle', U32), ('pad', U32), ('offset', U64)])
Framebuffer = record('Framebuffer', [(n, U32) for n in ('fb_id', 'width', 'height', 'pitch', 'bpp', 'depth', 'handle')])


def ioctl(fd, number, value):
    # Linux generic ioctl ABI (AArch64); layout is from drm_mode.h UAPI.
    request = 0xC0000000 | (C.sizeof(value) << 16) | (ord('d') << 8) | number
    raw = bytearray(bytes(value))
    fcntl.ioctl(fd, request, raw, True)
    C.memmove(C.addressof(value), bytes(raw), len(raw))
    return value


def displays(fd):
    resources = ioctl(fd, 0xA0, Resources())
    if not 1 <= resources.count_connectors <= 16:
        raise ValueError('Unexpected DRM connector count')
    ids = (U32 * resources.count_connectors)()
    resources.connector_id_ptr = C.addressof(ids)
    count = resources.count_connectors
    resources.count_fbs = resources.count_crtcs = resources.count_encoders = 0
    ioctl(fd, 0xA0, resources)
    if count != resources.count_connectors:
        raise ValueError('DRM connectors changed')
    result = []
    for identity in ids:
        mode = Mode()
        connector = ioctl(fd, 0xA7, Connector(connector_id=identity, count_modes=1, modes_ptr=C.addressof(mode)))
        if connector.connector_type != 16 or connector.connection != 1 or not connector.encoder_id:
            continue
        encoder = ioctl(fd, 0xA6, Encoder(encoder_id=connector.encoder_id))
        if not encoder.crtc_id:
            continue
        crtc = ioctl(fd, 0xA1, Crtc(crtc_id=encoder.crtc_id))
        if not crtc.mode_valid or (crtc.mode.hdisplay, crtc.mode.vdisplay) != (640, 480):
            raise ValueError('Early splash only supports the active 640x480 panels')
        result.append((identity, crtc))
    if len(result) != 2 or len({c.crtc_id for _, c in result}) != 2:
        raise ValueError('Expected two active DSI panels')
    return result


def load_frames(path):
    raw = Path(path).read_bytes()
    if len(raw) < 20 or len(raw) > 4 * 1024 * 1024:
        raise ValueError('Invalid splash file size')
    magic, width, height, count, fps = struct.unpack_from('<4s4I', raw)
    if magic != b'RGA1' or (width, height) != (640, 480) or not 2 <= count <= 48 or not 8 <= fps <= 24:
        raise ValueError('Invalid splash animation header')
    size = width * height * 4
    decoder = zlib.decompressobj()
    pixels = decoder.decompress(raw[20:], size * count + 1)
    if len(pixels) != size * count or not decoder.eof or decoder.unused_data:
        raise ValueError('Invalid splash animation payload')
    view = memoryview(pixels)
    return [view[i * size:(i + 1) * size] for i in range(count)], fps


def weston_running():
    for path in Path('/proc').glob('[0-9]*/comm'):
        try:
            if path.read_text().strip() == 'weston': return True
        except OSError:
            pass
    return False


def enabled(control):
    try:
        config = json.loads((control / 'config.json').read_text())
        root = Path(config['root'])
        return (
            config.get('schema') == 1 and config.get('mode') == 'rgds' and root.is_absolute()
            and (control / 'enabled').is_file() and not (control / 'installing').exists()
            and not (root / 'userdata/boot_stock').exists()
        )
    except (OSError, ValueError, KeyError, TypeError):
        return False


def uptime():
    return float(Path('/proc/uptime').read_text().split()[0])


def run(args):
    if not enabled(args.control) or weston_running() or (args.run_dir / 'rgds-splash.stop').exists(): return 0
    args.run_dir.mkdir(parents=True, exist_ok=True)
    lock = (args.run_dir / 'rgds-splash.lock').open('w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 0
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    for number in (signal.SIGTERM, signal.SIGINT):
        signal.signal(number, stop)
    state = dict(schema=1, started=uptime(), frames=0, panels=0)
    fd = None
    mapping = None
    saved = []
    buffer = None
    framebuffer = None
    changed = []
    released = False
    try:
        frames, fps = load_frames(args.assets / 'animation.rga')
        if stopping or weston_running() or (args.run_dir / 'rgds-splash.stop').exists(): return 0
        fd = os.open('/dev/dri/card0', os.O_RDWR | os.O_CLOEXEC)
        # Refuses EBUSY when another owner holds master. Never stops the compositor.
        fcntl.ioctl(fd, (ord('d') << 8) | 0x1E)
        saved = displays(fd)
        buffer = ioctl(fd, 0xB2, Dumb(width=640, height=480, bpp=32))
        if buffer.pitch < 2560 or buffer.pitch > 8192 or buffer.size < buffer.pitch * 480 or buffer.size > 8 * 1024 * 1024:
            raise ValueError('Unexpected scanout allocation')
        framebuffer = ioctl(
            fd, 0xAE, Framebuffer(width=640, height=480, pitch=buffer.pitch, bpp=32, depth=24, handle=buffer.handle)
        )
        offset = ioctl(fd, 0xB3, Mapping(handle=buffer.handle)).offset
        mapping = mmap.mmap(
            fd, buffer.size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset
        )

        def paint(frame):
            if buffer.pitch == 2560: mapping[:640 * 480 * 4] = frame
            else:
                for row in range(480):
                    mapping[row * buffer.pitch:row * buffer.pitch + 2560] = frame[row * 2560:(row + 1) * 2560]

        paint(frames[0])
        for connector, previous in saved:
            identity = U32(connector)
            ioctl(
                fd, 0xA2,
                Crtc(
                    set_connectors_ptr=C.addressof(identity),
                    count_connectors=1,
                    crtc_id=previous.crtc_id,
                    fb_id=framebuffer.fb_id,
                    mode_valid=1,
                    mode=previous.mode
                )
            )
            changed.append((connector, previous))
        state.update(panels=len(changed), first_frame=uptime())
        start = time.monotonic()
        # Only the small emblem/wordmark move. Single linear scanout avoids
        # GPU/context startup and touches no input, brightness or network state.
        while not stopping and time.monotonic() - start < 40:
            if not released and (args.run_dir / 'rgds-splash.stop').exists():
                # Weston --warm-up keeps this scanout until the first app. Give
                # up DRM master now, but keep animating our retained buffer.
                fcntl.ioctl(fd, (ord('d') << 8) | 0x1F)
                released = True
                state['master_released'] = uptime()
                (args.run_dir / 'rgds-splash.released').touch()
            if released:
                current = [ioctl(fd, 0xA1, Crtc(crtc_id=previous.crtc_id)).fb_id for _, previous in saved]
                if all(identity != framebuffer.fb_id for identity in current):
                    state['replaced'] = uptime()
                    break
            index = int((time.monotonic() - start) * fps) % len(frames)
            paint(frames[index])
            state['frames'] += 1
            if args.capture and state['frames'] in (1, 10):
                (args.run_dir / ('rgds-splash-%d.bgra' % state['frames'])).write_bytes(frames[index])
            time.sleep(1 / fps)
        state['reason'] = 'replaced' if 'replaced' in state else 'stopped' if stopping else 'watchdog'
    except (OSError, ValueError) as error:
        state['error'] = str(error)
    finally:
        # Never restore CRTCs after handing ownership to the compositor.
        for connector, previous in reversed(changed) if not released else ():
            identity = U32(connector)
            previous.set_connectors_ptr = C.addressof(identity)
            previous.count_connectors = 1
            try:
                ioctl(fd, 0xA2, previous)
            except OSError as error:
                state['restore_error'] = str(error)
        if mapping is not None: mapping.close()
        if fd is not None:
            if framebuffer is not None:
                try:
                    ioctl(fd, 0xAF, U32(framebuffer.fb_id))
                except OSError:
                    pass
            if buffer is not None:
                try:
                    ioctl(fd, 0xB4, U32(buffer.handle))
                except OSError:
                    pass
            os.close(fd)
        state['finished'] = uptime()
        (args.run_dir / 'rgds-splash.json').write_text(json.dumps(state) + '\n')
        lock.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('run', 'stop', 'probe'))
    parser.add_argument('--assets', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--control', type=Path, default=Path('/userdata/rgds-boot'))
    parser.add_argument('--run-dir', type=Path, default=Path('/run'))
    parser.add_argument('--capture', action='store_true')
    args = parser.parse_args()
    if args.action == 'stop':
        (args.run_dir / 'rgds-splash.stop').touch()
        # Wait for DRM master to be released before Weston starts, bounded to
        # one second. This is a cleanup barrier, never an animation hold.
        with (args.run_dir / 'rgds-splash.lock').open('a') as lock:
            for _ in range(100):
                if (args.run_dir / 'rgds-splash.released').exists(): break
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    time.sleep(.01)
        return 0
    if args.action == 'probe':
        fd = os.open('/dev/dri/card0', os.O_RDWR | os.O_CLOEXEC)
        try:
            print(
                json.dumps(
                    [
                        dict(connector=c, crtc=v.crtc_id, width=v.mode.hdisplay, height=v.mode.vdisplay)
                        for c, v in displays(fd)
                    ]
                )
            )
        finally:
            os.close(fd)
        return 0
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())
