"""Versioned per-game checkpoint slots. Immutable objects + atomic catalog commits."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import time
import zipfile
from contextlib import contextmanager
import fcntl
from library import game_key
from fields import write_fields
from engines import CORES

MAX_STATE = 128 * 1024 * 1024


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix='.save-')
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(value, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        try:
            fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError:
            pass
    finally:
        Path(tmp).unlink(missing_ok=True)


def sync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def publish_archive(pending, out):
    with Path(pending).open("rb") as stream:
        os.fsync(stream.fileno())
    os.replace(pending, out)
    sync_dir(Path(out).parent)


def copy_file(source, destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=destination.parent, prefix='.save-')
    try:
        with os.fdopen(fd, 'wb') as out, Path(source).open('rb') as inp:
            shutil.copyfileobj(inp, out, 1024 * 1024)
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, destination)
        sync_dir(destination.parent)
    finally:
        Path(tmp).unlink(missing_ok=True)


class SaveLibrary:
    def __init__(self, data, rom, core, build=''):
        if core not in CORES: raise ValueError('Unknown emulator.')
        self.data = Path(data)
        self.rom = Path(rom)
        self.core = core
        self.build = build
        self.game = self.data / 'library' / game_key(self.rom)
        self.root = self.game / 'slots' / core
        self.root.mkdir(parents=True, exist_ok=True)
        self.canonical = self.game / ('auto-' + core + '.dss')

    @contextmanager
    def lock(self):
        with (self.root / 'lock').open('a') as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Another save operation is running.') from None
            yield

    def catalog(self):
        try:
            value = json.loads((self.root / 'catalog.json').read_text())
        except FileNotFoundError:
            return dict(schema=1, slots={}, undo={})
        if value.get('schema') != 1 or not isinstance(value.get('slots'), dict):
            raise ValueError('Save catalog needs recovery.')
        return value

    def commit(self, value):
        atomic_json(self.root / 'catalog.json', value)
        self.publish(value)

    def publish(self, value=None):
        value = value or self.catalog()
        out = dict(core=self.core)
        for i in range(9):
            row = value['slots'].get(str(i), {})
            out['slot_' + str(i)] = row.get('created', '')
            out['image_' + str(i)] = str(self.root / 'objects' / row['object'] / 'preview.bmp') if row else ''
        out['undo'] = int(bool(value.get('undo')))
        out['undo_load'] = int(bool(value.get('undo_load')))
        write_fields(self.root / 'index.ini', out, durable=False)

    def checked(self, row):
        obj = row.get('object', '')
        if not re.fullmatch('[0-9a-f]{64}', obj): raise ValueError('Invalid save object.')
        path = self.root / 'objects' / obj / 'checkpoint.dss'
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_STATE or digest(path) != obj:
            raise ValueError('Save verification failed.')
        return path

    def make_object(self, source, screenshot=None, metadata=None):
        source = Path(source)
        if source.is_symlink() or not source.is_file() or not 0 < source.stat().st_size <= MAX_STATE:
            raise ValueError('No valid checkpoint is available.')
        sha = digest(source)
        folder = self.root / 'objects' / sha
        if not folder.exists():
            stage = Path(tempfile.mkdtemp(dir=self.root, prefix='.object-'))
            try:
                copy_file(source, stage / 'checkpoint.dss')
                if digest(stage / 'checkpoint.dss') != sha: raise ValueError('Checkpoint changed while copying.')
                if screenshot and Path(screenshot).is_file() and Path(screenshot).stat().st_size <= 4 * 1024 * 1024:
                    copy_file(screenshot, stage / 'preview.bmp')
                folder.parent.mkdir(exist_ok=True)
                sync_dir(stage)
                stage.rename(folder)
                sync_dir(folder.parent)
            finally:
                if stage.exists(): shutil.rmtree(stage)
        row = dict(object=sha, created=time.strftime('%Y-%m-%d %H:%M'), build=self.build)
        if metadata: row.update({k: metadata[k] for k in ('created', 'build') if isinstance(metadata.get(k), str)})
        self.checked(row)
        return row

    def store(self, slot, overwrite=False, screenshot=None, source=None, metadata=None):
        if type(slot) is not int or not 0 <= slot < 9: raise ValueError('Invalid save slot.')
        with self.lock():
            c = self.catalog()
            key = str(slot)
            if key in c['slots'] and not overwrite: raise ValueError('Confirm overwriting this slot.')
            row = self.make_object(source or self.canonical, screenshot, metadata)
            c['undo'] = dict(slot=key, previous=c['slots'].get(key))
            c['slots'][key] = row
            self.commit(c)

    def stage_load(self, slot):
        # The runner captures current emulation into canonical first. Keep it
        # as load undo before replacing canonical with the selected slot.
        with self.lock():
            c = self.catalog()
            row = c['slots'].get(str(slot))
            if not row: raise ValueError('This slot is empty.')
            target = self.checked(row)
            undo = self.make_object(self.canonical)
            c['undo_load'] = undo
            self.commit(c)
            copy_file(target, self.canonical)

    def undo_load(self):
        with self.lock():
            c = self.catalog()
            target = self.checked(c.get('undo_load', {}))
            current = self.make_object(self.canonical)
            c['undo_load'] = current
            self.commit(c)
            copy_file(target, self.canonical)

    def delete(self, slot):
        # Deleting only drops the catalog row; the object stays, so one undo restores it.
        if type(slot) is not int or not 0 <= slot < 9: raise ValueError('Invalid save slot.')
        with self.lock():
            c = self.catalog()
            key = str(slot)
            row = c['slots'].pop(key, None)
            if not row: raise ValueError('This slot is empty.')
            c['undo'] = dict(slot=key, previous=row)
            self.commit(c)

    def undo_write(self):
        with self.lock():
            c = self.catalog()
            u = c.get('undo', {})
            key = u.get('slot')
            if key not in map(str, range(9)): raise ValueError('Nothing to undo.')
            current = c['slots'].get(key)
            if u.get('previous'):
                self.checked(u['previous'])
                c['slots'][key] = u['previous']
            else:
                c['slots'].pop(key, None)
            c['undo'] = dict(slot=key, previous=current)
            self.commit(c)

    def export(self, slot):
        with self.lock():
            row = self.catalog()['slots'].get(str(slot), {})
            state = self.checked(row)
            meta = dict(
                schema=1,
                core=self.core,
                rom_sha256=digest(self.rom),
                checkpoint_sha256=row['object'],
                **{k: row.get(k, '')
                   for k in ('created', 'build')}
            )
            folder = self.data / 'exports'
            folder.mkdir(exist_ok=True)
            out = folder / (
                game_key(self.rom) + '-' + self.core + '-slot' + str(slot + 1) + '-' + str(time.time_ns()) +
                '.rgdssave.zip'
            )
            with zipfile.ZipFile(str(out) + '.pending', 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr('save.json', json.dumps(meta))
                z.write(state, 'checkpoint.dss')
                preview = state.parent / 'preview.bmp'
                if preview.is_file(): z.write(preview, 'preview.bmp')
            publish_archive(str(out) + '.pending', out)
            return out

    def import_slot(self, archive, slot, overwrite=False):
        archive = Path(archive)
        with zipfile.ZipFile(archive) as z:
            entries = z.infolist()
            names = [e.filename for e in entries]
            if len(names) != len(set(names)) or set(names) - {'save.json', 'checkpoint.dss', 'preview.bmp'} or sum(
                e.file_size for e in entries
            ) > MAX_STATE + 4 * 1024 * 1024 or z.getinfo('save.json').file_size > 4096:
                raise ValueError('Invalid save archive.')
            meta = json.loads(z.read('save.json'))
            if meta.get('schema') != 1 or meta.get('core') != self.core or meta.get('rom_sha256') != digest(self.rom):
                raise ValueError('This save belongs to a different game or emulator.')
            with tempfile.TemporaryDirectory(dir=self.root) as temp:
                temp = Path(temp)
                for name in names:
                    with z.open(name) as inp, (temp / name).open('wb') as out:
                        shutil.copyfileobj(inp, out, 1024 * 1024)
                if digest(temp / 'checkpoint.dss') != meta.get('checkpoint_sha256'):
                    raise ValueError('Save verification failed.')
                self.store(slot, overwrite, temp / 'preview.bmp', temp / 'checkpoint.dss', meta)


def backup_all(data):
    """Explicit portable backup. Excludes ROMs, BIOS, Wi-Fi and running engines."""
    from emulators import activity_lock, core_running
    data = Path(data)
    with activity_lock(data):
        if core_running(data): raise ValueError('Exit your game before backing up saves.')
        roots = [
            data / 'library', *[
                data / 'emulators' / engine / folder for engine, folder in [
                    ('drastic', 'backup'), ('drastic', 'savestates'), ('dsperate', 'saves'), ('dsperate', 'states'),
                    ('nnddss', 'User/backup'), ('nnddss', 'User/savestates'), ('melonds', 'saves'),
                    ('melonds', 'states'), ('rgds-experimental', 'saves'), ('rgds-experimental', 'states')
                ]
            ]
        ]
        roots += [data / 'emulators/rgds-play' / folder for folder in ('saves', 'states')]  # other systems
        selected = []
        for root in roots:
            if root.exists():
                for path in root.rglob('*'):
                    if path.is_file() and not path.is_symlink() and (
                        root != data / 'library' or path.suffix in ('.dss', '.sav', '.dsv', '.json')
                        or '/slots/' in str(path)
                    ):
                        selected.append(path)
        if sum(p.stat().st_size for p in selected) > shutil.disk_usage(data).free - 16 * 1024 * 1024:
            raise ValueError('Not enough space for a save backup.')
        out = data / 'exports' / ('save-backup-' + str(time.time_ns()) + '.zip')
        out.parent.mkdir(exist_ok=True)
        try:
            with zipfile.ZipFile(str(out) + '.pending', 'w', zipfile.ZIP_DEFLATED) as z:
                manifest = {}
                for path in selected:
                    name = path.relative_to(data).as_posix()
                    z.write(path, name)
                    manifest[name] = digest(path)
                z.writestr('backup.json', json.dumps(dict(schema=1, files=manifest)))
            publish_archive(str(out) + '.pending', out)
        finally:
            Path(str(out) + '.pending').unlink(missing_ok=True)
        return out
