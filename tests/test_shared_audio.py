"""Keep the stock playback override within the RGDS process tree."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import audio
import emulators
import shared_audio
import stock_handoff

STOCK = '''pcm.!default {
 type asym
 playback.pcm { type hooks slave.pcm "hw:0,0"
  hooks.0 { type ctl_elems hook_args [{ name "Playback Path" value "SPK" lock true preserve true }] }
 }
 capture.pcm { type hooks slave.pcm "hw:0,0" }
}'''


class SharedAudioTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.stock = Path(temp.name) / 'asound.conf'
        self.stock.write_text(STOCK)

    def test_children_share_playback_but_stock_restore_and_suspend_do_not(self):
        env = shared_audio.configure(ROOT, {}, self.stock)
        config = str(ROOT / 'runtime/alsa-rgds.conf')
        self.assertEqual(env['ALSA_CONFIG_PATH'], config)
        with patch.dict(os.environ, env, clear=True):
            for child in (emulators.base_environment(), audio.clean_environment()):
                self.assertEqual(child['ALSA_CONFIG_PATH'], config)
                self.assertEqual(child['RGDS_SHARED_AUDIO'], '1')
            restored = stock_handoff.clean_environment()
            self.assertNotIn('ALSA_CONFIG_PATH', restored)
            self.assertNotIn('RGDS_SHARED_AUDIO', restored)
        self.assertEqual(self.stock.read_text(), STOCK)

    def test_custom_audio_environment_is_preserved_including_stock_restore(self):
        for env in ({'ALSA_CONFIG_PATH':'/custom/audio.conf'}, {'SDL_AUDIODRIVER':'pulse'}):
            self.assertEqual(shared_audio.configure(ROOT, env.copy(), self.stock), env)
        with patch.dict(os.environ, {'ALSA_CONFIG_PATH':'/custom/audio.conf'}, clear=True):
            self.assertEqual(stock_handoff.clean_environment()['ALSA_CONFIG_PATH'], '/custom/audio.conf')

    def test_home_and_xdg_audio_overrides_are_respected(self):
        home = self.stock.parent
        for path,env in ((home / '.asoundrc', {'HOME':str(home)}),
                         (home / 'xdg/alsa/asoundrc', {'HOME':str(home), 'XDG_CONFIG_HOME':str(home/'xdg')})):
            path.parent.mkdir(parents=True, exist_ok=True);path.write_text('pcm.!default { type pulse }')
            self.assertEqual(shared_audio.configure(ROOT, env.copy(), self.stock), env)
            path.unlink()

    def test_missing_or_different_stock_profile_is_not_overridden(self):
        for text in ('', 'pcm.!default { type pulse }', STOCK.replace('"hw:0,0"','"hw:1,0"')):
            self.stock.write_text(text)
            self.assertEqual(shared_audio.configure(ROOT, {}, self.stock), {})
        self.stock.unlink()
        self.assertEqual(shared_audio.configure(ROOT, {}, self.stock), {})


class Stoppable:
    def __init__(self, pid=321): self.pid = pid; self.signals = []
    def send_signal(self, value): self.signals.append(value)


class StopOutsideMixerTests(unittest.TestCase):
    """SIGSTOP must land outside alsa-lib's dmix critical section."""
    def test_free_semaphore_or_no_mixer_stops_once(self):
        for owner in (0, None, 999):
            with self.subTest(owner=owner):
                child = Stoppable()
                self.assertTrue(shared_audio.stop_outside_mixer(child, holder=lambda: owner, settled=lambda pid: True, sleep=lambda s: None))
                self.assertEqual(child.signals, [signal.SIGSTOP])

    def test_holder_is_resumed_and_stopped_again_until_it_lets_go(self):
        child = Stoppable(); owners = iter([321, 321, 0]); slept = []
        self.assertTrue(shared_audio.stop_outside_mixer(child, holder=lambda: next(owners), settled=lambda pid: True, sleep=slept.append))
        self.assertEqual(child.signals, [signal.SIGSTOP, signal.SIGCONT] * 2 + [signal.SIGSTOP])
        self.assertEqual(len(slept), 2)

    def test_gives_up_stopped_after_the_retries(self):
        child = Stoppable()
        self.assertFalse(shared_audio.stop_outside_mixer(child, holder=lambda: 321, settled=lambda pid: True, retries=3, sleep=lambda s: None))
        self.assertEqual(child.signals[-1], signal.SIGSTOP)
        self.assertEqual(child.signals.count(signal.SIGCONT), 3)

    def test_waits_for_every_thread_to_stop_before_reading_the_semaphore(self):
        child = Stoppable(); states = iter([False, False, True]); order = []
        clock = iter(range(0, 100))
        def settled(pid): order.append('settled'); return next(states)
        def holder(): order.append('holder'); return 0
        self.assertTrue(shared_audio.stop_outside_mixer(child, holder=holder, settled=settled, sleep=lambda s: order.append('sleep'), clock=lambda: next(clock) / 1000))
        self.assertEqual(order, ['settled', 'sleep', 'settled', 'sleep', 'settled', 'holder'])

    def test_thread_states_are_read_from_proc(self):
        with tempfile.TemporaryDirectory() as folder:
            proc = Path(folder); tasks = proc / '7' / 'task'
            (tasks / '7').mkdir(parents=True); (tasks / '8').mkdir()
            (tasks / '7' / 'stat').write_text('7 (rgds) T 1 7 7 0 -1\n')
            (tasks / '8' / 'stat').write_text('8 (mali (x)) R 1 7 7 0 -1\n')
            with patch('shared_audio.Path', side_effect=lambda *parts: Path(folder) if parts == ('/proc',) else Path(*parts)):
                self.assertFalse(shared_audio.threads_stopped(7))
                (tasks / '8' / 'stat').write_text('8 (mali (x)) t 1 7 7 0 -1\n')
                self.assertTrue(shared_audio.threads_stopped(7))
                self.assertTrue(shared_audio.threads_stopped(12345))

    def test_host_without_the_mixer_reports_none(self):
        self.assertIsNone(shared_audio.mixer_holder(key=0x5247DEAD))


if __name__ == '__main__':unittest.main()
