"""UI acceptance for updates/statistics and large-library rail containment."""
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import test_visual_options as helpers
ROOT=Path(__file__).resolve().parents[1]

class OTAUITests(unittest.TestCase):
    setUp=helpers.VisualOptionsTests.setUp
    run_ui=helpers.VisualOptionsTests.run_ui
    pixel=staticmethod(helpers.VisualOptionsTests.pixel)
    def test_statistics_entry_and_back(self):
        self.run_ui(extra=('--settings','--section','statistics'))
        self.assertIn('27 7 0',(self.root/'state').read_text())
        self.run_ui('200 key Escape\n',('--settings','--section','statistics'))
        self.assertIn('1 7 0',(self.root/'state').read_text())
    def test_settings_home_tile_has_no_play_summary(self):
        data=self.run_ui('20 key Left\n40 key-up Left\n',('--home',))
        self.assertIn('home 0',(self.root/'state').read_text())
        self.assertEqual(sum(self.pixel(data,x,y)==(45,57,68) for x in range(50,592) for y in range(239,262)),0)
    def test_updates_are_reachable_and_unconfigured_is_explicit(self):
        script='20 key Down\n40 key-up Down\n80 key Down\n100 key-up Down\n140 key Down\n160 key-up Down\n220 key A\n'
        self.run_ui(script,('--settings','--section','system'))
        self.assertIn('17 5 0',(self.root/'state').read_text())
        self.assertIn('state=unconfigured',(self.root/'data/update/status.ini').read_text())
    def test_update_progress_renders_from_async_worker(self):
        binaries=self.root/'bin';binaries.mkdir();helper=binaries/'python3'
        helper.write_text('#!'+sys.executable+'\n'+'''import os,sys,time
from pathlib import Path
if 'update-check' in sys.argv:
 d=Path(sys.argv[sys.argv.index('--data')+1])/'update';d.mkdir(parents=True,exist_ok=True)
 (d/'status.ini').write_text('state=downloading\\nmessage=Downloading update…\\nprogress=50\\nversion=0.1.0\\n')
 time.sleep(10)
else:os.execv(sys.executable,[sys.executable,*sys.argv[1:]])
''');helper.chmod(0o755)
        (self.root/'state').write_text('1 0 0 ""\n17 5 0\n')
        with patch.dict(os.environ,PATH=str(binaries)+os.pathsep+os.environ['PATH']):data=self.run_ui('400 key A\n')
        self.assertGreater(sum(self.pixel(data,x,369)==(44,137,218) for x in range(52,546)),220)
    def test_large_library_thumb_stays_inside_viewport_at_every_position(self):
        source=self.root/'rail.cpp';binary=self.root/'rail'
        source.write_text('''#include "launcher/carousel.hpp"
#include <cassert>
int main(){for(size_t count:{0,1,4,5,62,100,1000,100000})for(int i=0;i<=1000;i++){
 auto r=rgds::RailGeometry::at(count,count>1?double(count-1)*i/1000:0);
 assert(r.thumb_left>=r.window_left+2);
 assert(r.thumb_left+r.thumb_width<=r.window_left+r.window_width-2);
 assert(r.window_left>=84 && r.window_left+r.window_width<=556);
 assert(r.thumb_width>=32);if(count<=4)assert(r.thumb_width>=118);
}}
''')
        subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True,capture_output=True)
        subprocess.run([str(binary)],check=True,capture_output=True)

if __name__=='__main__':unittest.main()
