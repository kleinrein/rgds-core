"""RG DS Media (play/src/media): the engines on made-up files, and the program driven by a script to screenshots.

Needs build/media/host (python3 tools/build_media.py host) and ffmpeg on the PATH for the test film.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'build/media/host/media-tests'
MEDIA = ROOT / 'build/media/host/rgds-media'


def make_film(path):
    meta = path.with_suffix('.meta')
    meta.write_text(
        ';FFMETADATA1\ntitle=Test Film\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=4000\ntitle=Intro\n'
        '[CHAPTER]\nTIMEBASE=1/1000\nSTART=4000\nEND=10000\ntitle=The rest\n'
    )
    srt = path.with_suffix('.in.srt')
    srt.write_text(
        '1\n00:00:01,000 --> 00:00:04,000\nHello from the test\n\n2\n00:00:06,000 --> 00:00:08,000\n'
        'Second line\n'
    )
    subprocess.run(
        [
            'ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=size=320x240:rate=25:duration=10', '-f',
            'lavfi', '-i', 'sine=frequency=440:duration=10', '-i',
            str(meta), '-i',
            str(srt), '-map', '0:v', '-map', '1:a', '-map', '3:s', '-map_metadata', '2', '-map_chapters', '2', '-c:v',
            'libx264', '-g', '50', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-c:s', 'srt',
            str(path)
        ],
        check=True
    )


def make_pdf(path, pages=3):
    objs = []
    kids = ' '.join(f'{3 + i * 2} 0 R' for i in range(pages))
    objs.append('<< /Type /Catalog /Pages 2 0 R >>')
    objs.append(f'<< /Type /Pages /Kids [{kids}] /Count {pages} >>')
    font = 3 + pages * 2
    for i in range(pages):
        objs.append(
            f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 420 595] /Resources << /Font << /F1 {font} 0 R >> >>'
            f' /Contents {4 + i * 2} 0 R >>'
        )
        text = f'BT /F1 28 Tf 60 500 Td (Page {i + 1}) Tj ET 0 0 1 rg 60 100 300 200 re f'
        objs.append(f'<< /Length {len(text)} >>\nstream\n{text}\nendstream')
    objs.append('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
    out = b'%PDF-1.4\n'
    offsets = []
    for n, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += f'{n} 0 obj\n{body}\nendobj\n'.encode()
    xref = len(out)
    out += f'xref\n0 {len(objs) + 1}\n0000000000 65535 f \n'.encode()
    for o in offsets:
        out += f'{o:010d} 00000 n \n'.encode()
    out += f'trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    path.write_bytes(out)


def make_epub(path):
    words = ('The quick handheld folded its two screens and read on. ' * 40).strip()
    chapters = [(f'c{i}.xhtml', f'Chapter {i}', ''.join(f'<p>{words}</p>' for _ in range(6))) for i in (1, 2, 3)]
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
        z.writestr(
            'META-INF/container.xml',
            '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:'
            'container"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/>'
            '</rootfiles></container>'
        )
        manifest = ''.join(
            f'<item id="{n[:-6]}" href="{n}" media-type="application/xhtml+xml"/>' for n, _, _ in chapters
        )
        spine = ''.join(f'<itemref idref="{n[:-6]}"/>' for n, _, _ in chapters)
        z.writestr(
            'book.opf', '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
            'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Test Book'
            '</dc:title><dc:creator>RG DS</dc:creator><dc:identifier id="id">x</dc:identifier></metadata>'
            f'<manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
            f'{manifest}</manifest><spine toc="ncx">{spine}</spine></package>'
        )
        points = ''.join(
            f'<navPoint id="p{i}" playOrder="{i}"><navLabel><text>{t}</text></navLabel>'
            f'<content src="{n}"/></navPoint>' for i, (n, t, _) in enumerate(chapters, 1)
        )
        z.writestr(
            'toc.ncx', '<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
            f'<head/><docTitle><text>Test Book</text></docTitle><navMap>{points}</navMap></ncx>'
        )
        nav = ''.join(f'<li><a href="{n}">{t}</a></li>' for n, t, _ in chapters)
        z.writestr(
            'nav.xhtml', '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" '
            'xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol>' + nav +
            '</ol></nav></body></html>'
        )
        for n, t, body in chapters:
            z.writestr(
                n, f'<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>{t}</title>'
                f'</head><body><h1>{t}</h1>{body}</body></html>'
            )


def png(w, h, colour, band=None):
    # A page: paper with a band of ink across its middle, so a render is never one colour.
    rows = []
    for y in range(h):
        c = band if band and h // 3 < y < h // 2 else colour
        rows.append(b'\0' + bytes(c) * w)
    raw = b''.join(rows)

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

    return (
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    )


def make_cbz(path, pages=6):
    with zipfile.ZipFile(path, 'w') as z:
        for i in range(pages):
            z.writestr(f'page{i + 1:02d}.png', png(300, 440, (250, 250, 250), band=(30 * i % 255, 60, 160)))
        # A wide double page at the end.
        z.writestr('page99.png', png(600, 440, (40, 120, 200), band=(250, 200, 40)))


@unittest.skipUnless(TESTS.exists() and MEDIA.exists(), 'Build first: python3 tools/build_media.py host')
@unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg not found')
class MediaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='rgmedia'))
        (cls.dir / 'videos/Series').mkdir(parents=True)
        (cls.dir / 'books').mkdir()
        cls.film = cls.dir / 'videos/Test.Film.2026.1080p.mkv'
        make_film(cls.film)
        shutil.copy(cls.film, cls.dir / 'videos/Series/Show.s01e01.mkv')
        shutil.copy(cls.film, cls.dir / 'videos/Series/Show.s01e02.mkv')
        cls.pdf = cls.dir / 'books/Paper.pdf'
        make_pdf(cls.pdf)
        cls.epub = cls.dir / 'books/Novel.epub'
        make_epub(cls.epub)
        cls.cbz = cls.dir / 'books/Comic.cbz'
        make_cbz(cls.cbz)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_tests(self, *args):
        r = subprocess.run([str(TESTS), *map(str, args)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_video_engine(self):
        self.run_tests('video', self.film)

    def test_pdf(self):
        self.run_tests('book', self.pdf, 3)

    def test_epub(self):
        self.run_tests('book', self.epub, 0)

    def test_cbz(self):
        self.run_tests('book', self.cbz, 7)

    def test_names(self):
        self.run_tests('names')

    def drive(self, script, args=(), seconds=30):
        shots = self.dir / 'shots'
        state = Path(tempfile.mkdtemp(prefix='rgms', dir=self.dir))
        (state / 'script.txt').write_text(script)
        env = dict(os.environ, SDL_AUDIODRIVER='dummy', RGDS_MEDIA_BATTERY='82', RGDS_MEDIA_WINDOW='1')
        r = subprocess.run(
            [
                str(MEDIA), '--videos', self.dir / 'videos', '--books', self.dir / 'books', '--state', state, '--fonts',
                ROOT / 'assets/fonts', '--locales', ROOT / 'assets/locales', '--shots', shots, '--script',
                state / 'script.txt', '--quit-after',
                str(seconds * 1000), *map(str, args)
            ],
            capture_output=True,
            text=True,
            timeout=seconds + 30,
            env=env,
            cwd=ROOT
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        return shots, state, r.stderr

    def test_library_film_and_back(self):
        shots, state, log = self.drive(
            '1500 shot library\n1550 key Right\n1600 key A\n3500 shot film\n3600 key Right\n'
            '4400 shot film_seek\n4500 key X\n5000 shot sound_sheet\n5100 key B\n'
            '5400 key B\n6200 shot back\n6300 quit\n'
        )
        for name in ('library', 'film', 'film_seek', 'sound_sheet', 'back'):
            self.assertTrue((shots / f'{name}.png').stat().st_size > 5000, name)
        # The film's place was kept.
        places = (state / 'places.tsv').read_text()
        self.assertTrue(places.strip(), log)

    def test_books_shelf_and_reader(self):
        shots, state, log = self.drive(
            '1500 key R\n2500 shot books\n2600 key A\n4000 shot reading\n'
            '4100 key Down\n4800 shot turned\n4900 key X\n5400 shot menu\n5500 quit\n'
        )
        for name in ('books', 'reading', 'turned', 'menu'):
            self.assertTrue((shots / f'{name}.png').stat().st_size > 5000, name)


class RuntimePlanTest(unittest.TestCase):
    """runtime/media.py: the command the Media tile runs, with the profile's places and the launcher's look."""
    def test_plan(self):
        with tempfile.TemporaryDirectory(prefix='rgmp') as tmp:
            app, data = Path(tmp) / 'app', Path(tmp) / 'data'
            binary = app / 'emulators/rgds-media/rgds-media'
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\n')
            binary.chmod(0o755)
            data.mkdir()
            env = dict(
                os.environ,
                RGDS_MEDIA_STATE=str(data / 'profiles/kid/media'),
                RGDS_MEDIA_PREFERENCES=str(data / 'profiles/kid/preferences.ini'),
                RGDS_MEDIA_MUSIC_TAB='1'
            )
            r = subprocess.run(
                ['python3', str(ROOT / 'runtime/media.py'), 'plan', '--app',
                 str(app), '--data',
                 str(data)],
                capture_output=True,
                text=True,
                env=env,
                cwd=ROOT / 'runtime'
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            import json
            command = json.loads(r.stdout)['command']
            self.assertEqual(command[0], str(binary.resolve()))
            self.assertEqual(command[command.index('--state') + 1], str(data / 'profiles/kid/media'))
            self.assertEqual(command[command.index('--preferences') + 1], str(data / 'profiles/kid/preferences.ini'))
            self.assertIn('--music-tab', command)
            self.assertIn('--books', command)


if __name__ == '__main__':
    unittest.main()
