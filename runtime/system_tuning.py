#!/usr/bin/env python3
"""Reversible RGDS tuning of stock services that slow startup or drain power.

Stock S99fstrim discards every free block of every SD partition at boot and
again each hour. On the RG DS that is ~13 s of card I/O starting just before
RGDS loads (8 s for the FAT games partition alone), stalling every startup
read and repeating hourly during play. While RGDS owns startup, the stock
script's execute bit is cleared so rcS skips it; its bytes are untouched. RGDS
then trims at most weekly, a few minutes after the menu appears, at idle CPU
and I/O priority. `remove` restores the exact original mode; returning to
Anbernic startup calls it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import time

from boot_control import CONTROL, atomic, location, read_json

STOCK_FSTRIM = Path('/etc/init.d/S99fstrim')
INITTAB = Path('/etc/inittab')
# Anbernic Linux 2026-05-14 /etc/inittab, inspected on the owner's RG DS.
REVIEWED_INITTAB = frozenset({'b35a65737cf2755f79f87530f2a833973c84a7eae6699dfa2a75fb5e739c21ae'})
UMOUNT_LINE = '::shutdown:/bin/umount -a -r'
POWEROFF_HELPER = Path('/usr/local/sbin/rgds-final-poweroff')
POWEROFF_LINE = '::shutdown:' + str(POWEROFF_HELPER)
# devtmpfs stays mounted (init holds /dev/console) when umount -a -r runs.
POWEROFF_MARKER = Path('/dev/.rgds-poweroff')
POWEROFF_SCRIPT = r"""#!/bin/sh
# RGDS_FINAL_POWEROFF_V1 - installed by RG DS Core (runtime/system_tuning.py).
# The last busybox ::shutdown action, after `umount -a -r`. Stock power-off
# lets the firmware assert the RK817 SLEEP pin; the PMIC then ignores the
# power key for 20-30 s, so the next start needs a second press. For an RGDS
# power-off only (never a reboot) and only once every block filesystem is
# read-only, power the PMIC off by register instead: SYS_CFG3 (0xf4) DEV_OFF,
# as in TheGammaSqueeze/rg_ds_kernel 7e5f173f4d. Any doubt keeps stock's path.
log() { echo "rgds-poweroff: $*" > /dev/kmsg 2>/dev/null; }
[ -f /dev/.rgds-poweroff ] || exit 0
[ -r /proc/mounts ] || mount -t proc proc /proc 2>/dev/null
[ -d /sys/kernel ] || mount -t sysfs sysfs /sys 2>/dev/null
[ -w /sys/rk8xx/rk8xx_dbg ] || { log "PMIC interface unavailable; stock power-off"; exit 0; }
sync
writable=$(awk '$1 ~ "^/dev/" && $4 ~ "(^|,)rw(,|$)" { printf "%s ", $2 }' /proc/mounts)
if [ -n "$writable" ]; then log "still writable: $writable; stock power-off"; exit 0; fi
echo "r f4" > /sys/rk8xx/rk8xx_dbg
value=$(dmesg | sed -n 's/.*\] f4 \([0-9a-f][0-9a-f]*\)$/\1/p' | tail -n 1)
case "$value" in ""|*[!0-9a-f]*) log "SYS_CFG3 unreadable; stock power-off"; exit 0;; esac
log "register power-off (SYS_CFG3 0x$value)"
sync
sleep 1
echo "w f4 $(printf %x $(( 0x$value | 1 )))" > /sys/rk8xx/rk8xx_dbg
sleep 2
log "PMIC still on; stock power-off"
exit 0
"""
# Anbernic Linux 2026-05-14 image, inspected on the owner's RG DS 2026-10-13.
REVIEWED_FSTRIM = frozenset({'906c5387943817930d44a04873fbaf45a581e2c5c738f0923d11dff4d86ff71f'})
TRIM_INTERVAL_SECONDS = 7 * 24 * 3600
TRIM_TYPES = frozenset({'ext4', 'vfat', 'exfat', 'f2fs'})


def journal_path(system_root='/'):
    return location(system_root, CONTROL) / 'tuning.json'


def _digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _process(proc, pid):
    try:
        command = (Path(proc) / str(pid) / 'cmdline').read_bytes().split(b'\0')
        parent = int((Path(proc) / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()[1])
    except (OSError, ValueError, IndexError):return None, None
    return command, parent


def stop_stock_loop(system_root='/', proc=Path('/proc')):
    """End this boot's stock hourly loop; an in-flight fstrim finishes normally.

    The stock script records `$$` from inside a `{ ...; }&` subshell, which is
    the exited parent's PID, so its own pid file cannot find the loop. Match the
    backgrounded script itself (reparented to init) by its exact command line.
    """
    script = str(STOCK_FSTRIM).encode();stopped = False
    try:names = [name for name in os.listdir(proc) if name.isdigit()]
    except OSError:return False
    loops = []
    for name in names:
        command, parent = _process(proc, name)
        if command and parent == 1 and script in command and command[command.index(script) + 1:command.index(script) + 2] == [b'start']:
            loops.append(int(name))
    for pid in loops:
        # Its idle `sleep 3600` child would otherwise linger for an hour.
        for name in names:
            command, parent = _process(proc, name)
            if parent == pid and command and command[0].endswith(b'sleep'):
                try:os.kill(int(name), signal.SIGKILL)
                except ProcessLookupError:pass
        try:os.kill(pid, signal.SIGKILL);stopped = True
        except ProcessLookupError:pass
    return stopped


def apply_poweroff(system_root='/'):
    """Add the final register power-off step to a reviewed stock inittab."""
    journal = journal_path(system_root);record = read_json(journal)
    inittab = location(system_root, INITTAB);helper = location(system_root, POWEROFF_HELPER)
    if record.get('poweroff'):
        saved = record['poweroff']
        # Keep the owned helper current; never touch a changed inittab.
        try:
            if inittab.is_file() and _digest(inittab) == saved.get('installed_sha256') and helper.read_bytes() != POWEROFF_SCRIPT.encode():
                atomic(helper, POWEROFF_SCRIPT.encode(), 0o755)
        except OSError:pass
        return dict(poweroff='register', changed=False)
    try:original = inittab.read_bytes()
    except FileNotFoundError:return dict(poweroff='absent', changed=False)
    if inittab.is_symlink() or hashlib.sha256(original).hexdigest() not in REVIEWED_INITTAB:
        return dict(poweroff='unreviewed', changed=False)
    lines = original.decode().splitlines()
    if lines.count(UMOUNT_LINE) != 1 or POWEROFF_LINE in lines:
        return dict(poweroff='unexpected', changed=False)
    lines.insert(lines.index(UMOUNT_LINE) + 1, POWEROFF_LINE)
    installed = ('\n'.join(lines) + '\n').encode()
    control = location(system_root, CONTROL)
    atomic(control / 'inittab.original', original, 0o644)
    record['schema'] = 1
    record['poweroff'] = dict(original_sha256=hashlib.sha256(original).hexdigest(),
                              installed_sha256=hashlib.sha256(installed).hexdigest(),
                              mode=stat.S_IMODE(inittab.stat().st_mode))
    atomic(journal, (json.dumps(record, sort_keys=True) + '\n').encode())
    atomic(helper, POWEROFF_SCRIPT.encode(), 0o755)
    atomic(inittab, installed, record['poweroff']['mode'])
    # busybox init reads inittab at boot; the step applies from the next boot.
    return dict(poweroff='register', changed=True)


def remove_poweroff(system_root='/'):
    journal = journal_path(system_root);record = read_json(journal)
    saved = record.get('poweroff')
    if not isinstance(saved, dict):return dict(poweroff='stock', changed=False)
    inittab = location(system_root, INITTAB);helper = location(system_root, POWEROFF_HELPER)
    control = location(system_root, CONTROL);changed = False
    try:
        if inittab.is_file() and not inittab.is_symlink() and _digest(inittab) == saved.get('installed_sha256'):
            original = (control / 'inittab.original').read_bytes()
            if hashlib.sha256(original).hexdigest() == saved.get('original_sha256'):
                atomic(inittab, original, int(saved.get('mode', 0o644)) & 0o7777);changed = True
    except (OSError, ValueError, TypeError):pass
    if changed or not inittab.is_file() or POWEROFF_LINE not in inittab.read_text(errors='replace'):
        helper.unlink(missing_ok=True)
        (control / 'inittab.original').unlink(missing_ok=True)
    record.pop('poweroff', None)
    if set(record) <= {'schema'}:journal.unlink(missing_ok=True)
    else:atomic(journal, (json.dumps(record, sort_keys=True) + '\n').encode())
    return dict(poweroff='stock', changed=changed)


def request_register_poweroff(system_root='/'):
    """Called by RGDS immediately before it dispatches stock `poweroff`."""
    try:location(system_root, POWEROFF_MARKER).write_text('1\n')
    except OSError:pass


def apply(system_root='/', proc=Path('/proc')):
    script = location(system_root, STOCK_FSTRIM);journal = journal_path(system_root)
    record = read_json(journal)
    if record.get('fstrim'):
        # Deferred on an earlier boot; a loop exists only if init ran it anyway.
        return dict(fstrim='deferred', changed=False, stopped=stop_stock_loop(system_root, proc))
    try:info = script.lstat()
    except FileNotFoundError:return dict(fstrim='absent', changed=False)
    if not stat.S_ISREG(info.st_mode):return dict(fstrim='unsafe', changed=False)
    if _digest(script) not in REVIEWED_FSTRIM:return dict(fstrim='unreviewed', changed=False)
    mode = stat.S_IMODE(info.st_mode)
    if not mode & 0o111:return dict(fstrim='disabled-by-owner', changed=False)
    # Journal before changing anything so recovery always knows the original.
    record['schema'] = 1
    record['fstrim'] = dict(path=str(STOCK_FSTRIM), sha256=_digest(script), previous_mode=mode)
    atomic(journal, (json.dumps(record, sort_keys=True) + '\n').encode())
    os.chmod(script, mode & ~0o111)
    stopped = stop_stock_loop(system_root, proc)
    return dict(fstrim='deferred', changed=True, stopped=stopped)


def remove(system_root='/'):
    journal = journal_path(system_root);record = read_json(journal)
    saved = record.get('fstrim')
    if not isinstance(saved, dict):return dict(fstrim='stock', changed=False)
    script = location(system_root, STOCK_FSTRIM);changed = False
    try:
        if script.is_file() and not script.is_symlink() and _digest(script) == saved.get('sha256'):
            os.chmod(script, int(saved['previous_mode']) & 0o7777);changed = True
    except (OSError, ValueError, TypeError, KeyError):pass
    record.pop('fstrim', None)
    if set(record) <= {'schema'}:journal.unlink(missing_ok=True)
    else:atomic(journal, (json.dumps(record, sort_keys=True) + '\n').encode())
    return dict(fstrim='stock', changed=changed)


def card_mounts(mounts=Path('/proc/mounts')):
    """Writable mounts on the boot card's own block devices."""
    result = []
    try:lines = Path(mounts).read_text().splitlines()
    except OSError:return result
    for line in lines:
        fields = line.split()
        if len(fields) < 4 or not (fields[0].startswith('/dev/mmcblk') or fields[0] == '/dev/root'):continue
        if fields[2] not in TRIM_TYPES or 'ro' in fields[3].split(','):continue
        target = fields[1].replace('\\040', ' ')
        if target not in result:result.append(target)
    return result


def trim_if_due(system_root='/', clock=time.time, run=subprocess.run, mounts=Path('/proc/mounts')):
    record_path = location(system_root, CONTROL) / 'trim.json'
    record = read_json(record_path)
    if not read_json(journal_path(system_root)).get('fstrim'):
        return dict(state='stock')  # Stock still trims hourly; never double up.
    if clock() - record.get('at', 0) < TRIM_INTERVAL_SECONDS:
        return dict(state='skipped')
    trimmed = {}
    for target in card_mounts(mounts):
        try:
            result = run(['/sbin/fstrim', target], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, timeout=300, check=False)
            trimmed[target] = result.returncode
        except (OSError, subprocess.SubprocessError):
            trimmed[target] = -1
    atomic(record_path, (json.dumps(dict(schema=1, at=int(clock()), mounts=trimmed), sort_keys=True) + '\n').encode())
    return dict(state='trimmed', mounts=trimmed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'remove', 'status', 'maintain'))
    parser.add_argument('--system-root', default='/')
    args = parser.parse_args()
    if args.action == 'apply':result = dict(fstrim=apply(args.system_root), poweroff=apply_poweroff(args.system_root))
    elif args.action == 'remove':result = dict(fstrim=remove(args.system_root), poweroff=remove_poweroff(args.system_root))
    elif args.action == 'status':result = dict(journal=read_json(journal_path(args.system_root)), trim=read_json(location(args.system_root, CONTROL) / 'trim.json'))
    else:
        from activate import background_priority
        background_priority()
        result = dict(tuning=apply(args.system_root), poweroff=apply_poweroff(args.system_root), trim=trim_if_due(args.system_root))
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    try:sys.exit(main())
    except (OSError, ValueError) as error:
        print('RGDS system tuning: ' + str(error), file=sys.stderr)
        sys.exit(1)
