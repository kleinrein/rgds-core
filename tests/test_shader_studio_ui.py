"""Drive the real shader studio through host SDL events and inspect its pixels.

No handheld, ROM execution or network connection is involved. The test inputs
use disposable navigation/preferences; shader selection sends preview IPC only.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'


def bmp_pixels(path):
    data=path.read_bytes();offset=struct.unpack_from('<I',data,10)[0];width,height=struct.unpack_from('<ii',data,18);bits=struct.unpack_from('<H',data,28)[0]
    if bits not in (24,32):raise ValueError('Unexpected screenshot format')
    stride=((width*bits+31)//32)*4;rows=[]
    for y in range(abs(height)):
        source=abs(height)-1-y if height>0 else y;start=offset+source*stride;row=[]
        for x in range(width):
            at=start+x*(bits//8);b,g,r=data[at:at+3];row.append((r,g,b))
        rows.append(row)
    return width,abs(height),rows


def patch(rows,x,y,w,h):return [color for row in rows[y:y+h] for color in row[x:x+w]]

def fingerprint(colors):return hashlib.sha256(bytes(channel for color in colors for channel in color)).hexdigest()


@unittest.skipUnless(BINARY.is_file(),'Build host launcher first')
class ShaderStudioUiTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        self.state=self.root/'state.txt';self.prefs=self.root/'preferences.ini';self.ui=self.root/'ui.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\ncore=0\nauto_load=1\nauto_save=1\n')
        self.ui.write_text('schema=1\nfilter=4\nanimations=0\nview=0\n')
        self.seed()

    def seed(self,game=False,focus=None,scene=0,mode=2,split=.5):
        if focus is None:focus=3 if game else 2  # LCD1x; game choices begin with Default.
        self.state.write_text(f'1 3 2 ""\n20 2 {focus}\nshader {int(game)}\nshader-preview {scene} {mode} {split}\n')

    def run_preview(self,script=None,game=False,frames=3,core='drastic'):
        screenshot=self.root/'studio.bmp';report=self.root/'cache.json'
        args=[str(BINARY),'--preview','--app',str(ROOT),'--data',str(self.root),'--state',str(self.state),'--preferences',str(self.prefs),'--screenshot',str(screenshot),'--cache-report',str(report)]
        args+=['--game-menu','--game-core',core] if game else ['--library']
        if script:
            path=self.root/'input.txt';path.write_text(script);args+=['--input-script',str(path),'--seconds','1']
        else:args+=['--frames',str(frames)]
        result=subprocess.run(args,cwd=ROOT,env=dict(os.environ,SDL_VIDEODRIVER='dummy'),capture_output=True,text=True,timeout=6)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertNotIn('Font fallback',result.stderr)
        return result,json.loads(report.read_text()),bmp_pixels(screenshot)

    def preview_state(self):
        lines=self.state.read_text().splitlines();page,section,focus=map(int,lines[1].split());marker,scene,mode,split=lines[3].split()
        self.assertEqual(marker,'shader-preview');return page,focus,int(scene),int(mode),float(split)

    def assert_cache(self,report):
        self.assertGreaterEqual(report['shader_preview_entries'],2);self.assertLessEqual(report['shader_preview_entries'],8)
        self.assertGreaterEqual(report['shader_preview_bytes'],2*640*480*4);self.assertLessEqual(report['shader_preview_bytes'],10*1024*1024)
        self.assertGreaterEqual(report['shader_preview_reads'],2);self.assertLessEqual(report['shader_preview_reads'],8)
        self.assertEqual(report['shader_preview_stamp_checks'],report['shader_preview_reads']*2)
        self.assertEqual(report['render_file_reads'],0);self.assertEqual(report['render_stamp_checks'],0)

    def test_real_scene_and_pixel_detail_render_in_general_and_every_game_menu(self):
        self.seed(mode=0);_,general,(w,h,rows)=self.run_preview();self.assertEqual((w,h),(640,960));self.assert_cache(general)
        picture=patch(rows,42,180,346,224);detail=patch(rows,424,146,184,138)
        self.assertGreater(len(set(picture)),120);self.assertGreater(len(set(detail)),40)
        for core in ('drastic','dsperate','nnddss'):
            with self.subTest(core=core):
                self.seed(game=True,mode=0);result,counts,(_,_,game_rows)=self.run_preview(game=True,core=core);self.assert_cache(counts)
                self.assertEqual(fingerprint(patch(game_rows,42,180,346,224)),fingerprint(picture));self.assertEqual(fingerprint(patch(game_rows,424,146,184,138)),fingerprint(detail))
                self.assertNotIn('Shader request (preview)',result.stdout)

    def test_comparison_modes_show_real_before_after_pixels(self):
        pictures=[]
        for mode in (0,1,2):
            self.seed(mode=mode);_,report,(_,_,rows)=self.run_preview();self.assertEqual(report['shader_preview_mode'],mode);pictures.append(patch(rows,42,180,346,224))
        filtered,original,split=pictures;self.assertNotEqual(filtered,original);self.assertNotEqual(split,original);self.assertNotEqual(split,filtered)
        self.assertGreater(sum(sum(color) for color in original),sum(sum(color) for color in filtered))
        # The Off option remains exactly the same picture in each compare mode.
        off=[]
        for mode in (0,1,2):
            self.seed(focus=0,mode=mode);_,_,(_,_,rows)=self.run_preview();off.append(patch(rows,42,180,168,224))
        self.assertEqual(fingerprint(off[0]),fingerprint(off[1]));self.assertEqual(fingerprint(off[0]),fingerprint(off[2]))

    def test_keyboard_scene_compare_and_wipe_survive_restart_without_saving_shader(self):
        before=(self.prefs.read_bytes(),self.ui.read_bytes())
        result,counts,_=self.run_preview('40 key Right\n80 key X\n120 key Y\n160 key [\n')
        self.assertEqual(self.preview_state()[:4],(20,3,1,2));self.assertAlmostEqual(self.preview_state()[4],.4)
        self.assertEqual(counts['shader_preview_scene'],1);self.assertEqual(counts['shader_preview_mode'],2);self.assertAlmostEqual(counts['shader_preview_split'],.4)
        self.assertEqual(before,(self.prefs.read_bytes(),self.ui.read_bytes()));self.assertNotIn('Shader request (preview)',result.stdout)
        self.assertFalse(list((self.root/'library').glob('*/game.ini')))
        _,restored,_=self.run_preview();self.assertEqual(self.preview_state()[:4],(20,3,1,2));self.assertAlmostEqual(restored['shader_preview_split'],.4)
        _,right,_=self.run_preview('40 key ]\n');self.assertAlmostEqual(right['shader_preview_split'],.5)
        _,left_shoulder,_=self.run_preview('40 joy-down 4\n60 joy-up 4\n');self.assertAlmostEqual(left_shoulder['shader_preview_split'],.4)
        _,right_shoulder,_=self.run_preview('40 joy-down 5\n60 joy-up 5\n');self.assertAlmostEqual(right_shoulder['shader_preview_split'],.5)

    def test_header_touch_scene_and_compare_are_preview_only(self):
        before=(self.prefs.read_bytes(),self.ui.read_bytes())
        result,report,(_,_,changed)=self.run_preview('40 click 420 45\n100 click 560 45\n')
        self.assertEqual(report['shader_preview_scene'],1);self.assertEqual(report['shader_preview_mode'],0)
        self.assertEqual(self.preview_state()[:4],(20,2,1,0));self.assertEqual(before,(self.prefs.read_bytes(),self.ui.read_bytes()));self.assertNotIn('Shader request (preview)',result.stdout)
        self.seed(scene=0,mode=0);_,_,(_,_,original)=self.run_preview()
        self.assertNotEqual(patch(changed,42,180,346,224),patch(original,42,180,346,224))

    def test_global_navigation_only_applies_when_a_is_pressed(self):
        before=(self.prefs.read_bytes(),self.ui.read_bytes());result,report,_=self.run_preview('40 key Right\n80 key X\n')
        self.assertEqual(before,(self.prefs.read_bytes(),self.ui.read_bytes()));self.assertEqual(report['page'],20);self.assertEqual(self.preview_state()[1],3)
        self.assertNotIn('Shader request (preview)',result.stdout)
        _,applied,_=self.run_preview('40 key A\n');self.assertIn('filter=3\n',self.ui.read_text());self.assertNotEqual(applied['page'],20)

    def test_game_navigation_only_requests_on_a_and_keeps_studio_open(self):
        self.seed(game=True);before=self.ui.read_bytes();result,report,_=self.run_preview('40 key Right\n80 key X\n120 key Y\n',game=True)
        self.assertEqual(before,self.ui.read_bytes());self.assertFalse(list((self.root/'library').glob('*/game.ini')));self.assertNotIn('Shader request (preview)',result.stdout);self.assertEqual(report['page'],20)
        result,applied,_=self.run_preview('40 key A\n',game=True)
        self.assertIn('Shader request (preview): shader:3',result.stdout);self.assertEqual(applied['page'],20);self.assertEqual(self.preview_state()[:4],(20,4,1,0));self.assertEqual(before,self.ui.read_bytes())
        options=list((self.root/'library').glob('*/game.ini'));self.assertEqual(len(options),1);self.assertIn('filter=3\n',options[0].read_text())

    def test_repeated_frames_use_bounded_warm_preview_assets_without_render_io(self):
        self.seed(focus=20);_,counts,_=self.run_preview(frames=40)
        self.assertEqual(counts['frames'],40);self.assert_cache(counts);self.assertEqual(counts['shader_preview_reads'],3)


if __name__=='__main__':unittest.main()
