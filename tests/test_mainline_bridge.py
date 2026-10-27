"""Exercise real bridge code using the kernel's physical button numbering."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'C++/SDL headers required')
class MainlineBridgeTests(unittest.TestCase):
    def test_raw_mainline_buttons_poll_events_hats_and_reserved_shortcuts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'mainline.cpp'
            source.write_text(CPP)
            flags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
            result = subprocess.run(
                [
                    'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(root / 'test')
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for core in ('drastic', 'dsperate', 'nnddss'):
                with self.subTest(core=core):
                    result = subprocess.run(
                        [str(root / 'test'), str(root), core],
                        capture_output=True,
                        text=True,
                        timeout=5,
                        env=dict(os.environ, RGDS_MENU_EXTERNAL='0')
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rocknix_joypad_on_rgds_os_moves_f_and_directions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'joypad.cpp'
            source.write_text(JOYPAD_CPP)
            flags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
            result = subprocess.run(
                [
                    'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(root / 'test')
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for core in ('drastic', 'rgds-experimental'):
                for platform in ('independent', 'stock'):
                    with self.subTest(core=core, platform=platform):
                        environment = dict(os.environ, RGDS_MENU_EXTERNAL='0', RGDS_SESSION_PLATFORM=platform)
                        result = subprocess.run(
                            [str(root / 'test'), str(root), core, platform],
                            capture_output=True,
                            text=True,
                            timeout=5,
                            env=environment
                        )
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


# ROCKNIX's retrogame_joypad as SDL numbers it on RG DS OS: faces, shoulders, triggers, Select, Start as on
# gpio-keys (0-9), then F (BTN_MODE) at 10, the stick clicks at 11-12 and the directions at 13-16.
JOYPAD_CPP = r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::array<Uint8,17> physical{};std::deque<SDL_Event> native_events;
SDL_Joystick* pad=reinterpret_cast<SDL_Joystick*>(1);
Uint8 raw_button(SDL_Joystick*,int button){return button>=0 && button<17?physical[button]:0;}
Uint8 raw_hat(SDL_Joystick*,int){return 0;}
const char* raw_name(SDL_Joystick*){return "retrogame_joypad";}
SDL_Joystick* raw_from_id(SDL_JoystickID id){return id==1?pad:nullptr;}
int raw_peep(SDL_Event* out,int count,SDL_eventaction,Uint32,Uint32){int n=0;while(n<count && !native_events.empty()){out[n++]=native_events.front();native_events.pop_front();}return n;}
int raw_poll(SDL_Event* out){return SDL_PeepEvents(out,1,SDL_GETEVENT,0,0xffffffff);}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"SDL_JoystickName"))return reinterpret_cast<void*>(&raw_name);
 if(!std::strcmp(name,"SDL_JoystickFromInstanceID"))return reinterpret_cast<void*>(&raw_from_id);
 if(!std::strcmp(name,"SDL_JoystickGetButton"))return reinterpret_cast<void*>(&raw_button);
 if(!std::strcmp(name,"SDL_JoystickGetHat"))return reinterpret_cast<void*>(&raw_hat);
 if(!std::strcmp(name,"SDL_PollEvent"))return reinterpret_cast<void*>(&raw_poll);
 if(!std::strcmp(name,"SDL_PeepEvents"))return reinterpret_cast<void*>(&raw_peep);
 return nullptr;
}
void button(int raw,bool down=true){SDL_Event e{};e.type=down?SDL_JOYBUTTONDOWN:SDL_JOYBUTTONUP;e.jbutton.which=1;e.jbutton.button=Uint8(raw);e.jbutton.state=down?SDL_PRESSED:SDL_RELEASED;native_events.push_back(e);}
int main(int argc,char** argv){
 assert(argc==4);fs::path root=argv[1];setenv("RGDS_CORE",argv[2],1);setenv("RGDS_SHARED_MENU","1",1);
 const bool os=std::string(argv[3])=="independent";
 setenv("RGDS_CONTROL_FILE",(root/"control").c_str(),1);initialize();control_shortcut.mode="anbernic";control_shortcut.action="menu";
 assert(mainline_joystick(pad)==os);
 if(!os)return 0; // the stock firmware's driver keeps its own numbering
 physical[1]=1;assert(SDL_JoystickGetButton(pad,0)==1); // BTN_EAST: A
 physical.fill(0);physical[9]=1;assert(SDL_JoystickGetButton(pad,7)==1); // Start
 physical.fill(0);physical[13]=physical[16]=1;
 assert(SDL_JoystickGetHat(pad,0)==(SDL_HAT_UP|SDL_HAT_RIGHT));
 SDL_Event e{};button(13);assert(SDL_PollEvent(&e)==1);
 assert(core=="drastic"?e.type==SDL_JOYBUTTONDOWN && e.jbutton.button==20:e.type==SDL_JOYHATMOTION && e.jhat.value==SDL_HAT_UP);
 button(13,false);assert(SDL_PollEvent(&e)==1);
 physical.fill(0);physical[10]=1;fs::remove(control+".menu");button(10); // F opens the menu
 while(SDL_PollEvent(&e))assert(e.type==SDL_JOYBUTTONUP || (e.type==SDL_JOYHATMOTION && e.jhat.value==0));
 assert(fs::exists(control+".menu"));
 physical.fill(0);physical[6]=1;assert(SDL_JoystickGetButton(pad,10)==1); // L2 is a trigger, not the menu
}
'''

CPP = r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::array<Uint8,17> physical{};std::deque<SDL_Event> native_events;
SDL_Joystick* pad=reinterpret_cast<SDL_Joystick*>(1);
Uint8 raw_button(SDL_Joystick*,int button){return button>=0 && button<17?physical[button]:0;}
Uint8 raw_hat(SDL_Joystick*,int){return 0;}
const char* raw_name(SDL_Joystick* stick){return stick==pad?"gpio-keys-control":"USB gamepad";}
SDL_Joystick* raw_from_id(SDL_JoystickID id){return id==1?pad:nullptr;}
int raw_peep(SDL_Event* out,int count,SDL_eventaction,Uint32,Uint32){int n=0;while(n<count && !native_events.empty()){out[n++]=native_events.front();native_events.pop_front();}return n;}
int raw_poll(SDL_Event* out){return SDL_PeepEvents(out,1,SDL_GETEVENT,0,0xffffffff);}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"SDL_JoystickName"))return reinterpret_cast<void*>(&raw_name);
 if(!std::strcmp(name,"SDL_JoystickFromInstanceID"))return reinterpret_cast<void*>(&raw_from_id);
 if(!std::strcmp(name,"SDL_JoystickGetButton"))return reinterpret_cast<void*>(&raw_button);
 if(!std::strcmp(name,"SDL_JoystickGetHat"))return reinterpret_cast<void*>(&raw_hat);
 if(!std::strcmp(name,"SDL_PollEvent"))return reinterpret_cast<void*>(&raw_poll);
 if(!std::strcmp(name,"SDL_PeepEvents"))return reinterpret_cast<void*>(&raw_peep);
 return nullptr;
}
void button(int raw,bool down=true,int which=1){SDL_Event e{};e.type=down?SDL_JOYBUTTONDOWN:SDL_JOYBUTTONUP;e.jbutton.which=which;e.jbutton.button=Uint8(raw);e.jbutton.state=down?SDL_PRESSED:SDL_RELEASED;native_events.push_back(e);}
int main(int argc,char** argv){
 assert(argc==3);fs::path root=argv[1];setenv("RGDS_CORE",argv[2],1);setenv("RGDS_SHARED_MENU","1",1);
 setenv("RGDS_CONTROL_FILE",(root/"control").c_str(),1);initialize();control_shortcut.mode="anbernic";control_shortcut.action="menu";
 physical[1]=1; // BTN_EAST is physical A, SDL raw button 1.
 assert(SDL_JoystickGetButton(pad,0)==1 && SDL_JoystickGetButton(pad,1)==0);
 {NativeInputGuard native(reading_native_buttons);assert(SDL_JoystickGetButton(pad,0)==0 && SDL_JoystickGetButton(pad,1)==1);}
 SDL_Event e{};button(1);assert(SDL_PollEvent(&e)==1 && e.type==SDL_JOYBUTTONDOWN && e.jbutton.button==0);
 button(0);assert(SDL_PeepEvents(&e,1,SDL_GETEVENT,0,0xffffffff)==1 && e.jbutton.button==1);
 physical.fill(0);physical[2]=1;assert(SDL_JoystickGetButton(pad,3)==1); // physical X
 physical.fill(0);physical[9]=1;assert(SDL_JoystickGetButton(pad,7)==1); // physical Start
 physical.fill(0);physical[8]=1;assert(SDL_JoystickGetButton(pad,6)==1); // physical Select
 physical.fill(0);physical[12]=physical[15]=1;
 assert(SDL_JoystickGetHat(pad,0)==(SDL_HAT_UP|SDL_HAT_RIGHT));
 if(core=="drastic")assert(SDL_JoystickGetButton(pad,20)==1 && SDL_JoystickGetButton(pad,23)==1);
 button(12);assert(SDL_PollEvent(&e)==1);assert(core=="drastic"?e.type==SDL_JOYBUTTONDOWN && e.jbutton.button==20:e.type==SDL_JOYHATMOTION && e.jhat.value==SDL_HAT_UP);
 button(15);assert(SDL_PollEvent(&e)==1);assert(core=="drastic"?e.jbutton.button==23:e.jhat.value==(SDL_HAT_UP|SDL_HAT_RIGHT));
 button(12,false);assert(SDL_PollEvent(&e)==1);assert(core=="drastic"?e.type==SDL_JOYBUTTONUP && e.jbutton.button==20:e.jhat.value==SDL_HAT_RIGHT);
 physical.fill(0);physical[16]=1;fs::remove(control+".menu");button(16);
 while(SDL_PollEvent(&e))assert(e.type==SDL_JOYBUTTONUP || (e.type==SDL_JOYHATMOTION && e.jhat.value==0));
 assert(fs::exists(control+".menu"));
 assert(SDL_JoystickGetButton(pad,8)==0 && SDL_JoystickGetButton(pad,16)==0); // Menu is never save-state.
 physical.fill(0);physical[6]=1;assert(SDL_JoystickGetButton(pad,10)==1);
 control_mapping.fast_forward_source=12;assert(SDL_JoystickGetButton(pad,10)==0);button(6);assert(SDL_PollEvent(&e)==0);
 control_mapping.fast_forward_source=rgds::control_none;control_mapping.sources[0]=12;
 assert(SDL_JoystickGetButton(pad,0)==1);button(6);assert(SDL_PollEvent(&e)==1 && e.jbutton.button==0);
 button(1,true,2);assert(SDL_PollEvent(&e)==1 && e.jbutton.button==1); // Other pads retain their own numbering.
}
'''
