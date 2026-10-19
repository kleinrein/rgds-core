"""Read-only device details stay on their page and expose long values."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'

@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class DeviceDetailsTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.data=self.root/'data';self.data.mkdir()
        binaries=self.root/'bin';binaries.mkdir();helper=binaries/'python3'
        firmware='Anbernic '+('long-firmware-identifier-'*55)+' END'
        self.content='state=ready\nbuild=RGDS test\nfirmware='+firmware+'\nkernel=Linux 6.1\nfree_gb=24.5\nip=192.0.2.1\n'
        (self.data/'device').mkdir()
        (self.data/'device/status.ini').write_text(self.content)
        helper.write_text('#!'+sys.executable+'\nimport sys\nfrom pathlib import Path\np=Path(sys.argv[sys.argv.index("--data")+1])/"device"\np.mkdir(parents=True,exist_ok=True)\n(p/"status.ini").write_text('+repr(self.content)+')\n')
        helper.chmod(0o755)
        self.env=dict(os.environ,SDL_VIDEODRIVER='dummy',PATH=str(binaries)+os.pathsep+os.environ.get('PATH',''))

    def run_ui(self,script,row=1):
        (self.root/'state').write_text(f'1 0 0 ""\n16 5 {row}\n')
        (self.root/'input').write_text(script)
        result=subprocess.run([str(BINARY),'--preview','--library','--data',str(self.data),'--state',str(self.root/'state'),
                               '--input-script',str(self.root/'input'),'--seconds','2','--cache-report',str(self.root/'report')],
                              cwd=ROOT,env=self.env,capture_output=True,text=True,timeout=6)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return json.loads((self.root/'report').read_text())

    def test_every_information_row_opens_details_instead_of_leaving(self):
        for row in range(5):
            with self.subTest(row=row):
                report=self.run_ui('500 key A\n',row)
                self.assertEqual(report['page'],16)
                self.assertTrue(report['picker_open'])
                self.assertTrue(report['picker_detail'])
                self.assertIn(f'16 5 {row}\n',(self.root/'state').read_text())

    def test_touch_and_a_close_only_the_popup_preserving_focus(self):
        report=self.run_ui('500 click 220 211\n700 key A\n')
        self.assertEqual(report['page'],16);self.assertFalse(report['picker_open'])
        self.assertIn('16 5 1\n',(self.root/'state').read_text())
        report=self.run_ui('500 key A\n700 click 520 452\n900 key Escape\n')
        self.assertEqual(report['page'],9)
        self.assertIn('9 5 4\n',(self.root/'state').read_text())   # Device information is the fifth System row

    def test_long_unbroken_value_can_scroll_to_its_last_line(self):
        script='500 key A\n'+''.join(f'{600+i*10} key Down\n' for i in range(70))
        report=self.run_ui(script)
        self.assertGreater(report['detail_lines'],8)
        self.assertEqual(report['detail_scroll'],report['detail_lines']-8)
        report=self.run_ui('500 key A\n650 tap 599 180\n750 drag 599 395\n850 release\n')
        self.assertEqual(report['detail_scroll'],report['detail_lines']-8)
        report=self.run_ui('500 key A\n650 tap 220 370\n750 drag 220 174\n850 release\n')
        self.assertEqual(report['detail_scroll'],7)

@unittest.skipUnless(shutil.which('clang++'),'Host compiler required')
class LidReadingTests(unittest.TestCase):
    def test_stock_open_closed_cycles_and_unknown_values(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'lid.cpp';binary=root/'lid'
            source.write_text(r'''#include "platform/system_controls.hpp"
#include <cassert>
#include <iomanip>
int main(int argc,char** argv){assert(argc==2);rgds::fs::path path=argv[1];
 double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
 for(int value:{0,1,0,1}){std::ofstream(path)<<std::setprecision(17)<<now<<' '<<value<<'\n';auto state=rgds::SystemControls::read_lid_state(path);assert(state && *state==(value==1));}
 std::optional<unsigned long long> generation;
 for(int value:{0,1}){std::ofstream(path)<<std::setprecision(17)<<now<<' '<<value<<" 42";auto state=rgds::SystemControls::read_lid_state(path,&generation);assert(state && *state==(value==1) && generation==42);}
 for(const char* value:{"2","-1","","missing","1 extra","1 -1","1 42 extra","1 18446744073709551616"}){std::ofstream(path)<<std::setprecision(17)<<now<<' '<<value;assert(!rgds::SystemControls::read_lid_state(path));}
 for(double stamp:{now-10,now+10}){std::ofstream(path)<<std::setprecision(17)<<stamp<<" 1";assert(!rgds::SystemControls::read_lid_state(path));}
 rgds::fs::remove(path);assert(!rgds::SystemControls::read_lid_state(path));}
''')
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True,capture_output=True,text=True)
            subprocess.run([str(binary),str(root/'hall')],check=True,capture_output=True,text=True)

if __name__=='__main__':unittest.main()
