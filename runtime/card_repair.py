#!/usr/bin/env python3
"""Repair a games card that the kernel has switched to read-only.

The stock firmware mounts the FAT games partition with errors=remount-ro and
never checks it. One inconsistent cluster chain (for example after power was
lost during a write) makes the kernel flip the whole card read-only mid-session:
volume, saves, settings, game launch and lid sleep then all fail quietly.

`start` checks that the card really is read-only, rate-limits attempts through
an ext4 journal on /userdata (the card itself is not writable), then detaches a
small shell script from /run. The script flushes, runs `fsck.fat -a` on the
still read-only mounted card - the same arrangement as checking a read-only root
filesystem - and reboots without remounting it writable. The kernel never
writes to a read-only FAT, so its stale in-memory state is simply discarded.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

from boot_control import CONTROL, atomic, read_json

JOURNAL = CONTROL / 'card-repair.json'
SCRIPT = Path('/run/rgds-card-repair.sh')
LOG = Path('/run/rgds-card-repair.log')
# Two attempts inside this window means the check does not fix the card.
WINDOW_SECONDS = 30 * 60
REPAIRABLE = {'vfat': ('fsck.fat', '-a', '-w'), 'exfat': ('fsck.exfat', '-y')}
STARTED, NOT_NEEDED, GIVE_UP, UNSUPPORTED = 0, 2, 3, 4


def unescape(field):
    return field.replace('\\040', ' ').replace('\\011', '\t').replace('\\012', '\n').replace('\\134', '\\')


def mount_of(path, mounts=Path('/proc/mounts')):
    """(device, mount point, fstype, options) of the mount holding `path`."""
    target = os.path.realpath(path)
    best = None
    try:
        lines = Path(mounts).read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        fields = line.split()
        if len(fields) < 4: continue
        device, point, fstype, options = unescape(fields[0]), unescape(fields[1]), fields[2], fields[3].split(',')
        if target == point or target.startswith(point.rstrip('/') + '/'):
            if best is None or len(point) >= len(best[1]): best = (device, point, fstype, options)
    return best


def read_only(path):
    try:
        return bool(os.statvfs(path).f_flag & os.ST_RDONLY)
    except OSError as error:
        return error.errno == errno.EROFS


def script(device, point, command):
    check = ' '.join(shlex.quote(part) for part in (*command, device))
    return (
        '#!/bin/sh\n'
        '# RG DS Core card repair (runtime/card_repair.py); runs from tmpfs.\n'
        'exec >' + shlex.quote(str(LOG)) + ' 2>&1 </dev/null\n'
        'cd /\n'
        'echo "rgds-card-repair: checking ' + device + '" > /dev/kmsg 2>/dev/null\n'
        'sync\n'
        # Let the launcher draw its explanation before the card is busy.
        'sleep 2\n'
        # Only repair while the card is still read-only; never under a writer.
        'if ! awk -v p=' + shlex.quote(point) +
        ' \'$2 == p && $4 ~ /(^|,)ro(,|$)/ { found = 1 } END { exit !found }\' /proc/mounts; then\n'
        '  echo "card is writable again; not checking"; exit 0\n'
        'fi\n' + check + '\n'
        'status=$?\n'
        'echo "fsck status $status"\n'
        'mkdir -p ' + shlex.quote(str(CONTROL)) + ' && cp ' + shlex.quote(str(LOG)) + ' ' +
        shlex.quote(str(CONTROL / 'card-repair.log')) + '\n'
        'sync\n'
        'echo "rgds-card-repair: fsck status $status, restarting" > /dev/kmsg 2>/dev/null\n'
        'reboot\n'
    )


def start(data, journal=JOURNAL, now=None, spawn=True):
    now = time.time() if now is None else now
    found = mount_of(data)
    if not read_only(data): return NOT_NEEDED, 'The memory card is writable.'
    if not found or found[2] not in REPAIRABLE: return UNSUPPORTED, 'This storage cannot be checked automatically.'
    device, point, fstype, _options = found
    if not device.startswith('/dev/'): return UNSUPPORTED, 'This storage cannot be checked automatically.'
    record = read_json(journal)
    attempts = [t for t in record.get('attempts', []) if isinstance(t, (int, float)) and now - t < WINDOW_SECONDS]
    if len(attempts) >= 2:
        return GIVE_UP, 'The automatic check did not fix the memory card. Copy your saves to a computer and check the card there.'
    attempts.append(now)
    atomic(
        journal,
        (json.dumps({
            'schema': 1,
            'attempts': attempts,
            'device': device,
            'fstype': fstype
        }, sort_keys=True) + '\n').encode()
    )
    if spawn:
        atomic(SCRIPT, script(device, point, REPAIRABLE[fstype]).encode(), 0o700)
        # A new session in / with no inherited descriptors: killing the
        # launcher (or the reboot) never interrupts the check itself.
        subprocess.Popen(
            ['/bin/sh', str(SCRIPT)],
            cwd='/',
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True
        )
    return STARTED, 'Checking the memory card.'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('status', 'start'):
        command = sub.add_parser(name)
        command.add_argument('--data', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == 'status':
        found = mount_of(args.data)
        print(
            json.dumps(
                {
                    'read_only': read_only(args.data),
                    'mount': found[1] if found else None,
                    'fstype': found[2] if found else None
                }
            )
        )
        return 0
    code, message = start(args.data)
    print(message, flush=True)
    return code


if __name__ == '__main__':
    sys.exit(main())
