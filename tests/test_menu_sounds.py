"""Exercise optional sound playback through the actual SDL launcher and PCM bank."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


class SoundBankTests(unittest.TestCase):
    def test_bank_is_distinct_finite_quiet_at_boundaries_and_small(self):
        clips = list((ROOT / 'assets/sounds').glob('*.wav'))
        self.assertEqual(len(clips), 17)
        hashes = set()
        total = 0
        for clip in clips:
            with self.subTest(clip=clip.name), wave.open(str(clip), 'rb') as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 32000))
                self.assertLessEqual(wav.getnframes(), 48000)
                raw = wav.readframes(wav.getnframes())
                samples = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
                self.assertLessEqual(abs(samples[0]), 1)
                self.assertLessEqual(abs(samples[-1]), 1)
                self.assertGreater(max(map(abs, samples)), 15000)
                self.assertLess(max(map(abs, samples)), 23000)
                total += len(raw)
                hashes.add(hashlib.sha256(raw).digest())
        self.assertEqual(len(hashes), 17)
        self.assertLess(total, 420000)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class MenuSoundUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script='', prefs=None, entry=('--settings',), seconds=2, audio='dummy'):
        # These are cue tests: the music voice (on by default) keeps the sink
        # open on its own, so it stays off unless a test asks for it.
        if prefs is not None and 'menu_music=' not in prefs: prefs += 'menu_music=0\n'
        if prefs is not None: (self.root / 'ui.ini').write_text(prefs)
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
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER=audio),
            capture_output=True,
            text=True,
            timeout=8
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report').read_text())

    def test_each_left_and_right_step_plays_in_home_and_game_library(self):
        for entry in (('--home',), ('--library',)):
            r = self.run_ui(
                '100 key Right\n150 key-up Right\n350 key Left\n400 key-up Left\n600 key Right\n650 key-up Right\n',
                'menu_sounds=1\nanimations=0\n', entry
            )
            self.assertEqual(r['sound_counts'][0], 3, entry)

    def test_sounds_are_on_by_default(self):
        r = self.run_ui('100 key A\n250 key Escape\n')
        self.assertEqual((r['menu_sounds'], r['sound_volume']), (1, 55))
        self.assertGreater(r['sound_plays'], 0)

    def test_sounds_off_does_not_load_or_open_audio(self):
        r = self.run_ui('100 key A\n250 key Escape\n', 'menu_sounds=0\nmenu_music=0\n')
        for key in ('menu_sounds', 'sound_reads', 'sound_opens', 'sound_plays', 'sound_bytes'):
            self.assertEqual(r[key], 0, key)

    def test_audio_is_independent_of_both_animation_flags_and_returns_to_idle(self):
        for view_transitions in (0, 1):
            r = self.run_ui(
                '100 key A\n500 key Escape\n', f'animations=0\nview_animations={view_transitions}\nmenu_sounds=1\n'
            )
            self.assertEqual(r['sound_counts'][2], 1)
            self.assertEqual(r['sound_counts'][3], 1)
            self.assertEqual(r['sound_reads'], 17)
            self.assertEqual(r['sound_opens'], 1)
            self.assertFalse(r['sound_active'])
            self.assertEqual(r['view_animation_frames'], 0)
            self.assertEqual(r['render_file_reads'], 0)
            self.assertLess(r['frames'], 30)

    def test_quiet_menu_releases_the_sink_and_reopens_for_the_next_cue(self):
        # SDL keeps a merely paused device feeding silence, holding the shared
        # stream, DAC and a CPU core; the menu closes it after 3 s of quiet.
        r = self.run_ui('100 key A\n4300 key Escape\n', 'animations=0\nmenu_sounds=1\n', seconds=5)
        self.assertEqual(r['sound_counts'][2], 1)
        self.assertEqual(r['sound_counts'][3], 1)
        self.assertEqual(r['sound_opens'], 2)
        self.assertGreaterEqual(r['sound_releases'], 1)
        self.assertEqual(r['sound_reads'], 17)

    def test_touch_enables_persists_volume_and_controller_disables(self):
        entry = ('--settings', '--section', 'sound')
        r = self.run_ui(
            '100 click 180 327\n220 click 180 129\n350 key Down\n450 key Right\n', 'menu_sounds=0\n', entry=entry
        )
        self.assertTrue(r['menu_sounds'])
        self.assertEqual(r['sound_volume'], 60)
        self.assertIn('menu_sounds=1\n', (self.root / 'ui.ini').read_text())
        r = self.run_ui('100 key Up\n220 key Left\n', entry=())
        self.assertFalse(r['menu_sounds'])
        self.assertFalse(r['sound_active'])
        self.assertIn('menu_sounds=0\n', (self.root / 'ui.ini').read_text())

    def test_zero_effect_volume_and_system_mute_are_silent(self):
        r = self.run_ui('100 key A\n', 'menu_sounds=1\nsound_volume=0\n')
        self.assertEqual(r['sound_opens'], 0)
        self.assertEqual(r['sound_plays'], 0)
        (self.root / 'volume.ini').write_text('volume=0\n')
        r = self.run_ui('100 key A\n', 'menu_sounds=1\nsound_volume=55\n')
        self.assertEqual(r['sound_plays'], 0)

    def test_rapid_input_is_bounded_and_never_stacks_a_queue(self):
        script = ''.join(f'{100+i*10} key {"Down" if i%2 else "Up"}\n' for i in range(30))
        r = self.run_ui(script, 'menu_sounds=1\nanimations=0\n')
        self.assertGreater(r['sound_plays'], 0)
        self.assertLessEqual(r['sound_plays'], 7)
        self.assertFalse(r['sound_active'])
        self.assertEqual(r['sound_reads'], 17)

    def test_missing_audio_device_preserves_navigation(self):
        r = self.run_ui('100 key A\n', 'menu_sounds=1\n', audio='unavailable-test-driver')
        self.assertEqual(r['page'], 2)
        self.assertEqual(r['sound_opens'], 0)
        self.assertEqual(r['sound_plays'], 0)

    def test_game_menu_forward_and_back_have_distinct_cues(self):
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key Right\n400 key A\n900 key Escape\n', 'menu_sounds=1\nanimations=0\n',
            ('--game-menu',)
        )
        self.assertEqual(r['page'], 12)
        self.assertEqual(r['sound_counts'][0], 3)
        self.assertEqual(r['sound_counts'][2], 1)
        self.assertEqual(r['sound_counts'][3], 1)

    def test_launch_and_cancel_use_a_flourish_and_back_without_changing_selection(self):
        r = self.run_ui('100 key A\n200 key A\n300 key Escape\n', 'menu_sounds=1\nanimations=0\n', ('--library',))
        self.assertEqual(r['sound_counts'][8], 1)
        self.assertEqual(r['sound_counts'][3], 1)
        self.assertFalse(r['sound_active'])

    def test_favourite_and_unfavourite_get_separate_sounds(self):
        r = self.run_ui('100 key X\n350 key A\n700 key A\n', 'menu_sounds=1\nanimations=0\n', ('--library',))
        self.assertEqual(r['sound_counts'][6], 1)
        self.assertEqual(r['sound_counts'][7], 1)

    def test_opening_an_existing_favourite_does_not_replay_the_favourite_reward(self):
        self.run_ui('100 key X\n350 key A\n900 key Escape\n', 'menu_sounds=1\nanimations=0\n', ('--library',))
        r = self.run_ui('100 key X\n', entry=('--library',))
        self.assertEqual(r['sound_counts'][6], 0)
        self.assertEqual(r['sound_counts'][2], 1)

    def test_invalid_volume_preferences_keep_a_safe_default(self):
        for level in ('nan', '-1', '101', '100extra'):
            r = self.run_ui('', f'menu_sounds=1\nsound_volume={level}\n', seconds=1)
            self.assertEqual(r['sound_volume'], 55)

    def test_microphone_test_closes_sound_output_and_restores_it_after_back(self):
        r = self.run_ui(
            '50 key Down\n100 key Down\n250 key A\n700 key Escape\n', 'menu_sounds=1\n',
            ('--settings', '--section', 'sound')
        )
        self.assertEqual(r['page'], 11)
        self.assertEqual(r['sound_opens'], 2)
        self.assertEqual(r['sound_counts'][2], 0)
        self.assertEqual(r['sound_counts'][3], 1)

    def test_sleep_preview_has_distinct_close_and_unlock_cues_and_releases_audio(self):
        # Sleep now: the sixth row (Dim when idle and Sleep timer in games sit under Menu idle sleep)
        script = '100 key Down\n200 key Down\n300 key Down\n400 key Down\n500 key Down\n600 key Down\n750 key A\n'
        for enabled in (0, 1):
            r = self.run_ui(
                script, f'menu_sounds={enabled}\nanimations=0\nview_animations=0\n',
                ('--settings', '--section', 'power')
            )
            self.assertEqual(r['sound_counts'][15:], [enabled, enabled])
            self.assertEqual(r['sound_opens'], 2 * enabled)
            self.assertFalse(r['sound_active'])


if __name__ == '__main__': unittest.main()
