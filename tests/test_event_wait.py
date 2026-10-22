"""Exercise the actual SDL event queue with a virtual gamepad and bounded waits."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'Host C++/SDL required')
class EventWaitTests(unittest.TestCase):
    def test_gamepad_events_remain_ordered_without_sdl_wait_polling(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'wait.cpp'
            binary = root / 'wait'
            source.write_text(
                r'''
#include "platform/event_wait.hpp"
#include <SDL.h>
#include <cassert>
#include <thread>
struct Api {
    int waits=0,delays=0,polls=0;Uint32 longest=0;
    int WaitEventTimeout(SDL_Event* e,int ms){++waits;return SDL_WaitEventTimeout(e,ms);}
    int PollEvent(SDL_Event* e){++polls;return SDL_PollEvent(e);}
    void Delay(Uint32 ms){++delays;longest=std::max(longest,ms);SDL_Delay(ms);}
};
int main(){
    assert(SDL_Init(SDL_INIT_EVENTS|SDL_INIT_JOYSTICK)==0);
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,2,2,1);assert(index>=0);
    auto* pad=SDL_JoystickOpen(index);assert(pad);
    SDL_Event e{};while(SDL_PollEvent(&e)){}
    Api api;assert(SDL_JoystickSetVirtualButton(pad,0,SDL_PRESSED)==0);
    bool down=false,up=false;
    auto began=SDL_GetTicks();
    while(SDL_GetTicks()-began<500 && !down){
        if(rgds::wait_ui_event(api,&e,50,true) && e.type==SDL_JOYBUTTONDOWN)down=true;
    }
    assert(down);assert(SDL_JoystickSetVirtualButton(pad,0,SDL_RELEASED)==0);
    while(SDL_GetTicks()-began<500 && !up){
        if(rgds::wait_ui_event(api,&e,50,true) && e.type==SDL_JOYBUTTONUP)up=true;
    }
    assert(up);while(SDL_PollEvent(&e)){}
    for(int i=0;i<20;i++)assert(!rgds::wait_ui_event(api,&e,50,true));
    assert(api.waits==0 && api.delays>=20 && api.longest<=8 && api.polls<100);
    SDL_JoystickClose(pad);SDL_JoystickDetachVirtual(index);
    while(SDL_PollEvent(&e)){}
    // Desktop/event-driven path still wakes promptly on a posted event.
    std::thread poster([]{SDL_Delay(20);SDL_Event wake{};wake.type=SDL_USEREVENT;SDL_PushEvent(&wake);});
    assert(rgds::wait_ui_event(api,&e,500,false));assert(e.type==SDL_USEREVENT && api.waits==1);
    poster.join();SDL_Quit();
}
'''
            )
            flags = subprocess.check_output(['sdl2-config', '--cflags', '--libs'], text=True).split()
            subprocess.run(
                [
                    'clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(ROOT / 'src'),
                    str(source), *flags, '-o',
                    str(binary)
                ],
                check=True,
                capture_output=True,
                text=True
            )
            result = subprocess.run(
                [str(binary)], env=dict(os.environ, SDL_VIDEODRIVER='dummy'), capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main()
