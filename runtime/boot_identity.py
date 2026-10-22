#!/usr/bin/env python3
"""One RG DS identity from power-on: the early splash service and the boot-image logo.

Used by the SD installer after a release is activated, and by the uninstaller
to put stock artwork back. Every step is best effort and reversible:

- the early splash service installs only owned files (boot_splash_install);
- the boot-image logo patches only the exact unsigned stock template and
  keeps the original partition bytes and vendor logo files (boot_branding).

Nothing here runs on an RG DS Plus (its panels and boot image are not
validated) or off the handheld. A failure is reported, never raised, so an
installation finishes with stock artwork rather than not at all.
"""
import argparse
import json
from pathlib import Path
import platform
import sys


def _eligible(system_root):
    root = Path(system_root).resolve()
    if root == Path('/'):
        if sys.platform != 'linux' or platform.machine() not in ('aarch64', 'arm64'):
            return 'Boot artwork is installed on the handheld only.'
        try:
            from device_profile import detect
            if detect()['model'] == 'rg-ds-plus':
                return 'Boot artwork is not validated on the RG DS Plus; stock artwork is kept.'
        except Exception:  # identification is a hint, never a blocker
            pass
    if not (root / 'etc/init.d/S49weston').is_file():
        return 'Expected stock Weston startup is missing; stock artwork is kept.'
    return None


def apply(release, system_root='/'):
    """Install or refresh both parts from `release`'s runtime and assets."""
    release = Path(release)
    result = dict(splash='skipped', logo='skipped', messages=[])
    reason = _eligible(system_root)
    if reason:
        result['messages'].append(reason)
        return result
    assets = release / 'assets/boot'
    try:
        from boot_splash_install import install
        install(release / 'runtime/boot_splash.py', assets, system_root)
        result['splash'] = 'installed'
    except (OSError, ValueError) as error:
        result['splash'] = 'failed'
        result['messages'].append('Early splash: ' + str(error))
    try:
        from boot_branding import configure_current
        record = configure_current(assets, system_root=system_root)
        result['logo'] = record.get('state', 'installed')
    except (OSError, ValueError) as error:
        result['logo'] = 'failed'
        result['messages'].append('Boot logo: ' + str(error))
    return result


def remove(release, system_root='/'):
    """Put stock artwork back: remove the splash service, restore the boot logo."""
    release = Path(release)
    result = dict(splash='skipped', logo='skipped', messages=[])
    root = Path(system_root).resolve()
    try:
        from boot_splash_install import install, STORE
        from boot_control import location
        if location(root, STORE / 'installation.json').exists():
            install(release / 'runtime/boot_splash.py', release / 'assets/boot', system_root, remove=True)
            result['splash'] = 'removed'
    except (OSError, ValueError) as error:
        result['splash'] = 'failed'
        result['messages'].append('Early splash: ' + str(error))
    try:
        from boot_branding import restore, store_path, load_record, discover_boot_device
        store = store_path(system_root)
        if (store / 'manifest.json').exists():
            record = load_record(store)
            if record.get('state') == 'installed':
                device = record.get('boot_device') or str(discover_boot_device(system_root))
                restore(device, system_root)
                result['logo'] = 'restored'
            else:
                result['logo'] = record.get('state', 'skipped')
    except (OSError, ValueError) as error:
        result['logo'] = 'failed'
        result['messages'].append('Boot logo: ' + str(error))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'remove'))
    parser.add_argument('--release', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--system-root', type=Path, default=Path('/'))
    args = parser.parse_args()
    sys.path.insert(0, str(args.release / 'runtime'))
    result = (apply if args.action == 'apply' else remove)(args.release, args.system_root)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
