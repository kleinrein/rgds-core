"""Exercise actual native return using host SDL and harmless slow helper fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config') and BINARY.exists(),'Host SDL/C++ required')
class LauncherReturnTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
        source=cls.root/'display.cpp';cls.library=cls.root/'display.dylib'
        source.write_text(r'''#include <SDL.h>
#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <string>
static int sessions=0;static Uint32 session_started=0;
extern "C" int SDL_Init(Uint32 flags){sessions++;auto fn=reinterpret_cast<decltype(&SDL_Init)>(dlsym(RTLD_NEXT,"SDL_Init"));int result=fn(flags);session_started=SDL_GetTicks();return result;}
extern "C" void SDL_Quit(){const char* root=std::getenv("RGDS_TEST_RETURN_DATA");if(sessions==1 && root)std::ofstream(std::string(root)+"/display-closed").put('1');auto fn=reinterpret_cast<decltype(&SDL_Quit)>(dlsym(RTLD_NEXT,"SDL_Quit"));fn();}
extern "C" int SDL_GetNumVideoDisplays(){return 2;}
extern "C" const char* SDL_GetDisplayName(int index){return index?"DSI-2":"DSI-1";}
extern "C" int SDL_GetDisplayBounds(int index,SDL_Rect* rect){*rect={0,index*480,640,480};return 0;}
extern "C" SDL_Window* SDL_CreateWindow(const char* name,int,int,int width,int height,Uint32){auto fn=reinterpret_cast<decltype(&SDL_CreateWindow)>(dlsym(RTLD_NEXT,"SDL_CreateWindow"));return fn(name,SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,width,height,SDL_WINDOW_HIDDEN);}
extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window* window,int,Uint32){auto fn=reinterpret_cast<decltype(&SDL_CreateRenderer)>(dlsym(RTLD_NEXT,"SDL_CreateRenderer"));return fn(window,-1,SDL_RENDERER_SOFTWARE);}
extern "C" int SDL_PollEvent(SDL_Event*){return 0;}
extern "C" int SDL_WaitEventTimeout(SDL_Event* event,int){
 static int first=0,next=0;SDL_Delay(5);Uint32 now=SDL_GetTicks()-session_started;
 if(sessions==1){
  if(first++==10){*event={};event->type=SDL_KEYDOWN;event->key.keysym.sym=SDLK_a;return 1;}
  const char* root=std::getenv("RGDS_TEST_RETURN_DATA");
  if(root && std::getenv("RGDS_TEST_CANCEL_LAUNCH")){
   static bool cancelled=false;static Uint32 cancel_at=0;static int step=0;
   if(!cancelled && std::ifstream(std::string(root)+"/preparing")){cancelled=true;cancel_at=now;*event={};event->type=SDL_KEYDOWN;event->key.keysym.sym=SDLK_b;return 1;}
   if(cancelled && std::ifstream(std::string(root)+"/cancelled") && now-cancel_at>=300 && step<4){*event={};event->type=step==1?SDL_KEYUP:SDL_KEYDOWN;event->key.keysym.sym=step<2?SDLK_RIGHT:SDLK_ESCAPE;step++;return 1;}
  }
  return 0;
 }
 const bool failed=std::getenv("RGDS_TEST_FAILED_RETURN");
 const int times[]={300,320,700,900,920};if(next==5 || now<Uint32(times[next]))return 0;
 // Wait for the asynchronous status helper to actually start before exiting.
 // A fixed900ms cutoff can kill its freshly forked Python process under load.
 if(next>=3){const char* root=std::getenv("RGDS_TEST_RETURN_DATA");if(root && !std::ifstream(std::string(root)+"/return-status-start"))return 0;}
 *event={};event->type=next==1?SDL_KEYUP:SDL_KEYDOWN;
 bool change=std::getenv("RGDS_TEST_CHANGE_RETURN");event->key.keysym.sym=next<2?(change?SDLK_VOLUMEUP:SDLK_RIGHT):next==2?(failed?SDLK_a:SDLK_r):SDLK_ESCAPE;next++;return 1;
}
''')
        flags=subprocess.check_output(['sdl2-config','--cflags','--libs'],text=True).split()
        result=subprocess.run(['clang++','-std=c++17','-shared','-fPIC',str(source),*flags,'-o',str(cls.library)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def exercise(self,fail_volume=False,change_volume=False,ota_trial=False):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);app=root/'app';data=root/'data';roms=root/'roms';runtime=app/'runtime'
            runtime.mkdir(parents=True);data.mkdir();roms.mkdir()
            (app/'assets').symlink_to(ROOT/'assets',target_is_directory=True);(app/'themes').symlink_to(ROOT/'themes',target_is_directory=True)
            for name in ('a.nds','b.nds'):(roms/name).write_bytes(b'fixture')
            if fail_volume:(data/'fail-volume').touch()
            (runtime/'system_controls.py').write_text('''import argparse,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('command');p.add_argument('value',nargs='?',type=int);p.add_argument('--data',type=Path,required=True);a=p.parse_args();a.data.mkdir(parents=True,exist_ok=True)
def trace(name):
 with (a.data/'trace').open('a') as f:f.write(name+' '+str(time.time())+'\\n')
if a.command=='status':
 trace('status-start')
 if (a.data/'game-exit').exists():(a.data/'return-status-start').touch();time.sleep(2)
 print('brightness_supported=1\\npower_supported=1\\nsleep_supported=0\\nlid_supported=0\\nbrightness=80\\nbattery=82\\ncharging=0\\nvolume_supported=1\\nvolume=0\\npoweroff_supported=0')
 trace('status-end')
else:
 busy=a.data/'busy'
 if busy.exists():trace('OVERLAP');raise SystemExit(9)
 busy.touch();trace(a.command+'-start')
 time.sleep(.15)
 failed=a.command=='volume' and a.value==0 and (a.data/'game-exit').exists() and (a.data/'fail-volume').exists()
 if not failed and a.command=='volume':(a.data/'volume.ini').write_text('schema=1\\nvolume='+str(a.value)+'\\n')
 trace(a.command+'-end');busy.unlink();raise SystemExit(1 if failed else 0)
''')
            (runtime/'emulators.py').write_text('''import sys,time,socket,os,json
from pathlib import Path
data=Path(sys.argv[sys.argv.index('--data')+1])
if (data/'game-exit').exists():
 (data/'retried-launch').touch();raise SystemExit(0)
assert not (data/'display-closed').exists(), 'SDL must remain visible during preparation'
# A real main-loop task must be serviced while preparation is still running.
request=data/'session/capture-request';request.parent.mkdir(exist_ok=True);request.touch()
deadline=time.monotonic()+2
while (request.exists() or not (data/'session/ui-snapshot-bottom.bmp').exists()) and time.monotonic()<deadline:time.sleep(.01)
assert not request.exists(), 'Launch preparation must not block the normal event loop'
assert (data/'session/ui-snapshot-bottom.bmp').exists()
time.sleep(.1)
with socket.socket(fileno=int(sys.argv[sys.argv.index('--display-handoff-fd')+1])) as channel:
 channel.settimeout(2);channel.sendall(b'R');assert channel.recv(1)==b'C'
assert (data/'display-closed').exists(), 'Emulator cannot open displays until SDL closes'
if os.environ.get('RGDS_UPDATE_TOKEN'):
 health=data/'session/update-health.json';before=json.loads(health.read_text())
 time.sleep(1.3)
 after=json.loads(health.read_text())
 assert after['token']==os.environ['RGDS_UPDATE_TOKEN'] and after['build']=='app'
 assert after['pulse']>before['pulse'], 'OTA heartbeat must continue while SDL is closed for immediate gameplay'
with (data/'launches').open('a') as f:f.write('launch\\n')
(data/'volume.ini').write_text('schema=1\\nvolume=0\\n')
(data/'system.ini').write_text('schema=1\\npower=2\\nbrightness=80\\nidle=0\\nlid=0\\nbattery_percent=1\\ntimezone=Europe/Oslo\\n')
(data/'game-exit').write_text(str(time.time()))
''')
            (data/'system.ini').write_text('schema=1\npower=2\nbrightness=60\nidle=0\nlid=0\nbattery_percent=1\ntimezone=Europe/Oslo\n')
            (data/'volume.ini').write_text('schema=1\nvolume=25\n')
            state=root/'state.txt';snapshot=root/'screen'
            env=dict(os.environ,RGDS_SDL_LIBRARY=str(self.library),SDL_VIDEODRIVER='dummy',RGDS_SYSTEM_MANAGED='1',RGDS_VOLUME_EXTERNAL='1')
            env['RGDS_TEST_RETURN_DATA']=str(data)
            if ota_trial:env['RGDS_UPDATE_TOKEN']='trial-immediate-play'
            if fail_volume:env['RGDS_TEST_FAILED_RETURN']='1'
            if change_volume:env['RGDS_TEST_CHANGE_RETURN']='1';env.pop('RGDS_VOLUME_EXTERNAL',None)
            child=subprocess.Popen([str(BINARY),'--library','--allow-launch','--app',str(app),'--data',str(data),
                '--rom-dir',str(roms),'--state',str(state),'--seconds','5','--screenshot',str(snapshot)],
                cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            delay=None;deadline=time.monotonic()+6
            try:
                while child.poll() is None and time.monotonic()<deadline:
                    marker=data/'game-exit';image=root/'screen-top.bmp'
                    if marker.exists() and image.exists():
                        ended=float(marker.read_text())
                        if image.stat().st_mtime>ended and delay is None:delay=image.stat().st_mtime-ended
                    time.sleep(.01)
                output,errors=child.communicate(timeout=3)
            finally:
                if child.poll() is None:child.kill();child.wait()
            self.assertEqual(child.returncode,0,output+errors)
            self.assertIsNotNone(delay,'The native launcher must redraw after the stub game exits\n'+output+errors+'\n'+((data/'trace').read_text() if (data/'trace').exists() else 'no trace'))
            self.assertLess(delay,1,'Slow status helpers must not block the first returned frame')
            self.assertEqual((data/'launches').read_text(),'launch\n')
            self.assertEqual((data/'retried-launch').exists(),fail_volume,'START retries preparation after a failed restore')
            self.assertEqual(int(state.read_text().split()[1]),0 if change_volume else 1,'Input must work while status remains slow')
            trace=(data/'trace').read_text();self.assertNotIn('OVERLAP',trace)
            ended=float((data/'game-exit').read_text())
            commands=[line.split()[0] for line in trace.splitlines() if float(line.split()[-1])>ended]
            self.assertEqual(commands[:6],['volume-start','volume-end','brightness-start','brightness-end','power-start','power-end'])
            self.assertIn('status-start',commands)
            if change_volume:self.assertIn('volume=5\n',(data/'volume.ini').read_text())
            return delay

    def test_starting_game_during_ota_trial_keeps_heartbeat_after_sdl_closes(self):
        self.exercise(ota_trial=True)

    def test_cancel_during_slow_preparation_restores_navigation_without_handoff(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);app=root/'app';data=root/'data';roms=root/'roms';runtime=app/'runtime'
            runtime.mkdir(parents=True);data.mkdir();roms.mkdir()
            for name in ('a.nds','b.nds'):(roms/name).write_bytes(b'fixture')
            (app/'assets').symlink_to(ROOT/'assets',target_is_directory=True)
            (app/'themes').symlink_to(ROOT/'themes',target_is_directory=True)
            (runtime/'system_controls.py').write_text('print("brightness_supported=0\\nvolume_supported=0\\nbattery=82\\ncharging=0")\n')
            (runtime/'emulators.py').write_text('''import sys,time,signal
from pathlib import Path
data=Path(sys.argv[sys.argv.index('--data')+1])
def cancel(*args):
 (data/'cancelled').touch();raise SystemExit(0)
signal.signal(signal.SIGTERM,cancel)
(data/'preparing').touch()
time.sleep(10)
(data/'should-not-start').touch()
''')
            state=root/'state'
            result=subprocess.run([str(BINARY),'--library','--allow-launch','--app',str(app),'--data',str(data),
                '--rom-dir',str(roms),'--state',str(state),'--seconds','4'],cwd=ROOT,
                env=dict(os.environ,RGDS_SDL_LIBRARY=str(self.library),SDL_VIDEODRIVER='dummy',
                         RGDS_SYSTEM_MANAGED='1',RGDS_TEST_CANCEL_LAUNCH='1',RGDS_TEST_RETURN_DATA=str(data)),
                capture_output=True,text=True,timeout=7)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertTrue((data/'cancelled').exists())
            self.assertFalse((data/'should-not-start').exists())
            self.assertEqual(int(state.read_text().split()[1]),1,'Navigation must resume after the cancellation fade')

    def test_return_redraw_and_navigation_do_not_wait_for_slow_status(self):
        self.exercise()

    def test_failed_audio_restore_allows_a_new_runner_preflight_without_blocking_navigation(self):
        self.exercise(True)

    def test_new_volume_input_is_deferred_then_applied_without_helper_overlap(self):
        self.exercise(change_volume=True)


if __name__=='__main__':unittest.main()
