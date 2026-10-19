"""Owned frontend requests and enhanced-raster sampling equivalence."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
class RgdsFrontendTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            cpp = folder/'test.cpp'; cpp.write_text(source)
            binary = folder/'test'
            subprocess.run(['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT/'dscore/src'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(folder)], check=True, timeout=15)

    def test_pause_requests_owned_menu_and_failure_does_not_publish_partial_request(self):
        self.compile_run(r'''
#include "frontend/sdl/rgds_frontend_ui.hpp"
#include <cassert>
#include <filesystem>
int main(int, char** argv) {
  namespace fs = std::filesystem;
  unsetenv("RGDS_CONTROL_FILE"); assert(!rgds::request_menu());
  const auto control = fs::path(argv[1])/"control";
  setenv("RGDS_CONTROL_FILE", control.c_str(), 1);
  for (int i=0;i<3;++i) {
    assert(rgds::request_menu());
    std::ifstream f(control.string()+".game-menu");std::string value;std::getline(f,value);
    assert(value=="RGDS");assert(fs::remove(control.string()+".game-menu"));
  }
  // A destination that cannot be atomically published reports failure,
  // keeps the existing object and removes its temporary request.
  fs::create_directory(control.string()+".game-menu");
  assert(!rgds::request_menu());assert(fs::is_directory(control.string()+".game-menu"));
  assert(!fs::exists(control.string()+".game-menu.engine-"+std::to_string(getpid())));
  fs::remove(control.string()+".game-menu");
  setenv("RGDS_CONTROL_FILE", (control/"missing").c_str(), 1);
  assert(!rgds::request_menu());
}
''')

    def test_live_menu_holds_while_input_is_blocked_and_reads_the_overlay_sequence(self):
        self.compile_run(r'''
#include "frontend/sdl/rgds_frontend_ui.hpp"
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <thread>
int main(int, char** argv) {
  namespace fs = std::filesystem;
  const auto control = fs::path(argv[1])/"control", frame = fs::path(argv[1])/"menu.frame";
  setenv("RGDS_CONTROL_FILE", control.c_str(), 1);
  setenv("RGDS_MENU_LIVE", "1", 1);
  setenv("RGDS_OVERLAY_FILE", frame.c_str(), 1);
  assert(!rgds::menu_hold());
  assert(rgds::overlay_sequence() == 0);   // the menu helper has not made the frame yet
  std::ofstream(control.string()+".blocked").close();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  assert(rgds::menu_hold());
  fs::remove(control.string()+".blocked");
  assert(rgds::menu_hold());               // polled at most every 20 ms
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  assert(!rgds::menu_hold());
  const uint32_t header[16] = {0x52474453, 640, 480, 6, 1};
  { std::ofstream out(frame, std::ios::binary); out.write(reinterpret_cast<const char*>(header), sizeof header); }
  assert(rgds::overlay_sequence() == 6);
}
''')

    def test_menu_hold_is_off_without_a_live_menu(self):
        self.compile_run(r'''
#include "frontend/sdl/rgds_frontend_ui.hpp"
#include <cassert>
#include <filesystem>
int main(int, char** argv) {
  const auto control = std::filesystem::path(argv[1])/"control";
  setenv("RGDS_CONTROL_FILE", control.c_str(), 1);
  setenv("RGDS_MENU_LIVE", "0", 1);
  std::ofstream(control.string()+".blocked").close();
  assert(!rgds::menu_hold());   // the paused menu SIGSTOPs this engine instead
}
''')

    def test_all_scales_scroll_offsets_and_modes_preserve_original_sample_addresses(self):
        self.compile_run(r'''
#include "core/gpu/rgds_sample_plan.hpp"
#include <cassert>
#include <cstdio>
int main() {
  rgds::SamplePlan plan;
  unsigned cases=0;
  for (unsigned scale=2;scale<=5;++scale) for (unsigned smooth=0;smooth<2;++smooth)
  for (unsigned offset=0;offset<512;++offset) {
    plan.update(scale,offset,smooth);
    const auto before=plan.phase;
    plan.update(scale,offset+512,smooth);
    for (unsigned p=0;p<3;++p) for (unsigned x=0;x<256;++x) {
      const unsigned count=2+(x&1), dx=x*2+x/2+std::min(p,count-1);
      unsigned a,b;
      if (smooth) {
        a=((4*dx+1)*scale/10+offset*scale)%(512*scale);
        b=((4*dx+3)*scale/10+offset*scale)%(512*scale);
      } else {
        a=b=((x+offset)&511)*scale+std::min(scale-1,(2*p+1)*scale/(2*count));
      }
      const auto t=plan.phase[p][x];
      assert(t.first==before[p][x].first && t.second==before[p][x].second);
      assert(t.first==(a<256*scale?a:rgds::SamplePlan::absent));
      assert(t.second==(b<256*scale?b:rgds::SamplePlan::absent));
      if (scale==2 && !smooth && p==2 && (x&1))
        assert(t.first==plan.phase[1][x].first); // Reused 2x panel sample.
      ++cases;
    }
  }
  std::printf("sample plan: %u tap pairs match the original sampler\n",cases);
  // The optimized interleave must preserve the exact 2/3-pixel DS footprint.
  std::array<unsigned,640> original{},interleaved{};
  for(unsigned phase=0;phase<3;++phase){
    for(unsigned x=0;x<256;++x){
      const unsigned dx=x*640/256+phase;
      if(dx<(x+1)*640/256)original[dx]=phase*256+x;
    }
    for(unsigned x=0;x<128;++x){
      if(phase==2)interleaved[5*x+4]=phase*256+2*x+1;
      else{interleaved[5*x+phase]=phase*256+2*x;interleaved[5*x+phase+2]=phase*256+2*x+1;}
    }
  }
  assert(original==interleaved);
}
''')


if __name__ == '__main__': unittest.main()
