"""Shared lid audio policy, sink lifetime and actual SDL queued playback."""
import contextlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import sleep_sounds
from fields import write_fields


class SleepSoundTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        self.volume = Mock()
        self.volume.saved.return_value = 55
        self.volume.apply.return_value = 55
        patcher = patch('sleep_sounds.Volume', return_value=self.volume)
        patcher.start()
        self.addCleanup(patcher.stop)

    def settings(self, **values):
        write_fields(self.data / 'ui.ini', dict(menu_sounds=1, sound_volume=55, **values))

    def test_off_zero_effects_and_system_mute_never_load_sdl(self):
        with patch('sleep_sounds.load_sdl') as load:
            for fields, master in (
                ({}, 55), ({
                    'menu_sounds': 0
                }, 55), ({
                    'menu_sounds': 1,
                    'sound_volume': 0
                }, 55), ({
                    'menu_sounds': 1,
                    'sound_volume': 55
                }, 0)
            ):
                write_fields(self.data / 'ui.ini', fields)
                self.volume.saved.return_value = master
                for cue in ('lid-close', 'lid-open'):
                    self.assertFalse(sleep_sounds.play(self.data, cue))
            load.assert_not_called()
            self.volume.apply.assert_not_called()

    def test_same_default_volume_and_independence_from_motion_flags(self):
        for value in ('nan', '-1', '101', '100extra', '55'):
            write_fields(self.data / 'ui.ini', dict(menu_sounds=1, sound_volume=value, animations=0, view_animations=0))
            self.assertEqual(sleep_sounds.effects_volume(self.data), 55)

    def fake_sdl(self):
        sdl = Mock()
        sdl.SDL_InitSubSystem.return_value = 0
        sdl.SDL_OpenAudioDevice.return_value = 7
        sdl.SDL_QueueAudio.return_value = 0
        sdl.SDL_GetQueuedAudioSize.side_effect = [512, 0]
        sdl.SDL_GetError.return_value = b'ALSA: Device or resource busy'
        return sdl

    def test_syncs_mixer_after_open_before_output_and_closes_before_return(self):
        self.settings()
        sdl = self.fake_sdl()
        order = []
        sdl.SDL_OpenAudioDevice.side_effect = lambda *args: order.append('open') or 7
        self.volume.apply.side_effect = lambda: order.append('volume') or 55
        sdl.SDL_PauseAudioDevice.side_effect = lambda *args: order.append('play')
        sdl.SDL_CloseAudioDevice.side_effect = lambda *args: order.append('close')
        sdl.SDL_QuitSubSystem.side_effect = lambda *args: order.append('quit')
        with patch('sleep_sounds.load_sdl', return_value=sdl), patch('sleep_sounds.time.sleep'):
            self.assertTrue(sleep_sounds.play(self.data, 'lid-close'))
        self.assertEqual(order, ['open', 'volume', 'play', 'close', 'quit'])
        _, buffer, size = sdl.SDL_QueueAudio.call_args.args
        samples = struct.unpack('<' + 'h' * (size // 2), buffer.raw[:size])
        self.assertEqual(samples[::2], samples[1::2])
        self.assertLess(max(map(abs, samples)), 8000)  # Effects volume scales PCM.

    def test_stereo_only_hardware_accepts_both_cues_at_their_original_duration(self):
        self.settings()
        sdl = self.fake_sdl()

        def stereo_only(_name, _capture, wanted, _obtained, _flags):
            self.assertEqual(wanted._obj.channels, 2)
            self.assertEqual(wanted._obj.freq, 32000)
            return 7 if wanted._obj.channels == 2 else 0

        sdl.SDL_OpenAudioDevice.side_effect = stereo_only
        sdl.SDL_GetQueuedAudioSize.side_effect = None
        sdl.SDL_GetQueuedAudioSize.return_value = 0
        with patch('sleep_sounds.load_sdl', return_value=sdl), patch('sleep_sounds.time.sleep'):
            for cue, frames in (('lid-close', 7680), ('lid-open', 16000)):
                self.assertTrue(sleep_sounds.play(self.data, cue))
                self.assertEqual(sdl.SDL_QueueAudio.call_args.args[2], frames * 4)

    def test_audio_error_includes_driver_reason(self):
        self.settings()
        sdl = self.fake_sdl()
        sdl.SDL_OpenAudioDevice.return_value = 0
        output = io.StringIO()
        with contextlib.redirect_stderr(output), patch('sleep_sounds.load_sdl', return_value=sdl):
            self.assertFalse(sleep_sounds.play(self.data, 'lid-close'))
        self.assertIn('ALSA: Device or resource busy', output.getvalue())

    def test_mute_during_sink_open_never_unpauses_and_still_releases_device(self):
        self.settings()
        self.volume.apply.return_value = 0
        sdl = self.fake_sdl()
        with patch('sleep_sounds.load_sdl', return_value=sdl):
            self.assertFalse(sleep_sounds.play(self.data, 'lid-open'))
        sdl.SDL_PauseAudioDevice.assert_not_called()
        sdl.SDL_CloseAudioDevice.assert_called_once_with(7)
        sdl.SDL_QuitSubSystem.assert_called_once_with(0x10)

    def test_missing_bank_or_device_is_nonfatal_and_stalled_audio_is_bounded(self):
        self.settings()
        sdl = self.fake_sdl()
        with contextlib.redirect_stderr(io.StringIO()), patch('sleep_sounds.load_sdl', return_value=sdl):
            self.assertFalse(sleep_sounds.play(self.data, 'lid-close', self.data))
            sdl.SDL_InitSubSystem.assert_not_called()
            sdl.SDL_OpenAudioDevice.return_value = 0
            self.assertFalse(sleep_sounds.play(self.data, 'lid-close'))
            sdl.SDL_QuitSubSystem.assert_called_once()
            sdl.reset_mock()
            sdl.SDL_OpenAudioDevice.return_value = 7
            sdl.SDL_GetQueuedAudioSize.side_effect = None
            sdl.SDL_GetQueuedAudioSize.return_value = 512
            with patch('sleep_sounds.time.monotonic', side_effect=[0, 2]):
                self.assertFalse(sleep_sounds.play(self.data, 'lid-open'))
            sdl.SDL_CloseAudioDevice.assert_called_once_with(7)

    def test_both_clips_finish_through_real_sdl_without_video(self):
        self.settings()
        with patch.dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='unavailable-test-driver'):
            for cue in ('lid-close', 'lid-open'):
                self.assertTrue(sleep_sounds.play(self.data, cue), cue)


if __name__ == '__main__': unittest.main()
