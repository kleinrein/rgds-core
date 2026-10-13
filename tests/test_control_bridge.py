"""Real owned bridge remaps physical SDL routes without devices or emulators."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'),'Host C++/SDL headers required')
class ControlBridgeTests(unittest.TestCase):
    def test_remapping_live_reload_auxiliary_consumption_and_shortcut_release(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'controls.cpp';binary=root/'controls'
            source.write_text(CPP)
            flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
            result=subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(ROOT/'src'),str(source),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for core in ('drastic','dsperate','nnddss'):
                with self.subTest(core=core):
                    result=subprocess.run([str(binary),str(root),core],capture_output=True,text=True,timeout=5,env=dict(os.environ,RGDS_MENU_EXTERNAL='0'))
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)


CPP=r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::array<Uint8,24> physical{};Uint8 physical_hat=0;std::deque<SDL_Event> native_events;
bool nested=false;
Uint8 raw_button(SDL_Joystick*,int button){return button>=0 && button<int(physical.size())?physical[button]:0;}
Uint8 raw_hat(SDL_Joystick*,int){return physical_hat;}
Uint8 raw_controller(SDL_GameController*,SDL_GameControllerButton button){int source=controller_source(button);return source>=8 && source<=11?Uint8((physical_hat&rgds::hat_bit(source))!=0):SDL_JoystickGetButton(nullptr,rgds::physical_button(source));}
Sint16 raw_axis(SDL_GameController*,SDL_GameControllerAxis axis){return (axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT && physical[10]) || (axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT && physical[11])?32767:0;}
int raw_peep(SDL_Event* output,int count,SDL_eventaction,Uint32,Uint32){int at=0;while(at<count && !native_events.empty()){output[at++]=native_events.front();native_events.pop_front();}return at;}
int raw_poll(SDL_Event* output){if(nested)return SDL_PeepEvents(output,1,SDL_GETEVENT,0,0xffffffff);return raw_peep(output,1,SDL_GETEVENT,0,0xffffffff);}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"SDL_JoystickGetButton"))return reinterpret_cast<void*>(&raw_button);
 if(!std::strcmp(name,"SDL_JoystickGetHat"))return reinterpret_cast<void*>(&raw_hat);
 if(!std::strcmp(name,"SDL_GameControllerGetButton"))return reinterpret_cast<void*>(&raw_controller);
 if(!std::strcmp(name,"SDL_GameControllerGetAxis"))return reinterpret_cast<void*>(&raw_axis);
 if(!std::strcmp(name,"SDL_PollEvent"))return reinterpret_cast<void*>(&raw_poll);
 if(!std::strcmp(name,"SDL_PeepEvents"))return reinterpret_cast<void*>(&raw_peep);
 return nullptr;
}
void profile(const std::string& value){std::ofstream(controls_global)<<"schema=1\nshortcut=anbernic\nshortcut_action=menu\n"<<value;test_time+=300;tick();while(!events.empty()){auto event=events.front();events.pop_front();assert(event.type==SDL_JOYBUTTONUP || event.type==SDL_CONTROLLERBUTTONUP || (event.type==SDL_JOYHATMOTION && !event.jhat.value) || (event.type==SDL_CONTROLLERAXISMOTION && !event.caxis.value));remember_control_event(event);}}
void button(int value,bool down=true){SDL_Event event{};event.type=down?SDL_JOYBUTTONDOWN:SDL_JOYBUTTONUP;event.jbutton.which=1;event.jbutton.button=Uint8(value);event.jbutton.state=down?SDL_PRESSED:SDL_RELEASED;native_events.push_back(event);}
int main(int argc,char** argv){
 assert(argc==3);fs::path root=argv[1];setenv("RGDS_CORE",argv[2],1);setenv("RGDS_SHARED_MENU","1",1);
 setenv("RGDS_CONTROL_FILE",(root/"control").c_str(),1);setenv("RGDS_CONTROLS_GLOBAL",(root/"controls.ini").c_str(),1);initialize();
 profile("preset=ab-swap\n");physical[0]=1;assert(SDL_JoystickGetButton(nullptr,0)==0 && SDL_JoystickGetButton(nullptr,1)==1);
 assert(SDL_GameControllerGetButton(nullptr,SDL_GameControllerButton(controller_button(0)))==0);
 assert(SDL_GameControllerGetButton(nullptr,SDL_GameControllerButton(controller_button(1)))==1); // Nested raw joystick query must not map twice.
 SDL_Event event{};button(0);nested=true;assert(SDL_PollEvent(&event)==1 && event.jbutton.button==1);nested=false;
 button(1);assert(SDL_PeepEvents(&event,1,SDL_GETEVENT,0,0xffffffff)==1 && event.jbutton.button==0);
 // A speed binding is reserved from game controls on both event/poll routes.
 physical.fill(0);profile("preset=custom\nfast_forward_button=l2\n");physical[10]=1;
 assert(SDL_JoystickGetButton(nullptr,10)==0);
 assert(SDL_GameControllerGetAxis(nullptr,SDL_CONTROLLER_AXIS_TRIGGERLEFT)==0);
 button(10);assert(SDL_PollEvent(&event)==0);
 event={};event.type=SDL_CONTROLLERAXISMOTION;event.caxis.axis=SDL_CONTROLLER_AXIS_TRIGGERLEFT;event.caxis.value=32767;native_events.push_back(event);assert(SDL_PollEvent(&event)==0);
 physical.fill(0);profile("preset=left-handed\n");physical[3]=1;
 assert(SDL_JoystickGetHat(nullptr,0)==SDL_HAT_UP);
 assert(SDL_GameControllerGetButton(nullptr,SDL_CONTROLLER_BUTTON_DPAD_UP)==1);
 button(3);assert(SDL_PollEvent(&event)==1);if(core=="drastic")assert(event.jbutton.button==20);else assert(event.type==SDL_JOYHATMOTION && event.jhat.value==SDL_HAT_UP);
 if(core!="drastic"){button(0);assert(SDL_PollEvent(&event)==1 && event.jhat.value==(SDL_HAT_UP|SDL_HAT_RIGHT));button(0,false);assert(SDL_PollEvent(&event)==1 && event.jhat.value==SDL_HAT_UP);}
 physical.fill(0);profile("preset=custom\nbind_a=l3\nbind_b=r3\n");physical[9]=physical[12]=1;
 assert(SDL_JoystickGetButton(nullptr,0)==1 && SDL_JoystickGetButton(nullptr,1)==1);
 assert(SDL_JoystickGetButton(nullptr,9)==0 && SDL_JoystickGetButton(nullptr,12)==0);
 assert(SDL_GameControllerGetButton(nullptr,SDL_CONTROLLER_BUTTON_LEFTSTICK)==0 && SDL_GameControllerGetButton(nullptr,SDL_CONTROLLER_BUTTON_RIGHTSTICK)==0);
 button(9);assert(SDL_PollEvent(&event)==1 && event.jbutton.button==0);
 button(12);assert(SDL_PollEvent(&event)==1 && event.jbutton.button==1);
 event={};event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=1;event.cbutton.button=SDL_CONTROLLER_BUTTON_RIGHTSTICK;event.cbutton.state=SDL_PRESSED;native_events.push_back(event);assert(SDL_PollEvent(&event)==1 && event.cbutton.button==controller_button(1));
 rgds::ControlMapping raw;raw.apply({{"preset","custom"},{"bind_a","l3"},{"bind_b","r3"}});unsigned char bits[96]{};bits[313/8]|=1<<(313%8);bits[316/8]|=1<<(316%8);raw.remap_key_bits(bits,sizeof(bits));
 assert(bits[304/8]&(1<<(304%8)));assert(bits[305/8]&(1<<(305%8)));assert(!(bits[313/8]&(1<<(313%8))) && !(bits[316/8]&(1<<(316%8))));
 int type=1,code=313,value=1;raw.remap_evdev_key(type,code,value);assert(type==1 && code==304 && value==1);
 physical.fill(0);profile("preset=custom\nbind_a=l2\n");physical[10]=1;
 assert(SDL_GameControllerGetButton(nullptr,SDL_GameControllerButton(controller_button(0)))==1);
 assert(SDL_GameControllerGetAxis(nullptr,SDL_CONTROLLER_AXIS_TRIGGERLEFT)==0);
 event={};event.type=SDL_CONTROLLERAXISMOTION;event.caxis.which=1;event.caxis.axis=SDL_CONTROLLER_AXIS_TRIGGERLEFT;event.caxis.value=32767;native_events.push_back(event);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN && event.cbutton.button==controller_button(0));
 // A global live reset takes effect without reopening the engine.
 physical.fill(0);physical[0]=1;profile("preset=default\n");assert(SDL_JoystickGetButton(nullptr,0)==1 && SDL_JoystickGetButton(nullptr,1)==0);
 for(bool start_first:{true,false}){
   rgds::ControlShortcut shortcut;auto first=start_first?rgds::control_start:rgds::control_menu,second=start_first?rgds::control_menu:rgds::control_start;
   assert(shortcut.feed(first,true,0)==rgds::ShortcutAction::none);assert(!shortcut.visible_start(10));assert(shortcut.feed(second,true,30)==rgds::ShortcutAction::home);
   assert(shortcut.poll(200)==rgds::ShortcutAction::none);shortcut.feed(rgds::control_menu,false,60);shortcut.feed(rgds::control_start,false,70);assert(!shortcut.visible_start(200));
 }
 control_shortcut=rgds::ControlShortcut{};test_time+=500;fs::remove(control+".menu");fs::remove(control+".home");
 button(8);button(7);assert(SDL_PollEvent(&event)==0);assert(fs::exists(control+".home") && !fs::exists(control+".menu"));test_time+=300;tick();assert(SDL_PollEvent(&event)==0);
 control_shortcut=rgds::ControlShortcut{};delayed_start_events.clear();fs::remove(control+".home");test_time+=500;
 button(7);button(8);assert(SDL_PollEvent(&event)==0);assert(fs::exists(control+".home"));test_time+=300;tick();assert(SDL_PollEvent(&event)==0);
 control_shortcut=rgds::ControlShortcut{};delayed_start_events.clear();fs::remove(control+".home");test_time+=500;
 button(8);assert(SDL_PollEvent(&event)==0 && !fs::exists(control+".menu"));test_time+=151;tick();assert(fs::exists(control+".menu"));
 // A Start already delivered before a later chord gets its release before
 // the save request, including controller-event engines that cache state.
 events.clear();delivered_controls.clear();control_shortcut=rgds::ControlShortcut{};test_time+=500;
 event={};event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=2;event.cbutton.button=SDL_CONTROLLER_BUTTON_START;event.cbutton.state=SDL_PRESSED;native_events.push_back(event);assert(SDL_PollEvent(&event)==0);test_time+=151;assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN);
 event={};event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=2;event.cbutton.button=SDL_CONTROLLER_BUTTON_GUIDE;event.cbutton.state=SDL_PRESSED;native_events.push_back(event);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONUP && event.cbutton.button==SDL_CONTROLLER_BUTTON_START);
 events.clear();delivered_controls.clear();control_shortcut=rgds::ControlShortcut{};profile("preset=default\n");
 event={};event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=3;event.cbutton.button=Uint8(controller_button(0));event.cbutton.state=SDL_PRESSED;native_events.push_back(event);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN);
 std::ofstream(controls_global)<<"schema=1\npreset=ab-swap\n";test_time+=300;tick();assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONUP && event.cbutton.button==controller_button(0));
 // Pause neutralizes cached controls before an injected save hotkey.
 event={};event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=3;event.cbutton.button=Uint8(controller_button(1));event.cbutton.state=SDL_PRESSED;native_events.push_back(event);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN);
 std::ofstream(control+".blocked")<<"pause";test_time+=30;tick();assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONUP);fs::remove(control+".blocked");
 test_time+=30;tick();profile("preset=custom\nbind_a=l2\n");
 auto trigger=[&](int value){SDL_Event axis{};axis.type=SDL_CONTROLLERAXISMOTION;axis.caxis.which=5;axis.caxis.axis=SDL_CONTROLLER_AXIS_TRIGGERLEFT;axis.caxis.value=Sint16(value);native_events.push_back(axis);};
 trigger(32767);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN);
 std::ofstream(control+".blocked")<<"pause";test_time+=30;tick();assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONUP);trigger(0);assert(SDL_PollEvent(&event)==0);
 fs::remove(control+".blocked");test_time+=30;tick();trigger(32767);assert(SDL_PollEvent(&event)==1 && event.type==SDL_CONTROLLERBUTTONDOWN);
 profile("preset=left-handed\n");auto hat=[&](int value){SDL_Event axis{};axis.type=SDL_JOYHATMOTION;axis.jhat.which=6;axis.jhat.hat=0;axis.jhat.value=Uint8(value);native_events.push_back(axis);};
 hat(SDL_HAT_UP);assert(SDL_PollEvent(&event)==1 && event.type==SDL_JOYBUTTONDOWN);
 std::ofstream(control+".blocked")<<"pause";test_time+=30;tick();assert(SDL_PollEvent(&event)==1 && event.type==SDL_JOYBUTTONUP);hat(0);assert(SDL_PollEvent(&event)==0);
 fs::remove(control+".blocked");test_time+=30;tick();hat(SDL_HAT_UP);assert(SDL_PollEvent(&event)==1 && event.type==SDL_JOYBUTTONDOWN);
 // Lid suspend can stop the core before it notices the input gate. The resume
 // marker alone must release delivered buttons and a held DSperate stylus.
 if(core=="dsperate"){route_ds_touch({{0,320,240}},false);events.clear();assert(ds_touch_down);}
 {std::ofstream file(control+".resume");file<<"monotonic=100\nrealtime=1000000100\n";}
 test_time+=30;tick();assert(delivered_controls.empty());
 assert(SDL_PollEvent(&event)==1 && event.type==SDL_JOYBUTTONUP);
 if(core=="dsperate"){assert(!ds_touch_down);assert(SDL_PollEvent(&event)==1 && event.type==SDL_FINGERUP);}
 assert(SDL_PollEvent(&event)==0);
}
'''


if __name__=='__main__':unittest.main()
