"""Typed native controller/joystick axis getters share the menu block boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'),'Host C++/SDL headers required')
class ControllerAxisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name);cls.binary=cls.root/'axis'
        source=cls.root/'axis.cpp';source.write_text(r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
Sint16 native_axis=24000;int controller_calls=0,joystick_calls=0,controller_lookups=0;bool missing=false;
Sint16 fake_controller(SDL_GameController*,SDL_GameControllerAxis axis){++controller_calls;assert(axis==SDL_CONTROLLER_AXIS_LEFTX);return native_axis;}
Sint16 fake_joystick(SDL_Joystick*,int axis){++joystick_calls;assert(axis==0);return native_axis;}
extern "C" void* dlsym(void*,const char* name){
 if(!std::strcmp(name,"SDL_GameControllerGetAxis")){++controller_lookups;return missing?nullptr:reinterpret_cast<void*>(&fake_controller);}
 if(!std::strcmp(name,"SDL_JoystickGetAxis"))return reinterpret_cast<void*>(&fake_joystick);
 return nullptr;
}
int main(int argc,char** argv){
 assert(argc==3);setenv("RGDS_CORE","dsperate",1);setenv("RGDS_CONTROL_FILE",(std::string(argv[1])+"/control").c_str(),1);
 bool managed=std::strcmp(argv[2],"unmanaged")!=0;missing=!std::strcmp(argv[2],"missing");setenv("RGDS_SHARED_MENU",managed?"1":"0",1);
 auto axis=[](){return SDL_GameControllerGetAxis(nullptr,SDL_CONTROLLER_AXIS_LEFTX);};
 if(missing){assert(axis()==0 && axis()==0 && controller_calls==0 && controller_lookups==1);return 0;}
 assert(axis()==24000 && SDL_JoystickGetAxis(nullptr,0)==24000);
 assert(axis()==24000 && controller_lookups==1); // Resolve once; do not cache returned axis values.
 std::ofstream(control+".blocked")<<"menu";test_time+=25;int calls=controller_calls;
 assert(axis()==(managed?0:24000));assert(SDL_JoystickGetAxis(nullptr,0)==(managed?0:24000));
 if(managed)assert(controller_calls==calls);
 native_axis=0;test_time+=25;assert(axis()==0 && SDL_JoystickGetAxis(nullptr,0)==0);
 unlink((control+".blocked").c_str());std::ofstream(control+".resume")<<"monotonic=1\nrealtime=1000000001\n";test_time+=25;
 assert(axis()==0 && SDL_JoystickGetAxis(nullptr,0)==0); // A current centered value must not revive a previous held value.
 native_axis=-18000;assert(axis()==-18000 && SDL_JoystickGetAxis(nullptr,0)==-18000 && controller_lookups==1);
}
''')
        flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
        result=subprocess.run(['clang++','-std=c++17','-O1',*flags,'-I'+str(ROOT/'src'),str(source),'-o',str(cls.binary)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def exercise(self,mode):
        with tempfile.TemporaryDirectory() as folder:
            result=subprocess.run([str(self.binary),folder,mode],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_managed_axes_are_neutral_during_menu_and_use_current_values_after_resume(self):self.exercise('managed')
    def test_unmanaged_axis_input_is_preserved(self):self.exercise('unmanaged')
    def test_missing_native_axis_api_stays_neutral(self):self.exercise('missing')


if __name__=='__main__':unittest.main()
