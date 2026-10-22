"""Carousel input uses elapsed animation time even when no frames were drawn."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class CarouselTimingTests(unittest.TestCase):
    def test_delayed_input_settles_animation_before_hit_testing_and_drag_origin(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "timing.cpp"
            source.write_text(
                r'''#include "launcher/carousel.hpp"
#include <cassert>
#include <cmath>
int main(){
 rgds::Carousel c;c.reset(1);c.select(0,20);
 // Thumb-edge grabs retain the exact position, even in large libraries.
 for(size_t count: {size_t(2),size_t(5),size_t(80),size_t(5000)}){
  for(double fraction: {0.0,.37,1.0}){
   double position=(count-1)*fraction;
   auto rail=rgds::RailGeometry::at(count,position);
   for(int x: {rail.thumb_left,rail.thumb_left+rail.thumb_width-1}){
    rgds::Carousel seek;seek.position=position;seek.seek_down(x,440,count,1000);
    seek.drag(x,count);assert(std::abs(seek.position-position)<1e-8);
    assert(seek.drag(-1000,count)==0);assert(seek.drag(2000,count)==count-1);
   }
  }
 }
 // A track press seeks, while empty/single-item libraries stay bounded.
 rgds::Carousel seek;seek.seek_down(550,440,5,1000);assert(seek.drag(550,5)==4);
 seek.seek_down(100,440,1,1000);assert(seek.drag(550,1)==0);
 assert(seek.drag(550,0)==0);
 // Presses respond immediately, release continuously, then return to idle.
 rgds::RailPress press;assert(!press.animating(1000,true));
 assert(press.set(true,1000,true));assert(press.value(1000)>0);
 assert(press.value(1050)>press.value(1000));assert(press.value(1100)==1);
 assert(!press.animating(1100,true));
 press.set(false,1200,true);assert(press.value(1200)==1);
 double released=press.value(1250);assert(released>0 && released<1);
 press.set(true,1250,true);assert(std::abs(press.value(1250)-released)<1e-8);
 press.set(false,1350,true);assert(press.value(1530)==0 && !press.animating(1530,true));
 press.set(true,1600,false);assert(press.value(1600,false)==1 && !press.animating(1600,false));
 press.set(false,1601,false);assert(press.value(1601,false)==0);
 // No update/draw happens for 280 ms. The next pointer-down must use the
 // completed target rather than the last rendered position at Games (1).
 assert(c.position==1 && c.moving);c.down(320,205,false,300);
 assert(c.position==0 && c.start_position==0 && !c.moving);
 assert(c.tapped(320,4)==0 && std::abs(c.position)<.08);
 // The drag origin is also the current target after an interrupted frame.
 c.dragging=false;c.reset(0);c.select(2,400);c.down(320,205,false,700);
 assert(c.start_position==2);assert(c.drag(184,5)==3);
 // A time sync must preserve an active drag rather than advance a tween.
 auto before=c.position;c.select(4,710);c.sync_input(1000);assert(c.position==before);
 // Arrow navigation starts its next tween from the elapsed previous target.
 c.dragging=false;c.reset(1);c.select(0,1200);c.sync_input(1500);c.select(1,1500);
 assert(c.from==0 && c.target==1 && c.moving);
 // A press before the animation has finished uses its actual intermediate
 // position; input synchronization does not force every tween to finish.
 c.reset(1);c.select(0,1600);c.down(320,205,false,1650);
 assert(c.position>0 && c.position<1 && c.start_position==c.position);
}
'''
            )
            binary = root / "timing"
            subprocess.run(
                [
                    "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                    str(source), "-o",
                    str(binary)
                ],
                check=True,
                capture_output=True,
                text=True
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
