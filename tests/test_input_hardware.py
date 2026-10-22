"""Raw touchscreen sequencing, held navigation and the real C++/Python command boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class InputHardwareTests(unittest.TestCase):
    def compile_and_run(self, code, setup=None):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            if setup: setup(root)
            source = root / "test.cpp"
            source.write_text(code)
            binary = root / "probe"
            subprocess.run(
                [
                    "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                    str(source), "-o",
                    str(binary)
                ],
                check=True,
                capture_output=True
            )
            result = subprocess.run([str(binary), str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_touch_reports_down_move_up_with_calibrated_coordinates(self):
        self.compile_and_run(
            '''#include "platform/touch.hpp"
#include <cassert>
int main(){rgds::TouchDecoder t;
t.feed(3,53,320);t.feed(3,54,240);t.feed(3,57,1);
auto down=t.feed(0,0,0);assert(down.size()==1 && down[0].kind==0 && down[0].x==320 && down[0].y==240);
t.feed(3,53,640);t.feed(3,54,480);auto move=t.feed(0,0,0);assert(move[0].kind==1 && move[0].x==639 && move[0].y==479);
t.feed(1,330,0);auto up=t.feed(0,0,0);assert(up[0].kind==2);assert(t.feed(0,0,0).empty());}
'''
        )

    def test_holding_accelerates_and_release_stops_navigation(self):
        self.compile_and_run(
            '''#include "launcher/repeat.hpp"
#include <cassert>
int main(){rgds::HoldRepeat h;h.press(1,1,0);assert(h.poll(319)==0);assert(h.poll(320)==1);
assert(h.poll(400)==0);assert(h.poll(450)==1);assert(h.poll(1500)==1);assert(h.poll(1545)==1);
h.release(1);assert(h.poll(2000)==0);h.press(1,-1,2100);assert(h.poll(2420)==-1);}
'''
        )

    def test_plus_native_touch_range_maps_to_logical_canvas(self):
        self.compile_and_run(
            '''#include "platform/touch.hpp"
#include <cassert>
int main(){for(bool slots:{false,true}){rgds::TouchDecoder t;
t.max_x=1023;t.max_y=767;t.uses_slots=slots;t.type_a=!slots;
if(slots)t.feed(3,47,0);
t.feed(3,57,7);t.feed(3,53,512);t.feed(3,54,384);
if(!slots)t.feed(0,2,0);
auto down=t.feed(0,0,0);assert(down.size()==1 && down[0].kind==0 && down[0].x==320 && down[0].y==240);
t.feed(3,57,7);t.feed(3,53,1023);t.feed(3,54,767);
if(!slots)t.feed(0,2,0);
auto move=t.feed(0,0,0);assert(move.size()==1 && move[0].x==639 && move[0].y==479);
t.feed(3,57,-1);if(!slots)t.feed(0,2,0);
auto up=t.feed(0,0,0);assert(up.size()==1 && up[0].kind==2);}}
'''
        )

    def test_native_system_helper_passes_value_before_optional_arguments(self):
        def setup(root):
            (root / "app/runtime").mkdir(parents=True)
            (root / "app/runtime/system_controls.py").write_text(
                '''import argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('command');p.add_argument('value',nargs='?',type=int);p.add_argument('--data',type=Path,required=True)
a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
if a.command=='brightness':(a.data/'value.txt').write_text(str(a.value))
'''
            )

        self.compile_and_run(
            '''#include "platform/system_controls.hpp"
#include <cassert>
int main(int argc,char** argv){(void)argc;rgds::fs::path root=argv[1];rgds::SystemControls s(false,root/"app",root/"data");
assert(s.set("brightness",55));std::ifstream f(root/"data/value.txt");int n=0;f>>n;assert(n==55);}
''', setup
        )


if __name__ == "__main__": unittest.main()
