"""A fresh pause menu ignores controls that were already held when it opened.

SDL posts every joystick button that is down on its first update as a new
press, and the touch decoder starts a contact that began before it opened. The
menu opens with Resume focused, so a held A (accelerate), B, Start, Select or
Menu, or a stylus resting over Resume, used to close the menu at once.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from library import game_key

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"
RESUME = (150, 120)  # The Resume tile on the lower panel.


@unittest.skipUnless(BINARY.exists(), "Build the host launcher first")
class HeldInputAtOpenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve();self.data = self.root / "data"
        self.rom = self.root / "fixture.nds";self.rom.write_bytes(bytes(0x640))
        (self.data / "library" / game_key(self.rom)).mkdir(parents=True)

    def run_menu(self, held=None, script="", events=None):
        command = [str(BINARY), "--preview", "--game-menu", "--game-core", "rgds-experimental", "--game-rom", str(self.rom),
                   "--app", str(ROOT), "--data", str(self.data), "--seconds", "1"]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy");env.pop("RGDS_PREVIEW_HELD_BUTTONS", None)
        if held is not None:env["RGDS_PREVIEW_HELD_BUTTONS"] = ",".join(map(str, held))
        if script:
            (self.root / "input").write_text(script);command += ["--input-script", str(self.root / "input")]
        if events is not None:
            recording = self.root / "touch.events"
            recording.write_bytes(b"".join(struct.pack("<qqHHi", ms // 1000, (ms % 1000) * 1000, kind, code, value)
                                           for ms, kind, code, value in events))
            command += ["--touch-recording", str(recording)]
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [line for line in result.stdout.splitlines() if line.startswith("RGDS menu helper:")]

    @staticmethod
    def contact(ms, x, y, press=False):
        return ([(ms, 1, 330, 1)] if press else []) + [(ms, 3, 57, 3), (ms, 3, 53, x), (ms, 3, 54, y), (ms, 0, 2, 0), (ms, 0, 0, 0)]

    @staticmethod
    def lift(ms):
        return [(ms, 1, 330, 0), (ms, 0, 2, 0), (ms, 0, 0, 0)]

    def test_buttons_held_at_open_do_not_resume(self):
        # A, B, Select, Start and Menu all reach Resume on a fresh menu.
        for button in (0, 1, 6, 7, 8):
            with self.subTest(button=button):
                lines = self.run_menu(held=[button])
                self.assertIn("RGDS menu helper: held at open: button %d; ignored until released" % button, lines)
                self.assertFalse([line for line in lines if " resume after " in line], lines)

    def test_a_held_button_works_again_after_release_and_fresh_presses_act(self):
        lines = self.run_menu(held=[0], script="200 joy-up 0\n400 joy-down 0\n")
        resumes = [line for line in lines if " resume after " in line]
        self.assertEqual(len(resumes), 1, lines);self.assertIn("after joystick button 0", resumes[0])
        lines = self.run_menu(script="300 joy-down 1\n")
        self.assertTrue([line for line in lines if " resume after joystick button 1 " in line], lines)

    def test_stylus_resting_over_resume_at_open_is_not_a_tap(self):
        x, y = RESUME
        held = self.contact(0, x, y) + self.contact(60, x, y) + self.lift(150)
        lines = self.run_menu(events=held)
        self.assertIn("RGDS menu helper: held at open: touch; ignored until released", lines)
        self.assertFalse([line for line in lines if " resume after " in line], lines)
        # The next tap after the resting contact lifted still selects Resume.
        lines = self.run_menu(events=held + self.contact(400, x, y, press=True) + self.lift(450))
        self.assertTrue([line for line in lines if " resume after touch up at %d,%d " % (x, y) in line], lines)

    def test_a_fresh_tap_after_open_resumes(self):
        x, y = RESUME
        lines = self.run_menu(events=self.contact(300, x, y, press=True) + self.lift(350))
        self.assertNotIn("RGDS menu helper: held at open: touch; ignored until released", lines)
        self.assertTrue([line for line in lines if " resume after touch up at %d,%d " % (x, y) in line], lines)


@unittest.skipUnless(shutil.which("clang++"), "Host C++ compiler required")
class HeldTouchDecoderTests(unittest.TestCase):
    def test_held_contact_is_ignored_until_every_contact_lifts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder);source = root / "held.cpp";binary = root / "held"
            source.write_text(r'''#include "platform/touch.hpp"
#include <cassert>
void contact(rgds::TouchDecoder& t,int id,int x,int y){t.feed(3,57,id);t.feed(3,53,x);t.feed(3,54,y);t.feed(0,2,0);}
int main(){
 // Goodix Type A: a stylus already down when the reader opened sends no BTN_TOUCH press.
 rgds::TouchDecoder t;t.type_a=true;t.hold_until_release();
 contact(t,3,150,120);assert(t.feed(0,0,0).empty());
 contact(t,3,170,130);assert(t.feed(0,0,0).empty());
 contact(t,3,170,130);contact(t,4,500,400);assert(t.feed(0,0,0).empty());
 contact(t,4,500,400);assert(t.feed(0,0,0).empty());
 t.feed(1,330,0);t.feed(0,2,0);assert(t.feed(0,0,0).empty());
 t.feed(1,330,1);contact(t,5,150,120);auto down=t.feed(0,0,0);assert(down.size()==1 && down[0].kind==0 && down[0].x==150);
 t.feed(1,330,0);t.feed(0,2,0);auto up=t.feed(0,0,0);assert(up.size()==1 && up[0].kind==2);
 // Without a held contact the same frames are an ordinary tap.
 rgds::TouchDecoder fresh;fresh.type_a=true;contact(fresh,3,150,120);assert(fresh.feed(0,0,0)[0].kind==0);
 // An idle panel: the hold ends at the first empty frame and costs no tap.
 rgds::TouchDecoder idle;idle.type_a=true;idle.hold_until_release();idle.feed(0,2,0);assert(idle.feed(0,0,0).empty());
 idle.feed(1,330,1);contact(idle,6,40,40);assert(idle.feed(0,0,0)[0].kind==0);
}
''')
            build = subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"), str(source), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":unittest.main()
