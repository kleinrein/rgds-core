"""Optional menu music: the ADPCM track, its preferences and the launcher's playback state."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
TRACK = ROOT / 'assets/music/menu.wav'


class MusicTrackTests(unittest.TestCase):
    def test_track_is_mono_ima_adpcm_at_32khz_and_under_90_seconds(self):
        data = TRACK.read_bytes()
        self.assertEqual((data[:4], data[8:12]), (b'RIFF', b'WAVE'))
        at = 12
        fmt = None
        size = 0
        while at + 8 <= len(data):
            chunk, length = data[at:at + 4], struct.unpack('<I', data[at + 4:at + 8])[0]
            if chunk == b'fmt ': fmt = struct.unpack('<HHIIHH', data[at + 8:at + 24])
            if chunk == b'data': size = length
            at += 8 + length + (length & 1)
        self.assertEqual((fmt[0], fmt[1], fmt[2], fmt[5]), (0x11, 1, 32000, 4))
        seconds = size // fmt[4] * ((fmt[4] - 4) * 2 + 1) / 32000
        self.assertGreater(seconds, 20)
        self.assertLess(seconds, 90)
        self.assertLess(len(data), 1_600_000)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class MenuMusicUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script='', prefs='', entry=('--home',), seconds=2):
        (self.root / 'ui.ini').write_text('schema=1\nanimations=0\n' + prefs)
        (self.root / 'input').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.root), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input'), '--cache-report',
                str(self.root / 'report'), '--seconds',
                str(seconds), *entry
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=12
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report').read_text())

    def test_music_is_on_by_default(self):
        r = self.run_ui('100 key Right\n')
        self.assertEqual((r['menu_music'], r['music_reads'], r['music_volume']), (1, 1, 40))
        self.assertTrue(r['music_playing'])

    def test_music_off_reads_nothing_and_opens_no_sink(self):
        r = self.run_ui('100 key Right\n', 'menu_music=0\nmenu_sounds=0\n')
        self.assertEqual((r['menu_music'], r['music_reads'], r['music_playing'], r['sound_opens']), (0, 0, 0, 0))

    def test_music_on_decodes_once_and_plays_without_the_cue_bank(self):
        r = self.run_ui('100 key Right\n400 key Left\n', 'menu_music=1\nmenu_sounds=0\nmusic_volume=40\n')
        self.assertEqual(r['music_reads'], 1)
        self.assertGreater(r['music_samples'], 32000 * 20)
        self.assertTrue(r['music_playing'])
        self.assertTrue(r['sound_open_now'])
        self.assertEqual(r['sound_reads'], 0, 'cues stay off; only the track is read')

    def test_invalid_music_volume_keeps_the_default(self):
        for level in ('nan', '-5', '120'):
            r = self.run_ui('', f'menu_music=1\nmusic_volume={level}\n', seconds=1)
            self.assertEqual(r['music_volume'], 40)

    def test_sounds_and_music_page_toggles_music_and_saves_it(self):
        # Sound -> Sounds & music (row 2) -> Menu music (row 2) -> A toggles it on.
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key A\n500 key Down\n600 key Down\n700 key A\n',
            'menu_music=0\n', ('--settings', '--section', 'sound'),
            seconds=3
        )
        self.assertEqual(r['menu_music'], 1)
        self.assertIn('menu_music=1\n', (self.root / 'ui.ini').read_text())
        self.assertEqual(r['music_reads'], 1)


if __name__ == '__main__': unittest.main()
