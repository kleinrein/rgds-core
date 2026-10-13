"""Language scrolling must not commit during gestures, and both fonts keep Vietnamese marks."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class LanguagePickerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.languages = [line.split('\t')[0] for line in (ROOT/'assets/locales/languages.tsv').read_text().splitlines()]
        self.maximum = (len(self.languages)-8)*40
        (self.root/'ui.ini').write_text('schema=1\nlanguage=en-US\nanimations=0\nview_animations=0\n')
        (self.root/'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script, screenshot=None):
        (self.root/'input').write_text(script)
        command = [str(BINARY), '--preview', '--settings', '--section', 'system',
                   '--data', str(self.root), '--state', str(self.root/'state'),
                   '--input-script', str(self.root/'input'), '--cache-report', str(self.root/'report'), '--seconds', '1']
        if screenshot:
            command += ['--screenshot', str(screenshot)]
        result = subprocess.run(command, cwd=ROOT, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        report = json.loads((self.root/'report').read_text())
        self.assertEqual(report['render_file_reads'], 0)
        return report

    def language(self):
        return next(line for line in (self.root/'ui.ini').read_text().splitlines() if line.startswith('language='))

    def test_thumb_grab_does_not_jump_or_select(self):
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 588 150\n160 release\n')
        self.assertEqual(report['picker_scroll'], 0)
        self.assertTrue(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')

    def test_thumb_drag_reaches_last_language_and_touch_persists(self):
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 588 150\n160 drag 588 400\n200 release\n')
        self.assertEqual(report['picker_scroll'], self.maximum)
        self.assertTrue(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 588 150\n160 drag 588 400\n200 release\n260 click 180 385\n')
        self.assertFalse(report['picker_open'])
        self.assertEqual(self.language(), 'language='+self.languages[-1])
        report = self.run_ui('30 key A\n40 key-up A\n')
        self.assertEqual(report['picker_focus'], len(self.languages)-1)
        self.assertEqual(report['picker_scroll'], self.maximum)

    def test_swipe_and_cancel_preserve_language_and_keyboard_focus_stays_visible(self):
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 180 350\n160 drag 180 80\n200 release\n')
        self.assertTrue(report['picker_open'])
        self.assertEqual(report['picker_scroll'], self.maximum)
        self.assertGreaterEqual(report['picker_focus']*40, report['picker_scroll'])
        self.assertEqual(self.language(), 'language=en-US')
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 180 350\n160 drag 180 80\n200 release\n260 key Escape\n')
        self.assertFalse(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')

    def test_wheel_scroll_is_bounded_and_does_not_apply(self):
        report = self.run_ui('30 key A\n40 key-up A\n120 wheel 0 -99\n')
        self.assertEqual(report['picker_scroll'], self.maximum)
        self.assertTrue(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')
        report = self.run_ui('30 key A\n40 key-up A\n120 wheel 0 -99\n160 wheel 0 99\n')
        self.assertEqual(report['picker_scroll'], 0)

    def test_finger_swipe_uses_same_viewport_and_focus_loss_cancels_pending_tap(self):
        report = self.run_ui('30 key A\n40 key-up A\n120 finger-down 0.28125 0.8645833\n160 finger-move 0.28125 0.5833333\n200 finger-up 0.28125 0.5833333\n')
        self.assertEqual(report['picker_scroll'], self.maximum)
        self.assertTrue(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')
        report = self.run_ui('30 key A\n40 key-up A\n120 tap 180 145\n140 focus-lost\n160 release\n')
        self.assertTrue(report['picker_open'])
        self.assertEqual(self.language(), 'language=en-US')

    def test_vietnamese_renders_with_both_typefaces_and_picker_stays_idle(self):
        for font in ('rounded', 'pixel'):
            with self.subTest(font=font):
                (self.root/'preferences.ini').write_text(f'schema=1\nmode=light\nfont={font}\n')
                (self.root/'ui.ini').write_text('schema=1\nlanguage=vi-VN\nanimations=0\nview_animations=0\n')
                screenshot = self.root/(font+'.bmp')
                report = self.run_ui('30 key A\n40 key-up A\n', screenshot)
                self.assertEqual(report['picker_focus'], self.languages.index('vi-VN'))
                self.assertLess(report['frames'], 12)
                self.assertGreater(screenshot.stat().st_size, 640*960)


@unittest.skipUnless(shutil.which('clang++'), 'Host C++ compiler required')
class VietnamesePixelTests(unittest.TestCase):
    def test_vowel_shape_and_tone_survive_for_every_extended_vietnamese_letter(self):
        import unicodedata
        source = '''#include "launcher/pixel_text.hpp"\n#include <cassert>\nint main(){\n'''
        extra = [0x102,0x103,0x110,0x111,0x128,0x129,0x168,0x169,0x1a0,0x1a1,0x1af,0x1b0]
        shapes = {'\u0302':1, '\u0306':2, '\u031b':3}
        tones = {'\u0300':1, '\u0301':2, '\u0309':3, '\u0303':4, '\u0323':5}
        for cp in extra+list(range(0x1ea0,0x1efa)):
            decomposed = unicodedata.normalize('NFD', chr(cp).upper())
            if decomposed == 'Đ':
                base, shape, tone = 'D', 4, 0
            else:
                base = decomposed[0]
                shape = next((shapes[c] for c in decomposed if c in shapes), 0)
                tone = next((tones[c] for c in decomposed if c in tones), 0)
            source += f"{{auto glyph=rgds::vietnamese_pixel_glyph({cp});assert(glyph.base=='{base}' && glyph.shape=={shape} && glyph.tone=={tone});}}\n"
        source += 'assert(!rgds::vietnamese_pixel_glyph(65).base);\n}\n'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'pixel.cpp').write_text(source)
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(root/'pixel.cpp'),'-o',str(root/'pixel')],check=True)
            subprocess.run([str(root/'pixel')],check=True)


if __name__ == '__main__':
    unittest.main()
