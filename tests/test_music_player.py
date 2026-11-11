"""The music player: its rules under sanitizers (tests/music_player_test.cpp), and the engine playing real files.

The engine tests need `ffmpeg` to make their files and the host engine (tools/build_music.py); they run it with SDL's
dummy audio driver, which takes sound at the real rate and throws it away.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / 'build/host/rgds-music'


@unittest.skipUnless(shutil.which('clang++'), 'C++ required')
class MusicRulesTests(unittest.TestCase):
    def test_equaliser_analyser_library_queue_and_pictures_under_sanitizers(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'music_test'
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/music_player_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            data = Path(tmp) / 'data'
            data.mkdir()
            run = subprocess.run([str(binary), str(data)], capture_output=True, text=True, timeout=240)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('music player: ok', run.stdout)


def tone(path, frequency, seconds, codec=None, **tags):
    command = [
        'ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
        f'sine=frequency={frequency}:duration={seconds},aformat=channel_layouts=stereo'
    ]
    if codec:
        command += ['-c:a', codec]
    for key, value in tags.items():
        command += ['-metadata', f'{key}={value}']
    subprocess.run(command + [str(path)], check=True)


@unittest.skipUnless(ENGINE.exists() and shutil.which('ffmpeg'), 'Build the host engine first; ffmpeg required')
class MusicEngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.music = Path(cls.temp.name) / 'music'
        album = cls.music / 'Night Drive'
        album.mkdir(parents=True)
        (cls.music / 'Loose').mkdir()
        for number, (name, codec) in enumerate((('01.mp3', 'libmp3lame'), ('02.flac', 'flac'), ('03.m4a', 'aac')), 1):
            tone(
                album / name,
                220 * number,
                3,
                codec,
                title=f'Track {number}',
                artist='The Testers',
                album='Night Drive',
                track=f'{number}/3',
                date='2024'
            )
        subprocess.run(
            [
                'ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=0x3060c0:s=300x200', '-frames:v',
                '1',
                str(album / 'cover.jpg')
            ],
            check=True
        )
        tone(cls.music / 'Loose/untagged.wav', 110, 2)
        (cls.music / 'Loose/broken.mp3').write_text('not music')
        (cls.music / 'Loose/._untagged.wav').write_bytes(b'\0' * 64)  # what macOS leaves on an exFAT card

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        # A socket's path has to be short: not under the (long) temporary folder on a Mac.
        self.run_dir = Path(tempfile.mkdtemp(prefix='rgm', dir='/tmp'))
        self.library = Path(tempfile.mkdtemp(prefix='rgml', dir='/tmp'))
        self.process = None
        self.addCleanup(self.stop)

    def stop(self):
        if self.process and self.process.poll() is None:
            self.send('quit')
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        shutil.rmtree(self.run_dir, ignore_errors=True)
        shutil.rmtree(self.library, ignore_errors=True)

    def serve(self):
        self.process = subprocess.Popen(
            [
                str(ENGINE), 'serve', '--run',
                str(self.run_dir), '--library',
                str(self.library), '--root',
                str(self.music), '--bt-sink',
                str(self.run_dir / 'bt-sink')
            ],
            env=dict(os.environ, SDL_AUDIODRIVER='dummy'),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        self.wait(lambda s: s.get('scanning') == '0' and s.get('library_serial', '0') != '0')

    def send(self, *words):
        subprocess.run([str(ENGINE), 'send', '--run', str(self.run_dir), *map(str, words)], check=True)

    def status(self):
        result = subprocess.run([str(ENGINE), 'status', '--run', str(self.run_dir)], capture_output=True, text=True)
        return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def wait(self, condition, seconds=8):
        deadline = time.time() + seconds
        state = {}
        while time.time() < deadline:
            state = self.status()
            if state and condition(state):
                return state
            time.sleep(.05)
        self.fail(f'engine never got there: {state}')

    def queue(self, *names):
        (self.library / 'queue.tsv').write_text(''.join(f'{self.music / name}\n' for name in names))

    def test_scan_reads_tags_and_covers_and_skips_what_is_not_music(self):
        result = subprocess.run(
            [str(ENGINE), 'scan', '--library',
             str(self.library), '--root',
             str(self.music)],
            capture_output=True,
            text=True
        )
        self.assertEqual(result.stdout.strip(), '4 tracks', result.stderr)
        rows = [line.split('\t') for line in (self.library / 'library.tsv').read_text().splitlines()[1:]]
        by_name = {Path(row[0]).name: row for row in rows}
        self.assertEqual(set(by_name), {'01.mp3', '02.flac', '03.m4a', 'untagged.wav'})
        flac = by_name['02.flac']
        self.assertEqual(flac[4:7], ['Track 2', 'The Testers', 'Night Drive'])
        self.assertEqual((flac[9], flac[11], flac[12]), ('2', '2024', 'FLAC'))
        self.assertTrue(2900 <= int(flac[3]) <= 3100)
        # The folder's cover, once, at the three sizes the screens draw.
        self.assertTrue(flac[17] and flac[17] == by_name['01.mp3'][17])
        self.assertEqual(by_name['untagged.wav'][17], '')
        for size in (44, 88, 200):
            picture = self.library / 'art' / f'{flac[17]}-{size}.bmp'
            self.assertEqual(picture.read_bytes()[:2], b'BM')
            self.assertEqual(int.from_bytes(picture.read_bytes()[18:22], 'little'), size)
        # A second scan opens nothing again and changes nothing.
        before = (self.library / 'library.tsv').read_bytes()
        subprocess.run(
            [str(ENGINE), 'scan', '--library',
             str(self.library), '--root',
             str(self.music)],
            check=True,
            capture_output=True
        )
        self.assertEqual(before, (self.library / 'library.tsv').read_bytes())

    def test_every_format_decodes_to_its_whole_length(self):
        for name, seconds in (
            ('Night Drive/01.mp3', 3), ('Night Drive/02.flac', 3), ('Night Drive/03.m4a', 3), ('Loose/untagged.wav', 2)
        ):
            result = subprocess.run([str(ENGINE), 'probe', str(self.music / name)], capture_output=True, text=True)
            fields = dict(line.split('=', 1) for line in result.stdout.splitlines())
            self.assertTrue(abs(int(fields['decoded_ms']) - seconds * 1000) < 80, (name, fields))
            self.assertTrue(.05 < float(fields['rms']) < .2, (name, fields))  # sound, not silence

    def test_plays_seeks_moves_on_without_a_gap_and_stops_at_the_end(self):
        self.queue('Night Drive/01.mp3', 'Night Drive/02.flac', 'Loose/untagged.wav')
        self.serve()
        self.send('load', 0, 0, 1)
        state = self.wait(lambda s: s['status'] == 'playing' and int(s['position_ms']) > 300)
        self.assertEqual((state['index'], state['count'], state['codec']), ('0', '3', 'MP3'))
        self.assertGreater(float(state['viz_band']), .5)  # the analyser hears the tone
        self.send('seek', 2500)
        state = self.wait(lambda s: 2500 <= int(s['position_ms']) < 2900 or s['index'] == '1')
        self.assertEqual(state['index'], '0')
        # The next track starts by itself, where the first ended.
        state = self.wait(lambda s: s['index'] == '1')
        self.assertEqual((state['status'], state['codec']), ('playing', 'FLAC'))
        self.assertLess(int(state['position_ms']), 600)
        self.send('pause')
        self.wait(lambda s: s['status'] == 'paused')
        time.sleep(.3)  # the fade out still plays a few hundredths of a second
        paused = self.status()
        time.sleep(.3)
        self.assertEqual(self.status()['position_ms'], paused['position_ms'])
        self.send('next')
        self.wait(lambda s: s['index'] == '2' and s['codec'] == 'WAV')
        self.send('play')
        self.send('seek', 1500)
        # After the last track: back at the first, waiting.
        state = self.wait(lambda s: s['index'] == '0' and s['status'] == 'paused')
        self.assertEqual(state['position_ms'], '0')

    def test_repeat_one_stays_and_repeat_all_wraps(self):
        self.queue('Loose/untagged.wav', 'Night Drive/02.flac')
        self.serve()
        self.send('repeat', 2)
        self.send('load', 0, 1500, 1)
        self.wait(lambda s: s['status'] == 'playing' and int(s['position_ms']) >= 1500)
        state = self.wait(lambda s: int(s['track_serial']) >= 2)
        self.assertEqual((state['index'], state['status']), ('0', 'playing'))
        self.send('repeat', 1)
        self.send('jump', 1)
        self.send('seek', 2500)
        state = self.wait(lambda s: s['index'] == '0' and int(s['track_serial']) >= 4)
        self.assertEqual(state['status'], 'playing')

    def test_a_file_that_will_not_open_is_skipped_and_named(self):
        self.queue('Loose/broken.mp3', 'Night Drive/02.flac')
        self.serve()
        self.send('load', 0, 0, 1)
        state = self.wait(lambda s: s['status'] == 'playing' and s['index'] == '1')
        self.assertIn('broken.mp3', state['error'])

    def test_the_session_and_the_equaliser_survive_a_restart(self):
        self.queue('Night Drive/01.mp3', 'Night Drive/02.flac')
        self.serve()
        self.send('eq', 1, 1, 6, 5, 4, 2, 0, 0, 0, 0, 0, 0)
        state = self.wait(lambda s: s['eq_on'] == '1')
        self.assertEqual(state['eq_preset'], '1')
        self.assertTrue(-7.5 < float(state['eq_preamp']) < -6)  # the curve's peak, taken off in front
        self.send('load', 1, 1000, 0)
        self.wait(lambda s: s['index'] == '1' and s['status'] == 'paused')
        self.send('quit')
        self.process.wait(timeout=5)
        self.serve()
        state = self.wait(lambda s: s['status'] == 'paused' and s['index'] == '1')
        self.assertTrue(900 <= int(state['position_ms']) <= 1100, state)
        self.assertEqual((state['eq_on'], state['eq_preset']), ('1', '1'))

    def test_a_headset_that_goes_pauses_the_song(self):
        self.queue('Night Drive/02.flac')
        self.serve()
        (self.run_dir / 'bt-sink').write_text('AA:BB:CC:DD:EE:FF\n')
        self.send('load', 0, 0, 1)
        state = self.wait(lambda s: s['status'] == 'playing' and s['route'] == 'bluetooth')
        self.assertEqual(state['route'], 'bluetooth')
        (self.run_dir / 'bt-sink').unlink()
        state = self.wait(lambda s: s['route'] == 'speaker')
        self.assertEqual(self.wait(lambda s: s['status'] != 'playing')['status'], 'paused')


if __name__ == '__main__':
    unittest.main()
