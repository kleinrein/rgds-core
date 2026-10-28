"""Experimental fork: native event regressions and independent launch/save identity."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_runtime import Fixture
import emulators
from emulator_profiles import game_options
from library import game_key, write_fields

ROOT = Path(__file__).resolve().parents[1]
ENGINE = 'rgds-experimental'


class ExperimentalLaunchTests(Fixture):
    def prepare(self):
        self.binary(ENGINE)
        self.binary('dsperate')
        bridge = self.app / 'emulators/hooks/librgdsbridge.so'
        bridge.parent.mkdir(parents=True)
        bridge.write_bytes(b'bridge')
        rom = self.root / 'Zelda.nds'
        rom.write_bytes(b'0123456789abAZEP' + bytes(512))
        return rom

    def test_selection_launch_and_saves_are_independent(self):
        rom = self.prepare()
        write_fields(self.data / 'library' / game_key(rom) / 'game.ini', dict(core=4))
        baseline = emulators.launch_plan('dsperate', self.app, self.data, rom)
        before = {p: p.read_bytes() for p in (self.data / 'emulators/dsperate').rglob('*') if p.is_file()}
        plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(game_options(self.data, rom)['core'], 4)
        self.assertEqual(Path(plan['command'][0]).name, ENGINE)
        self.assertIn('--dual-window', plan['command'])
        self.assertIn('--no-autoload', plan['command'])
        self.assertEqual(plan['env']['RGDS_CORE'], 'dsperate')  # shared native protocol
        self.assertEqual(plan['env']['RGDS_OWN_FF_HUD'], '1')
        self.assertEqual(baseline['env']['RGDS_OWN_FF_HUD'], '0')
        self.assertNotIn('RGDS_STATE_ATOMIC_REPLACE', plan['env'])
        for key in ('HOME', 'XDG_CONFIG_HOME', 'RGDS_STATE_DIR', 'RGDS_STATE_SLOT', 'RGDS_AUTOSTATE_FILE'):
            self.assertNotEqual(plan['env'][key], baseline['env'][key])
            self.assertIn(ENGINE, plan['env'][key])
        config = (self.data / 'emulators' / ENGINE / 'config.ini').read_text()
        self.assertIn(str(self.data / 'emulators' / ENGINE / 'saves'), config)
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_high_resolution_selects_private_renderer_and_preserves_core_identity(self):
        rom = self.prepare()
        engine = self.app / 'emulators' / ENGINE
        (engine / 'rgds-experimental-vulkan').write_bytes(b'candidate')
        (engine / 'lib').mkdir()
        (engine / 'lib/libvulkan_panfrost.so').write_bytes(b'driver')
        write_fields(
            self.data / 'library' / game_key(rom) / 'game.ini', dict(core=4, rgds_scale=5, rgds_smooth=0, rgds_2d=2)
        )
        plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(Path(plan['command'][0]).name, 'rgds-experimental-vulkan')
        self.assertEqual(plan['env']['RGDS_2D_FILTER'], '2')
        self.assertEqual(plan['env']['RGDS_VK_SCALE'], '5')
        self.assertEqual(plan['env']['RGDS_ACTIVE_SCALE'], '5')
        self.assertEqual(plan['env']['RGDS_3D_SMOOTH'], '0')
        self.assertEqual(plan['env']['PANVK_ATOM_STRIDE'], '56')
        self.assertIn(ENGINE, plan['env']['RGDS_STATE_DIR'])
        self.assertTrue(Path(plan['env']['VK_ICD_FILENAMES']).is_file())
        (engine / 'lib/libvulkan_panfrost.so').unlink()
        with self.assertRaisesRegex(ValueError, 'high-resolution renderer is missing'):
            emulators.launch_plan(ENGINE, self.app, self.data, rom)

    def test_heavy_scenes_choice_reaches_high_resolution_engine(self):
        rom = self.prepare()
        engine = self.app / 'emulators' / ENGINE
        (engine / 'rgds-experimental-vulkan').write_bytes(b'candidate')
        (engine / 'lib').mkdir()
        (engine / 'lib/libvulkan_panfrost.so').write_bytes(b'driver')
        game = self.data / 'library' / game_key(rom) / 'game.ini'
        with patch.dict(os.environ):
            os.environ.pop('DS_ADAPTIVE', None)
            # Adaptive is the default; "Always sharp" turns it off; junk falls back.
            for stored, expected in ((None, '1'), (0, '0'), (1, '1'), (7, '1')):
                fields = dict(core=4, rgds_scale=2)
                if stored is not None: fields['rgds_adaptive'] = stored
                write_fields(game, fields)
                self.assertEqual(game_options(self.data, rom)['rgds_adaptive'], int(expected))
                self.assertEqual(
                    emulators.launch_plan(ENGINE, self.app, self.data, rom)['env']['DS_ADAPTIVE'], expected
                )
            # Native runs the lean binary, which has no adaptive resolution.
            write_fields(game, dict(core=4, rgds_scale=1, rgds_adaptive=0))
            self.assertNotIn('DS_ADAPTIVE', emulators.launch_plan(ENGINE, self.app, self.data, rom)['env'])
            # Benchmark A/B runs pass DS_ADAPTIVE explicitly; that wins.
            write_fields(game, dict(core=4, rgds_scale=3))
            os.environ['DS_ADAPTIVE'] = '0'
            self.assertEqual(emulators.launch_plan(ENGINE, self.app, self.data, rom)['env']['DS_ADAPTIVE'], '0')

    def test_2d_filter_defaults_off_and_ignores_out_of_range_values(self):
        rom = self.prepare()
        plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(plan['env']['RGDS_2D_FILTER'], '0')
        write_fields(self.data / 'library' / game_key(rom) / 'game.ini', dict(core=4, rgds_2d=7))
        plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(plan['env']['RGDS_2D_FILTER'], '0')

    def test_missing_fork_never_falls_back_to_standard(self):
        self.binary('dsperate')
        with self.assertRaisesRegex(ValueError, 'missing'):
            emulators.launch_plan(ENGINE, self.app, self.data, self.root / 'game.nds', prepare=False)

    def test_native_autoload_only_looks_inside_selected_engine(self):
        rom = self.prepare()
        with patch.object(emulators, 'dsperate_resume_state', return_value=None) as resume:
            emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(resume.call_args.args[-1], ENGINE)

    def test_native_launch_uses_the_lean_engine_and_high_resolution_uses_vulkan(self):
        rom = self.prepare()
        engine = self.app / 'emulators' / ENGINE
        (engine / 'rgds-experimental-vulkan').write_bytes(b'candidate')
        (engine / 'lib').mkdir()
        (engine / 'lib/libvulkan_panfrost.so').write_bytes(b'private')
        plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        # The Vulkan build costs ~2.6 ms/frame even with GPU 3D off.
        self.assertEqual(Path(plan['command'][0]).name, 'rgds-experimental')
        self.assertNotIn('DS_GPU3D', plan['env'])
        self.assertNotIn('VK_ICD_FILENAMES', plan['env'])
        self.assertEqual(plan['env']['RGDS_ACTIVE_SCALE'], '1')
        self.assertIn('nickname = RG DS Core', (self.data / 'emulators' / ENGINE / 'config.ini').read_text())
        with patch.dict(emulators.os.environ, {'RGDS_VK_SCALE': '3'}):
            plan = emulators.launch_plan(ENGINE, self.app, self.data, rom)
        self.assertEqual(Path(plan['command'][0]).name, 'rgds-experimental-vulkan')
        self.assertEqual(plan['env']['DS_GPU3D'], '1')
        self.assertEqual(plan['env']['RGDS_ACTIVE_SCALE'], '3')


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'Host SDL2 development files required')
class NativeStylusTests(unittest.TestCase):
    def test_fork_input_events_reach_ds_frame_without_cross_contact_strokes(self):
        source = ROOT / 'dscore/src'
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            # Real fork Input::handle and Input::frame; only panel geometry is
            # stubbed. No physical-device or retail-game acceptance is inferred.
            shutil.copy2(source / 'frontend/sdl/input.cpp', tmp / 'input.cpp')
            (tmp / 'display.h').write_text(
                '''#pragma once
#include "core/types.h"
namespace ds::sdl {
class Display { public:
 int screen=1;
 u32 window_id() const { return screen==1?2:1; }
 int only_screen() const { return screen; }
 void output_size(int& w,int& h) const { w=640;h=480; }
 bool map_point(int x,int y,int& s,int& sx,int& sy) const {
  if(x<0 || y<0 || x>=640 || y>=480)return false;
  s=screen;sx=x*256/640;sy=y*192/480;return true;
 }
 void on_resize() {}
};
}
'''
            )
            (tmp / 'test.cpp').write_text(
                r'''
#include "frontend/sdl/input.h"
#include "display.h"
#include <cassert>
int main(){
 ds::sdl::Input in;ds::sdl::Display panel;
 auto finger=[&](Uint32 type,SDL_FingerID id,float x,float y,SDL_TouchID device=7){
  SDL_Event e{};e.type=type;e.tfinger.touchId=device;e.tfinger.fingerId=id;e.tfinger.x=x;e.tfinger.y=y;
  in.handle(e,panel);
 };
 panel.screen=0;finger(SDL_FINGERDOWN,9,.5f,.5f);assert(!in.frame().down);
 panel.screen=1;finger(SDL_FINGERMOTION,1,.5f,.5f);assert(!in.frame().down);
 finger(SDL_FINGERDOWN,1,0,0);auto f=in.frame();assert(f.down && f.x==0 && f.y==0);
 // A continuous diagonal survives interleaved second-finger down/move/up.
 for(int i=1;i<=100;i++){
  finger(SDL_FINGERDOWN,2,.1f,.9f);finger(SDL_FINGERMOTION,2,.9f,.1f);
  finger(SDL_FINGERUP,2,.9f,.1f);
  finger(SDL_FINGERMOTION,1,float(i)/100,float(i)/100);
  f=in.frame();assert(f.down);assert(f.x==std::min(255,int(float(i)/100*640)*256/640));
 }
 assert(f.x==255 && f.y==191);
 SDL_Event mouse{};mouse.type=SDL_MOUSEBUTTONUP;mouse.button.button=SDL_BUTTON_LEFT;
 in.handle(mouse,panel);assert(in.frame().down);
 finger(SDL_FINGERUP,1,1,1,8);assert(in.frame().down); // wrong touch device
 finger(SDL_FINGERUP,1,1,1);assert(!in.frame().down);
 finger(SDL_FINGERMOTION,2,.2f,.8f);assert(!in.frame().down);
 // Separate short tap is visible for one frame, then lifts.
 finger(SDL_FINGERDOWN,3,.5f,.5f);finger(SDL_FINGERUP,3,.5f,.5f);
 f=in.frame();assert(f.down && f.x==128 && f.y==96);assert(!in.frame().down);
 // Out-of-range normalized coordinates clamp instead of losing a stroke.
 finger(SDL_FINGERDOWN,4,1.01f,-.1f);f=in.frame();assert(f.down && f.x==255 && f.y==0);
 finger(SDL_FINGERUP,4,1.01f,-.1f);assert(!in.frame().down);
}
'''
            )
            flags = shlex.split(subprocess.check_output(['sdl2-config', '--cflags', '--libs'], text=True))
            flags += [
                '-I' + str(Path(f[2:]).parent) for f in flags if f.startswith('-I') and Path(f[2:]).name == 'SDL2'
            ]
            command = [
                'clang++', '-std=c++17', '-I' + str(source), '-I' + str(source / 'frontend/sdl'),
                str(tmp / 'test.cpp'),
                str(tmp / 'input.cpp'),
                str(source / 'frontend/sdl/config.cpp'),
                str(source / 'frontend/sdl/pad_faces.cpp'), *flags, '-o',
                str(tmp / 'test')
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(tmp / 'test')], check=True, timeout=10)


if __name__ == '__main__': unittest.main()
