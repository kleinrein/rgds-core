"""Library zoom: a finger on each screen pinches the library out to a wall of games (library_zoom.hpp)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class ZoomLogicTests(unittest.TestCase):
    def test_pinch_levels_snap_and_wall_geometry(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "zoom.cpp"
            source.write_text(
                r'''#include "launcher/library_zoom.hpp"
#include <cassert>
#include <cmath>
using namespace rgds;
int main(){
 // One finger alone is no pinch; the second joins it, and only a real change in span engages it.
 CrossPinch p;
 assert(!p.touch(true,0,320,300));
 assert(p.touch(false,0,320,200) && p.both() && !p.engaged && p.levels()==0);
 p.touch(false,1,320,190);assert(!p.engaged);
 // Closing toward the hinge zooms out (positive levels), spreading zooms in.
 p.touch(true,1,320,360);p.touch(false,1,320,140);assert(p.engaged && p.levels()>0);
 const double closed=p.levels();
 p.touch(true,1,320,100);p.touch(false,1,320,460);assert(p.levels()<0);
 assert(p.touch(true,2,320,100) && !p.both() && p.levels()==0);
 // The zoom follows the fingers, gives a little past either end, and snaps to a level when they part.
 LibraryZoom z;z.pinch_begin();z.pinch(closed);assert(z.level>0 && z.level<1);
 z.pinch(.4);assert(z.release(0,true)==1 && z.animating);
 for(unsigned t=0;t<=400;t+=16)z.update(t);
 assert(z.level==1 && z.resting());
 z.pinch_begin();z.pinch(5);assert(z.level>2 && z.level<2.25);
 assert(z.release(0,false)==2 && z.level==2);
 z.pinch_begin();z.pinch(-5);assert(z.level<0 && z.level>-.25);
 assert(z.release(0,false)==0 && !z.shown());
 // A short pinch settles back where it began.
 z.go(1,0,false);z.pinch_begin();z.pinch(.1);assert(z.release(0,false)==1);
 // The walls: six covers a row, then ten icons; taps land on tiles, never past the last game.
 const auto wall=ZoomWall::at(1),dense=ZoomWall::at(2);
 assert(wall.columns==6 && dense.columns==10 && wall.left()>0 && dense.left()>0);
 assert(wall.hit(wall.x(7)+5,wall.y(7,0)+5,0,72)==7);
 assert(wall.hit(wall.x(5)+5,wall.y(70,0)+5,0,72)==-1);
 assert(wall.hit(320,100,0,72)==-1);
 const int centred=wall.centred(40,72),row=wall.y(40,centred);
 assert(row>ZoomWall::top && row+wall.tile<ZoomWall::bottom);
 const int shown=wall.revealed(71,72,0);assert(shown==wall.maximum(72) && wall.y(71,shown)+wall.tile<=ZoomWall::bottom);
 assert(wall.first(0)==0 && wall.last(0,72)>=18 && dense.last(0,72)>=50);
}
'''
            )
            binary = root / "zoom"
            subprocess.run(
                ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Isrc",
                 str(source), "-o",
                 str(binary)],
                cwd=ROOT,
                check=True
            )
            subprocess.run([str(binary)], check=True)


# Two fingers in the preview window: its upper half stands in for the top panel's touch screen.
PINCH = (
    "1300 finger-down 0.5 0.70 1\n1340 finger-down 0.5 0.30 2\n"
    "1380 finger-move 0.5 0.66 1\n1420 finger-move 0.5 0.34 2\n"
    "1460 finger-move 0.5 0.62 1\n1500 finger-move 0.5 0.38 2\n"
    "1540 finger-move 0.5 0.60 1\n1580 finger-move 0.5 0.40 2\n"
    "1620 finger-up 0.5 0.60 1\n1640 finger-up 0.5 0.40 2\n"
)


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class ZoomUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.roms = self.root / "roms"
        self.roms.mkdir()
        for name in ("Advance", "Bomber", "Castle", "Dragon", "Elite", "Final", "Golden", "Hotel"):
            for number in range(1, 7):
                (self.roms / f"{name} {number}.nds").write_bytes(b"fixture")
        self.state = self.root / "state.txt"
        self.state.write_text('1 0 0 ""\n')
        (self.root / "ui.ini").write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")

    def run_ui(self, script, seconds=4, keep_zoom=False):
        self.state.write_text('1 0 0 ""\n')  # every run starts on the first game
        if not keep_zoom:  # and in the library as it is, unless the test carries the zoom over
            (self.root / "ui.ini").write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")
        path = self.root / "input.txt"
        path.write_text(script)
        result = subprocess.run(
            [
                str(BINARY), "--preview", "--library", "--data",
                str(self.root / "data"), "--preferences",
                str(self.root / "preferences.ini"), "--state",
                str(self.state), "--rom-dir",
                str(self.roms), "--input-script",
                str(path), "--seconds",
                str(seconds), "--screenshot",
                str(self.root / "screen.bmp")
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"),
            capture_output=True,
            text=True,
            timeout=15
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def selected(self):
        return int(self.state.read_text().split()[1])

    def test_the_wall_moves_in_rows(self):
        # On the wall Down is a row of six.
        self.run_ui(PINCH + "2200 key Down\n2300 key Right\n")
        self.assertEqual(self.selected(), 7)

    def test_b_goes_back_and_the_wall_is_kept(self):
        self.run_ui(PINCH + "2200 key Escape\n")
        self.assertIn("library_zoom=1", (self.root / "ui.ini").read_text())
        # The next start opens on the wall: Down is a row of six at once.
        self.run_ui("1500 key Down\n", keep_zoom=True)
        self.assertEqual(self.selected(), 6)

    def test_the_header_button_steps_through_the_levels(self):
        self.run_ui("1500 click 598 99\n2000 key Down\n")
        self.assertEqual(self.selected(), 6)
        # Twice more: the dense wall, then the library as it was (where Down is nothing).
        self.run_ui("1500 click 598 99\n1800 click 598 99\n2100 click 598 99\n2600 key Down\n")
        self.assertEqual(self.selected(), 0)
        self.assertNotIn("library_zoom=", (self.root / "ui.ini").read_text())

    def test_a_tap_picks_and_a_second_tap_zooms_into_the_game(self):
        self.run_ui(PINCH + "2200 click 271 292\n")
        self.assertEqual(self.selected(), 8)
        self.run_ui(PINCH + "2200 click 271 292\n2500 click 271 292\n2900 key Right\n")
        self.assertEqual(self.selected(), 9)
        self.assertFalse((self.root / "data/session/core.json").exists())

    def test_one_finger_on_the_lower_screen_is_unchanged(self):
        self.run_ui("1300 finger-down 0.5 0.70 1\n1340 finger-move 0.3 0.70 1\n1380 finger-up 0.3 0.70 1\n")
        self.assertGreater(self.selected(), 0)


if __name__ == "__main__":
    unittest.main()
