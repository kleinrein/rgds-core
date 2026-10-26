#!/usr/bin/env python3
"""Game files for PortMaster ports, sent from a phone or a computer over Wi-Fi.

Half of PortMaster's ports need files the owner bought (a Steam folder, a GOG
installer, a data.win). While the launcher's Add game files page is open, this
serves a small web page on the handheld: a phone scans the QR code on the
bottom screen, a Windows or macOS computer opens http://<hostname>.local (or
the address) and types the four-digit code once. Files and whole folders are
dropped on the page and land where the port expects them, in chunks that resume
after a dropped connection. Zip files can be unpacked on the handheld.

The launcher reads session/port-files.ini (address, code, progress) and
session/port-files.qr (the QR code as rows of 0 and 1). The server stops when
the launcher closes the page, when the launcher goes away, or after a long idle
time.
"""
import argparse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import socket
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zipfile

sys.dont_write_bytecode = True
from fields import read_fields, write_fields
import portmaster
import qr

PAGE = Path(__file__).resolve().parent / 'portmaster_files/upload.html'
IDLE_SECONDS = 30 * 60
CHUNK_LIMIT = 256 * 1024 * 1024  # one request's body; the page sends 32 MiB
SPARE_BYTES = 64 * 1024 * 1024  # left free on the card after an upload
FAT_LIMIT = 4 * 1024 * 1024 * 1024 - 1
# Clutter computers add beside copied files; never written to the card.
JUNK = {'.ds_store', 'thumbs.db', 'desktop.ini', '.localized', '__macosx', '.spotlight-v100', '.trashes', '.fseventsd'}
# Characters FAT refuses in names.
FAT_BAD = set('<>:"\\|?*') | {chr(c) for c in range(32)}


class Refused(Exception):
    """A request the page gets a plain message for."""
    def __init__(self, message, status=HTTPStatus.BAD_REQUEST, **extra):
        super().__init__(message)
        self.status = status
        self.extra = extra


def local_address():
    """The address other machines on the network reach this one at (no packet is sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(('192.0.2.1', 9))
            return probe.getsockname()[0]
        except OSError:
            return ''


def clean_parts(relative):
    """A relative path from the page as safe parts, or None for clutter. Refused for anything leaving the folder."""
    parts = [part for part in str(relative).replace('\\', '/').split('/') if part not in ('', '.')]
    if not parts: raise Refused('A file needs a name.')
    if any(part == '..' for part in parts): raise Refused('That path leaves the port\'s folder.')
    if any(part.lower() in JUNK or part.startswith('._') for part in parts): return None
    cleaned = []
    for part in parts:
        part = ''.join('_' if char in FAT_BAD else char for char in part).rstrip(' .')
        if not part or len(part.encode()) > 255: raise Refused('A file name is not allowed on the card: ' + relative)
        cleaned.append(part)
    return cleaned


def mount_type(path):
    """The filesystem type of the mount holding path ('vfat' on the RG DS card)."""
    path, best = str(Path(path).resolve()), ('', '')
    try:
        for line in Path('/proc/mounts').read_text().splitlines():
            fields = line.split()
            if len(fields) < 3: continue
            point = fields[1].replace('\\040', ' ')
            if (path == point or path.startswith(point.rstrip('/') + '/')) and len(point) > len(best[0]):
                best = (point, fields[2])
    except OSError:
        pass
    return best[1]


def free_bytes(path):
    try:
        stat = os.statvfs(path)
        return stat.f_bavail * stat.f_frsize
    except OSError:
        return 0


class Ports:
    """The installed ports, their target folders and what the owner has added (library/<key>/port-files.ini)."""
    def __init__(self, data, folders):
        self.data = Path(data)
        self.folders = folders
        self.lock = threading.Lock()

    def all(self):
        result = []
        for script in sorted(self.folders.ports.glob('*.sh')) if self.folders.ports.is_dir() else []:
            port = portmaster.details(script, self.folders)
            if port is None: continue
            added = read_fields(self.record(script))
            port['targets'] = self.targets(Path(port['folder']), port['target'])
            port['files_added'] = int(added.get('files', 0) or 0)
            port['bytes_added'] = int(added.get('bytes', 0) or 0)
            port['cover'] = any((Path(port['folder']) / name).is_file() for name in COVERS)
            result.append(port)
        # Ports waiting for files first, then by title.
        result.sort(key=lambda port: (port['ready'] or port['files_added'] > 0, port['title'].lower()))
        return result

    def find(self, port_id):
        for port in self.all():
            if port['id'] == port_id: return port
        raise Refused('That port is not installed any more.', HTTPStatus.NOT_FOUND)

    @staticmethod
    def targets(folder, target):
        """The folders a page may put files in: the port's own and its subfolders, except PortMaster's own."""
        names = ['']
        try:
            names += sorted(
                entry.name for entry in folder.iterdir()
                if entry.is_dir() and entry.name.lower() not in ('conf', 'licenses', 'libs', 'lib', '.rgds')
            )
        except OSError:
            pass
        if target and target not in names: names.append(target)
        return names

    def destination(self, port_id, target, relative):
        port = self.find(port_id)
        if target not in self.targets(Path(port['folder']), port['target']):
            raise Refused('That folder is not part of the port.')
        parts = clean_parts(relative)
        if parts is None: return port, None
        base = Path(port['folder']) / target if target else Path(port['folder'])
        path = base.joinpath(*parts)
        if Path(port['folder']).resolve() not in path.resolve().parents:
            raise Refused('That path leaves the port\'s folder.')
        return port, path

    def record(self, script):
        from library import game_key
        return self.data / 'library' / game_key(script) / 'port-files.ini'

    def added(self, port, files, size):
        with self.lock:
            path = self.record(port['script'])
            fields = read_fields(path)
            path.parent.mkdir(parents=True, exist_ok=True)
            write_fields(
                path,
                dict(
                    files=int(fields.get('files', 0) or 0) + files,
                    bytes=int(fields.get('bytes', 0) or 0) + size,
                    at=int(time.time())
                )
            )


COVERS = ('cover.png', 'cover.jpg', 'screenshot.png', 'screenshot.jpg')


class Status:
    """session/port-files.ini for the launcher, written at most a few times a second."""
    def __init__(self, path, base):
        self.path = Path(path) if path else None
        self.fields = dict(base)
        self.written = 0.0
        self.lock = threading.Lock()
        self.token = 0

    def update(self, force=False, **fields):
        with self.lock:
            self.fields.update({key: str(value) for key, value in fields.items()})
            now = time.monotonic()
            if not self.path or (not force and now - self.written < .25): return
            self.written = now
            self.token += 1
            self.fields['token'] = str(self.token)
            try:
                write_fields(self.path, self.fields, durable=False)
            except OSError:
                pass


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, ports, status, code, selected):
        super().__init__(address, Handler)
        self.ports = ports
        self.status = status
        self.code = code
        self.selected = selected
        self.tokens = set()
        self.failures = []
        self.seen = time.monotonic()
        self.busy = 0
        self.file_locks = {}
        self.lock = threading.Lock()

    def file_lock(self, path):
        with self.lock:
            return self.file_locks.setdefault(str(path), threading.Lock())


class Handler(BaseHTTPRequestHandler):
    server_version = 'RG-DS-Core'
    protocol_version = 'HTTP/1.1'

    def log_message(self, format, *args):  # noqa: A002 - BaseHTTPRequestHandler's name
        print('port files: ' + self.address_string() + ' ' + format % args, file=sys.stderr, flush=True)

    # --- plumbing

    def reply(self, status, body=b'', kind='application/json', headers=()):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        for key, value in headers:
            self.send_header(key, value)
        self.end_headers()
        if self.command != 'HEAD': self.wfile.write(body)

    def query(self):
        values = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
        return {key: value[-1] for key, value in values.items()}

    def authorised(self):
        for part in self.headers.get('Cookie', '').split(';'):
            name, _, value = part.strip().partition('=')
            if name == 'rgds' and value in self.server.tokens: return True
        return False

    def read_body(self, limit):
        length = int(self.headers.get('Content-Length') or 0)
        if length < 0 or length > limit: raise Refused('That request is too big.', HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        return length

    def handle_one_request(self):
        self.server.seen = time.monotonic()
        super().handle_one_request()

    def run(self, action):
        try:
            action()
        except Refused as refused:
            self.close_connection = True
            self.reply(refused.status, dict(error=str(refused), **refused.extra))
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            self.close_connection = True
            self.server.status.update(force=True, state='error', error=str(error))
            self.reply(HTTPStatus.INTERNAL_SERVER_ERROR, dict(error=str(error)))

    # --- routes

    def do_GET(self):  # noqa: N802 - http.server's name
        self.run(self.get)

    def do_PUT(self):  # noqa: N802
        self.run(self.put)

    def do_POST(self):  # noqa: N802
        self.run(self.post)

    def get(self):
        route = urlsplit(self.path).path
        if route in ('/', '/index.html'):
            self.reply(
                HTTPStatus.OK,
                PAGE.read_bytes(),
                'text/html; charset=utf-8',
                headers=[
                    (
                        'Content-Security-Policy', "default-src 'self'; style-src 'unsafe-inline'; "
                        "script-src 'unsafe-inline'; img-src 'self' data:"
                    )
                ]
            )
            return
        if route == '/favicon.ico':
            self.reply(HTTPStatus.NO_CONTENT, b'', 'image/x-icon')
            return
        if not self.authorised(): raise Refused('Type the code shown on the handheld.', HTTPStatus.UNAUTHORIZED)
        if route == '/api/ports':
            ports = self.server.ports.all()
            free = free_bytes(self.server.ports.folders.ports)
            self.reply(
                HTTPStatus.OK,
                dict(
                    ports=[
                        {
                            key: value
                            for key, value in port.items() if key not in ('script', 'folder')
                        } for port in ports
                    ],
                    selected=self.server.selected,
                    free=free,
                    fat=mount_type(self.server.ports.folders.ports) in ('vfat', 'msdos', 'fat')
                )
            )
            return
        if route.startswith('/api/cover/'):
            port = self.server.ports.find(route.rsplit('/', 1)[1])
            for name in COVERS:
                path = Path(port['folder']) / name
                if path.is_file() and path.stat().st_size < 8 * 1024 * 1024:
                    kind = 'image/png' if name.endswith('.png') else 'image/jpeg'
                    self.reply(HTTPStatus.OK, path.read_bytes(), kind)
                    return
            raise Refused('No picture.', HTTPStatus.NOT_FOUND)
        if route == '/api/offset':
            query = self.query()
            _port, path = self.server.ports.destination(
                query.get('port', ''), query.get('target', ''), query.get('path', '')
            )
            have = 0
            if path is not None:
                part = path.with_name(path.name + '.rgds-part')
                have = part.stat().st_size if part.is_file() else 0
            self.reply(HTTPStatus.OK, dict(offset=have))
            return
        raise Refused('Not here.', HTTPStatus.NOT_FOUND)

    def post(self):
        route = urlsplit(self.path).path
        if route == '/api/pair':
            body = self.rfile.read(self.read_body(256))
            now = time.monotonic()
            failures = self.server.failures = [at for at in self.server.failures if now - at < 30]
            if len(failures) >= 5: raise Refused('Too many tries. Wait half a minute.', HTTPStatus.TOO_MANY_REQUESTS)
            try:
                code = str(json.loads(body or b'{}').get('code', '')).strip()
            except ValueError:
                code = ''
            if not secrets.compare_digest(code, self.server.code):
                failures.append(now)
                raise Refused('That is not the code on the handheld.', HTTPStatus.FORBIDDEN)
            token = secrets.token_urlsafe(24)
            self.server.tokens.add(token)
            self.server.status.update(force=True, state='connected', connected=len(self.server.tokens))
            self.reply(
                HTTPStatus.OK,
                dict(ok=True),
                headers=[('Set-Cookie', 'rgds=%s; Path=/; HttpOnly; SameSite=Strict' % token)]
            )
            return
        if not self.authorised(): raise Refused('Type the code shown on the handheld.', HTTPStatus.UNAUTHORIZED)
        if route == '/api/unpack':
            query = self.query()
            port, path = self.server.ports.destination(
                query.get('port', ''), query.get('target', ''), query.get('path', '')
            )
            if path is None or not path.is_file(): raise Refused('That zip file is not on the handheld.')
            self.server.busy += 1
            try:
                files, size = unpack(path, self.server.status, port['title'])
            finally:
                self.server.busy -= 1
            path.unlink()
            self.server.ports.added(port, files, 0)
            self.server.status.update(force=True, state='received', file='', files_done=files, files_total=files)
            self.reply(HTTPStatus.OK, dict(files=files, bytes=size))
            return
        if route == '/api/progress':
            # The page's view of the whole drop, for the bottom screen.
            body = self.rfile.read(self.read_body(4096))
            try:
                value = json.loads(body or b'{}')
            except ValueError:
                value = {}
            fields = {key: int(value.get(key, 0) or 0) for key in ('done', 'total', 'files_done', 'files_total')}
            state = 'receiving' if fields['files_done'] < fields['files_total'] else 'received'
            self.server.status.update(force=state == 'received', state=state, **fields)
            self.reply(HTTPStatus.OK, dict(ok=True))
            return
        raise Refused('Not here.', HTTPStatus.NOT_FOUND)

    def put(self):
        if not self.authorised(): raise Refused('Type the code shown on the handheld.', HTTPStatus.UNAUTHORIZED)
        if urlsplit(self.path).path != '/api/upload': raise Refused('Not here.', HTTPStatus.NOT_FOUND)
        query = self.query()
        port, path = self.server.ports.destination(
            query.get('port', ''), query.get('target', ''), query.get('path', '')
        )
        length = self.read_body(CHUNK_LIMIT)
        try:
            offset, size = int(query.get('offset', 0)), int(query.get('size', 0))
        except ValueError:
            raise Refused('Bad offset or size.') from None
        final = query.get('final') == '1'
        if path is None:  # clutter such as .DS_Store: read and forget
            self.rfile.read(length)
            self.reply(HTTPStatus.OK, dict(skipped=True))
            return
        if size < 0 or offset < 0 or offset + length > size: raise Refused('Bad offset or size.')
        if size > FAT_LIMIT and mount_type(path.parent if path.parent.exists() else port['folder']
                                           ) in ('vfat', 'msdos'):
            raise Refused('%s is bigger than 4 GB, which the games card cannot hold in one file.' % path.name)
        with self.server.file_lock(path):
            part = path.with_name(path.name + '.rgds-part')
            have = part.stat().st_size if part.is_file() else 0
            if have < offset:
                raise Refused('Resume from the handheld\'s copy.', HTTPStatus.CONFLICT, offset=have)
            if offset == 0 and size - length > free_bytes(port['folder']) - SPARE_BYTES:
                raise Refused('Not enough space on the card for %s.' % path.name, HTTPStatus.INSUFFICIENT_STORAGE)
            path.parent.mkdir(parents=True, exist_ok=True)
            self.server.busy += 1
            self.server.status.update(state='receiving', port=port['id'], title=port['title'], file=path.name)
            try:
                with part.open('r+b' if have else 'wb') as out:
                    out.truncate(offset)
                    out.seek(offset)
                    remaining = length
                    while remaining:
                        chunk = self.rfile.read(min(remaining, 1024 * 1024))
                        if not chunk: raise Refused('The connection dropped.', HTTPStatus.BAD_REQUEST)
                        out.write(chunk)
                        remaining -= len(chunk)
                    if final:
                        out.flush()
                        os.fsync(out.fileno())
            finally:
                self.server.busy -= 1
            if final:
                if part.stat().st_size != size:
                    raise Refused('The file arrived incomplete.', HTTPStatus.CONFLICT, offset=part.stat().st_size)
                part.replace(path)
                self.server.ports.added(port, 1, size)
        self.reply(HTTPStatus.OK, dict(offset=offset + length, done=final))


def unpack(archive, status, title):
    """Unpack a zip beside itself. A single top folder is dropped (game downloads are usually wrapped in one)
    unless it is one of the folders ports keep files in (gamedata, assets...); symlinks and clutter are skipped."""
    folder = archive.parent
    with zipfile.ZipFile(archive) as zf:
        members = [info for info in zf.infolist() if not info.is_dir()]
        names = []
        for info in members:
            if (info.external_attr >> 16) & 0o170000 == 0o120000: continue  # symlink: FAT cannot hold it
            parts = clean_parts(info.filename)
            if parts is not None: names.append((info, parts))
        tops = {parts[0] for _info, parts in names if len(parts) > 1}
        if len(tops) == 1 and all(len(parts) > 1 for _info, parts in names) and \
                next(iter(tops)).lower() not in portmaster.FILE_FOLDERS:
            names = [(info, parts[1:]) for info, parts in names]
        total = sum(info.file_size for info, _parts in names)
        if total > free_bytes(folder) - SPARE_BYTES: raise Refused('Not enough space on the card to unpack this.')
        done = 0
        for index, (info, parts) in enumerate(names):
            target = folder.joinpath(*parts)
            if folder.resolve() not in target.resolve().parents: raise Refused('The zip has a path outside its folder.')
            if info.file_size > FAT_LIMIT: raise Refused('%s is bigger than 4 GB.' % parts[-1])
            target.parent.mkdir(parents=True, exist_ok=True)
            pending = target.with_name(target.name + '.rgds-part')
            with zf.open(info) as source, pending.open('wb') as out:
                while True:
                    chunk = source.read(1024 * 1024)
                    if not chunk: break
                    out.write(chunk)
                    done += len(chunk)
                    status.update(
                        state='unpacking',
                        title=title,
                        file=parts[-1],
                        done=done,
                        total=total,
                        files_done=index,
                        files_total=len(names)
                    )
            pending.replace(target)
        return len(names), total


def watch(server, parent, stop):
    """Stop when the launcher has gone, or after IDLE_SECONDS without a request or an upload."""
    while not stop.wait(1):
        if os.getppid() != parent or (not server.busy and time.monotonic() - server.seen > IDLE_SECONDS):
            server.shutdown()
            return


def serve(args):
    folders = portmaster.Folders()
    data = Path(args.data)
    session = data / 'session'
    session.mkdir(parents=True, exist_ok=True)
    status_path, qr_path = session / 'port-files.ini', session / 'port-files.qr'
    selected = ''
    if args.script:
        port = portmaster.details(args.script, folders)
        selected = port['id'] if port else ''
    ip = args.ip or local_address()
    code = '%04d' % secrets.randbelow(10000)
    server = None
    for number in (80, 8080, 8780):
        try:
            server = Server(('', number), Ports(data, folders), None, code, selected)
            break
        except OSError:
            continue
    if server is None:
        write_fields(status_path, dict(state='error', error='Could not open a network port.'), durable=False)
        return 1
    number = server.server_address[1]
    suffix = '' if number == 80 else ':%d' % number
    host = socket.gethostname().split('.')[0]
    address = 'http://%s%s' % (ip, suffix) if ip else ''
    named = 'http://%s.local%s' % (host, suffix) if host and host != 'localhost' else ''
    link = (address or named) + '/?c=' + code + ('#' + selected if selected else '')
    server.status = Status(
        status_path,
        dict(state='waiting', address=address, named=named, code=code, port=selected, link=link, pid=os.getpid())
    )
    pending = qr_path.with_name(qr_path.name + '.pending')
    pending.write_text('\n'.join(qr.text_rows(link)) + '\n')
    pending.replace(qr_path)
    server.status.update(force=True)
    print('port files: serving %s and %s (code %s)' % (address, named, code), file=sys.stderr, flush=True)
    stop = threading.Event()
    threading.Thread(target=watch, args=(server, os.getppid(), stop), daemon=True).start()
    try:
        import signal
        signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown, daemon=True).start())
        server.serve_forever(poll_interval=.5)
    finally:
        stop.set()
        server.server_close()
        status_path.unlink(missing_ok=True)
        qr_path.unlink(missing_ok=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('action', choices=('serve',))
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--app', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--script', help='the port the page opens on')
    parser.add_argument('--ip', default='', help='the Wi-Fi address the launcher knows')
    return serve(parser.parse_args())


if __name__ == '__main__':
    sys.exit(main())
