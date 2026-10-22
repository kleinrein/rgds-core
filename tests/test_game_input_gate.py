"""Owned evdev gate preserves native GT9xx frames and consumes menu/stale input."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'Host C++ compiler required')
class GameInputGateTests(unittest.TestCase):
    def test_native_type_a_coordinates_releases_menu_and_resume_filtering(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'input.cpp'
            binary = root / 'input'
            source.write_text(
                r'''
#include "platform/touch.hpp"
#include "platform/game_input.hpp"
#include <cassert>
int main(){
 assert(rgds::game_switcher_code(312,true) && rgds::game_switcher_code(354,true));
 assert(!rgds::game_switcher_code(158,true) && !rgds::game_switcher_code(316,true));
 assert(rgds::game_switcher_code(316,false) && !rgds::game_switcher_code(312,false));
 rgds::TouchDecoder decoder;decoder.type_a=true;std::vector<rgds::TouchPoint> points;
 auto feed=[&](int type,int code,int value){rgds::TouchPacket packet{1000000,type,code,value};auto original=packet;
  assert(!rgds::filter_game_input(packet,false,true,0,0));assert(packet.type==original.type && packet.code==original.code && packet.value==original.value && packet.micros==original.micros);
  auto result=decoder.feed(packet.type,packet.code,packet.value);points.insert(points.end(),result.begin(),result.end());
 };
 feed(1,330,1);feed(3,53,320);feed(3,54,240);feed(3,57,7);feed(0,2,0);feed(0,0,0);
 assert(points.size()==1 && points.back().kind==0 && points.back().x==320 && points.back().y==240);points.clear();
 feed(3,53,640);feed(3,54,480);feed(3,57,7);feed(0,2,0);feed(0,0,0);
 assert(points.size()==1 && points.back().kind==1 && points.back().x==639 && points.back().y==479);points.clear();
 feed(1,330,0);feed(0,0,0);assert(points.size()==1 && points.back().kind==2);
 rgds::TouchPacket guide{1000000,1,312,1};assert(rgds::filter_game_input(guide,false,true,0,0));assert(guide.type==0 && guide.code==0);
 rgds::TouchPacket stick{1000000,1,316,1};assert(!rgds::filter_game_input(stick,false,true,0,0));assert(stick.type==1 && stick.value==1);
 rgds::TouchPacket generic{1000000,1,316,1};assert(rgds::filter_game_input(generic,false,false,0,0));
 rgds::TouchPacket tracking{1000000,3,57,7};assert(!rgds::filter_game_input(tracking,true,true,0,0));assert(tracking.type==3 && tracking.code==57 && tracking.value==-1);
 rgds::TouchPacket button{1000000,1,330,1};rgds::filter_game_input(button,true,true,0,0);assert(button.type==1 && button.code==330 && button.value==0);
 rgds::TouchPacket stale{99000000,3,53,400};rgds::filter_game_input(stale,false,true,100,1000000100);assert(stale.type==0 && stale.code==0);
 rgds::TouchPacket fresh{101000000,3,53,400};rgds::filter_game_input(fresh,false,true,100,1000000100);assert(fresh.type==3 && fresh.value==400);
 // Releasing a held key/stylus while asleep must clear cached native state.
 for(auto packet:std::array<rgds::TouchPacket,4>{{{99000000,1,304,0},{99000000,1,330,0},{99000000,3,57,-1},{99000000,3,16,0}}}){
  auto original=packet;rgds::filter_game_input(packet,false,true,100,1000000100);
  assert(packet.type==original.type && packet.code==original.code && packet.value==original.value);
 }
 rgds::TouchPacket held{99000000,1,304,1};rgds::filter_game_input(held,false,true,100,1000000100);assert(held.type==0);
 rgds::TouchPacket stale_origin{99000000,3,53,0};rgds::filter_game_input(stale_origin,false,true,100,1000000100);assert(stale_origin.type==0);
}
'''
            )
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bridge_evdev_always_uses_libc(self):
        source = (ROOT / 'src/platform/emulator_bridge.cpp').read_text()
        self.assertRegex(source, r'if \(input\) \{\s*static void\* libc = dlopen\("libc\.so\.6"')
        self.assertNotIn('std::strcmp(path,touch)', source)


if __name__ == '__main__': unittest.main()
