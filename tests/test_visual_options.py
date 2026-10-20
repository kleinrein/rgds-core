"""Actual SDL interaction coverage for icon styles and the expanded language picker."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'

@unittest.skipUnless(BINARY.exists(),'Build host launcher first')
class VisualOptionsTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.prefs=self.root/'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\n')
        (self.root/'ui.ini').write_text('schema=1\nhome_layout=carousel\nbackground=plain\n')   # the carousel rail over a plain background
    def run_ui(self,script='',extra=()):
        path=self.root/'input.txt';path.write_text(script);bmp=self.root/'screen.bmp'
        result=subprocess.run([str(BINARY),'--preview','--data',str(self.root/'data'),'--preferences',str(self.prefs),'--state',str(self.root/'state'),'--input-script',str(path),'--seconds','1','--screenshot',str(bmp),*extra],cwd=ROOT,env=dict(os.environ,SDL_VIDEODRIVER='dummy'),capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr)
        return bmp.read_bytes()
    @staticmethod
    def pixel(data,x,y):
        offset=struct.unpack_from('<I',data,10)[0];width,height=struct.unpack_from('<ii',data,18)
        index=offset+((height-1-y if height>0 else y)*width+x)*4
        b,g,r=data[index:index+3];return r,g,b
    def test_touch_icon_style_survives_restart_and_keeps_font(self):
        # Icon style is the fifth row: scroll it into view, open its picker.
        self.run_ui('10 wheel 0 -3\n20 click 300 337\n180 click 140 326\n',('--settings','--section','theme'))
        self.assertIn('icon_style=pixel',(self.root/'ui.ini').read_text())
        self.assertIn('font=rounded',self.prefs.read_text())
        pixel=self.run_ui(extra=('--home',))
        self.run_ui('10 wheel 0 -3\n20 click 300 337\n180 click 140 378\n',('--settings','--section','theme'))
        self.assertIn('icon_style=rich',(self.root/'ui.ini').read_text())
        rich=self.run_ui(extra=('--home',))
        # The category symbol actually switches renderer; the setting is not decorative.
        def icon(data):return [self.pixel(data,x,y) for y in range(108,165) for x in range(291,349)]
        self.assertNotEqual(icon(pixel),icon(rich))
    def test_controller_can_select_pixel_icons(self):
        script=''.join(f'{20+i*60} key Down\n{40+i*60} key-up Down\n' for i in range(4))+'340 key Left\n360 key-up Left\n'
        self.run_ui(script,('--settings','--section','theme'))
        self.assertIn('icon_style=pixel',(self.root/'ui.ini').read_text())
    def test_language_is_first_and_sorted_italian_is_touch_reachable(self):
        self.run_ui('20 key A\n40 key-up A\n200 click 180 307\n',('--settings','--section','system'))
        self.assertIn('language=it-IT',(self.root/'ui.ini').read_text())
    def test_touch_can_scroll_to_nynorsk_and_swedish_and_back_to_english(self):
        # The sheet's scrollbar: a drag to the end shows the last languages.
        for language,y in [('nn-NO',190),('sv-SE',350)]:
            with self.subTest(language=language):
                (self.root/'ui.ini').write_text('schema=1\nlanguage=en-US\n')
                self.run_ui(f'20 click 180 129\n200 tap 588 130\n240 drag 588 410\n280 release\n400 click 180 {y}\n',('--settings','--section','system'))
                self.assertIn(f'language={language}\n',(self.root/'ui.ini').read_text())
                self.run_ui('20 click 180 129\n200 tap 588 400\n240 drag 588 60\n280 release\n400 click 180 110\n',('--settings','--section','system'))
                self.assertIn('language=en-US\n',(self.root/'ui.ini').read_text())
    def test_scrolling_then_cancel_keeps_the_selected_language(self):
        (self.root/'ui.ini').write_text('schema=1\nlanguage=en-US\n')
        self.run_ui('20 click 180 129\n200 tap 588 130\n240 drag 588 410\n280 release\n400 click 70 452\n',('--settings','--section','system'))
        self.assertIn('language=en-US\n',(self.root/'ui.ini').read_text())
    def test_home_has_wide_blue_thumb_and_grey_position_line(self):
        data=self.run_ui(extra=('--home',))
        blue=(35,110,174)  # the glossy accent of the arrow buttons
        # DSi bar: square accent arrow buttons at both ends, a white glass grabber over the ticks.
        self.assertGreaterEqual(sum(self.pixel(data,x,928)==blue for x in range(8,56)),20)
        self.assertGreaterEqual(sum(self.pixel(data,x,928)==blue for x in range(584,632)),20)
        self.assertGreaterEqual(sum(self.pixel(data,x,928)==(255,255,255) for x in range(72,568)),30)

if __name__=='__main__':unittest.main()
