"""Drive the actual bridge with a fake clock/audio/input and real state files."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++") and shutil.which("sdl2-config"),"C++/SDL headers required")
class EmulatorBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.folder=Path(cls.temp.name);cls.binary=cls.folder/"bridge-test"
        source=cls.folder/"test.cpp"
        source.write_text(r'''
#define RGDS_BRIDGE_TESTS
#define RGDS_BRIDGE_PROFILE
#include "platform/emulator_bridge.cpp"
#include <cassert>
int fake_peep(SDL_Event* output,int,SDL_eventaction,Uint32,Uint32){
 output[0]={};output[0].type=SDL_JOYBUTTONDOWN;output[0].jbutton.button=8;
 output[1]={};output[1].type=SDL_JOYBUTTONDOWN;output[1].jbutton.button=0;return 2;
}
int adapter_draws=0,thread_switches=0,graphics_switches=0;
int fake_graphics(int scale,int smooth,int skip){++graphics_switches;assert(smooth==1 && skip==0);return scale!=4;}
int fake_threading(int){++thread_switches;return 1;}
int fake_load(){return 1;}
int fake_copy(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*){++adapter_draws;return 0;}
Uint8 fake_button(SDL_Joystick*,int button){return button<13?1:0;}
Uint8 native_keys[512]{};
const Uint8* fake_keys(int* count){*count=512;return native_keys;}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"rgds_set_graphics"))return reinterpret_cast<void*>(&fake_graphics);
 if(!std::strcmp(name,"rgds_set_threaded_3d"))return reinterpret_cast<void*>(&fake_threading);
 if(!std::strcmp(name,"rgds_load_checkpoint"))return reinterpret_cast<void*>(&fake_load);
 if(!std::strcmp(name,"SDL_RenderCopy"))return reinterpret_cast<void*>(&fake_copy);
 if(!std::strcmp(name,"SDL_PeepEvents"))return reinterpret_cast<void*>(&fake_peep);
 if(!std::strcmp(name,"SDL_JoystickGetButton"))return reinterpret_cast<void*>(&fake_button);
 if(!std::strcmp(name,"SDL_GetKeyboardState"))return reinterpret_cast<void*>(&fake_keys);
 return nullptr;
}
std::string state(){std::ifstream file(control+".result");return std::string(std::istreambuf_iterator<char>(file),{});}
void command(const char* id,const char* action){std::ofstream file(control);file<<id<<' '<<action<<'\n';}
int main(int argc,char** argv){
 assert(argc==3 || argc==4);fs::path root=argv[1];fs::create_directories(root/"states");
 setenv("RGDS_CORE",argv[2],1);setenv("RGDS_SHARED_MENU","1",1);
 std::ofstream(root/"controls.ini")<<"schema=1\nshortcut=anbernic\nshortcut_action=menu\n";setenv("RGDS_CONTROLS_GLOBAL",(root/"controls.ini").c_str(),1);
 setenv("RGDS_CONTROL_FILE",(root/"control").c_str(),1);setenv("RGDS_STATE_DIR",(root/"states").c_str(),1);
 setenv("RGDS_STATE_SLOT",(root/"states/slot.dss").c_str(),1);setenv("RGDS_AUTOSTATE_FILE",(root/"auto.dss").c_str(),1);
 if(argc==4)setenv("RGDS_STATE_ATOMIC_REPLACE","1",1);
 initialize();
 if(argc==4 && std::string(argv[3])=="graphics"){
   setenv("RGDS_OWN_FF_HUD","1",1);
   command("graphics-1","graphics:3:1:0");test_time=30;tick();
   assert(graphics_switches==0 && !pending_graphics_token.empty());
   test_time=60;tick();assert(graphics_switches==0);
   SDL_Event event{};SDL_PollEvent(&event);
   assert(graphics_switches==1 && pending_graphics_token.empty() && enhanced_game_texture);
   assert(state().find("token=graphics-1")!=std::string::npos && state().find("state=graphics-applied")!=std::string::npos);
   command("graphics-fail","graphics:4:1:0");test_time=90;SDL_PollEvent(&event);
   assert(graphics_switches==2 && enhanced_game_texture && state().find("state=graphics-failed")!=std::string::npos);
   command("graphics-invalid","graphics:2:1:0extra");test_time=120;SDL_PollEvent(&event);
   assert(graphics_switches==2 && state().find("state=graphics-failed")!=std::string::npos);
   command("graphics-native","graphics:1:1:0");test_time=150;SDL_PollEvent(&event);
   assert(graphics_switches==3 && !enhanced_game_texture);
   return 0;
 }
 if(argc==4 && core=="dsperate"){
   assert(atomic_state_replace);
   auto slot=root/"states/slot.dss",temp=fs::path(slot.string()+".tmp");
   auto valid=[&](const fs::path& path){
     std::ofstream out(path,std::ios::binary);
     auto word=[&](unsigned value){for(int i=0;i<4;i++)out.put(static_cast<char>((value>>(8*i))&255));};
     out.write("DSST",4);word(4);out.write("HEAD",4);word(48);out<<std::string(48,'h');
     out.write("VIEW",4);word(19);out<<std::string(19,'v');
   };
   valid(slot);valid(root/"auto.dss");
   command("atomic-save","save");test_time=30;tick();
   test_time=100;tick();assert(saving); // An existing valid state is not a new save.
   std::ofstream(temp)<<"partial";test_time=1000;tick();assert(saving);
   fs::rename(temp,slot);test_time=1100;tick();assert(saving); // Even a renamed truncated file is refused.
   valid(temp);fs::rename(temp,slot);test_time=1150;tick();assert(!saving && state().find("state=saved")!=std::string::npos);
   assert(rgds::complete_dsperate_state((root/"auto.dss").string()));
   command("quick-save","save");test_time=1200;tick();valid(temp);fs::rename(temp,slot);
   test_time=1250;tick();assert(!saving && state().find("state=saved")!=std::string::npos); // 50 ms, no 600 ms floor.
   command("in-place","save");test_time=1300;tick();valid(slot);test_time=1400;tick();test_time=2100;tick();assert(saving);
   test_time=8500;tick();assert(!saving && failed && state().find("state=save-timeout")!=std::string::npos);
   fs::remove(slot);command("first-save","save");test_time=8600;tick();std::ofstream(temp)<<"partial";
   test_time=8650;tick();assert(saving);valid(temp);fs::rename(temp,slot);test_time=8700;tick();assert(!saving);
   autostate=(root/"target-directory").string();fs::create_directory(autostate);
   command("copy-fails","save-quit");test_time=8800;tick();valid(temp);fs::rename(temp,slot);test_time=8850;tick();
   assert(!saving && !quitting && failed && state().find("state=save-failed")!=std::string::npos);
   assert(rgds::complete_dsperate_state(slot.string()));
   return 0;
 }
 if(argc==4)assert(!atomic_state_replace);
 if(core=="drastic"){setenv("RGDS_TOUCH_LIBRARY","fixture",1);assert(SDL_RenderCopy(nullptr,nullptr,nullptr,nullptr)==0);assert(adapter_draws==1);}
 if(core=="dsperate" || core=="melonds"){
   route_ds_touch({{0,320,240},{1,639,479}},false);
   assert(events.size()==2 && events.front().type==SDL_FINGERDOWN);
   assert(events.front().tfinger.windowID==0 && events.front().tfinger.x==.5f && events.front().tfinger.y==.5f);
   assert(events.back().type==SDL_FINGERMOTION && events.back().tfinger.x<1);events.clear();
   route_ds_touch({},true);assert(events.size()==1 && events.front().type==SDL_FINGERUP);events.clear();
   route_ds_touch({{1,100,100}},false);assert(events.empty());
   route_ds_touch({{2,100,100},{0,200,200},{2,200,200}},false);
   assert(events.size()==2 && events.front().type==SDL_FINGERDOWN && events.back().type==SDL_FINGERUP);events.clear();
   route_ds_touch({{0,300,300},{2,300,300}},true);assert(events.empty());
 }
 SDL_Event out[2]{};assert(SDL_PeepEvents(out,2,SDL_GETEVENT,0,0xffffffff)==1);
 assert(out[0].jbutton.button==0 && fs::exists(control+".menu"));
 fs::remove(control+".menu");
 SDL_Event home{};home.type=SDL_KEYDOWN;home.key.keysym.sym=SDLK_HOME;
 assert(menu_event(home) && fs::exists(control+".game-menu") && !fs::exists(control+".menu"));
 assert(SDL_JoystickGetButton(nullptr,8)==0);
 command("save-1","save");test_time=30;test_clock_step=1;tick();test_clock_step=0;assert(state().find("state=saving")!=std::string::npos);assert(saving && !failed);
 SDL_Event event{};assert(SDL_PollEvent(&event)==1);
 assert(event.type==(core=="drastic"?SDL_JOYBUTTONDOWN:SDL_KEYDOWN));
 if(core=="drastic"){
   assert(event.jbutton.button==16);
   assert(SDL_JoystickGetButton(nullptr,16)==1);
   assert(SDL_JoystickGetButton(nullptr,16)==0); // A held save must not start a write every frame.
 }else assert(event.key.keysym.sym==SDLK_F5);
 test_time=80;assert(SDL_PollEvent(&event)==0); // Release must not drain in the press frame.
 if(core!="drastic"){int count=0;auto keys=SDL_GetKeyboardState(&count);assert(keys[SDL_SCANCODE_F5]==1);}
 std::ofstream(root/"states/unrelated.dss")<<"unrelated state";
 test_time=200;tick();assert(SDL_PollEvent(&event)==1);
 assert(event.type==(core=="drastic"?SDL_JOYBUTTONUP:SDL_KEYUP));
 test_time=900;tick();assert(state().find("state=saving")!=std::string::npos);
 std::ofstream(root/"states/slot.dss")<<"complete checkpoint";
 test_time=1000;tick();test_time=1700;tick();assert(state().find("state=saved")!=std::string::npos);
 std::ifstream saved(root/"auto.dss");assert(std::string(std::istreambuf_iterator<char>(saved),{})=="complete checkpoint");
 command("load-1","load");test_time=1800;test_clock_step=1;tick();test_clock_step=0;assert(state().find(core=="melonds"?"state=loaded":"state=load-requested")!=std::string::npos);assert(loading==(core!="melonds"));
 test_time=2500;tick();assert(state().find(core=="melonds"?"state=loaded":"state=load-dispatched")!=std::string::npos);
 auto queued=events.size(),deferred=releases.size();
 command("speed-1","fast-forward");test_time=2530;tick();
 assert(state().find("state=fast-forward-on")!=std::string::npos);assert(fast_forward);
 if(core=="nnddss") { int n=0;auto keys=SDL_GetKeyboardState(&n);assert(keys[SDL_SCANCODE_TAB]==1 && native_keys[SDL_SCANCODE_TAB]==0); }
 assert(events.size()==queued+2 && releases.size()==deferred);
 if(core=="drastic"){assert(events[queued].jbutton.button==19 && events[queued].type==SDL_JOYBUTTONDOWN);assert(events.back().type==SDL_JOYBUTTONUP);}
 else{assert(events[queued].key.keysym.sym==SDLK_TAB && events[queued].type==SDL_KEYDOWN);assert(events.back().type==SDL_KEYUP);}
 command("speed-2","fast-forward");test_time=2560;tick();
 assert(state().find("state=fast-forward-off")!=std::string::npos);
 assert(!fast_forward);
 if(core=="drastic")assert(SDL_JoystickGetButton(nullptr,9)==1); // Preserve physical buttons; never synthesize a speed flag.
 command("quit-1","quit");test_time=2600;tick();assert(state().find("state=saving")!=std::string::npos);
 test_time=10000;tick();assert(state().find("state=save-timeout")!=std::string::npos);assert(!quitting);
 autosave=false; // Shader/restart saves are mandatory even when home autosaves are disabled.
 command("retry","save-quit");test_time=10100;tick();assert(state().find("token=retry")!=std::string::npos);assert(saving);
 std::ofstream(root/"states/slot.dss")<<"checkpoint before exit";
 test_time=10200;tick();test_time=10900;tick();assert(state().find("state=saved")!=std::string::npos);
 std::ifstream latest(root/"auto.dss");assert(std::string(std::istreambuf_iterator<char>(latest),{})=="checkpoint before exit");
 bool saw_quit=false;for(int i=0;i<32 && SDL_PollEvent(&event);i++)if(event.type==SDL_QUIT)saw_quit=true;
 assert(saw_quit); // A real quit event follows the completed checkpoint for every core.
 assert(state().find("token=retry")!=std::string::npos && state().find("state=saved")!=std::string::npos); // Exit must retain the verified save receipt for the switcher.
 // A same-size rewrite with restored mtime still counts as a new save.
 struct stat old_slot{};assert(stat((root/"states/slot.dss").c_str(),&old_slot)==0);
 quitting=false;quit_at=0;command("same-size","save");test_time=11000;tick();
 std::ofstream(root/"states/slot.dss")<<"CHECKPOINT BEFORE EXIT";
#ifdef __APPLE__
 timespec times[]={old_slot.st_atimespec,old_slot.st_mtimespec};
#else
 timespec times[]={old_slot.st_atim,old_slot.st_mtim};
#endif
 assert(utimensat(AT_FDCWD,(root/"states/slot.dss").c_str(),times,0)==0);
 test_time=11100;tick();test_time=11800;tick();assert(state().find("state=saved")!=std::string::npos);
 std::ifstream rewritten(root/"auto.dss");assert(std::string(std::istreambuf_iterator<char>(rewritten),{})=="CHECKPOINT BEFORE EXIT");
 // A failed copy cannot destroy the completed checkpoint.
 assert(copy_state((root/"missing.dss").string(),(root/"auto.dss").string()));
 std::ifstream preserved(root/"auto.dss");assert(std::string(std::istreambuf_iterator<char>(preserved),{})=="CHECKPOINT BEFORE EXIT");
 SmallFile cache;cache.path=(root/"small-status").string();std::ofstream(cache.path)<<"old";assert(cache.refresh() && cache.value=="old");
 rgds::bridge_metrics()={};for(int i=0;i<100;i++)assert(!cache.refresh());assert(rgds::bridge_metrics().file_reads==0);
 std::ofstream(cache.path+".pending")<<"new";rename((cache.path+".pending").c_str(),cache.path.c_str());assert(cache.refresh() && cache.value=="new"); // Same-size atomic replacement must not be missed.
 unlink(cache.path.c_str());assert(cache.refresh() && cache.value.empty());assert(!cache.refresh());std::ofstream(cache.path)<<"new";assert(cache.refresh());
 test_time=12000;int key_count=0;assert(SDL_GetKeyboardState(&key_count)==native_keys);
 native_keys[SDL_SCANCODE_HOME]=1;auto filtered=SDL_GetKeyboardState(&key_count);assert(filtered!=native_keys && filtered[SDL_SCANCODE_HOME]==0 && native_keys[SDL_SCANCODE_HOME]==1);native_keys[SDL_SCANCODE_HOME]=0;
 std::ofstream(control+".blocked")<<"menu";test_time+=30;filtered=SDL_GetKeyboardState(&key_count);assert(filtered!=native_keys);
 unlink((control+".blocked").c_str());std::ofstream(control+".resume")<<"monotonic=100\nrealtime=1000000100\n";test_time+=30;SDL_GetKeyboardState(&key_count);assert(resume_monotonic==100 && resume_realtime==1000000100);
 frame_stats.enabled=true;frame_stats.reset();frame_stats.present(1,false);frame_stats.present(16668,false);
 const auto before_resume=frame_stats.frames();assert(stats_resumed);
 measure_present(reinterpret_cast<void*>(1));assert(!stats_resumed && frame_stats.frames()==before_resume);
 frame_stats.enabled=false;
 rgds::bridge_metrics()={};for(int i=0;i<100;i++)SDL_GetKeyboardState(&key_count);assert(rgds::bridge_metrics().file_reads==0 && rgds::bridge_metrics().keyboard_copies==0);
 for(int value=0;value<2;value++){
   command(value?"thread-on":"thread-off",value?"threaded:1":"threaded:0");test_time+=30;tick();
   assert(state().find(core=="melonds"?(value?"state=threaded-on":"state=threaded-off"):"state=threaded-failed")!=std::string::npos);
 }
 assert(thread_switches==(core=="melonds"?2:0));
 // Physical-button speed requests use a separate channel and never replace
 // the current checkpoint/menu command. Hold release works while blocked.
 saving=loading=false;events.clear();releases.clear();fast_forward=false;
 auto speed=[&](const char* id,const char* action){std::ofstream(control+".speed-request")<<"token="<<id<<"\naction="<<action<<'\n';test_time+=30;tick();};
 speed("hold-on","fast-forward-on");assert(fast_forward && events.size()==2);
 std::ofstream(control+".blocked")<<"menu";
 speed("hold-off","fast-forward-off");assert(!fast_forward && events.size()==4);
 speed("blocked-press","fast-forward-on");assert(!fast_forward && events.size()==4);
 unlink((control+".blocked").c_str());
 speed("toggle","fast-forward-toggle");assert(fast_forward && events.size()==6);
 return 0;
}
''')
        flags=subprocess.check_output(["sdl2-config","--cflags"],text=True).split()
        result=subprocess.run(["clang++","-std=c++17","-O0",*flags,"-I"+str(ROOT/"src"),str(source),"-o",str(cls.binary)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def test_verified_atomic_writer_skips_delay_without_accepting_old_or_partial_states(self):
        for core in ("dsperate", "drastic", "nnddss", "melonds"):
            with self.subTest(core=core), tempfile.TemporaryDirectory() as folder:
                result=subprocess.run([str(self.binary),folder,core,"atomic"],capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_each_engine_has_the_same_bounded_checkpoint_and_menu_protocol(self):
        for core in ("drastic","dsperate","nnddss","melonds"):
            with self.subTest(core=core),tempfile.TemporaryDirectory() as folder:
                result=subprocess.run([str(self.binary),folder,core],capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_graphics_wait_for_event_boundary_and_preserve_renderer_on_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            result=subprocess.run([str(self.binary),folder,'dsperate','graphics'],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=="__main__":unittest.main()
