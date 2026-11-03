"""Copies of each game's saves, taken before every launch.

A game's own save must survive anything: an engine update, a resume point that rolls the cartridge back, a bug, a
test run. Nothing here is ever needed to start a game, so a failure only prints a line. Only this module deletes
under save-history, and an older copy is deleted only once a newer one is in place.

    save-history/<game key, or 3ds/psp for their shared folders>/<time>/<path under userdata>
    save-history/<game key>/resume/<time>/...                 the last resume points
    save-history/<game key>/resume/before-<build>/<time>/...  the one each update found: newer ones never push it out
"""
import hashlib, os, shutil, sys, time
from pathlib import Path

HISTORY = 'save-history'
KEEP_SAVES = 10
KEEP_STATES = 2  # resume points are megabytes (3DS 20 MB)
KEEP_UPDATES = 4
MIN_FREE = 256 * 1024 * 1024
DS = 'rgds-experimental'
TANUKI = 'emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds'
SHARED = {'3ds': (TANUKI + '/savedata', TANUKI + '/extdata'), 'psp': ('emulators/rgds-play/PSP/SAVEDATA',)}
# (folder under userdata, extension), newest wins; a RetroArch .srm can be padded, so it is the last resort
OLD_DS_SAVES = (
    ('emulators/drastic/backup', '.dsv'), ('emulators/dsperate/saves', '.sav'), ('emulators/nnddss/User/backup', '.sav'),
    ('emulators/nnddss/User/backup', '.dsv'), ('emulators/melonds/saves', '.sav')
)
DESMUME_MAGIC = b'|-DESMUME SAVE-|'
DESMUME_FOOTER = 122


def files_under(path):
    if path.is_file(): return [path]
    if not path.is_dir(): return []
    return sorted(p for p in path.rglob('*') if p.is_file() and not p.is_symlink())


def signature(data, files, contents):
    """Whether two copies hold the same thing; a resume point goes by size and time, hashing 20 MB would slow a launch."""
    h = hashlib.sha256()
    for path in files:
        h.update(path.relative_to(data).as_posix().encode() + b'\0')
        if contents: h.update(path.read_bytes())
        else: h.update(b'%d %d' % (path.stat().st_size, path.stat().st_mtime_ns))
    return h.hexdigest()


def copies(folder):
    if not folder.is_dir(): return []
    return sorted((p for p in folder.iterdir() if p.is_dir() and p.name.isdigit()), key=lambda p: int(p.name))


def keep_copy(data, folder, files, keep, contents=True):
    """Copy files to a new generation unless the newest already holds them. Returns the new folder, or None."""
    files = [p for p in files if p.stat().st_size]
    if not files: return None
    mark = signature(data, files, contents)
    existing = copies(folder)
    if existing and (existing[-1] / '.signature').is_file():
        if (existing[-1] / '.signature').read_text() == mark: return None
    if shutil.disk_usage(data).free - sum(p.stat().st_size for p in files) < MIN_FREE:
        print('Save history: not enough space for another copy.', file=sys.stderr)
        return None
    target = folder / str(time.time_ns())
    pending = target.with_name(target.name + '.pending')
    for path in files:
        copy = pending / path.relative_to(data)
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, copy)
        try:
            os.utime(copy, ns=(path.stat().st_atime_ns, path.stat().st_mtime_ns))
        except OSError:
            pass
    (pending / '.signature').write_text(mark)
    os.rename(pending, target)
    for old in copies(folder)[:-keep]:
        shutil.rmtree(old)
    return target


def game_saves(data, core, rom, identity):
    """(history folder name, the files that hold the game's own save)"""
    from library import game_key
    if core == DS:
        return game_key(identity), [data / 'emulators' / DS / 'saves' / (rom.stem + '.sav')]
    if core == 'rgds-play':
        from play_options import system_of
        system = system_of(identity)
        if system in SHARED: return system, [data / folder for folder in SHARED[system]]
    return None, []  # the player keeps three of its own (play/src/app/storage.cpp, prepare_save)


def keep_resume_point(data, state, folder, build):
    """A new engine may refuse this file, and quitting would then replace it, so the one an update found stays:
    a later engine that reads it, or going back to the previous system, can still use it."""
    last = folder / '.build'
    seen = last.read_text() if last.is_file() else None
    if build and seen and seen != build:
        keep_copy(data, folder / ('before-' + build), [state], 1, contents=False)
        pinned = sorted(folder.glob('before-*'), key=lambda p: p.stat().st_mtime_ns)
        for old in pinned[:-KEEP_UPDATES]:
            shutil.rmtree(old)
    keep_copy(data, folder, [state], KEEP_STATES, contents=False)
    if build and seen != build: last.write_text(build)


def protect(data, core, rom, identity=None, build=None):
    """Before a launch. Never raises: an unwritable history must not stop anyone playing."""
    data, rom = Path(data), Path(rom)
    identity = Path(identity or rom)
    try:
        if core == DS: import_old_ds_save(data, rom)
        name, paths = game_saves(data, core, rom, identity)
        if name:
            files = [file for path in paths for file in files_under(path)]
            keep_copy(data, data / HISTORY / name, files, KEEP_SAVES)
        from library import game_key
        state = data / 'library' / game_key(identity) / ('auto-' + core + '.dss')
        if state.is_file():
            keep_resume_point(data, state, data / HISTORY / game_key(identity) / 'resume', build)
    except (OSError, ValueError) as error:
        print('Save history: ' + str(error), file=sys.stderr)


def raw_ds_save(path):
    bytes_ = path.read_bytes()
    if bytes_.endswith(DESMUME_MAGIC): bytes_ = bytes_[:-DESMUME_FOOTER]
    return bytes_


def import_old_ds_save(data, rom):
    """A game with no DSCore save takes the newest one an earlier emulator left: copied, the old file stays."""
    target = data / 'emulators' / DS / 'saves' / (rom.stem + '.sav')
    if target.exists(): return None
    found = [rom.with_name(rom.stem + ext) for ext in ('.sav', '.dsv')]
    found += [data / folder / (rom.stem + ext) for folder, ext in OLD_DS_SAVES]
    found = sorted((p for p in found if p.is_file()), key=lambda p: p.stat().st_mtime_ns, reverse=True)
    if rom.with_name(rom.stem + '.srm').is_file(): found.append(rom.with_name(rom.stem + '.srm'))
    for path in found:
        bytes_ = raw_ds_save(path)
        if not bytes_ or len(set(bytes_)) == 1: continue  # never saved to
        target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name + '.importing')
        with pending.open('wb') as out:
            out.write(bytes_)
            out.flush()
            os.fsync(out.fileno())
        os.rename(pending, target)
        print('Save history: imported ' + str(path), flush=True)
        return path
    return None


def restore(data, copy):
    """Put a copy back; what is there now becomes a copy first, so a restore can be undone."""
    data, copy = Path(data), Path(copy)
    kept = [p for p in files_under(copy) if p.name != '.signature']
    current = [data / p.relative_to(copy) for p in kept]
    keep_copy(data, copy.parent, [p for p in current if p.is_file()], KEEP_SAVES + 1)
    for path, target in zip(kept, current):
        target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name + '.restoring')
        shutil.copyfile(path, pending)
        os.rename(pending, target)
    return current


if __name__ == '__main__':
    # save_guard.py DATA            lists the copies
    # save_guard.py DATA restore C  puts copy C (a folder from the list) back
    root = Path(sys.argv[1])
    if sys.argv[2:3] == ['restore']:
        for restored in restore(root, sys.argv[3]):
            print(restored)
    else:
        for found in sorted((root / HISTORY).glob('**/.signature')):
            print(found.parent)
