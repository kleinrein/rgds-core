"""Run the real embedded menu against delayed IPC, with host SDL only."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/host/rgds-launcher'
sys.path.insert(0,str(ROOT/'runtime'))
from library import read_fields,write_fields


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config') and (ROOT/'build/host/rgds-launcher').exists(),'Host launcher/SDL required')
class MenuUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
        cls.app=cls.root/'app';(cls.app/'runtime').mkdir(parents=True)
        for folder in ('assets','themes'):(cls.app/folder).symlink_to(ROOT/folder,target_is_directory=True)
        # The real menu synchronizes the hardware mixer when opening audio.
        # Host SDL has no RK817 mixer; keep that boundary outside this IPC test.
        (cls.app/'runtime/system_controls.py').write_text('import sys\nsys.exit(0)\n')
        source=cls.root/'input.cpp';cls.library=cls.root/'input.dylib'
        source.write_text(r'''
#include <SDL.h>
extern "C" int SDL_PollEvent(SDL_Event*){return 0;}
extern "C" int SDL_WaitEventTimeout(SDL_Event* e,int){
 static Uint32 began=SDL_GetTicks();static int next=0;
 const int times[]={100,150,200,260,320,380,650,720,780,840};
 const SDL_Keycode keys[]={SDLK_HOME,SDLK_HOME,SDLK_DOWN,SDLK_a,SDLK_a,SDLK_a,SDLK_a,SDLK_DOWN,SDLK_DOWN,SDLK_a};
 SDL_Delay(10);if(next==10 || SDL_GetTicks()-began<Uint32(times[next]))return 0;
 *e={};e->type=SDL_KEYDOWN;e->key.keysym.sym=keys[next++];return 1;
}
''')
        flags=subprocess.check_output(['sdl2-config','--cflags','--libs'],text=True).split()
        result=subprocess.run(['clang++','-std=c++17','-shared','-fPIC',str(source),*flags,'-o',str(cls.library)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def exercise(self,acknowledge,sounds=False):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);data=root/'data';status=data/'session/game-menu.status';request=data/'session/game-menu.request'
            write_fields(status,dict(state='paused',checkpoint=1,request_token='old',message=''))
            if sounds:
                write_fields(data/'preferences.ini',dict(schema=1,mode='light',font='rounded'))
                write_fields(data/'ui.ini',dict(menu_sounds=1,animations=0))
                write_fields(data/'volume.ini',dict(volume=55))
            env=dict(os.environ,RGDS_SDL_LIBRARY=str(self.library),RGDS_OVERLAY_FILE=str(root/'frame'),RGDS_MENU_EXTERNAL='1',RGDS_SYSTEM_MANAGED='1',RGDS_VOLUME_EXTERNAL='1',SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy')
            child=subprocess.Popen([str(BINARY),'--cache-report',str(root/'report'),'--game-menu','--app',str(self.app),'--data',str(data),'--seconds','2'],env=env,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            deadline=time.monotonic()+4;seen_save=False
            try:
                while child.poll() is None and time.monotonic()<deadline:
                    fields=read_fields(request)
                    if fields.get('action')=='slot-save:0':
                        seen_save=True
                        if acknowledge:write_fields(status,dict(state='paused',checkpoint=1,request_token=fields['token'],message='Checkpoint saved.'))
                    time.sleep(.01)
                output,errors=child.communicate(timeout=2)
            finally:
                if child.poll() is None:child.kill();child.wait()
            self.assertEqual(child.returncode,0,output+errors)
            self.assertTrue(seen_save,'Opening hardware presses must not immediately close the menu')
            if sounds:
                counts=json.loads((root/'report').read_text())['sound_counts']
                self.assertEqual(counts[10],int(acknowledge),'Save reward must follow the matching completed acknowledgement')
            self.assertEqual(read_fields(request)['action'],'slot-undo' if acknowledge else 'slot-save:0')

    def test_old_status_cannot_unlock_a_save_or_allow_a_second_tap_to_replace_it(self):
        self.exercise(False)

    def test_matching_acknowledgement_unlocks_the_next_menu_action(self):
        self.exercise(True)

    def test_pending_save_does_not_play_a_success_sound(self):
        self.exercise(False,True)

    def test_acknowledged_save_plays_one_success_sound(self):
        self.exercise(True,True)

    def test_home_route_changes_an_open_switcher_to_the_game_menu(self):
        self.exercise_menu_route('game-menu',12)

    def test_anbernic_route_opens_switcher_in_the_existing_menu(self):
        self.exercise_menu_route('switcher',36)

    def exercise_menu_route(self, view, page):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);status=data/'session/game-menu.status';report=data/'report'
            write_fields(status,dict(state='paused',view_request=0))
            env=dict(os.environ,RGDS_OVERLAY_FILE=str(data/'frame'),RGDS_MENU_EXTERNAL='1',RGDS_SYSTEM_MANAGED='1',RGDS_VOLUME_EXTERNAL='1',SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy')
            child=subprocess.Popen([str(BINARY),'--cache-report',str(report),'--game-switcher' if view=='game-menu' else '--game-menu','--app',str(self.app),'--data',str(data),'--seconds','2'],env=env,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                deadline=time.monotonic()+2
                while not (data/'frame').exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.005)
                self.assertTrue((data/'frame').exists())
                write_fields(status,dict(state='paused',view_request=1,view=view))
                output,errors=child.communicate(timeout=4)
                self.assertEqual(child.returncode,0,output+errors)
                self.assertEqual(json.loads(report.read_text())['page'],page)
                self.assertFalse((data/'session/game-menu.request').exists())
            finally:
                if child.poll() is None:child.kill();child.wait()
                child.communicate()

    def test_sleep_handshake_closes_and_reopens_menu_audio_without_duplicate_wake_cue(self):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);session=data/'session';report=data/'report'
            write_fields(session/'game-menu.status',dict(state='paused'))
            write_fields(data/'preferences.ini',dict(schema=1,mode='light',font='rounded'))
            write_fields(data/'ui.ini',dict(menu_sounds=1))
            write_fields(data/'volume.ini',dict(volume=55))
            env=dict(os.environ,RGDS_OVERLAY_FILE=str(data/'frame'),RGDS_MENU_EXTERNAL='1',RGDS_SYSTEM_MANAGED='1',RGDS_VOLUME_EXTERNAL='1',SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy')
            child=subprocess.Popen([str(BINARY),'--cache-report',str(report),'--game-menu','--app',str(self.app),'--data',str(data),'--seconds','2'],env=env,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                (session/'game-menu.audio-sleep').touch()
                deadline=time.monotonic()+2
                while not (session/'game-menu.audio-sleep.ready').exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.005)
                self.assertTrue((session/'game-menu.audio-sleep.ready').exists())
                (session/'game-menu.audio-sleep').unlink();(session/'game-menu.wake').touch()
                output,errors=child.communicate(timeout=4)
                self.assertEqual(child.returncode,0,output+errors)
                result=json.loads(report.read_text())
                # The sink opens only for a cue and closes when quiet, so an idle
                # menu holds nothing across the sleep handshake.
                self.assertEqual(result['sound_opens'],0)
                self.assertEqual(result['sound_open_now'],0)
                self.assertEqual(result['sound_reads'],17)
                self.assertEqual(result['sound_counts'][15:],[0,0])  # Hardware owner supplies the cues.
                self.assertFalse((session/'game-menu.wake').exists())
            finally:
                if child.poll() is None:child.kill();child.wait()
                child.communicate()


if __name__=='__main__':unittest.main()
