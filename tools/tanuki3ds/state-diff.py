#!/usr/bin/env python3
"""state-diff.py FIRST SECOND: are two 3DS states the same, chunk by chunk? Needs zstd.

What an engine writes straight after loading a state must equal the state it read, or something was lost on the way.
The pictures (PICS) can differ by rounding in the GL driver, so they are only reported.
"""
import subprocess, sys

HEADER = 20


def chunks(path):
    with open(path, 'rb') as stream:
        stream.seek(HEADER)
        raw = subprocess.run(['zstd', '-dc'], stdin=stream, capture_output=True, check=True).stdout
    found, at = {}, 0
    while at < len(raw):
        tag = raw[at:at + 4].decode('ascii')
        size = int.from_bytes(raw[at + 4:at + 12], 'little')
        found[tag] = raw[at + 12:at + 12 + size]
        at += 12 + size
    return found


def main():
    first, second = chunks(sys.argv[1]), chunks(sys.argv[2])
    failed = False
    for tag in first.keys() | second.keys():
        a, b = first.get(tag), second.get(tag)
        if a == b:
            print(f'{tag}: same ({len(a or b)} bytes)')
            continue
        where = next((i for i, (x, y) in enumerate(zip(a or b'', b or b'')) if x != y), None)
        print(f'{tag}: differs, {len(a or b"")} and {len(b or b"")} bytes, first at {where}')
        failed = failed or tag != 'PICS'
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
