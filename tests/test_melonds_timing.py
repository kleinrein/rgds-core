"""The FF presentation limiter must preserve normal output and resume promptly."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MelonDSPresentationTests(unittest.TestCase):
    def test_normal_output_fast_limit_and_stall_recovery(self):
        compiler = shutil.which('c++')
        if not compiler: self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            source = directory / 'timing.cpp'
            source.write_text(
                r'''
#include "src/experimental/melonds/presentation.hpp"
#include <cassert>
int main(){
    rgds_melonds::PresentationClock clock;
    // Every normal frame is presented, including closely spaced catch-up frames.
    for(unsigned long long us=1;us<100000;us+=100)assert(clock.due(false,false,us));
    unsigned presentations=0;
    for(unsigned long long us=1;us<=1000000;us+=100)presentations+=clock.due(true,false,us);
    assert(presentations>=59 && presentations<=60);
    // A stall produces a single current frame, with no queued presentation debt.
    assert(clock.due(true,false,5000000));
    assert(!clock.due(true,false,5000001));
    assert(clock.due(false,false,5000002));
    assert(clock.due(true,false,5000003));
    clock.reset();assert(clock.due(true,false,5000004));
    // Controlled experiments can retain the previous every-frame behavior.
    for(unsigned long long us=5000005;us<5001005;us++)assert(clock.due(true,true,us));
}
'''
            )
            binary = directory / 'timing'
            subprocess.run(
                [compiler, '-std=c++17', '-I', str(ROOT),
                 str(source), '-o', str(binary)],
                check=True,
                capture_output=True,
                text=True
            )
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)


if __name__ == '__main__': unittest.main()
