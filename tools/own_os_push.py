#!/usr/bin/env python3
"""Update a running RG DS OS handheld in place over SSH, through its A/B slots (docs/OWN_OS_PLAN.md).

The new root filesystem goes to the slot that is not running, its kernel and device tree to that slot's folder on
the boot partition, and extlinux.conf's DEFAULT moves to it. The data partition (games, saves, settings) is not
touched, and the old slot stays as it was: `--slot a|b --switch-only` boots back into it.

  own_os_push.py HOST [--reboot]            e.g. own_os_push.py 192.168.68.120 --reboot
  own_os_push.py HOST --slot a --switch-only [--reboot]
  own_os_push.py HOST --uboot [--reboot]    also writes the build's U-Boot at 32K (the running one is kept in
                                            /data/rgds/logs/u-boot-before-<time>.bin)
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
from os_update import replace_label  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.cache/own-os/build'
DTB = 'rk3568-anbernic-rg-ds.dtb'
KERNEL = 'Image.lz4'
FILES = ('rootfs.squashfs', KERNEL, DTB, 'extlinux.conf', 'u-boot-rockchip.bin')


def ssh(host, command, check=True, **kwargs):
    return subprocess.run(
        [
            'ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', '-o',
            'LogLevel=ERROR', 'root@' + host, command
        ],
        check=check,
        **kwargs
    )


def digest(path):
    hash = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            hash.update(block)
    return hash.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('host')
    parser.add_argument('--slot', choices=('a', 'b'), help='default: the one not running')
    parser.add_argument('--switch-only', action='store_true', help='only change the slot that boots next')
    parser.add_argument('--uboot', action='store_true', help="also write the build's U-Boot to the card")
    parser.add_argument('--reboot', action='store_true')
    args = parser.parse_args()
    running = ssh(args.host, 'cat /run/rgds/slot', capture_output=True, text=True).stdout.strip()
    slot = args.slot or ('b' if running == 'a' else 'a')
    if slot == running and not args.switch_only:
        raise SystemExit(f'Slot {slot} is running; write the other one.')
    part = {'a': 2, 'b': 3}[slot]
    if not args.switch_only:
        missing = [name for name in FILES if not (BUILD / name).is_file()]
        if missing:
            raise SystemExit('Missing in .cache/own-os/build: ' + ', '.join(missing) + '. Run tools/own_os_build.py.')
        rootfs = BUILD / 'rootfs.squashfs'
        size = rootfs.stat().st_size
        on_slot = lambda: ssh(
            args.host, f'head -c {size} $(cat /run/rgds/disk)p{part} | sha256sum', capture_output=True, text=True
        ).stdout.split()[0]
        if on_slot() == digest(rootfs):
            print(f'slot {slot} already holds this root filesystem', flush=True)
        else:
            print(f'writing {size / 1048576:.0f} MiB to slot {slot} (running: {running})', flush=True)
            with rootfs.open('rb') as stream:
                ssh(
                    args.host,
                    # A partition shorter than the image would leave a broken slot; check before writing.
                    f'D=$(cat /run/rgds/disk)p{part}; '
                    f'[ $(($(cat /sys/class/block/${{D##*/}}/size) * 512)) -ge {size} ] || exit 3; '
                    f'dd of=$D bs=1M conv=fsync 2>/dev/null',
                    stdin=stream
                )
        if on_slot() != digest(rootfs):
            raise SystemExit(f'Slot {slot} does not match the image after writing; the running slot stays default.')
        ssh(
            args.host,
            f'mount -o remount,rw /boot && mkdir -p /boot/{slot} && rm -f /boot/{slot}/Image && '
            # U-Boot's boot counter, idle, for a card made before it existed (usr/libexec/rgds/trial)
            '[ -f /boot/bootcount ] || printf \'\\275\\001\\000\\000\' > /boot/bootcount'
        )
        try:
            for name in (KERNEL, DTB):
                remote = ssh(
                    args.host,
                    f'sha256sum /boot/{slot}/{name} 2>/dev/null',
                    check=False,
                    capture_output=True,
                    text=True
                ).stdout.split()
                if remote and remote[0] == digest(BUILD / name):
                    continue
                # The boot partition holds two kernels and no spare third; this slot is not running and does not
                # boot until DEFAULT names it, so its file is replaced in place.
                with (BUILD / name).open('rb') as stream:
                    ssh(args.host, f'rm -f /boot/{slot}/{name}.new && cat > /boot/{slot}/{name}', stdin=stream)
                remote = ssh(args.host, f'sha256sum /boot/{slot}/{name}', capture_output=True, text=True).stdout
                if remote.split()[0] != digest(BUILD / name):
                    raise SystemExit(
                        f'/boot/{slot}/{name} does not match after writing; the running slot stays default.'
                    )
        finally:
            ssh(args.host, 'sync; mount -o remount,ro /boot')
    # extlinux.conf: this slot's LABEL block from the build (its kernel's name, its command line), the other slot's
    # kept as it is (it still boots what it holds), and DEFAULT moved to this slot.
    current = ssh(args.host, 'cat /boot/extlinux/extlinux.conf', capture_output=True, text=True).stdout
    if not args.switch_only: current = replace_label(current, slot, (BUILD / 'extlinux.conf').read_text())
    current = '\n'.join(
        f'DEFAULT {slot}' if line.startswith('DEFAULT ') else line for line in current.splitlines()
    ) + '\n'
    ssh(
        args.host, 'mount -o remount,rw /boot && cat > /boot/extlinux/extlinux.conf.new && '
        'mv /boot/extlinux/extlinux.conf.new /boot/extlinux/extlinux.conf && sync; mount -o remount,ro /boot; '
        'grep -E "^DEFAULT|LINUX" /boot/extlinux/extlinux.conf',
        input=current,
        text=True
    )
    if args.uboot:
        uboot = BUILD / 'u-boot-rockchip.bin'
        size = uboot.stat().st_size
        print(f'writing U-Boot ({size / 1024:.0f} KiB) at 32K of the card', flush=True)
        with uboot.open('rb') as stream:
            ssh(
                args.host, f'D=$(cat /run/rgds/disk); dd if=$D bs=512 skip=64 count={(size + 511) // 512} '
                f'of=/data/rgds/logs/u-boot-before-$(date +%s).bin 2>/dev/null && '
                f'dd of=$D bs=512 seek=64 conv=fsync 2>/dev/null && sync',
                stdin=stream
            )
    if slot != running:
        # The same safety net an OTA update gets (usr/libexec/rgds/trial): three failed boots of the new slot and
        # U-Boot boots the running one again.
        build = (BUILD / 'build-id').read_text().strip() if (BUILD / 'build-id').is_file() else 'push'
        ssh(
            args.host,
            f'[ -x /usr/libexec/rgds/trial ] && /usr/libexec/rgds/trial arm {slot} {running} {build} push; true'
        )
    if args.reboot:
        # The connection drops as the handheld goes down.
        ssh(args.host, 'reboot', check=False)
        print('rebooting into slot ' + slot)


if __name__ == '__main__':
    sys.exit(main())
