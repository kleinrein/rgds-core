"""Keep the stock playback override within the RGDS process tree."""
import os
from pathlib import Path
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


if __name__ == '__main__':unittest.main()
