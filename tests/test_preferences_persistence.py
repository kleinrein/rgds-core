"""Read saved preferences in a fresh process, including every supported engine."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class PreferencePersistenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        source = cls.folder / 'preferences.cpp'
        source.write_text(r'''#include "launcher/settings.hpp"
#include <iostream>
int main(int argc,char**argv){
 if(argc==2){std::cout<<rgds::core_count;return 0;}
 if(argc<3)return 2;
 try {
  rgds::fs::path path=argv[2];
  if(std::string(argv[1])=="save"){
   rgds::Preferences p;p.core=unsigned(std::stoi(argv[3]));
   p.dark=true;p.pixel=true;p.auto_load=false;p.auto_save=false;
   p.menu_sounds=true;p.sound_volume=70;p.animations=true;p.view_animations=true;
   p.touch_indicator=false;p.rich_icons=false;p.language="nb-NO";
   p.layout=2;p.view=2;p.filter=14;p.artwork_provider=2;p.drastic_backend=1;
   rgds::save_preferences(path,p);return 0;
  }
  auto p=rgds::load_preferences(path);
  std::cout<<p.core<<' '<<p.dark<<' '<<p.pixel<<' '<<p.auto_load<<' '<<p.auto_save<<' '
   <<p.menu_sounds<<' '<<p.sound_volume<<' '<<p.animations<<' '<<p.view_animations<<' '
   <<p.touch_indicator<<' '<<p.rich_icons<<' '<<p.language<<' '<<p.layout<<' '<<p.view<<' '
   <<p.filter<<' '<<p.artwork_provider<<' '<<p.drastic_backend<<'\n';
 }catch(const std::exception&e){std::cerr<<e.what();return 1;}
}
''')
        cls.helper = cls.folder / 'preferences'
        subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src'),str(source),'-o',str(cls.helper)],
                       check=True,capture_output=True,text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.data = Path(temp.name)
        self.path = self.data / 'preferences.ini'

    def save(self, core):
        subprocess.run([str(self.helper),'save',str(self.path),str(core)],check=True)

    def read(self):
        return subprocess.check_output([str(self.helper),'read',str(self.path)],text=True).strip()

    def test_every_engine_preserves_all_choices_in_a_new_process(self):
        count=int(subprocess.check_output([str(self.helper),'count'],text=True))
        for core in range(count):
            with self.subTest(core=core):
                self.save(core)
                # NNDDSS (2) is retired: a stored choice of it reads back as DSCore (4).
                self.assertEqual(self.read(),f'{4 if core == 2 else core} 1 1 0 0 1 70 1 1 0 0 nb-NO 2 2 14 2 1')

    def test_unknown_or_invalid_engine_does_not_reset_unrelated_preferences(self):
        for value in ('5','99','-1','','x','4extra','4294967296','999999999999999999999',' 4'):
            with self.subTest(value=value):
                self.save(4)
                self.path.write_text(self.path.read_text().replace('core=4\n','core='+value+'\n'))
                # An unknown engine falls back to the default, DSCore (4).
                self.assertEqual(self.read(),'4 1 1 0 0 1 70 1 1 0 0 nb-NO 2 2 14 2 1')

    @unittest.skipUnless(BINARY.exists(),'Build host launcher first')
    def test_actual_launcher_reloads_experimental_preferences_without_fallback(self):
        self.save(4)
        for attempt in range(2):
            report=self.data/'report.json'
            result=subprocess.run([str(BINARY),'--preview','--home','--data',str(self.data),
                '--preferences',str(self.path),'--frames','2','--cache-report',str(report)],cwd=ROOT,
                env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy'),
                capture_output=True,text=True,timeout=8)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertNotIn('Preferences fallback',result.stderr)
            observed=json.loads(report.read_text())
            self.assertEqual((observed['menu_sounds'],observed['sound_volume'],observed['view_animations']),(1,70,1))
            self.assertEqual(self.read(),'4 1 1 0 0 1 70 1 1 0 0 nb-NO 2 2 14 2 1')


if __name__=='__main__':unittest.main()
