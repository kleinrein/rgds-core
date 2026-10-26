"""Add game files: the QR code, the upload server on the handheld and the launcher page (docs/PORTMASTER_PLAN.md)."""
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key, read_fields
import portmaster
import port_files
import qr

PAGE_LIBRARY, PAGE_PORT_FILES = 0, 42


def make_port(ports, folder, title, ready, instructions='', subfolders=('gamedata',)):
    ports.mkdir(parents=True, exist_ok=True)
    script = ports / (title + '.sh')
    script.write_text('#!/bin/bash\n# PORTMASTER: %s.zip, %s.sh\n' % (folder, title))
    (ports / folder).mkdir(parents=True)
    for name in subfolders:
        (ports / folder / name).mkdir()
    attr = dict(title=title, rtr=ready, inst=instructions, store=[dict(name='Steam', gameurl='https://example.com/x')])
    (ports / folder / 'port.json').write_text(json.dumps(dict(attr=attr)))
    return script


class QRTests(unittest.TestCase):
    def test_sizes_and_finder_patterns(self):
        for text, size in (('A', 21), ('http://192.168.68.120/?c=4821#catsonmars', 29), ('y' * 200, 57)):
            rows = qr.encode(text)
            self.assertEqual(len(rows), size)
            self.assertTrue(all(len(row) == size for row in rows))
            for x, y in ((0, 0), (size - 7, 0), (0, size - 7)):
                self.assertEqual(rows[y][x:x + 7], [1] * 7)  # finder pattern's top edge
                self.assertEqual(rows[y + 1][x:x + 7], [1, 0, 0, 0, 0, 0, 1])
                self.assertEqual(rows[y + 3][x:x + 7], [1, 0, 1, 1, 1, 0, 1])
            self.assertEqual(rows[size - 8][8], 1)  # the dark module
        self.assertEqual(qr.text_rows('A')[0][:7], '1111111')

    def test_too_long(self):
        with self.assertRaises(ValueError):
            qr.encode('x' * 400)

    def test_reed_solomon_known_block(self):
        # ISO/IEC 18004 Annex I example (version 1-M, "01234567"): its 10 error correction codewords.
        data = [16, 32, 12, 86, 97, 128, 236, 17, 236, 17, 236, 17, 236, 17, 236, 17]
        self.assertEqual(qr._rs_remainder(data, qr._rs_divisor(10)), [165, 36, 212, 193, 237, 54, 199, 135, 44, 85])


class PathTests(unittest.TestCase):
    def test_clean_parts(self):
        self.assertEqual(port_files.clean_parts('Content/a.xnb'), ['Content', 'a.xnb'])
        self.assertEqual(port_files.clean_parts('a\\b\\c.txt'), ['a', 'b', 'c.txt'])
        self.assertEqual(port_files.clean_parts('what?:*.dat'), ['what___.dat'])
        self.assertIsNone(port_files.clean_parts('__MACOSX/x'))
        self.assertIsNone(port_files.clean_parts('dir/._x'))
        self.assertIsNone(port_files.clean_parts('.DS_Store'))
        for bad in ('../x', 'a/../../x', '', '/'):
            with self.assertRaises(port_files.Refused):
                port_files.clean_parts(bad)

    def test_file_target(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            self.assertEqual(portmaster.file_target(folder, 'copy files to ports/x/assets'), 'assets')
            (folder / 'assets').mkdir()
            (folder / 'gamedata').mkdir()
            self.assertEqual(portmaster.file_target(folder, ''), 'gamedata')
            self.assertEqual(portmaster.file_target(folder, 'Copy all game files to /ports/x/assets'), 'assets')
            self.assertEqual(portmaster.file_target(folder, 'into the assets folder please'), 'assets')


class NotesTests(unittest.TestCase):
    def test_notes_mark_ports_that_need_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            folders = portmaster.Folders(root / 'pm')
            celeste = make_port(
                folders.ports, 'celeste', 'Celeste', False, 'Copy all files\nto ports/celeste/gamedata.'
            )
            apotris = make_port(folders.ports, 'apotris', 'Apotris', True, 'Ready to run!', ())
            (folders.ports / 'Loose.sh').write_text('#!/bin/bash\n')  # not an installed port
            self.assertEqual(portmaster.notes(root / 'data', folders), 2)
            celeste_notes = read_fields(root / 'data/library' / game_key(celeste) / 'port.ini')
            self.assertEqual(celeste_notes['needs_files'], '1')
            self.assertEqual(celeste_notes['target'], 'gamedata')
            self.assertEqual(celeste_notes['instructions'], 'Copy all files to ports/celeste/gamedata.')
            self.assertEqual(read_fields(root / 'data/library' / game_key(apotris) / 'port.ini')['needs_files'], '0')
            self.assertEqual(portmaster.notes(root / 'data', folders), 0)  # unchanged: nothing written


class ServerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.pm = self.root / 'pm'
        self.data = self.root / 'data'
        self.script = make_port(self.pm / 'ports', 'celeste', 'Celeste', False, 'Copy to ports/celeste/gamedata.')
        env = dict(os.environ, RGDS_PORTMASTER_ROOT=str(self.pm))
        self.server = subprocess.Popen(
            [
                sys.executable, '-B',
                str(ROOT / 'runtime/port_files.py'), 'serve', '--data',
                str(self.data), '--script',
                str(self.script), '--ip', '127.0.0.1'
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True
        )
        self.addCleanup(self.stop)
        self.status = self.data / 'session/port-files.ini'
        deadline = time.monotonic() + 10
        while not self.status.is_file():
            self.assertIsNone(self.server.poll(), self.server.stderr.read() if self.server.poll() is not None else '')
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        fields = read_fields(self.status)
        self.code = fields['code']
        link = fields['link']
        self.port = int(link.split('://', 1)[1].split('/', 1)[0].rsplit(':', 1)[1]
                        ) if ':' in link.split('://', 1)[1].split('/', 1)[0] else 80
        self.cookie = ''

    def stop(self):
        if self.server.poll() is None:
            self.server.terminate()
            self.server.wait(timeout=10)
        self.server.stderr.close()

    def call(self, method, path, body=b'', headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        headers = dict(headers or {})
        if self.cookie: headers['Cookie'] = self.cookie
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        cookie = response.getheader('Set-Cookie')
        if cookie: self.cookie = cookie.split(';', 1)[0]
        connection.close()
        return response.status, json.loads(data) if data and data[:1] in b'{[' else data

    def pair(self):
        status, body = self.call('POST', '/api/pair', json.dumps(dict(code=self.code)).encode())
        self.assertEqual((status, body), (200, dict(ok=True)))

    def test_qr_and_status_for_the_launcher(self):
        fields = read_fields(self.status)
        self.assertEqual(fields['state'], 'waiting')
        self.assertEqual(len(self.code), 4)
        self.assertTrue(fields['link'].endswith('/?c=%s#celeste' % self.code))
        rows = (self.data / 'session/port-files.qr').read_text().split()
        self.assertEqual(rows, qr.text_rows(fields['link']))

    def test_page_is_open_and_the_api_needs_the_code(self):
        status, page = self.call('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn(b'Add game files', page)
        self.assertEqual(self.call('GET', '/api/ports')[0], 401)
        self.assertEqual(self.call('POST', '/api/pair', b'{"code":"x"}')[0], 403)
        self.pair()
        status, body = self.call('GET', '/api/ports')
        self.assertEqual(status, 200)
        self.assertEqual(body['selected'], 'celeste')
        port = body['ports'][0]
        self.assertEqual((port['id'], port['ready'], port['target']), ('celeste', False, 'gamedata'))
        self.assertNotIn('folder', port)  # no card paths go out

    def test_pairing_is_rate_limited(self):
        for _ in range(5):
            self.assertEqual(self.call('POST', '/api/pair', b'{"code":"x"}')[0], 403)
        self.assertEqual(self.call('POST', '/api/pair', json.dumps(dict(code=self.code)).encode())[0], 429)

    def put(self, path, data, offset, size, final, target='gamedata'):
        query = 'port=celeste&target=%s&path=%s&offset=%d&size=%d&final=%d' % (target, path, offset, size, int(final))
        return self.call('PUT', '/api/upload?' + query, data)

    def test_chunks_resume_and_land_in_the_target(self):
        self.pair()
        blob = os.urandom(300000)
        self.assertEqual(self.put('Content/a.bin', blob[:100000], 0, len(blob), False)[0], 200)
        self.assertEqual(
            self.call('GET', '/api/offset?port=celeste&target=gamedata&path=Content/a.bin')[1], dict(offset=100000)
        )
        status, body = self.put('Content/a.bin', blob[200000:], 200000, len(blob), True)
        self.assertEqual((status, body['offset']), (409, 100000))  # a gap: resume from the handheld's copy
        self.assertEqual(self.put('Content/a.bin', blob[100000:], 100000, len(blob), True)[0], 200)
        target = self.pm / 'ports/celeste/gamedata/Content/a.bin'
        self.assertEqual(target.read_bytes(), blob)
        self.assertFalse(target.with_name('a.bin.rgds-part').exists())
        added = read_fields(self.data / 'library' / game_key(self.script) / 'port-files.ini')
        self.assertEqual((added['files'], added['bytes']), ('1', str(len(blob))))

    def test_refusals(self):
        self.pair()
        self.assertEqual(self.put('../../escape', b'x', 0, 1, True)[0], 400)
        self.assertEqual(self.put('x', b'x', 0, 1, True, target='..')[0], 400)
        self.assertEqual(self.put('x', b'xx', 0, 1, True)[0], 400)  # more than the declared size
        self.assertEqual(self.put('__MACOSX/x', b'x', 0, 1, True)[1], dict(skipped=True))
        self.assertFalse((self.root / 'escape').exists())
        self.assertEqual(sorted(p.name for p in (self.pm / 'ports/celeste/gamedata').iterdir()), [])

    def test_zip_unpacks_without_its_wrapper_folder(self):
        self.pair()
        archive = self.root / 'game.zip'
        with zipfile.ZipFile(archive, 'w') as zf:
            zf.writestr('Celeste/Celeste.exe', b'exe')
            zf.writestr('Celeste/Content/a.txt', b'a')
            zf.writestr('Celeste/.DS_Store', b'junk')
        blob = archive.read_bytes()
        self.assertEqual(self.put('game.zip', blob, 0, len(blob), True)[0], 200)
        status, body = self.call('POST', '/api/unpack?port=celeste&target=gamedata&path=game.zip')
        self.assertEqual((status, body['files']), (200, 2))
        folder = self.pm / 'ports/celeste/gamedata'
        self.assertEqual((folder / 'Celeste.exe').read_bytes(), b'exe')
        self.assertEqual((folder / 'Content/a.txt').read_bytes(), b'a')
        self.assertFalse((folder / 'game.zip').exists())
        self.assertFalse((folder / '.DS_Store').exists())

    def test_stops_and_cleans_up(self):
        self.server.terminate()
        self.server.wait(timeout=10)
        self.assertFalse(self.status.exists())
        self.assertFalse((self.data / 'session/port-files.qr').exists())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class PageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.nds = self.root / 'nds'
        self.nds.mkdir()
        (self.nds / 'Alpha.nds').write_bytes(bytes(512))
        self.folders = portmaster.Folders(self.root / 'pm')
        self.script = make_port(self.folders.ports, 'celeste', 'Celeste', False, 'Copy to ports/celeste/gamedata.')
        portmaster.notes(self.data, self.folders)

    def run_ui(self, script):
        (self.data / 'ui.ini').write_text('schema=1\nhome_layout=carousel\nbackground=plain\nshow_systems=1\n')
        (self.data / 'state').write_text('1 0 4 ""\nhome 1\n')
        (self.data / 'input').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--rom-dir',
                str(self.nds), '--system-dir', 'ports=' + str(self.folders.ports), '--input-script',
                str(self.data / 'input'), '--state',
                str(self.data / 'state'), '--seconds', '2', '--cache-report',
                str(self.data / 'report.json')
            ],
            cwd=ROOT,
            env=dict(
                os.environ,
                SDL_VIDEODRIVER='dummy',
                SDL_AUDIODRIVER='dummy',
                RGDS_PORTMASTER_ROOT=str(self.folders.root)
            ),
            capture_output=True,
            text=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.data / 'report.json').read_text())

    def test_a_port_that_needs_files_opens_the_page_and_back_returns(self):
        self.assertEqual(self.run_ui('100 key A\n300 key Right\n500 key A\n900 key A\n')['page'], PAGE_PORT_FILES)
        self.assertEqual(
            self.run_ui('100 key A\n300 key Right\n500 key A\n900 key A\n1300 key B\n')['page'], PAGE_LIBRARY
        )

    def test_play_anyway_is_remembered_and_files_added_skip_the_page(self):
        record = self.data / 'library' / game_key(self.script) / 'port-files.ini'
        self.run_ui('100 key A\n300 key Right\n500 key A\n900 key A\n1300 key A\n')
        self.assertEqual(read_fields(record).get('skipped'), '1')
        record.write_text('files=3\n')
        self.assertEqual(self.run_ui('100 key A\n300 key Right\n500 key A\n900 key A\n')['page'], PAGE_LIBRARY)


if __name__ == '__main__':
    unittest.main()
