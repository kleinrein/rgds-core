"""Library scrolling geometry is shared by drawing and touch hit testing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('clang++'),'Requires clang++')
class LibraryViewportTests(unittest.TestCase):
    def test_reveal_continuous_scroll_rows_and_rail_endpoints(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'test.cpp'
            path.write_text(r'''#include "launcher/library_viewport.hpp"
#include "launcher/model.hpp"
#include <cassert>
int main(){
 rgds::LibraryViewport list;
 assert(list.maximum(0)==0 && list.row_at(148,0)==-1);
 list.reveal(3,62);assert(list.scroll==0);
 list.reveal(4,62);assert(list.scroll==0); // the taller list keeps this row visible
 assert(list.row_at(147,62)==0 && list.row_at(194,62)==0 && list.row_at(195,62)==-1);
 // Row 6 starts at 147 + 6*54 - 27 = 444, so its first 19 pixels
 // remain visible before the renderer's exclusive bottom clip at 463.
 list.scroll=27;assert(list.first()==0 && list.last(62)==7);
 assert(list.row_at(147,62)==0 && list.row_at(174,62)==1);
 list.seek(10000,16,62);assert(list.scroll==list.maximum(62));
 assert(list.last(62)==62 && list.thumb_top(62)+list.thumb(62)==463);
 list.seek(-10,16,62);assert(list.scroll==0);
 list.reveal(61,62);assert(list.scroll==3032);
 list.clamp(2);assert(list.scroll==0 && list.thumb(2)==316);
 list.select(10,62,100,true);assert(list.scroll==0 && list.moving);
 list.update(195);assert(list.scroll>0 && list.scroll<list.target);
 int halfway=list.scroll;list.select(0,62,195,true);assert(list.scroll==halfway);
 list.update(385);assert(list.scroll==0 && !list.moving);
 list.select(10,62,400,false);assert(list.scroll==278 && !list.moving);
 list.select(20,62,500,true);list.seek(-10,16,62);assert(!list.moving && list.scroll==0);
 assert(rgds::library_title("0123 - Puzzle 2 (Europe) (En,Fr) (Rev 1)")=="Puzzle 2");
 assert(rgds::library_title("Puzzle (Advance)")=="Puzzle (Advance)");
 assert(rgds::library_title("Sims 2, The (Europe)")=="The Sims 2");
}''')
            binary=Path(folder)/'test'
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(path),'-o',str(binary)],check=True,capture_output=True)
            subprocess.run([str(binary)],check=True,capture_output=True)
