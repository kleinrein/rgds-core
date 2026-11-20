"""The music player in the launcher (preview backend): off until asked for, its Home tile, playing by touch.

The playing test starts the real engine (build/host/rgds-music, SDL's dummy audio driver) and needs `ffmpeg` to make
its two songs.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
ENGINE = ROOT / 'build/host/rgds-music'


def keys(start, *names):
    return ''.join(
        f'{start + i * 120} key {name}\n{start + i * 120 + 30} key-up {name}\n' for i, name in enumerate(names)
    )


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class MusicUITests(unittest.TestCase):
    def setUp(self):
        # Short, for the engine's socket (a Mac's own temporary folder is too deep for one).
        self.root = Path(tempfile.mkdtemp(prefix='rgmu', dir='/tmp'))
        self.addCleanup(self.cleanup)
        self.prefs = self.root / 'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        self.ui = self.root / 'ui.ini'
        self.ui.write_text('schema=1\nhome_layout=carousel\nbackground=plain\n')
        self.music = self.root / 'music'

    def cleanup(self):
        if ENGINE.exists():
            subprocess.run(
                [str(ENGINE), 'send', '--run', str(self.root / 'data/music/run'), 'quit'], capture_output=True
            )
            time.sleep(.2)
        shutil.rmtree(self.root, ignore_errors=True)

    def run_ui(self, script='', extra=(), seconds=1):
        (self.root / 'input.txt').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.root / 'data'), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input.txt'), '--seconds',
                str(seconds), '--cache-report',
                str(self.root / 'report.json'), '--music-dir',
                str(self.music), *extra
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=seconds + 8
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report.json').read_text())

    def engine_status(self):
        result = subprocess.run(
            [str(ENGINE), 'status', '--run', str(self.root / 'data/music/run')], capture_output=True, text=True
        )
        return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def test_off_by_default_with_no_tile_and_no_engine(self):
        report = self.run_ui(extra=('--home',))
        self.assertEqual(report['home_tiles'], 6)  # Settings, Games, Favourites, Recents, PictoChat, Sound toy
        self.assertEqual(report['music_open'], 0)
        self.assertFalse((self.root / 'data/music').exists())

    def test_the_switch_in_sound_settings_adds_the_tile_and_is_remembered(self):
        # Sound: Volume, Mute, Test microphone, Sounds & music, Vibration, Music player.
        self.run_ui(keys(40, 'Down', 'Down', 'Down', 'Down', 'Down', 'A'), ('--settings', '--section', 'sound'))
        self.assertIn('music_player=1', self.ui.read_text())
        self.assertEqual(self.run_ui(extra=('--home',))['home_tiles'], 7)
        # With it on there is a seventh row, Play with the lid closed; off is written, on is the default.
        self.run_ui(keys(40, *['Down'] * 6, 'A'), ('--settings', '--section', 'sound'))
        self.assertIn('music_lid=0', self.ui.read_text())
        self.run_ui(keys(40, *['Down'] * 5, 'A'), ('--settings', '--section', 'sound'))
        self.assertNotIn('music_player=1', self.ui.read_text())
        self.assertEqual(self.run_ui(extra=('--home',))['home_tiles'], 6)

    def test_the_tile_opens_the_player_and_back_leaves_it(self):
        self.ui.write_text(self.ui.read_text() + 'music_player=1\n')
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        report = self.run_ui(keys(100, 'A'), ('--home',))
        self.assertEqual((report['music_open'], report['music_view']), (1, 0))  # the library: nothing plays yet
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        report = self.run_ui(keys(100, 'A', ']', ']'), ('--home',))
        self.assertEqual(report['music_tab'], 2)  # R, R: Songs, Albums, Artists
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        report = self.run_ui(keys(100, 'A', 'Escape'), ('--home',))
        self.assertEqual(report['music_open'], 0)
        self.assertIn('home 5', (self.root / 'state').read_text())

    @unittest.skipUnless(ENGINE.exists() and shutil.which('ffmpeg'), 'Build the host engine first; ffmpeg required')
    def test_a_tapped_song_plays_and_keeps_playing_after_the_menu_is_gone(self):
        self.music.mkdir()
        for name, frequency in (('a.flac', 330), ('b.flac', 440)):
            subprocess.run(
                [
                    'ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    f'sine=frequency={frequency}:duration=20', '-metadata', f'title=Song {name[0].upper()}',
                    str(self.music / name)
                ],
                check=True
            )
        self.ui.write_text(self.ui.read_text() + 'music_player=1\n')
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        # Open Music, give the engine a moment to find the two songs, tap the second row (the first is Shuffle all).
        report = self.run_ui(keys(100, 'A') + '2200 tap 300 176\n2260 release\n', ('--home',), seconds=4)
        self.assertEqual((report['music_open'], report['music_view']), (1, 1))  # Now playing
        state = self.engine_status()
        self.assertEqual((state['status'], state['index'], state['count']), ('playing', '0', '2'))
        self.assertTrue(state['path'].endswith('a.flac'))
        # The launcher has left; the song has not.
        time.sleep(.5)
        later = self.engine_status()
        self.assertEqual(later['status'], 'playing')
        self.assertGreater(int(later['position_ms']), int(state['position_ms']))
        # The next menu finds it playing and opens on it; Start pauses.
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        report = self.run_ui(keys(300, 'A'), ('--home',), seconds=1)
        self.assertEqual((report['music_open'], report['music_view']), (1, 1))
        # Shut, the player still shows in the status bar while the song plays.
        (self.root / 'state').write_text('1 0 0 ""\nhome 0\n')
        report = self.run_ui('', ('--home',), seconds=2)
        self.assertEqual(report['music_open'], 0)
        self.assertGreaterEqual(report['music_badge'], 0)

    def test_the_players_own_pages_travel_like_the_launchers(self):
        self.ui.write_text(self.ui.read_text() + 'music_player=1\nview_animations=1\nview_transition_style=push\n')
        (self.root / 'state').write_text('1 0 0 ""\nhome 5\n')
        # Open, next tab, previous tab: three transitions, none of them Back.
        report = self.run_ui(keys(100, 'A', ']', '['), ('--home',), seconds=2)
        self.assertEqual(report['music_tab'], 0)
        self.assertEqual((report['view_transitions'], report['back_transitions']), (3, 0))


if __name__ == '__main__':
    unittest.main()
