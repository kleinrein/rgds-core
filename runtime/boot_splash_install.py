#!/usr/bin/env python3
"""Install/remove only owned early-splash files. No firmware or vendor-script edits."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
from boot_control import atomic, location
from boot_splash import load_frames

STORE = Path('/usr/local/share/rgds-boot-splash')
START = Path('/etc/init.d/S12rgds-splash')
STOP = Path('/etc/init.d/S48rgds-splash')
TAG = '# RGDS_EARLY_SPLASH_V1'


def script(action, capture=False):
    command = '/usr/bin/python3 -B ' + str(STORE / 'boot_splash.py')
    if action == 'start':
        return (
            '#!/bin/sh\n' + TAG + '\ncase "$1" in\n  start)\n'
            '    ' + command + ' run' + (' --capture' if capture else '') + ' >>/var/log/rgds-splash.log 2>&1 &\n'
            '    ;;\n  stop) ' + command + ' stop ;;\nesac\nexit 0\n'
        ).encode()
    return ('#!/bin/sh\n' + TAG + '\n' + command + ' stop >>/var/log/rgds-splash.log 2>&1\nexit 0\n').encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def install(source, assets, system_root='/', remove=False, capture=False):
    root = Path(system_root).resolve()
    if root == Path('/') and (sys.platform != 'linux' or platform.machine() not in ('aarch64', 'arm64')):
        raise ValueError('Early boot installation requires the RG DS or an explicit fixture root')
    targets = {
        STORE / 'boot_splash.py': (Path(source).read_bytes(), 0o644),
        STORE / 'animation.rga': (Path(assets, 'animation.rga').read_bytes(), 0o644),
        START: (script('start', capture), 0o755),
        STOP: (script('stop'), 0o755)
    } if not remove else {}
    manifest = location(root, STORE / 'installation.json')
    if manifest.is_symlink(): raise ValueError('Splash manifest is a symlink')
    record = json.loads(manifest.read_text()) if manifest.exists() else None
    allowed = {str(p) for p in (STORE / 'boot_splash.py', STORE / 'animation.rga', START, STOP)}
    if record is not None and (record.get('schema') != 1 or set(record.get('files', {})) != allowed):
        raise ValueError('Invalid splash ownership record')
    paths = [Path(p) for p in sorted(allowed)]
    for path in paths:
        target = location(root, path)
        if target.is_symlink() or any(parent.is_symlink() for parent in target.parents):
            raise ValueError('Splash path contains a symlink: ' + str(path))
        if target.exists() and (record is None or sha(target.read_bytes()) != record['files'][str(path)]):
            raise ValueError('Splash file is not owned or changed externally: ' + str(path))
    if remove:
        # Stop launching on the next boot before removing the implementation.
        for path in (START, STOP, STORE / 'animation.rga', STORE / 'boot_splash.py'):
            location(root, path).unlink(missing_ok=True)
        manifest.unlink(missing_ok=True)
        return dict(installed=False)
    if not location(root, Path('/etc/init.d/S49weston')).is_file():
        raise ValueError('Expected stock Weston startup is missing')
    load_frames(Path(assets) / 'animation.rga')
    # Publish the code/art before the start hook. Restore all previous bytes if
    # any write fails, including an upgrade interrupted between its two hooks.
    before = {
        p: (location(root, p).read_bytes(),
            location(root, p).stat().st_mode & 0o777) if location(root, p).exists() else None
        for p in targets
    }
    previous = manifest.read_bytes() if manifest.exists() else None
    try:
        for path in (STORE / 'boot_splash.py', STORE / 'animation.rga', STOP, START):
            content, mode = targets[path]
            atomic(location(root, path), content, mode)
        atomic(
            manifest,
            (json.dumps(dict(schema=1, files={
                str(p): sha(v[0])
                for p, v in targets.items()
            }), sort_keys=True) + '\n').encode()
        )
    except BaseException:
        for p, old in before.items():
            if old is None: location(root, p).unlink(missing_ok=True)
            else: atomic(location(root, p), old[0], old[1])
        if previous is None: manifest.unlink(missing_ok=True)
        else: atomic(manifest, previous)
        raise
    return dict(
        installed=True,
        starts=str(START),
        handoff=str(STOP),
        vendor_scripts_modified=False,
        boot_partition_written=False
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'remove'))
    parser.add_argument('--assets', type=Path, default=Path(__file__).resolve().parents[1] / 'assets/boot')
    parser.add_argument('--system-root', type=Path, default=Path('/'))
    parser.add_argument(
        '--capture', action='store_true', help='Capture two actual animation frames on the next boot for validation'
    )
    args = parser.parse_args()
    print(
        json.dumps(
            install(
                Path(__file__).with_name('boot_splash.py'), args.assets, args.system_root, args.action == 'remove',
                args.capture
            ),
            indent=2
        )
    )


if __name__ == '__main__': main()
