"""Whose files: the device root (userdata) and the active profile's player root (docs/PROFILES_PLAN.md).

The owner's player root is userdata itself, the same paths as before profiles existed, so nothing ever moves and a build
that knows nothing of profiles reads the owner's saves as they are. Anyone else's is userdata/profiles/<id>.

    userdata/profiles/active      the current profile's id; the launcher writes it, never while a game runs
    userdata/profiles/index.ini   present once profiles were set up: then a missing id is an error, not the owner
    userdata/profiles/<id>/       the same layout as the owner's player files

A function about a player's files takes the player root and finds shared files with device_root(); launcher/profiles.hpp
holds the same rules.
"""
import re
import shutil
import time
from pathlib import Path

PROFILES = 'profiles'
ACTIVE = 'active'
INDEX = 'index.ini'
OWNER = 'owner'
REMOVED = '.removed'
PROFILE_ID = re.compile(r'p[0-9]+')
# The owner's player files, under the device root. A plan for anyone else must not name these.
OWNERS_FILES = (
    'library', 'emulators', 'resume', 'save-history', 'system-defaults', 'cache/disks', 'preferences.ini', 'ui.ini',
    'ds.ini', 'pictochat.ini'
)
# Under those, but one copy for every profile: read only, or nothing of the player's in them.
SHARED_FILES = ('emulators/rgds-experimental/panvk.json', 'emulators/rgds-experimental/cheats')


class ProfileError(ValueError):
    pass


def device_root(path):
    path = Path(path)
    return path.parent.parent if path.parent.name == PROFILES and PROFILE_ID.fullmatch(path.name) else path


def player_root(data):
    """The active profile's folder. Never falls back to the owner's when profiles exist but the active one is unclear:
    a game started there would write into the owner's saves."""
    device = device_root(data)
    folder = device / PROFILES
    try:
        active = (folder / ACTIVE).read_text(encoding='utf-8').strip()
    except FileNotFoundError:
        if (folder / INDEX).exists():
            raise ProfileError('No profile is selected. No game was started and no save was touched.') from None
        return device
    except (OSError, UnicodeError) as error:
        raise ProfileError('The active profile could not be read: ' + str(error)) from None
    if active == OWNER: return device
    if PROFILE_ID.fullmatch(active) and (folder / active).is_dir(): return folder / active
    raise ProfileError('The active profile is missing. No game was started and no save was touched.')


def look_root(data):
    """The folder to read a player's look from, for what must work whatever the profile state (a sleep sound)."""
    try:
        return player_root(data)
    except ProfileError:
        return device_root(data)


def is_owner(player):
    return device_root(player) == Path(player)


def player_roots(data):
    """Every folder holding someone's saves, the owner's first, and profiles removed but not yet erased."""
    device = device_root(data)
    folder = device / PROFILES
    roots = [device]
    if folder.is_dir():
        roots += sorted(p for p in folder.iterdir() if p.is_dir() and PROFILE_ID.fullmatch(p.name))
    if (folder / REMOVED).is_dir():
        roots += sorted(p for p in (folder / REMOVED).iterdir() if p.is_dir())
    return roots


def remove_profile(data, profile):
    """A backup first, then the folder steps aside: nothing is erased, and every later backup still holds it."""
    from emulators import activity_lock
    from save_library import backup_all, sync_dir
    device = device_root(data)
    folder = device / PROFILES / str(profile)
    if not PROFILE_ID.fullmatch(str(profile)) or not folder.is_dir(): raise ValueError('This profile does not exist.')
    if player_root(device) == folder: raise ValueError('Switch to another profile first.')
    export = backup_all(device)
    with activity_lock(device):
        kept = device / PROFILES / REMOVED / (str(profile) + '-' + str(time.time_ns()))
        kept.parent.mkdir(parents=True, exist_ok=True)
        folder.rename(kept)
        sync_dir(kept.parent)
        sync_dir(folder.parent)
    return export, kept


def erase_removed(data):
    """The one place saves are deleted, asked for by the owner in words: the removed profiles, nothing else. Not while a
    game runs."""
    from emulators import activity_lock, core_running
    device = device_root(data)
    folder = device / PROFILES / REMOVED
    if player_root(device) != device: raise ValueError('Only the owner can erase removed profiles.')
    with activity_lock(device):
        if core_running(device): raise ValueError('Exit your game first.')
        erased = 0
        if folder.is_dir():
            for kept in sorted(folder.iterdir()):
                if kept.is_dir() and PROFILE_ID.fullmatch(kept.name.partition('-')[0]):
                    shutil.rmtree(kept)
                    erased += 1
    return erased


def plan_paths(plan):
    """Every path a launch plan hands the engine, as (where it came from, text), without the two it only reads from the
    device root by design: the game's box art and the cheat lists."""
    command = plan['command']
    for at, part in enumerate(command):
        if at == 0 or command[at - 1] not in ('--art', '--shared-cheats'): yield 'command', str(part)
    yield 'cwd', str(plan.get('cwd', ''))
    for key, value in plan['env'].items():
        for part in str(value).split(':'):
            yield key, part


def check_plan(plan, data, player, core):
    """Refuse a launch that would mix saves: for anyone but the owner, nothing may point at the owner's player files.
    A port is the one exception: its saves live in its own folder, shared by every profile."""
    device, player = Path(device_root(data)), Path(player)
    if player == device or core == 'portmaster': return
    for key in ('HOME', 'RGDS_AUTOSTATE_FILE', 'RGDS_STATE_DIR', 'RGDS_STATE_SLOT'):
        value = plan['env'].get(key)
        if value and not Path(value).is_relative_to(player):
            raise ProfileError(key + ' is outside this profile. No game was started and no save was touched.')
    for key, text in plan_paths(plan):
        path = Path(text)
        if not path.is_absolute(): continue
        if path.is_relative_to(player) or not path.is_relative_to(device): continue
        relative = path.relative_to(device)
        if any(relative.is_relative_to(shared) for shared in SHARED_FILES): continue
        if any(relative.is_relative_to(owners) for owners in OWNERS_FILES):
            raise ProfileError(key + ' points at the owner\'s files. No game was started and no save was touched.')
