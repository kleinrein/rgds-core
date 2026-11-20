"""Album > Send to phone (runtime/album_share.py): pairing, the listing, downloads and the zip, and nothing outside
the album, against a real server on a loopback port."""
import http.client
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
import album_share  # noqa: E402
from port_files import Status  # noqa: E402
import screenshots  # noqa: E402


def bmp(path, colour):
    r, g, b = colour
    pixels = bytes((b, g, r)) * (32 * 24)
    head = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    head += struct.pack('<IiiHHIIiiII', 40, 32, -24, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    Path(path).write_bytes(head + pixels)


class AlbumShareTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        bmp(self.data / 'a.bmp', (200, 10, 10))
        self.first = screenshots.save(self.data, self.data / 'a.bmp', key='k1', title='Kart: Deluxe/2', now=1)
        screenshots.save(self.data, self.data / 'a.bmp', key=screenshots.LAUNCHER, now=2)
        (self.data / 'session').mkdir()
        (self.data / 'session/secret.ini').write_text('nope')
        status = Status(self.data / 'session/album-share.ini', {})
        self.server = album_share.Server(('127.0.0.1', 0), self.data, status, '1234')
        threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .05}, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.cookie = ''

    def request(self, method, path, body=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_address[1], timeout=5)
        headers = {'Cookie': self.cookie} if self.cookie else {}
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response, data

    def pair(self, code='1234'):
        response, _ = self.request('POST', '/api/pair', json.dumps({'code': code}))
        if response.status == 200: self.cookie = response.getheader('Set-Cookie').split(';')[0]
        return response.status

    def test_nothing_without_the_code(self):
        self.assertEqual(self.request('GET', '/')[0].status, 200)  # the page itself asks for the code
        self.assertEqual(self.request('GET', '/api/album')[0].status, 401)
        self.assertEqual(self.pair('9999'), 403)
        self.assertEqual(self.request('GET', '/api/all.zip')[0].status, 401)

    def test_listing_downloads_and_zip(self):
        self.assertEqual(self.pair(), 200)
        response, body = self.request('GET', '/api/album')
        pictures = json.loads(body)['pictures']
        self.assertEqual([p['title'] for p in pictures], ['Menus', 'Kart: Deluxe/2'])
        ident = next(p['id'] for p in pictures if p['game'] == 'k1')
        response, png = self.request('GET', '/api/picture/' + ident + '?download=1')
        self.assertEqual(response.status, 200)
        self.assertEqual(png, self.first.read_bytes())
        # A safe file name on the phone: the game's title, no slashes or colons.
        self.assertIn("filename*=UTF-8''Kart%20Deluxe%202%20", response.getheader('Content-Disposition'))
        response, archive = self.request('GET', '/api/all.zip')
        names = zipfile.ZipFile(io.BytesIO(archive)).namelist()
        self.assertEqual(len(names), 2)
        self.assertTrue(any(name.startswith('Kart Deluxe 2/') for name in names))

    def test_only_album_pictures(self):
        self.pair()
        for path in (
            '/api/picture/..%2Fsession%2Fsecret.ini', '/api/picture/k1/..%2F..%2Fsession%2Fsecret.ini',
            '/api/picture/k1/album.ini', '/api/picture/k1/missing.png', '/api/other'
        ):
            self.assertEqual(self.request('GET', path)[0].status, 404, path)


if __name__ == '__main__':
    unittest.main()
