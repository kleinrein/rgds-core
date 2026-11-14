"""What fills the data card, measured only when the Storage view asks (Settings > System > Device information).

Nothing here runs in the background: the launcher starts it when the popup opens and stops it when the popup
closes. It walks the data partition once, adds up allocated bytes per kind of content and reports the running
totals about four times a second, so the bar grows while the card is read.
"""
import os
import time
from pathlib import Path

# Order is the bar's order; "other" is whatever the partition holds that nothing above claims.
CATEGORIES = ('games', 'saves', 'artwork', 'music', 'cache', 'system', 'other')
GAME_FOLDERS = ('roms', 'ports', 'homebrew')


def mount_root(path):
    """The top of the partition `path` lives on (a data folder that is not a mount point stops at its parent)."""
    path = Path(path).resolve()
    for folder in (path, *path.parents):
        if os.path.ismount(folder): return folder
    return path


def classify(relative, data_relative):
    """The category of a file or folder, given its path under the scan root and the data folder's own path there."""
    parts = relative.parts
    if not parts: return 'other'
    if parts[0] in GAME_FOLDERS: return 'games'
    if parts[0] == 'music': return 'music'
    inside = data_relative and parts[:len(data_relative)] == data_relative
    if inside:
        name = parts[len(data_relative)] if len(parts) > len(data_relative) else ''
        if name == 'cache': return 'cache'
        if name == 'library': return 'artwork'
        if name.startswith(('save', 'state', 'resume')): return 'saves'
        return 'system'
    if data_relative and parts[:len(data_relative) - 1] == data_relative[:-1] and len(data_relative) > 1:
        return 'system'  # the rgds folder beside userdata: the app, engines and logs
    return 'other'


def allocated(info):
    """Bytes the file really takes: FAT rounds up to a cluster, and free space counts clusters."""
    blocks = getattr(info, 'st_blocks', 0)
    return max(info.st_size, blocks * 512) if blocks else info.st_size


def scan(data, publish, preview=False, interval=0.25, clock=time.monotonic, root=None):
    """Measure the partition holding `data`; `publish(totals, done)` gets the running bytes per category.

    In the desktop preview the scan root is the data folder's parent, so the host's whole disk is never walked."""
    data = Path(data).resolve()
    root = Path(root).resolve() if root else data.parent if preview else mount_root(data)
    try:
        data_relative = data.relative_to(root).parts
    except ValueError:
        data_relative = ()
    totals = dict.fromkeys(CATEGORIES, 0)
    try:
        device = root.stat().st_dev
    except OSError:
        publish(totals, True)
        return totals
    last = clock()
    stack = [(root, ())]
    while stack:
        folder, relative = stack.pop()
        try:
            entries = list(os.scandir(folder))
        except OSError:
            continue
        for entry in entries:
            here = relative + (entry.name,)
            try:
                info = entry.stat(follow_symlinks=False)
            except OSError:
                continue
            if entry.is_symlink(): continue
            kind = classify(Path(*here), data_relative)
            if entry.is_dir(follow_symlinks=False) and info.st_dev == device: stack.append((Path(entry.path), here))
            totals[kind] += allocated(info)
            if clock() - last >= interval:
                publish(totals, False)
                last = clock()
    publish(totals, True)
    return totals
