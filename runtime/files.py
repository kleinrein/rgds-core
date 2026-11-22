#!/usr/bin/env python3
"""The file manager's hands (docs/NETWORK_PLAN.md section 3): copy, move, delete to the trash, rename, new folder,
extract a zip, and a preview of one file for the upper screen.

The launcher lists folders itself and runs this for anything that changes the card, one command at a time, reading
its progress from a status file. The rules about what needs a second "are you sure" live here only: a command that
touches a save or the system's own folder stops with confirm=1 and the launcher asks, then runs it again with
--confirmed.
"""
import argparse
import os
from pathlib import Path
import shutil
import sys
import time
import zipfile

from fields import write_fields
from profiles import device_root

# exFAT, the card's file system, refuses these in a name; so does FAT for anything copied to a computer's stick.
ILLEGAL = set('\\/:*?"<>|')
SAVE_SUFFIXES = (
    '.sav', '.srm', '.dss', '.dsv', '.eep', '.fla', '.sra', '.mcr', '.mcd', '.vmu', '.bcr', '.rtc', '.state', '.st0',
    '.st1', '.st2', '.st3', '.st4', '.st5', '.st6', '.st7', '.st8', '.st9'
)
TRASH_DAYS = 30
CHUNK = 1 << 20
IMAGES = ('.png', '.bmp')
TEXTS = (
    '.txt', '.md', '.ini', '.cfg', '.json', '.xml', '.cue', '.m3u', '.gdi', '.nfo', '.log', '.csv', '.tsv', '.sh',
    '.yml', '.yaml', '.toml'
)


class Refused(Exception):
    """A message for the owner. confirm: the command may go ahead once they say yes."""
    def __init__(self, message, confirm=False):
        super().__init__(message)
        self.confirm = confirm


class Status:
    def __init__(self, path, task):
        self.path, self.task = Path(path) if path else None, task
        self.fields = dict(state='busy', task=task, done=0, total=0, current='', message='', confirm=0, result='')
        self.written = 0.0

    def publish(self, force=False, **fields):
        self.fields.update(fields)
        if not self.path: return
        now = time.monotonic()
        if force or now - self.written >= 0.2:
            self.written = now
            write_fields(self.path, self.fields, durable=False)


def check_name(name):
    name = name.strip()
    if not name or name in ('.', '..'):
        raise Refused('Type a name.')
    bad = sorted({c for c in name if c in ILLEGAL or ord(c) < 32})
    if bad:
        shown = ' '.join('space' if c == ' ' else c for c in bad if ord(c) >= 32)
        raise Refused(f'A name cannot have {shown} in it.' if shown else 'A name cannot have control characters.')
    if name.endswith('.'):
        raise Refused('A name cannot end with a dot.')
    if len(name.encode('utf-8')) > 255:
        raise Refused('That name is too long.')
    return name


def is_inside(path, folder):
    try:
        Path(path).resolve().relative_to(Path(folder).resolve())
        return True
    except ValueError:
        return False


def sensitive(path, data):
    """Why this needs a yes first, or ''. The system's own folder holds every save, setting and engine; a save file
    outside it (a .sav beside its game) is still somebody's progress."""
    path = Path(path)
    if is_inside(path, trash_root(data)):
        return ''
    if is_inside(path, device_root(data)):
        return 'This is in the RG DS system folder, where saves and settings live.'
    if path.is_file() and path.suffix.lower() in SAVE_SUFFIXES:
        return 'This is a save file.'
    if path.is_dir():
        for inner in path.rglob('*'):
            if inner.suffix.lower() in SAVE_SUFFIXES:
                return 'This folder has saves in it.'
    return ''


def guard(paths, data, confirmed, verb):
    if confirmed: return
    for path in paths:
        why = sensitive(path, data)
        if why:
            raise Refused(f'{why} {verb} it anyway?', confirm=True)


def free_name(folder, name):
    target = Path(folder) / name
    if not target.exists():
        return target
    path = Path(name)
    stem, suffix = (name, '') if target.is_dir() or not path.suffix else (path.stem, path.suffix)
    for number in range(2, 10000):
        candidate = Path(folder) / f'{stem} ({number}){suffix}'
        if not candidate.exists():
            return candidate
    raise Refused(f'There are too many copies of {name} already.')


def size_of(path):
    path = Path(path)
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def _copy_file(source, target, status):
    pending = target.with_name(target.name + '.part')
    with open(source, 'rb') as reader, open(pending, 'wb') as writer:
        while True:
            block = reader.read(CHUNK)
            if not block: break
            writer.write(block)
            status.publish(done=status.fields['done'] + len(block))
        writer.flush()
        os.fsync(writer.fileno())
    os.replace(pending, target)
    try:
        shutil.copystat(source, target)
    except OSError:
        pass


def _copy_tree(source, target, status):
    if source.is_dir():
        target.mkdir()
        for inner in sorted(source.iterdir()):
            _copy_tree(inner, target / inner.name, status)
    else:
        status.publish(current=source.name)
        _copy_file(source, target, status)


def transfer(sources, folder, data, move=False, confirmed=False, status=None):
    status = status or Status(None, 'move' if move else 'copy')
    sources, folder = [Path(p) for p in sources], Path(folder)
    if not folder.is_dir():
        raise Refused('That folder is not there any more.')
    for source in sources:
        if not source.exists():
            raise Refused(f'{source.name} is not there any more.')
        if source.is_dir() and is_inside(folder, source):
            raise Refused(f'{source.name} cannot go inside itself.')
    if move: guard(sources, data, confirmed, 'Move')
    total = sum(size_of(source) for source in sources)
    free = shutil.disk_usage(folder).free
    same_disk = move and all(source.stat().st_dev == folder.stat().st_dev for source in sources)
    if not same_disk and total > free:
        raise Refused(f'Not enough space: this needs {human(total)}, {human(free)} is free.')
    status.publish(force=True, total=total)
    placed = []
    for source in sources:
        if move and source.parent.resolve() == folder.resolve():
            placed.append(source)
            continue
        target = free_name(folder, source.name)
        if same_disk:
            os.rename(source, target)
            status.publish(done=status.fields['done'] + size_of(target))
        else:
            _copy_tree(source, target, status)
            if move:
                shutil.rmtree(source) if source.is_dir() else source.unlink()
        placed.append(target)
    return placed


def trash_root(data):
    return device_root(data) / 'trash'


def purge_trash(data, days=TRASH_DAYS, now=None):
    root = trash_root(data)
    now = now or time.time()
    if not root.is_dir(): return 0
    removed = 0
    for batch in root.iterdir():
        try:
            old = now - batch.stat().st_mtime > days * 86400
        except OSError:
            continue
        if days == 0 or old:
            shutil.rmtree(batch, ignore_errors=True) if batch.is_dir() else batch.unlink(missing_ok=True)
            removed += 1
    return removed


def delete(paths, data, confirmed=False, status=None):
    """Into the trash on the same card, so it is a rename and can be taken back by hand for 30 days. Things already
    in the trash go for good."""
    status = status or Status(None, 'delete')
    paths = [Path(p) for p in paths]
    for path in paths:
        if not path.exists() and not path.is_symlink():
            raise Refused(f'{path.name} is not there any more.')
    guard(paths, data, confirmed, 'Delete')
    purge_trash(data)
    batch = trash_root(data) / time.strftime('%Y%m%d-%H%M%S')
    status.publish(force=True, total=len(paths))
    for count, path in enumerate(paths, 1):
        if is_inside(path, trash_root(data)):
            shutil.rmtree(path) if path.is_dir() else path.unlink()
        else:
            batch.mkdir(parents=True, exist_ok=True)
            target = free_name(batch, path.name)
            try:
                os.rename(path, target)
            except OSError:
                # Another card or disk: the trash is no help there, so it goes now.
                shutil.rmtree(path) if path.is_dir() else path.unlink()
            else:
                with open(batch / '.from.tsv', 'a', encoding='utf-8') as index:
                    index.write(f'{target.name}\t{path}\n')
        status.publish(done=count, current=path.name)
    return len(paths)


def rename(path, name, data, confirmed=False):
    path, name = Path(path), check_name(name)
    if not path.exists():
        raise Refused(f'{path.name} is not there any more.')
    if name == path.name:
        return path
    target = path.with_name(name)
    if target.exists() and target.name.lower() != path.name.lower():
        raise Refused(f'There is already something called {name} here.')
    guard([path], data, confirmed, 'Rename')
    if target.name.lower() == path.name.lower():
        # exFAT ignores case: go through a temporary name so "zelda.gba" can become "Zelda.gba".
        step = free_name(path.parent, '.rename-' + name)
        os.rename(path, step)
        path = step
    os.rename(path, target)
    return target


def make_folder(parent, name):
    parent, name = Path(parent), check_name(name)
    target = parent / name
    if target.exists():
        raise Refused(f'There is already something called {name} here.')
    target.mkdir()
    return target


def extract(archive, folder=None, status=None):
    """A zip into a new folder named after it, beside it. Names that would climb out of that folder are refused."""
    status = status or Status(None, 'extract')
    archive = Path(archive)
    folder = Path(folder) if folder else archive.parent
    try:
        packed = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile):
        raise Refused(f'{archive.name} is not a zip this can open.') from None
    with packed:
        members = [m for m in packed.infolist() if not m.filename.startswith('__MACOSX/')]
        total = sum(m.file_size for m in members)
        free = shutil.disk_usage(folder).free
        if total > free:
            raise Refused(f'Not enough space: this needs {human(total)}, {human(free)} is free.')
        target = free_name(folder, archive.stem)
        pending = target.with_name('.' + target.name + '.part')
        shutil.rmtree(pending, ignore_errors=True)
        pending.mkdir()
        status.publish(force=True, total=total)
        try:
            for member in members:
                destination = (pending / member.filename).resolve()
                if not is_inside(destination, pending):
                    raise Refused(f'{archive.name} has a file that would land outside its folder.')
                if any(c in ILLEGAL - {'/', '\\'} for c in Path(member.filename).name):
                    raise Refused(f'{archive.name} has names the card cannot store.')
                if member.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                status.publish(current=Path(member.filename).name)
                with packed.open(member) as reader, open(destination, 'wb') as writer:
                    while True:
                        block = reader.read(CHUNK)
                        if not block: break
                        writer.write(block)
                        status.publish(done=status.fields['done'] + len(block))
            os.rename(pending, target)
        finally:
            shutil.rmtree(pending, ignore_errors=True)
    return target


def human(count):
    for unit in ('bytes', 'KB', 'MB', 'GB', 'TB'):
        if count < 1024 or unit == 'TB':
            return f'{count} bytes' if unit == 'bytes' else f'{count:.1f} {unit}'
        count /= 1024


def peek(path, out):
    """What the upper screen shows for one file: out.txt (up to 12 lines) and, for a picture, out.bmp."""
    path, out = Path(path), Path(out)
    lines, picture = [], None
    suffix = path.suffix.lower()
    try:
        if suffix == '.zip':
            with zipfile.ZipFile(path) as packed:
                names = [i for i in packed.infolist() if not i.is_dir() and not i.filename.startswith('__MACOSX/')]
                lines = [f'{len(names)} files, {human(sum(i.file_size for i in names))} unpacked']
                lines += [f'{i.filename}  {human(i.file_size)}' for i in names[:11]]
        elif suffix in IMAGES:
            from screenshots import decode_png, fit, read_bmp
            picture = decode_png(path.read_bytes()) if suffix == '.png' else read_bmp(path)
            lines = [f'{picture.width} × {picture.height}']
            picture = fit(picture, 300, 288)
        elif suffix in TEXTS:
            with open(path, 'rb') as stream:
                head = stream.read(2048)
            if b'\0' not in head:
                lines = head.decode('utf-8', 'replace').splitlines()[:12]
    except Exception:  # noqa: BLE001 - a file that does not decode just has no preview
        lines, picture = [], None
    out.parent.mkdir(parents=True, exist_ok=True)
    bitmap = out.with_suffix('.bmp')
    if picture:
        from screenshots import write_bmp
        write_bmp(bitmap, picture)
    else:
        bitmap.unlink(missing_ok=True)
    text = out.with_suffix('.txt')
    pending = text.with_name(text.name + '.pending')
    # The first line names the file, whole, so the launcher can tell a stale preview from the one it asked for.
    pending.write_text(str(path) + '\n' + ''.join(line[:120] + '\n' for line in lines), encoding='utf-8')
    os.replace(pending, text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--status', type=Path)
    parser.add_argument('--confirmed', action='store_true')
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('copy', 'move'):
        command = commands.add_parser(name)
        command.add_argument('paths', nargs='+', type=Path)
        command.add_argument('--to', type=Path, required=True)
    commands.add_parser('delete').add_argument('paths', nargs='+', type=Path)
    renaming = commands.add_parser('rename')
    renaming.add_argument('path', type=Path)
    renaming.add_argument('name')
    folder = commands.add_parser('mkdir')
    folder.add_argument('parent', type=Path)
    folder.add_argument('name')
    extracting = commands.add_parser('extract')
    extracting.add_argument('path', type=Path)
    commands.add_parser('empty-trash')
    peeking = commands.add_parser('peek')
    peeking.add_argument('path', type=Path)
    peeking.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == 'peek':
        peek(args.path, args.out)
        return 0
    status = Status(args.status, args.command)
    status.publish(force=True)
    try:
        if args.command in ('copy', 'move'):
            placed = transfer(args.paths, args.to, args.data, args.command == 'move', args.confirmed, status)
            result = placed[0] if placed else ''
        elif args.command == 'delete':
            delete(args.paths, args.data, args.confirmed, status)
            result = ''
        elif args.command == 'rename':
            result = rename(args.path, args.name, args.data, args.confirmed)
        elif args.command == 'mkdir':
            result = make_folder(args.parent, args.name)
        elif args.command == 'extract':
            result = extract(args.path, status=status)
        else:
            purge_trash(args.data, days=0)
            result = ''
    except Refused as refused:
        status.publish(
            force=True,
            state='confirm' if refused.confirm else 'error',
            message=str(refused),
            confirm=int(refused.confirm)
        )
        return 3 if refused.confirm else 1
    except OSError as error:
        status.publish(force=True, state='error', message=f'That did not work: {error.strerror or error}.')
        return 1
    status.publish(force=True, state='done', done=status.fields['total'], result=str(result))
    return 0


if __name__ == '__main__':
    sys.exit(main())
