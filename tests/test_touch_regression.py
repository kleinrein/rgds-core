"""Goodix Type A captures and real SDL finger events, with no device access."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/rgds-launcher"


@unittest.skipUnless(shutil.which("clang++"), "Requires host C++")
class TouchDecoderRegressionTests(unittest.TestCase):
    def test_raw_protocols_contact_identity_framing_and_loss_recovery(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "touch.cpp"
            source.write_text(r'''#include "platform/touch.hpp"
#include <cassert>
void contact(rgds::TouchDecoder& t,int id,int x,int y){t.feed(3,57,id);t.feed(3,53,x);t.feed(3,54,y);t.feed(0,2,0);}
int main(){
 rgds::TouchDecoder t;t.type_a=true;
 t.feed(1,330,1);contact(t,7,180,200);contact(t,9,550,400);auto first=t.feed(0,0,0);assert(first.size()==1 && first[0].kind==0 && first[0].x==180);
 contact(t,9,550,400);contact(t,7,190,210);auto moved=t.feed(0,0,0);assert(moved.size()==1 && moved[0].kind==1 && moved[0].x==190);
 contact(t,9,550,400);contact(t,7,190,210);assert(t.feed(0,0,0).empty());
 contact(t,9,550,400);auto up=t.feed(0,0,0);assert(up.size()==1 && up[0].kind==2 && up[0].x==190);
 contact(t,9,550,400);assert(t.feed(0,0,0).empty());t.feed(1,330,0);t.feed(0,2,0);t.feed(0,0,0);
 t.feed(1,330,1);contact(t,11,320,240);assert(t.feed(0,0,0)[0].kind==0);
 auto lost=t.feed(0,3,0);assert(lost.size()==1 && lost[0].kind==3);contact(t,11,0,0);assert(t.feed(0,0,0).empty());
 t.feed(1,330,1);contact(t,12,320,240);assert(t.feed(0,0,0).empty());
 t.feed(1,330,0);t.feed(0,2,0);t.feed(0,0,0);t.feed(1,330,1);contact(t,13,320,240);assert(t.feed(0,0,0)[0].kind==0);
 rgds::TouchDecoder b;b.feed(3,47,3);b.feed(3,57,42);b.feed(3,53,320);b.feed(3,54,240);assert(b.feed(0,0,0)[0].x==320);
 b.feed(3,47,1);b.feed(3,57,90);b.feed(3,53,40);b.feed(3,54,30);assert(b.feed(0,0,0).empty());
 b.feed(3,47,3);b.feed(3,57,-1);assert(b.feed(0,0,0)[0].kind==2);
 rgds::TouchDecoder single;single.min_x=100;single.max_x=1100;single.min_y=200;single.max_y=1200;
 single.feed(1,330,1);assert(single.feed(0,0,0).empty());single.feed(3,0,600);assert(single.feed(0,0,0).empty());single.feed(3,1,700);auto down=single.feed(0,0,0);assert(down[0].x==320 && down[0].y==240);
 single.feed(3,0,600);single.feed(3,1,700);assert(single.feed(0,0,0).empty());single.feed(1,330,0);assert(single.feed(0,0,0)[0].kind==2);
 // Corners and centre use the same panel coordinates as hit testing.
 for(auto point:std::array<std::pair<int,int>,5>{{{0,0},{640,0},{0,480},{640,480},{320,240}}}){
  rgds::TouchDecoder panel;panel.type_a=true;contact(panel,1,point.first,point.second);
  auto mapped=panel.feed(0,0,0);assert(mapped.size()==1);
  assert(mapped[0].x==std::min(point.first,639) && mapped[0].y==std::min(point.second,479));
 }
 // Type A may report coordinates without BTN_TOUCH or tracking IDs.
 rgds::TouchDecoder coordinates;coordinates.type_a=true;
 coordinates.feed(3,53,160);coordinates.feed(3,54,120);coordinates.feed(0,2,0);
 auto implicit=coordinates.feed(0,0,0);assert(implicit.size()==1 && implicit[0].kind==0);
 coordinates.feed(0,2,0);auto implicit_up=coordinates.feed(0,0,0);assert(implicit_up.size()==1 && implicit_up[0].kind==2);
 // Wake discards an existing gesture until release, without losing a fresh tap.
 rgds::TouchDecoder wake;wake.type_a=true;contact(wake,1,100,100);assert(wake.feed(0,0,0)[0].kind==0);
 wake.discard_contact();contact(wake,1,120,120);assert(wake.feed(0,0,0).empty());
 wake.feed(1,330,0);wake.feed(0,2,0);assert(wake.feed(0,0,0).empty());
 wake.discard_contact();wake.feed(1,330,1);contact(wake,2,200,200);assert(wake.feed(0,0,0)[0].kind==0);
 // Literal Linux AArch64 input_event bytes, split inside type/code/value.
 std::array<unsigned char,24> event{};event[0]=1;event[16]=3;event[18]=57;for(int i=20;i<24;i++)event[i]=255;
 rgds::TouchPackets packets;assert(packets.feed(event.data(),19).empty());auto complete=packets.feed(event.data()+19,5);
 assert(complete.size()==1 && complete[0].micros==1000000 && complete[0].type==3 && complete[0].code==57 && complete[0].value==-1);
}
''')
            binary = root / "touch"
            subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                            str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(BINARY.exists(), "Build host launcher first")
class FingerRoutingRegressionTests(unittest.TestCase):
    @staticmethod
    def recording(events):
        return b"".join(struct.pack("<qqHHi", ms // 1000, (ms % 1000) * 1000, kind, code, value)
                        for ms, kind, code, value in events)

    @staticmethod
    def goodix_tap(ms, x, y, identity):
        return [(ms, 1, 330, 1), (ms, 3, 57, identity), (ms, 3, 53, x), (ms, 3, 54, y),
                (ms, 0, 2, 0), (ms, 0, 0, 0), (ms + 20, 1, 330, 0),
                (ms + 20, 0, 2, 0), (ms + 20, 0, 0, 0)]

    def run_touch(self, script="", events=None, extra=()):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            state = root / "state.txt"
            state.write_text('1 0 0 ""\n')
            inputs = root / "fingers.txt";inputs.write_text(script)
            command = [str(BINARY), "--preview", "--home", "--state", str(state), "--preferences", str(root / "preferences.ini"),
                       "--input-script", str(inputs), "--seconds", "1", *extra]
            if events is not None:
                recording = root / "touch.events";recording.write_bytes(self.recording(events))
                command += ["--touch-recording", str(recording)]
            result = subprocess.run(command, cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return state.read_text().splitlines()

    def test_goodix_type_a_raw_capture_selects_and_opens_settings(self):
        events = [(0, 0, 0, 0)] + self.goodix_tap(20, 184, 205, 7) + self.goodix_tap(300, 320, 205, 9)
        self.assertIn("1 0 0", self.run_touch(events=events))

    def test_sdl_fingers_work_when_no_raw_source_is_available(self):
        script = "20 finger-down 0.2875 0.7135417\n40 finger-up 0.2875 0.7135417\n300 finger-down 0.5 0.7135417\n320 finger-up 0.5 0.7135417\n"
        self.assertIn("1 0 0", self.run_touch(script))

    def test_idle_raw_source_does_not_let_sdl_steal_the_next_gesture(self):
        script = "20 finger-down 0.5 0.7135417\n40 finger-up 0.5 0.7135417\n"
        self.assertEqual(self.run_touch(script, events=[(0, 0, 0, 0)]), ['1 0 0 ""', 'home 1'])

    def test_offset_sdl_arriving_first_cannot_move_a_left_raw_tap_to_the_middle(self):
        # The compositor reports centre while evdev reports the Settings tile
        # to its left. Deliver SDL first, including a longer-held duplicate.
        script = "10 finger-down 0.5 0.7135417\n80 finger-up 0.5 0.7135417\n290 finger-down 0.8 0.7135417\n360 finger-up 0.8 0.7135417\n"
        events = [(0, 0, 0, 0)] + self.goodix_tap(20, 184, 205, 7) + self.goodix_tap(300, 320, 205, 9)
        lines = self.run_touch(script, events)
        self.assertIn('1 0 0', lines)
        self.assertIn('home 0', lines)

    def test_unflagged_mouse_duplicates_cannot_steal_raw_touch(self):
        script = "10 click 320 205\n290 click 500 205\n"
        events = [(0, 0, 0, 0)] + self.goodix_tap(20, 184, 205, 7) + self.goodix_tap(300, 320, 205, 9)
        lines = self.run_touch(script, events)
        self.assertIn('1 0 0', lines)
        self.assertIn('home 0', lines)

    def test_raw_header_and_footer_back_work_with_offset_sdl_duplicates(self):
        for x, y in ((50, 42), (72, 452)):
            for extra, expected in ((('--settings',), False), (('--settings', '--section', 'theme'), True)):
                with self.subTest(position=(x, y), extra=extra):
                    script = "10 finger-down 0.5 0.7135417\n80 finger-up 0.5 0.7135417\n"
                    events = [(0, 0, 0, 0)] + self.goodix_tap(20, x, y, 7)
                    lines = self.run_touch(script, events, extra)
                    self.assertEqual('1 0 0' in lines, expected)

    def test_raw_and_sdl_duplicates_produce_one_gesture(self):
        events = [(0, 0, 0, 0)] + self.goodix_tap(20, 184, 205, 7) + self.goodix_tap(300, 320, 205, 9)
        script = "20 finger-down 0.2875 0.7135417\n40 finger-up 0.2875 0.7135417\n300 finger-down 0.5 0.7135417\n320 finger-up 0.5 0.7135417\n"
        lines = self.run_touch(script, events)
        self.assertIn("1 0 0", lines)
        self.assertIn("home 0", lines)

    def test_top_panel_fingers_are_ignored_and_second_finger_cannot_steal_drag(self):
        script = "20 finger-down 0.5 0.2 9\n40 finger-up 0.5 0.2 9\n60 finger-down 0.5 0.7135417 1\n80 finger-down 0.2875 0.7135417 2\n100 finger-up 0.2875 0.7135417 2\n120 finger-move 0.2875 0.7135417 1\n140 finger-up 0.2875 0.7135417 1\n"
        self.assertEqual(self.run_touch(script), ['1 0 0 ""', "home 2"])

    def test_sdl_focus_loss_releases_source_ownership_without_raw_device(self):
        script = "20 finger-down 0.5 0.7135417 7\n40 focus-lost\n100 finger-down 0.5 0.7135417 9\n120 finger-move 0.2875 0.7135417 9\n140 finger-up 0.2875 0.7135417 9\n"
        self.assertEqual(self.run_touch(script), ['1 0 0 ""', "home 2"])

@unittest.skipUnless(BINARY.exists() and shutil.which('clang++') and shutil.which('sdl2-config'), 'Host SDL compiler required')
class NativeTouchWindowTests(unittest.TestCase):
    def test_kms_top_window_association_still_routes_to_lower_touch_panel(self, panel_width=640, panel_height=480):
        """Emulate two KMS windows without opening a device or reading evdev."""
        import json
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'native.cpp'
            source.write_text(r'''#include <SDL.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cassert>
static Uint32 top=0;
static int width(){return std::atoi(std::getenv("RGDS_TEST_PANEL_WIDTH"));}
static int height(){return std::atoi(std::getenv("RGDS_TEST_PANEL_HEIGHT"));}
extern "C" int SDL_GetNumVideoDisplays(){return 2;}
extern "C" const char* SDL_GetDisplayName(int index){return index?"DSI-2":"DSI-1";}
extern "C" int SDL_GetDisplayBounds(int index,SDL_Rect* rect){*rect={0,index*height(),width(),height()};return 0;}
extern "C" SDL_Window* SDL_CreateWindow(const char* name,int,int,int width,int height,Uint32){
 assert(width==::width() && height==::height());
 auto fn=reinterpret_cast<decltype(&SDL_CreateWindow)>(dlsym(RTLD_NEXT,"SDL_CreateWindow"));
 auto* window=fn(name,0,0,width,height,SDL_WINDOW_HIDDEN);if(!top)top=SDL_GetWindowID(window);return window;
}
extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window* window,int,Uint32){
 auto fn=reinterpret_cast<decltype(&SDL_CreateRenderer)>(dlsym(RTLD_NEXT,"SDL_CreateRenderer"));return fn(window,-1,SDL_RENDERER_SOFTWARE);
}
extern "C" int SDL_PollEvent(SDL_Event* event){
 static Uint32 began=SDL_GetTicks();static int next=0;
 if(next==2 || SDL_GetTicks()-began<Uint32(100+next*40))return 0;
 *event={};event->type=next++?SDL_FINGERUP:SDL_FINGERDOWN;
 event->tfinger.windowID=top;event->tfinger.touchId=1;event->tfinger.fingerId=1;
 event->tfinger.x=200.f/640;event->tfinger.y=130.f/480;return 1;
}
extern "C" int SDL_WaitEventTimeout(SDL_Event*,int){SDL_Delay(5);return 0;}
''')
            library = root/('native.dylib' if os.uname().sysname=='Darwin' else 'native.so')
            flags = subprocess.check_output(['sdl2-config','--cflags','--libs'], text=True).split()
            subprocess.run(['clang++','-std=c++17','-shared','-fPIC',str(source),*flags,'-o',str(library)],check=True,capture_output=True,text=True)
            report = root/'report.json'
            result = subprocess.run([str(BINARY),'--settings','--data',str(root),'--state',str(root/'state'),
                                     '--seconds','1','--cache-report',str(report),'--screenshot',str(root/'panel')],cwd=ROOT,
                                    env=dict(os.environ,SDL_VIDEODRIVER='dummy',RGDS_SDL_LIBRARY=str(library),
                                             RGDS_TEST_PANEL_WIDTH=str(panel_width),RGDS_TEST_PANEL_HEIGHT=str(panel_height),
                                             RGDS_TOUCH_DEV=str(root/'no-device'),RGDS_VOLUME_EXTERNAL='1',RGDS_SYSTEM_MANAGED='1'),
                                    capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(json.loads(report.read_text())['page'],2)
            for panel in ('top','bottom'):
                raw=(root/('panel-'+panel+'.bmp')).read_bytes()
                self.assertEqual(struct.unpack_from('<ii',raw,18),(panel_width,panel_height))

    def test_plus_native_panels_render_and_route_lower_touch(self):
        self.test_kms_top_window_association_still_routes_to_lower_touch_panel(1024,768)


if __name__ == "__main__":
    unittest.main()
