"""Media in the launcher (src/launcher/media_shelf.hpp): Music, Videos and Books as one level with section pills.

Needs the host launcher (build/host/rgds-launcher), rgds-media (python3 tools/build_media.py host) for its --index
mode, and ffmpeg for the test film.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_media import make_film, make_pdf  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
MEDIA = ROOT / 'build/media/host/rgds-media'


def keys(start, *names):
    return ''.join(
        f'{start + i * 150} key {name}\n{start + i * 150 + 30} key-up {name}\n' for i, name in enumerate(names)
    )


@unittest.skipUnless(
    BINARY.exists() and MEDIA.exists() and shutil.which('ffmpeg'), 'Build the launcher and rgds-media; needs ffmpeg'
)
class MediaLevelTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='rgme', dir='/tmp'))
        self.addCleanup(shutil.rmtree, self.root, True)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')
        (self.root / 'ui.ini').write_text(
            'schema=1\nhome_layout=carousel\nbackground=plain\nview_animations=1\nview_transition_style=push\n'
        )
        shelves = self.root / 'shelves'
        (shelves / 'videos/Show').mkdir(parents=True)
        (shelves / 'books').mkdir(parents=True)
        make_film(shelves / 'videos/Film.mkv')
        shutil.copy(shelves / 'videos/Film.mkv', shelves / 'videos/Show/Show.S01E01.mkv')
        make_pdf(shelves / 'books/Manual.pdf')
        self.state = self.root / 'data/media'

    def run_ui(self, script='', seconds=3, media=MEDIA):
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')  # the Media tile
        (self.root / 'input.txt').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--home', '--data',
                str(self.root / 'data'), '--preferences',
                str(self.root / 'preferences.ini'), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input.txt'), '--seconds',
                str(seconds), '--cache-report',
                str(self.root / 'report.json')
            ],
            cwd=ROOT,
            env=dict(
                os.environ,
                SDL_VIDEODRIVER='dummy',
                SDL_AUDIODRIVER='dummy',
                RGDS_MEDIA_BINARY=str(media),
                RGDS_MEDIA_ROOT=str(self.root / 'shelves')
            ),
            capture_output=True,
            text=True,
            timeout=seconds + 30
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report.json').read_text())

    def test_the_tile_opens_the_shelves_and_the_section_is_remembered(self):
        # Without the music player: Videos and Books only. The index lists both shelves and makes posters.
        report = self.run_ui(keys(100, 'A'))
        self.assertEqual((report['music_open'], report['media_section']), (1, 1))
        index = (self.state / 'index.tsv').read_text()
        self.assertIn('Film', index)
        self.assertIn('Manual', index)
        self.assertTrue(list((self.state / 'posters').glob('*.bmp')))
        self.assertEqual(report['media_shelf_items'], 2)  # the Show folder and the film
        # R2 (e) steps to Books, and the next open starts there.
        report = self.run_ui(keys(100, 'A') + keys(900, 'e'))
        self.assertEqual(report['media_section'], 2)
        self.assertIn('section=2', (self.state / 'launcher.ini').read_text())
        report = self.run_ui(keys(100, 'A'))
        self.assertEqual((report['media_section'], report['media_shelf_items']), (2, 1))

    def test_a_folder_opens_and_back_leaves_it_then_the_level(self):
        (self.state).mkdir(parents=True)
        (self.state / 'launcher.ini').write_text('section=1\n')
        # The folder comes first (names in order: Show is a folder); A opens it, travelling forward.
        report = self.run_ui(keys(100, 'A') + keys(1500, 'A'))
        self.assertEqual((report['media_folder_depth'], report['media_shelf_items']), (1, 1))
        report = self.run_ui(keys(100, 'A') + keys(1500, 'A', 'b'))
        self.assertEqual((report['music_open'], report['media_folder_depth']), (1, 0))
        report = self.run_ui(keys(100, 'A') + keys(1500, 'A', 'b', 'b'))
        self.assertEqual(report['music_open'], 0)
        self.assertGreaterEqual(report['back_transitions'], 2)

    def test_an_older_rgds_media_opens_the_old_way(self):
        # One without --index (a launcher ahead of its rgds-media): the tile starts it instead of the shelves.
        old = self.root / 'old-rgds-media'
        old.write_text('#!/bin/sh\nsleep 30\n')
        old.chmod(0o755)
        report = self.run_ui(keys(100, 'A'), media=old)
        self.assertEqual(report['music_open'], 0)
        self.assertFalse((self.state / 'index.tsv').exists())

    def test_sections_change_in_place(self):
        (self.root / 'ui.ini').write_text((self.root / 'ui.ini').read_text() + 'music_player=1\n')
        (self.state).mkdir(parents=True)
        (self.state / 'launcher.ini').write_text('section=0\n')
        # Music, then R2 to Videos, R2 to Books, R2 round to Music: four crossfades in place, none of them Back.
        report = self.run_ui(keys(100, 'A') + keys(900, 'e', 'e', 'e'), seconds=4)
        self.assertEqual(report['media_section'], 0)
        self.assertEqual((report['view_transitions'], report['back_transitions']), (4, 0))


if __name__ == '__main__':
    unittest.main()
