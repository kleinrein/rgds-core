#!/usr/bin/env python3
"""Record what the RG DS's controls report, for comparing two input setups (docs/OWN_BSP_PLAN.md, Phase C step 2).

  pad_log.py SECONDS NAME [NAME...] [--rumble] [--start-on-press] [--json OUT]

Grabs each input device with one of the NAMEs for SECONDS (so presses do not reach the menu meanwhile) and prints,
per device: its identity and capabilities, then each key code's presses, and each axis's rest value (the first
reading), range seen and report rate. --rumble plays a 300 ms rumble on the first device that has FF_RUMBLE, one
second in. --start-on-press waits (up to 3 minutes) for the first button press and starts the SECONDS from there,
so the person at the handheld starts it. Run it with the device's own python3; it needs nothing but the standard
library.
"""
import array, fcntl, json, os, select, struct, sys, time

EV_SYN, EV_KEY, EV_ABS, EV_FF = 0, 1, 3, 0x15
FF_RUMBLE = 0x50
EVENT = struct.Struct('llHHi')
ABS_NAMES = {0: 'ABS_X', 1: 'ABS_Y', 2: 'ABS_Z', 3: 'ABS_RX', 4: 'ABS_RY', 5: 'ABS_RZ', 16: 'ABS_HAT0X', 17: 'ABS_HAT0Y'}


def ioc(direction, kind, number, size):
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | number


def eviocgname(fd):
    buf = array.array('B', [0] * 256)
    fcntl.ioctl(fd, ioc(2, 'E', 0x06, 256), buf, True)
    return buf.tobytes().split(b'\0')[0].decode(errors='replace')


def eviocgid(fd):
    buf = array.array('H', [0] * 4)
    fcntl.ioctl(fd, ioc(2, 'E', 0x02, 8), buf, True)
    return list(buf)


def bits(fd, kind, size):
    buf = array.array('B', [0] * size)
    fcntl.ioctl(fd, ioc(2, 'E', 0x20 + kind, size), buf, True)
    return [i for i in range(size * 8) if buf[i // 8] >> (i % 8) & 1]


def absinfo(fd, code):
    buf = array.array('i', [0] * 6)
    fcntl.ioctl(fd, ioc(2, 'E', 0x40 + code, 24), buf, True)
    return dict(zip(('value', 'min', 'max', 'fuzz', 'flat', 'res'), buf))


def rumble(fd):
    # struct ff_effect: type, id, direction, trigger(button, interval), replay(length, delay), then the union:
    # ff_rumble_effect = strong_magnitude, weak_magnitude
    # 48 bytes on a 64-bit kernel: the union after replay is 8-byte aligned (ff_periodic_effect holds a pointer)
    effect = struct.pack('HhHHHHH2xHH', FF_RUMBLE, -1, 0, 0, 0, 300, 0, 0xC000, 0x8000)
    effect += b'\0' * (48 - len(effect))
    buf = array.array('B', effect)
    fcntl.ioctl(fd, ioc(1, 'E', 0x80, len(effect)), buf, True)  # EVIOCSFF
    effect_id = struct.unpack_from('h', buf.tobytes(), 2)[0]
    os.write(fd, EVENT.pack(0, 0, EV_FF, effect_id, 1))
    return effect_id


def main():
    args = sys.argv[1:]
    want_rumble = '--rumble' in args
    on_press = '--start-on-press' in args
    out = None
    if '--json' in args:
        out = args[args.index('--json') + 1]
        args = args[:args.index('--json')] + args[args.index('--json') + 2:]
    args = [a for a in args if a not in ('--rumble', '--start-on-press')]
    seconds, names = float(args[0]), args[1:]
    devices = []
    for entry in sorted(os.listdir('/dev/input')):
        if not entry.startswith('event'):
            continue
        path = '/dev/input/' + entry
        try:
            fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except OSError:
            continue
        name = eviocgname(fd)
        if name not in names:
            os.close(fd)
            continue
        fcntl.ioctl(fd, ioc(1, 'E', 0x90, 4), 1)  # EVIOCGRAB
        dev = dict(path=path, name=name, id=eviocgid(fd), keys=bits(fd, EV_KEY, 96), ff=bits(fd, EV_FF, 16),
                   axes={}, presses={}, abs={}, abs_events=0, syn=0)
        for code in bits(fd, EV_ABS, 8):
            dev['axes'][code] = absinfo(fd, code)
        devices.append((fd, dev))
    if not devices:
        raise SystemExit('none of these devices: ' + ', '.join(names))
    if on_press:
        print('waiting for the first button press', flush=True)
        waited = time.monotonic()
        first = None
        while first is None and time.monotonic() - waited < 180:
            ready, _, _ = select.select([fd for fd, _ in devices], [], [], 0.5)
            for fd, dev in devices:
                if fd in ready:
                    data = os.read(fd, EVENT.size * 64)
                    for i in range(0, len(data), EVENT.size):
                        _, _, kind, code, value = EVENT.unpack_from(data, i)
                        if kind == EV_KEY and value == 1 and first is None:
                            first = code
                            dev['presses'][code] = 1
        if first is None:
            raise SystemExit('no button press within 3 minutes')
        print(f'started by key {first}', flush=True)
    start = time.monotonic()
    rumbled = not want_rumble
    while time.monotonic() - start < seconds:
        if not rumbled and time.monotonic() - start > 1:
            for fd, dev in devices:
                if FF_RUMBLE in dev['ff']:
                    dev['rumble_effect'] = rumble(fd)
                    break
            rumbled = True
        ready, _, _ = select.select([fd for fd, _ in devices], [], [], 0.2)
        for fd, dev in devices:
            if fd not in ready:
                continue
            data = os.read(fd, EVENT.size * 64)
            for i in range(0, len(data), EVENT.size):
                _, _, kind, code, value = EVENT.unpack_from(data, i)
                if kind == EV_KEY and value == 1:
                    dev['presses'][code] = dev['presses'].get(code, 0) + 1
                elif kind == EV_ABS:
                    seen = dev['abs'].setdefault(code, dict(first=value, low=value, high=value))
                    seen['low'] = min(seen['low'], value)
                    seen['high'] = max(seen['high'], value)
                    dev['abs_events'] += 1
                elif kind == EV_SYN and code == 0:
                    dev['syn'] += 1
    took = time.monotonic() - start
    report = []
    for fd, dev in devices:
        fcntl.ioctl(fd, ioc(1, 'E', 0x90, 4), 0)
        os.close(fd)
        print(f"== {dev['name']} {dev['path']} id={['%04x' % v for v in dev['id']]}")
        print(f"   keys {dev['keys']}  ff {dev['ff']}")
        for code, info in sorted(dev['axes'].items()):
            seen = dev['abs'].get(code)
            print(f"   {ABS_NAMES.get(code, code):9} range {info['min']}..{info['max']} fuzz {info['fuzz']} "
                  f"flat {info['flat']} | rest {info['value']}"
                  + (f" seen {seen['low']}..{seen['high']}" if seen else ' not moved'))
        print(f"   presses {dict(sorted(dev['presses'].items()))}")
        print(f"   {dev['syn'] / took:.0f} reports/s, {dev['abs_events']} axis events in {took:.0f} s"
              + (f", rumble effect {dev['rumble_effect']}" if 'rumble_effect' in dev else ''))
        report.append(dev)
    if out:
        with open(out, 'w') as stream:
            json.dump(report, stream, indent=1, default=str)


if __name__ == '__main__':
    main()
