"""Direct NNDDSS evdev translation, independent from SDL's physical state."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class MainlineEvdevTests(unittest.TestCase):
    def test_native_keys_bits_directions_and_reserved_speed(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'test.cpp'
            binary = Path(folder) / 'test'
            source.write_text(
                r'''
#include "platform/control_mapping.hpp"
#include <cassert>
int main(){
 rgds::ControlMapping mapping;std::array<bool,16> held{};
 auto event=[&](int code,int value,int expected_type,int expected_code,int expected_value){
  int type=1;mapping.remap_evdev_key(type,code,value,true,true,&held);
  assert(type==expected_type && code==expected_code && value==expected_value);
 };
 event(305,1,1,304,1);event(304,1,1,305,1); // Physical A/B to native loader.
 event(307,1,1,307,1);event(308,1,1,306,1); // X/Y
 event(315,1,1,311,1);event(314,1,1,310,1); // Start/Select
 event(544,1,3,17,-1);event(545,1,3,17,0);event(545,0,3,17,-1);event(544,0,3,17,0);
 event(546,1,3,16,-1);event(547,1,3,16,0);event(546,0,3,16,1);event(547,0,3,16,0);
 mapping.sources[0]=12;event(312,1,1,304,1); // L2 mapped to A, never Menu.
 mapping.sources[0]=rgds::control_none;mapping.fast_forward_source=12;
 event(312,1,0,0,0);event(312,0,0,0,0);
 mapping=rgds::ControlMapping{};unsigned char bits[128]{};
 auto set=[&](int code){bits[code/8]|=1<<(code%8);};
 auto down=[&](int code){return (bits[code/8]&(1<<(code%8)))!=0;};
 set(305);set(315);set(312);set(317);
 mapping.remap_key_bits(bits,sizeof(bits),false,true);
 assert(down(304) && !down(305) && !down(311) && !down(312) && !down(317));
 mapping.sources[0]=12;std::fill(std::begin(bits),std::end(bits),0);set(312);
 mapping.remap_key_bits(bits,sizeof(bits),true,true);assert(down(304) && !down(312));
 // Stock behavior is preserved when mainline is not selected.
 int type=1,code=304,value=1;mapping=rgds::ControlMapping{};
 mapping.remap_evdev_key(type,code,value);assert(type==1 && code==304 && value==1);
}
'''
            )
            subprocess.run(
                [
                    'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(binary)
                ],
                check=True,
                capture_output=True
            )
            subprocess.run([str(binary)], check=True, capture_output=True)
