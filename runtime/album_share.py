#!/usr/bin/env python3
"""The Album on a phone: screenshots and photos to look at and save, over Wi-Fi.

While the launcher's Send to phone sheet is open (Album > Y), this serves a small page on the handheld. A phone
scans the QR code on the upper screen (the link carries the four-digit code), a computer opens the address and
types the code once. The page shows the pictures by game, newest first; each one downloads on its own, or all of
them in one zip. It only reads: nothing on the handheld can be changed from the page.

Pairing, the status file and stopping are port_files.py's: session/album-share.ini (address, code, how many
downloads) and session/album-share.qr (the QR as rows of 0 and 1) for the launcher; the server stops when the
launcher closes the sheet, when the launcher goes away, or after a long idle time.
"""
import argparse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import socket
import socketserver
import sys
import threading
import time
from urllib.parse import parse_qs, quote, unquote, urlsplit
import zipfile

sys.dont_write_bytecode = True
from fields import write_fields
from port_files import IDLE_SECONDS, Refused, Status, local_address
import qr
import screenshots

PAGE = Path(__file__).resolve().parent / 'album_files/album.html'


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, data, status, code):
        super().__init__(address, Handler)
        self.data = Path(data)
        self.status = status
        self.code = code
        self.tokens = set()
        self.failures = []
        self.seen = time.monotonic()
        self.busy = 0
        self.downloads = 0
        self.lock = threading.Lock()

    def server_bind(self):
        # As port_files: no reverse lookup of our own name (it can block for a minute offline).
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = 'localhost', self.server_address[1]

    def counted(self):
        with self.lock:
            self.downloads += 1
            self.status.update(force=True, state='connected', downloads=self.downloads)


def listing(data):
    """The album for the page: one entry a picture, newest first, by game."""
    pictures = []
    for path, key, title, system, date in screenshots.entries(data):
        stem = path.stem
        pictures.append(
            dict(
                id=key + '/' + path.name,
                game=key,
                title='Menus' if key == screenshots.LAUNCHER else (title or key),
                system=system,
                date='%s-%s-%s' % (date[:4], date[4:6], date[6:8]) if len(date) == 8 else date,
                time='%s:%s' % (stem[9:11], stem[11:13]) if len(stem) >= 15 else '',
                bytes=path.stat().st_size if path.exists() else 0
            )
        )
    return pictures


def picture(data, ident):
    """An album picture by its id (<game key>/<name>.png), refused outside the album."""
    key, _, name = unquote(ident).partition('/')
    if not key or not name or '/' in name or name.startswith('.'):
        raise Refused('No such picture.', HTTPStatus.NOT_FOUND)
    try:
        path = screenshots.inside_album(data, screenshots.album_root(data) / key / name)
    except ValueError:
        raise Refused('No such picture.', HTTPStatus.NOT_FOUND) from None
    if not path.is_file(): raise Refused('No such picture.', HTTPStatus.NOT_FOUND)
    return path


def download_name(data, path):
    """What a saved picture is called on the phone: the game and the moment, not our folder key."""
    info = screenshots.read_fields(path.parent / 'album.ini')
    title = 'Menus' if path.parent.name == screenshots.LAUNCHER else info.get('title') or path.parent.name
    safe = ''.join(c if c.isalnum() or c in ' -_' else ' ' for c in title).strip() or 'RG DS'
    return '%s %s.png' % (' '.join(safe.split()), path.stem)


class Handler(BaseHTTPRequestHandler):
    server_version = 'RG-DS-Core'
    protocol_version = 'HTTP/1.1'

    def log_message(self, format, *args):  # noqa: A002 - BaseHTTPRequestHandler's name
        print('album share: ' + self.address_string() + ' ' + format % args, file=sys.stderr, flush=True)

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

    def authorised(self):
        for part in self.headers.get('Cookie', '').split(';'):
            name, _, value = part.strip().partition('=')
            if name == 'rgds' and value in self.server.tokens: return True
        return False

    def handle_one_request(self):
        self.server.seen = time.monotonic()
        super().handle_one_request()

    def run(self, action):
        try:
            action()
        except Refused as refused:
            self.close_connection = True
            self.reply(refused.status, dict(error=str(refused), **refused.extra))
        except (OSError, ValueError) as error:
            self.close_connection = True
            self.reply(HTTPStatus.INTERNAL_SERVER_ERROR, dict(error=str(error)))

    def do_GET(self):  # noqa: N802 - http.server's name
        self.run(self.get)

    def do_HEAD(self):  # noqa: N802
        self.run(self.get)

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
                        'Content-Security-Policy',
                        "default-src 'self'; img-src 'self' blob:; style-src 'unsafe-inline'; "
                        "script-src 'unsafe-inline'; connect-src 'self'"
                    )
                ]
            )
            return
        if not self.authorised(): raise Refused('Type the code shown on the handheld.', HTTPStatus.UNAUTHORIZED)
        data = self.server.data
        if route == '/api/album':
            self.reply(HTTPStatus.OK, dict(pictures=listing(data), device=socket.gethostname().split('.')[0]))
        elif route.startswith('/api/picture/'):
            path = picture(data, route[len('/api/picture/'):])
            query = {key: value[-1] for key, value in parse_qs(urlsplit(self.path).query).items()}
            headers = [('Cache-Control', 'private, max-age=3600')]
            if query.get('download') == '1':
                name = download_name(data, path)
                headers.append(('Content-Disposition', "attachment; filename*=UTF-8''" + quote(name)))
                self.server.counted()
            self.reply(HTTPStatus.OK, path.read_bytes(), 'image/png', headers=headers)
        elif route == '/api/all.zip':
            self.send_zip(data)
        else:
            raise Refused('Not here.', HTTPStatus.NOT_FOUND)

    def send_zip(self, data):
        """Every picture in one zip, folders by game. PNGs are compressed already: stored, streamed as it is built."""
        entries = screenshots.entries(data)
        if not entries: raise Refused('The album is empty.', HTTPStatus.NOT_FOUND)
        self.server.busy += 1
        try:
            self.send_response(HTTPStatus.OK)
            self.send_header('Content-Type', 'application/zip')
            self.send_header('Content-Disposition', "attachment; filename*=UTF-8''" + quote('RG DS Album.zip'))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            if self.command == 'HEAD': return
            used = set()
            with zipfile.ZipFile(_Sink(self.wfile), 'w', zipfile.ZIP_STORED) as archive:
                for path, _key, _title, _system, _date in entries:
                    name = download_name(data, path)
                    folder = name.rsplit(' ', 1)[0] if ' ' in name else 'RG DS'
                    arcname = folder + '/' + name
                    if arcname in used: continue
                    used.add(arcname)
                    archive.write(path, arcname)
            self.server.counted()
        finally:
            self.server.busy -= 1

    def post(self):
        route = urlsplit(self.path).path
        if route != '/api/pair': raise Refused('Not here.', HTTPStatus.NOT_FOUND)
        length = int(self.headers.get('Content-Length') or 0)
        if length < 0 or length > 256: raise Refused('That request is too big.', HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        body = self.rfile.read(length)
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


class _Sink:
    """A write-only, unseekable stream for zipfile (it then writes data descriptors instead of seeking back)."""
    def __init__(self, stream):
        self.stream, self.offset = stream, 0

    def write(self, data):
        self.stream.write(data)
        self.offset += len(data)
        return len(data)

    def tell(self):
        return self.offset

    def flush(self):
        self.stream.flush()


def watch(server, parent, stop):
    while not stop.wait(1):
        if os.getppid() != parent or (not server.busy and time.monotonic() - server.seen > IDLE_SECONDS):
            server.shutdown()
            return


def serve(args):
    data = Path(args.data)
    session = data / 'session'
    session.mkdir(parents=True, exist_ok=True)
    status_path, qr_path = session / 'album-share.ini', session / 'album-share.qr'
    ip = args.ip or local_address()
    code = '%04d' % secrets.randbelow(10000)
    server = None
    for number in args.ports:
        try:
            server = Server(('', number), data, None, code)
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
    link = (address or named) + '/?c=' + code
    server.status = Status(
        status_path, dict(state='waiting', address=address, named=named, code=code, link=link, pid=os.getpid())
    )
    pending = qr_path.with_name(qr_path.name + '.pending')
    pending.write_text('\n'.join(qr.text_rows(link)) + '\n')
    pending.replace(qr_path)
    server.status.update(force=True)
    print('album share: serving %s and %s (code %s)' % (address, named, code), file=sys.stderr, flush=True)
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
    parser.add_argument('--ip', default='', help='the Wi-Fi address the launcher knows')
    parser.add_argument('--ports', type=lambda v: [int(p) for p in v.split(',')], default=[80, 8080, 8780])
    return serve(parser.parse_args())


if __name__ == '__main__':
    sys.exit(main())
