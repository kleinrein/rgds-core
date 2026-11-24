#!/usr/bin/env python3
"""One queue for every copy from a source into the handheld (docs/NETWORK_PLAN.md section 2).

A job is a file or a whole folder (a disc set, a DOS or ScummVM game) on a source from net_sources. It is copied
into network/incoming/<job>/ as .part files that resume where they stopped, its size is checked, and then it is
placed: into the games folder its system names when it goes to the library, into the inbox when nothing says which
system it is, or into the folder the job names. The runner publishes its progress to a status file the launcher
polls, and the queue itself survives sleep, a dropped Wi-Fi and a reboot.
"""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import shutil
import sys
import threading
import time
import zipfile

from fields import write_fields
from net_sources import Cancelled, SourceError, clean_path, open_source
from play_options import _system_of_games_folder, system_of
from profiles import device_root

DS_EXTENSIONS = ('.nds', '.dsi', '.srl', '.cia')  # dsiware.SUFFIXES
# The disc sheets that name a game made of several files; a folder's system is read from these first.
SHEETS = ('.m3u', '.cue', '.gdi', '.ccd', '.toc', '.chd', '.cdi')
# Systems that share another's games folder (src/launcher/model.hpp shelf_of; the Mega-CD and PC Engine CD keep
# their own folders, which the session lists).
SHELVES = {'gg': 'sms', 'sg': 'sms'}
# Room kept free on the card beyond the job itself, so a full card never stops a save from being written.
SPARE = 64 << 20
RETRIES = 3
FINISHED = ('done', 'cancelled')


def app_root():
    return Path(__file__).resolve().parents[1]


def samba_bundle():
    bundle = app_root() / 'services/samba'
    return bundle if (bundle / 'bin/smbclient').is_file() else None


class Queue:
    """network/transfers.json under a lock; every change is one read-modify-write."""
    def __init__(self, data):
        self.root = device_root(data) / 'network'
        self.path = self.root / 'transfers.json'
        self.incoming = self.root / 'incoming'

    def read(self):
        try:
            record = json.loads(self.path.read_text(encoding='utf-8'))
            if isinstance(record, dict) and isinstance(record.get('jobs'), list):
                return record
        except (OSError, ValueError):
            pass
        return {'jobs': [], 'next': 1, 'serial': 0}

    @contextmanager
    def change(self):
        self.root.mkdir(parents=True, exist_ok=True)
        with open(self.root / '.transfers.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            record = self.read()
            before = json.dumps(record, sort_keys=True)
            yield record
            if json.dumps(record, sort_keys=True) != before:
                pending = self.path.with_name('.transfers.pending')
                with open(pending, 'w', encoding='utf-8') as stream:
                    json.dump(record, stream, indent=1)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(pending, self.path)

    def job(self, ident):
        return next((job for job in self.read()['jobs'] if job['id'] == ident), None)

    def update(self, ident, **fields):
        with self.change() as record:
            for job in record['jobs']:
                if job['id'] == ident:
                    job.update(fields)
                    return dict(job)
        return None

    def staging(self, job):
        return self.incoming / str(job['id'])


def _walk(source, path, prefix=''):
    files = []
    for entry in source.list(path):
        inner = f'{path}/{entry.name}' if path else entry.name
        relative = f'{prefix}{entry.name}'
        if entry.is_dir:
            files += _walk(source, inner, relative + '/')
        else:
            files.append([relative, entry.size])
    return files


def add(data, source_id, path, to='library', bundle=None):
    """Queue a file or folder. It is looked up now, so a typo or a wrong password shows at once, not later."""
    path = clean_path(path)
    if not path:
        raise SourceError('Choose a file or a folder, not the whole source.')
    queue = Queue(data)
    source = open_source(data, source_id, bundle=bundle)
    entry = source.stat(path)
    files = _walk(source, path) if entry.is_dir else [[entry.name, entry.size]]
    if entry.is_dir and not files:
        raise SourceError(f'{entry.name} is an empty folder.')
    job = {
        'kind': 'folder' if entry.is_dir else 'file',
        'source': source_id,
        'path': path,
        'name': entry.name,
        'parent': path.rpartition('/')[0].rpartition('/')[2],
        'files': files,
        'size': sum(s for _, s in files),
        'to': str(to),
        'state': 'queued',
        'received': 0,
        'error': '',
        'placed': '',
        'system': '',
        'added': int(time.time()),
        'attempts': 0
    }
    with queue.change() as record:
        for other in record['jobs']:
            if other['source'] == source_id and other['path'] == path and other['state'] not in FINISHED:
                return other
        job['id'] = record['next']
        record['next'] += 1
        record['jobs'].append(job)
    return job


def cancel(data, ident):
    queue = Queue(data)
    job = queue.update(ident, state='cancelled')
    if job and job['state'] == 'cancelled':
        shutil.rmtree(queue.staging(job), ignore_errors=True)
    return job


def retry(data, ident):
    return Queue(data).update(ident, state='queued', error='', attempts=0)


def clear(data):
    """Forget finished jobs; a failed one stays until it is retried or cancelled."""
    queue = Queue(data)
    with queue.change() as record:
        record['jobs'] = [job for job in record['jobs'] if job['state'] not in FINISHED]


def detect(staged):
    """The system of a staged file or folder, or None when only the owner can say."""
    staged = Path(staged)
    if staged.is_dir():
        members = sorted(
            (p for p in staged.rglob('*') if p.is_file()),
            key=lambda p: (p.suffix.lower() not in SHEETS, len(p.parts), p.name.lower())
        )
        for member in members:
            found = detect(member)
            if found: return found
        return _system_of_games_folder(staged / 'x')
    suffix = staged.suffix.lower()
    if suffix in DS_EXTENSIONS:
        return 'nds'
    found = system_of(staged)
    if found == 'ports':
        return None
    if found != 'nds':
        return found
    if suffix == '.zip':
        try:
            with zipfile.ZipFile(staged) as archive:
                if any(Path(name).suffix.lower() in DS_EXTENSIONS for name in archive.namelist()):
                    return 'nds'
        except (OSError, zipfile.BadZipFile):
            pass
    # system_of says 'nds' for anything it does not know; only a games folder in the source's path can say more.
    games_folder = _system_of_games_folder(staged)
    return games_folder if games_folder and games_folder != 'nds' else None


def _free_name(target):
    if not target.exists():
        return target
    stem, suffix = (target.name, '') if target.is_dir() else (target.stem, target.suffix)
    for number in range(2, 1000):
        candidate = target.with_name(f'{stem} ({number}){suffix}')
        if not candidate.exists():
            return candidate
    raise SourceError(f'There are too many copies of {target.name} already.')


def _same(staged, target):
    if staged.is_dir() or not target.is_file():
        return False
    return staged.stat().st_size == target.stat().st_size


def place(staged, to, roms, inbox):
    """Move a finished copy to where it belongs; (final path, system or '')."""
    staged = Path(staged)
    if to == 'library':
        system = detect(staged)
        folder = Path(roms) / SHELVES.get(system, system) if system else Path(inbox)
    else:
        system, folder = '', Path(to)
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / staged.name
    if _same(staged, target):
        staged.unlink()
        return target, system or ''
    target = _free_name(target)
    shutil.move(str(staged), str(target))
    return target, system or ''


class Runner:
    def __init__(self, data, roms, inbox, status, bundle=None, sleep=time.sleep):
        self.data, self.roms, self.inbox, self.status = data, Path(roms), Path(inbox), Path(status)
        self.queue = Queue(data)
        self.bundle = bundle
        self.sleep = sleep
        self.current = None
        self.rate = 0.0

    def publish(self, **fields):
        record = self.queue.read()
        jobs = record['jobs']
        base = dict(
            state='idle',
            job='',
            name='',
            received=0,
            size=0,
            rate=0,
            error='',
            queued=sum(job['state'] in ('queued', 'running') for job in jobs),
            failed=sum(job['state'] == 'failed' for job in jobs),
            serial=record.get('serial', 0)
        )
        base.update(fields)
        write_fields(self.status, base, durable=False)

    def received(self, job):
        total = 0
        for relative, _ in job['files']:
            target = self.target_of(job, relative)
            for path in (target, target.with_name(target.name + '.part')):
                if path.is_file():
                    total += path.stat().st_size
        return total

    @staticmethod
    def top(job):
        return job['name']

    def target_of(self, job, relative):
        staging = self.queue.staging(job)
        if job['kind'] == 'folder':
            return staging / self.top(job) / relative
        # A single file sits in a folder named like the one it came from, so a plain .bin from a "Genesis" folder
        # still says Mega Drive when it is placed.
        return staging / (job['parent'] or '_') / relative

    def watch(self, job, stop_event):
        last_bytes, last_time = self.received(job), time.monotonic()
        while not stop_event.wait(0.5):
            now, got = time.monotonic(), self.received(job)
            if now > last_time:
                self.rate = 0.7 * self.rate + 0.3 * max(0, got - last_bytes) / (now - last_time)
            last_bytes, last_time = got, now
            self.publish(
                state='running', job=job['id'], name=job['name'], received=got, size=job['size'], rate=int(self.rate)
            )

    def stopper(self, ident):
        checked = [0.0, False]

        def stop():
            if time.monotonic() - checked[0] >= 0.5:
                checked[0] = time.monotonic()
                job = self.queue.job(ident)
                checked[1] = not job or job['state'] == 'cancelled'
            return checked[1]

        return stop

    def fetch(self, job):
        staging = self.queue.staging(job)
        need = job['size'] - self.received(job) + SPARE
        staging.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(staging).free
        if free < need:
            raise SourceError(
                f'Not enough space on the card: {job["name"]} needs {_size(need - SPARE)} more, '
                f'{_size(max(0, free - SPARE))} is free.'
            )
        source = open_source(self.data, job['source'], bundle=self.bundle)
        stop = self.stopper(job['id'])
        for relative, size in job['files']:
            target = self.target_of(job, relative)
            if target.is_file() and target.stat().st_size == size:
                continue
            part = target.with_name(target.name + '.part')
            part.parent.mkdir(parents=True, exist_ok=True)
            if part.exists() and part.stat().st_size > size:
                part.unlink()  # the file on the server got smaller: start over
            remote = f'{job["path"]}/{relative}' if job['kind'] == 'folder' else job['path']
            source.fetch(remote, part, stop)
            got = part.stat().st_size if part.exists() else 0
            if got != size:
                part.unlink(missing_ok=True)
                raise SourceError(f'{relative} arrived with the wrong size. It will be copied again.', True)
            os.replace(part, target)
        return staging / self.top(job) if job['kind'] == 'folder' else self.target_of(job, job['files'][0][0])

    def run_job(self, job):
        stop_event = threading.Event()
        watcher = threading.Thread(target=self.watch, args=(job, stop_event), daemon=True)
        watcher.start()
        try:
            staged = self.fetch(job)
            final, system = place(staged, job['to'], self.roms, self.inbox)
        finally:
            stop_event.set()
            watcher.join()
        shutil.rmtree(self.queue.staging(job), ignore_errors=True)
        with self.queue.change() as record:
            record['serial'] = record.get('serial', 0) + 1
            for other in record['jobs']:
                if other['id'] == job['id']:
                    other.update(
                        state='done', received=job['size'], placed=str(final), system=system, error='', attempts=0
                    )

    def next_job(self):
        with self.queue.change() as record:
            for job in record['jobs']:
                # 'running' left by a runner that died (power off mid-copy) resumes like a queued one.
                if job['state'] in ('queued', 'running'):
                    job['state'] = 'running'
                    return dict(job)
        return None

    def run(self, wait=0.0):
        """Work through the queue. With another runner busy, give it `wait` seconds to finish: one that is just
        leaving may not have seen the job added a moment ago."""
        self.status.parent.mkdir(parents=True, exist_ok=True)
        with open(self.status.with_name(self.status.name + '.lock'), 'a') as lock:
            deadline = time.monotonic() + wait
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        return 0  # another runner has the queue
                    time.sleep(0.1)
            while True:
                job = self.next_job()
                if not job:
                    self.publish()
                    return 0
                self.rate = 0.0
                try:
                    self.run_job(job)
                except Cancelled:
                    shutil.rmtree(self.queue.staging(job), ignore_errors=True)
                except SourceError as error:
                    attempts = job.get('attempts', 0) + 1
                    again = error.transient and attempts < RETRIES
                    current = self.queue.job(job['id'])
                    if current and current['state'] == 'cancelled':
                        continue
                    self.queue.update(
                        job['id'],
                        state='queued' if again else 'failed',
                        error=str(error),
                        attempts=attempts,
                        received=self.received(job)
                    )
                    if again:
                        self.publish(state='waiting', job=job['id'], name=job['name'], error=str(error))
                        self.sleep(3 * attempts)


def _size(count):
    for unit in ('bytes', 'KB', 'MB', 'GB'):
        if count < 1024 or unit == 'GB':
            return f'{count:.0f} {unit}' if unit == 'bytes' else f'{count:.1f} {unit}'
        count /= 1024


def _answer(path, error):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name('.' + path.name + '.pending')
    pending.write_text('error\t' + error.replace('\n', ' ') + '\n' if error else 'ok\n', encoding='utf-8')
    os.replace(pending, path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--roms', type=Path, default=Path('/data/roms'))
    parser.add_argument('--inbox', type=Path, default=Path('/data/inbox'))
    parser.add_argument('--status', type=Path, default=Path('/run/rgds/transfers'))
    parser.add_argument('--bundle', type=Path, default=None, help='the Samba bundle (default: the app\'s own)')
    commands = parser.add_subparsers(dest='command', required=True)
    adding = commands.add_parser('add')
    adding.add_argument('source')
    adding.add_argument('paths', nargs='+')
    adding.add_argument('--to', default='library')
    adding.add_argument('--run', action='store_true', help='then work through the queue')
    adding.add_argument('--answer', type=Path, help="'ok' or 'error<TAB>message' for the launcher")
    for name in ('cancel', 'retry'):
        commands.add_parser(name).add_argument('id', type=int)
    commands.add_parser('clear')
    commands.add_parser('list')
    commands.add_parser('run')
    args = parser.parse_args(argv)
    bundle = args.bundle or samba_bundle()
    try:
        if args.command == 'add':
            try:
                for path in args.paths:
                    print(json.dumps(add(args.data, args.source, path, args.to, bundle=bundle)))
            except SourceError as error:
                if args.answer: _answer(args.answer, str(error))
                raise
            if args.answer: _answer(args.answer, '')
            if args.run:
                return Runner(args.data, args.roms, args.inbox, args.status, bundle=bundle).run(wait=3)
        elif args.command == 'cancel':
            print(json.dumps(cancel(args.data, args.id)))
        elif args.command == 'retry':
            print(json.dumps(retry(args.data, args.id)))
        elif args.command == 'clear':
            clear(args.data)
        elif args.command == 'list':
            print(json.dumps(Queue(args.data).read()['jobs']))
        elif args.command == 'run':
            return Runner(args.data, args.roms, args.inbox, args.status, bundle=bundle).run()
    except SourceError as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
