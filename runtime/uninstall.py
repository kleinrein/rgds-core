#!/usr/bin/env python3
"""Remove RG DS Core from the card and the stock system; saves and settings stay.

Run from the stock menu by APPS -> Uninstall RG DS Core. In order: refuse while a
game runs, put the stock autostart hook back (with the deferred trim service
and the shutdown step), restore the stock boot artwork, then delete the
application files under the installation root. `userdata` (saves, settings,
artwork cache, logs) and `games` are kept, so a later install finds them again.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

from activate import read_pointer, stop_session   # noqa: E402
import boot_control                                 # noqa: E402
import boot_identity                                # noqa: E402

KEEP = ('userdata', 'games')
APP_FILES = ('releases', 'state', 'inbox', 'docs', 'main.sh', 'inventory.sh', 'update_guard.py', '__pycache__')


def remove_tree(path):
    path = Path(path)
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def uninstall(root, system_root='/', stop=True):
    root = Path(root).resolve()
    result = dict(root=str(root), startup='skipped', artwork='skipped', removed=[], kept=[], messages=[])
    if root.name != 'rgds' or not root.is_dir():
        raise ValueError('Expected an rgds installation folder, not ' + str(root))
    if stop:
        stop_session(root / 'userdata')   # refuses while a game is running
    build = read_pointer(root / 'state/active')
    release = root / 'releases' / build if build else None

    # Boot artwork first: its backups live under the control folder (the
    # boot-image logo store), which goes away with the startup hook below.
    if release is not None and (release / 'runtime/boot_identity.py').is_file():
        try:
            artwork = boot_identity.remove(release, system_root)
            result['artwork'] = 'restored' if 'failed' not in (artwork['splash'], artwork['logo']) else 'failed'
            result['messages'].extend(artwork['messages'])
        except Exception as error:   # artwork must never stop the uninstall
            result['artwork'] = 'failed'
            result['messages'].append('Boot artwork: ' + str(error))

    # Startup: the owned autostart hook goes back to the stock original (or away),
    # the deferred SD trim service and the final power-off step return to stock.
    control = boot_control.location(system_root, boot_control.CONTROL)
    try:
        boot_control.uninstall(system_root)
        result['startup'] = 'restored'
    except (OSError, ValueError) as error:
        result['startup'] = 'failed'
        result['messages'].append('Startup hook: ' + str(error))
        # The marker alone makes the hook exit before it touches anything.
        (control / 'enabled').unlink(missing_ok=True)
    leftovers = sorted(p.name for p in control.iterdir()) if control.is_dir() else []
    # Backups still waiting to be put back (an original hook, inittab, trim
    # record or boot logo) must survive; otherwise the owned control folder goes too.
    logo = json.loads((control / 'branding/manifest.json').read_text()) if (control / 'branding/manifest.json').is_file() else {}
    pending = set(leftovers) & {'installation.json', 'inittab.original', 'tuning.json'} or logo.get('state') == 'installed' or result['artwork'] == 'failed'
    if control.is_dir() and result['startup'] == 'restored' and not pending:
        shutil.rmtree(control, ignore_errors=True)
    elif leftovers:
        result['messages'].append('Kept ' + str(control) + ' (' + ', '.join(leftovers) + ').')

    # Application files. Everything else in the root belongs to the user.
    for name in APP_FILES:
        path = root / name
        if path.exists() or path.is_symlink():
            remove_tree(path)
            result['removed'].append(name)
    for path in sorted(root.iterdir()):
        if path.name in KEEP:
            result['kept'].append(path.name)
        elif path.name not in APP_FILES:
            result['messages'].append('Left ' + path.name + ' in place (not part of the application).')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True, help='the rgds installation folder on the card')
    parser.add_argument('--system-root', type=Path, default=Path('/'), help='a fixture root instead of the live system (tests)')
    args = parser.parse_args()
    # A fixture system root has no live session to stop.
    result = uninstall(args.root, str(args.system_root), stop=args.system_root.resolve() == Path('/'))
    print(json.dumps(result, indent=2, sort_keys=True))
    print('Removed RG DS Core from ' + result['root'] + '. Saves and settings stay in rgds/userdata.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print('Uninstall: ' + str(error), file=sys.stderr)
        sys.exit(1)
