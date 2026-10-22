"""Deterministic presentation cadence, pauses and frame-time statistics."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ required')
class FrameStatsTests(unittest.TestCase):
    def test_cadence_and_menu_time(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'test.cpp'
            binary = root / 'test'
            source.write_text(
                r'''#include "platform/frame_stats.hpp"
#include "platform/nnddss_stats.hpp"
#include <cassert>
#include <cmath>
int main(int argc,char** argv){
 rgds::FrameCounterRate game;
 assert(!game.sample(1,100,false));assert(game.sample(1000001,115,false));assert(game.fps==15);
 game.sample(1100000,116,true);assert(!game.sample(9000000,116,false));
 assert(game.sample(10000000,176,false));assert(game.fps==60);
 game.reset();game.sample(1,0xfffffff0u,false);game.sample(1000001,44,false);assert(game.fps==60);
 assert(argc==2);rgds::FrameStats stats;
 for(unsigned i=0;i<120;i++)stats.present(1+i*16667,false);
 assert(stats.frames()==0);stats.enabled=true;
 for(unsigned i=0;i<121;i++)stats.present(1+i*16667,false);
 assert(stats.frames()==120 && std::abs(stats.fps()-60)<.01);
 assert(stats.percentile(.95)==16.7);stats.write(argv[1]);
 stats.present(3000000,true);stats.present(9000000,true);
 stats.present(10000000,false);stats.present(10016667,false);
 assert(stats.frames()==121 && stats.percentile(.99)==16.7);
 stats.reset();assert(stats.frames()==0);
 for(unsigned i=0;i<62;i++)stats.present(1+i*33333,false);
 assert(std::abs(stats.fps()-30)<.01 && std::abs(stats.percentile(.95)-33.3)<.001);
}'''
            )
            subprocess.run(
                ['clang++', '-std=c++17', '-O2', '-I' + str(ROOT / 'src'),
                 str(source), '-o',
                 str(binary)],
                check=True,
                capture_output=True
            )
            output = root / 'stats'
            subprocess.run([str(binary), str(output)], check=True)
            self.assertIn('fps=60.0', output.read_text())
            self.assertIn('frames=120', output.read_text())
