"""Optional spring and push transitions: persistence, actual SDL rendering, input and idle cost."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


class ViewSpringTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'C++ compiler required')
    def test_spring_bounds_interruptions_wraparound_and_hit_testing(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'spring.cpp'
            source.write_text(r'''#include "launcher/view_motion.hpp"
#include "launcher/settings.hpp"
#include <cassert>
int main(int argc,char**argv){
 assert(argc==2);
 rgds::Preferences saved;saved.view_style=rgds::ViewStyle::Push;saved.view_animations=true;
 auto pref_path=rgds::fs::path(argv[1])/"preferences.ini";rgds::save_preferences(pref_path,saved);
 auto loaded=rgds::load_preferences(pref_path);assert(loaded.view_style==rgds::ViewStyle::Push && loaded.view_animations);
 std::ofstream(pref_path.parent_path()/"ui.ini")<<"schema=1\nview_animations=1\nview_transition_style=future-style\n";
 loaded=rgds::load_preferences(pref_path);assert(loaded.view_style==rgds::ViewStyle::Spring && loaded.view_animations);
 rgds::Preferences prefs;assert(!prefs.view_animations && prefs.animations);
 rgds::ViewMotion m;assert(!m.update(100));assert(m.pose(100).sx==1);
 m.start(100);bool overshoot=false;
 for(unsigned n=100;n<580;n++){
  auto p=m.pose(n);assert(p.sx>.97 && p.sx<1.03 && p.sy>.97 && p.sy<1.03);assert(p.tilt==0);assert(std::abs(p.x)<35);
  if(p.x<0)overshoot=true;
  double angle=p.tilt*3.141592653589793/180;
  double dx=(180-320)*p.sx,dy=(220-240)*p.sy;
  int x=int(std::round(320+p.x+dx*std::cos(angle)-dy*std::sin(angle)));
  int y=int(std::round(240+p.y+dx*std::sin(angle)+dy*std::cos(angle)));
  auto hit=m.unproject(x,y,n);assert(std::abs(hit.first-180)<=1 && std::abs(hit.second-220)<=1);
 }
 rgds::ViewMotion push;
 for(auto route:{rgds::ViewTravel::Forward,rgds::ViewTravel::Back,rgds::ViewTravel::Left,rgds::ViewTravel::Right}){
  push.start(100,route,rgds::ViewStyle::Push);double previous=1;
  for(unsigned n=100;n<400;n++){
   auto layers=push.push(n);auto p=push.pose(n);
   assert(p.y==0 && p.sx==1 && p.sy==1 && p.tilt==0 && p.trail==0);
   assert(std::abs(layers.incoming)<=640 && std::abs(layers.outgoing)<=640);
   assert(std::abs(layers.incoming)<=previous*640+0.001);previous=std::abs(layers.incoming)/640;
   assert(push.element(0,{24,96,566,66},true,n).x==0 && push.element(0,{24,96,566,66},true,n).y==0 && push.accent(0,n)==0);
   int x=int(std::round(180+p.x));auto hit=push.unproject(x,220,n);assert(std::abs(hit.first-180)<=1 && hit.second==220);
  }
  assert(push.pose(400).x==0);assert(push.update(400) && !push.active() && !push.update(401));
 }
 push.start(100,rgds::ViewTravel::Forward,rgds::ViewStyle::Push);auto enter=push.push(100);assert(enter.incoming==640 && enter.outgoing==0 && enter.incoming_above);
 push.start(100,rgds::ViewTravel::Back,rgds::ViewStyle::Push);auto leave=push.push(100);assert(leave.incoming==-160 && leave.outgoing==0 && !leave.incoming_above);
 push.start(UINT32_MAX-100,rgds::ViewTravel::Back,rgds::ViewStyle::Push);assert(push.update(30) && push.running);push.update(200);assert(!push.running);
 assert(overshoot);assert(m.update(720));assert(!m.running && !m.update(721));
 assert(m.pose(580).sx==1 && m.pose(580).x==0);
 m.start(800);auto forward=m.pose(800);m.start(820,rgds::ViewTravel::Back);auto back=m.pose(820);assert(back.x<0 && forward.x>0 && back.sx>1 && forward.sx<1);
 m.stop();assert(m.unproject(180,220,621)==std::make_pair(180,220));
 m.start(UINT32_MAX-100);assert(m.update(30) && m.running);m.update(600);assert(!m.running);
 rgds::MotionRect first{24,96,566,66},last{24,342,566,66};
 m.interact(1000,60,120);assert(m.energy==1);m.interact(1060,60,120);assert(m.energy>1 && m.energy<=1.05);
 m.start(1060);auto variant=m.variant;auto e=m.element(0,first,true,1100);
 auto same=m.element(0,first,true,1100);assert(e.x==same.x && e.y==same.y);
 auto other=m.element(3,last,false,1130);assert(e.x!=other.x || e.y!=other.y);
 m.start(1200);assert(m.variant!=variant);
 for(unsigned t=1200;t<1820;t++)for(unsigned id=0;id<64;id++){
  auto offset=m.element(id,first,id==0,t);assert(std::abs(offset.x)<10 && std::abs(offset.y)<12);
 }
 m.update(1820);auto rest=m.element(0,first,true,1820);assert(!m.active() && rest.x==0 && rest.y==0);
 m.interact(2400);assert(m.energy==1);auto focus=m.element(0,first,true,2460),neighbour=m.element(1,last,false,2460);
 assert(std::abs(focus.y)>std::abs(neighbour.y));m.update(2700);assert(!m.active());
 rgds::MotionHits hits;hits.add(first,20,-10,96,412);
 assert(hits.unproject(60,100)==std::make_pair(40,110));
 assert(hits.unproject(60,90)==std::make_pair(60,90)); // clipped pixels never capture a tap
 hits.add({24,96,566,66},-3,2);assert(hits.unproject(60,100)==std::make_pair(63,98));
 for(int i=0;i<100;i++)hits.add(first,0,0);assert(hits.count==64);hits.clear();assert(hits.count==0);
}
''')
            binary = Path(folder) / 'spring'
            subprocess.run(['clang++', '-std=c++17', '-I'+str(ROOT/'src'), str(source), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary), folder], check=True)


@unittest.skipUnless(BINARY.exists(), 'Build the host launcher first')
class ViewAnimationUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script='', entry=('--settings',), seconds=2, env_extra=None):
        (self.root/'input').write_text(script)
        result = subprocess.run([str(BINARY), '--preview', '--data', str(self.root),
            '--state', str(self.root/'state'), '--input-script', str(self.root/'input'),
            '--seconds', str(seconds), '--cache-report', str(self.root/'report'), *entry],
            cwd=ROOT, env=dict(os.environ, SDL_VIDEODRIVER='dummy', **(env_extra or {})), capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        return json.loads((self.root/'report').read_text())

    def enable(self, enabled=True, general=True, style="spring"):
        (self.root/'ui.ini').write_text(f'schema=1\nanimations={int(general)}\nview_animations={int(enabled)}\nview_transition_style={style}\n')

    def assert_settled(self, report):
        self.assertFalse(report['view_animation_active'])
        self.assertFalse(report['view_canvas_failed'])
        self.assertEqual(report['render_file_reads'], 0)
        self.assertLess(report['frames'], 150)

    def test_legacy_preferences_and_default_do_not_enable_view_motion(self):
        for entry in (('--settings',), ('--game-menu',)):
            report = self.run_ui('100 key A\n', entry)
            self.assertEqual(report['view_animations'], 0)
            self.assertEqual(report['view_animation_frames'], 0)
            self.assertEqual(report['element_moves'], 0)

    def test_touch_toggle_persists_and_keyboard_disables(self):
        report = self.run_ui('100 click 180 301\n250 click 180 240\n', ('--settings', '--section', 'theme'))
        self.assertTrue(report['view_animations'])
        self.assertIn('view_animations=1\n', (self.root/'ui.ini').read_text())
        self.assertGreater(report['view_animation_frames'], 0)
        self.assert_settled(report)
        report = self.run_ui('100 key Left\n', ())
        self.assertFalse(report['view_animations'])
        self.assertIn('view_animations=0\n', (self.root/'ui.ini').read_text())

    def test_launcher_changes_animate_and_return_to_idle(self):
        self.enable()
        short = self.run_ui('100 key A\n800 key Escape\n')
        self.assertEqual(short['view_transitions'], 2)
        self.assertEqual(short['back_transitions'], 1)
        self.assertGreater(short['element_moves'], 0)
        self.assertGreater(short['view_animation_frames'], 10)
        self.assert_settled(short)
        long = self.run_ui('100 key A\n800 key Escape\n', seconds=3)
        self.assertLessEqual(long['frames'], short['frames']+12)

    def test_game_menu_subpage_and_back_animate(self):
        self.enable()
        report = self.run_ui('100 key Down\n140 key Down\n180 key Right\n220 key A\n900 key Escape\n', ('--game-menu',))
        self.assertIn(11, report['visited_pages'])  # Sound
        self.assertEqual(report['page'], 12)       # QuickMenu
        self.assertEqual(report['back_transitions'], 1)  # Back to a higher enum is still backwards.
        self.assertGreater(report['element_moves'], 0)
        self.assertEqual(report['view_transitions'], 2)
        self.assertGreater(report['view_animation_frames'], 10)
        self.assert_settled(report)

    def test_rapid_navigation_does_not_queue_or_block_input(self):
        self.enable()
        report = self.run_ui('100 key A\n140 key Escape\n180 key A\n220 key Escape\n')
        self.assertEqual(report['page'], 1)
        self.assertEqual(report['view_transitions'], 4)
        self.assert_settled(report)

    def test_focus_reacts_without_navigating_and_settles(self):
        self.enable()
        report = self.run_ui('200 key Down\n280 key Down\n')
        self.assertEqual(report['page'], 1)
        self.assertEqual(report['view_transitions'], 0)
        self.assertGreater(report['element_moves'], 0)
        self.assertGreater(report['view_energy'], 1)
        self.assert_settled(report)

    def test_touch_during_view_motion_still_opens_the_visible_control(self):
        self.enable()
        # Theme enters at 100 ms. At 340 ms the visible Animations row is
        # settling close to its final centre; the inverse map tracks both layers.
        report = self.run_ui('100 key A\n340 click 300 301\n')
        self.assertEqual(report['page'], 33)  # Motion
        self.assertEqual(report['view_transitions'], 2)
        self.assert_settled(report)

    def test_home_and_library_layout_changes_animate(self):
        self.enable()
        report = self.run_ui('100 key A\n800 key Y\n1000 key Down\n1100 key Right\n1200 key A\n1800 key Escape\n', ('--home',), seconds=3)
        self.assertFalse(report['home_menu'])
        self.assertGreaterEqual(report['view_transitions'], 2)
        self.assertGreater(report['view_animation_frames'], 10)
        self.assert_settled(report)

    def test_push_selection_is_optional_and_persists_with_touch_and_controller(self):
        report = self.run_ui('100 click 180 301\n250 click 520 335\n', ('--settings', '--section', 'theme'))
        self.assertEqual(report['view_transition_style'], 'push')
        self.assertFalse(report['view_animations'])  # Choosing a style leaves motion off.
        self.assertIn('view_transition_style=push\n', (self.root/'ui.ini').read_text())
        report = self.run_ui('100 key Left\n', ())  # Restore focus on the style row.
        self.assertEqual(report['view_transition_style'], 'spring')
        self.assertFalse(report['view_animations'])
        report = self.run_ui('100 key Right\n', ())
        self.assertEqual(report['view_transition_style'], 'push')
        self.assert_settled(report)

    def test_push_parent_navigation_and_idle(self):
        self.enable(style='push')
        script = '100 key A\n550 click 300 301\n1000 key Escape\n1450 key Escape\n'
        report = self.run_ui(script, seconds=3)
        self.assertEqual(report['view_transition_style'], 'push')
        self.assertEqual(report['view_transitions'], 4)
        self.assertEqual(report['back_transitions'], 2)
        self.assertEqual(report['page'], 1)
        self.assertGreater(report['push_animation_frames'], 20)
        self.assertEqual(report['element_moves'], 0)
        self.assert_settled(report)
        long = self.run_ui(script, seconds=4)
        self.assertLessEqual(long['frames'], report['frames']+12)

    def test_push_rapid_reversal_and_game_menu_back(self):
        self.enable(style='push')
        report = self.run_ui('100 key A\n140 key Escape\n180 key A\n220 key Escape\n')
        self.assertEqual(report['page'], 1)
        self.assertEqual(report['view_transitions'], 4)
        self.assertGreater(report['push_animation_frames'], 0)
        self.assert_settled(report)
        report = self.run_ui('100 key Down\n140 key Down\n180 key Right\n220 key A\n700 key Escape\n', ('--game-menu',))
        self.assertEqual(report['page'], 12)
        self.assertEqual(report['back_transitions'], 1)
        self.assertEqual(report['view_transitions'], 2)
        self.assertGreater(report['push_animation_frames'], 10)
        self.assert_settled(report)

    def test_push_touch_only_activates_the_incoming_page(self):
        self.enable(style='push')
        report = self.run_ui('100 key A\n130 click 70 50\n')
        self.assertEqual(report['page'], 2)  # The old Settings page is still visible on the left.
        self.assertEqual(report['view_transitions'], 1)
        self.assert_settled(report)
        report = self.run_ui('100 key A\n280 click 312 301\n')
        self.assertEqual(report['page'], 33)  # Visible Animations control during the push.
        self.assertGreaterEqual(report['view_transitions'], 2)
        self.assert_settled(report)

    def test_push_restored_subpage_back_then_reopen_is_forward(self):
        self.enable(style='push')
        (self.root/'state').write_text('1 0 1 ""\n33 0 0\n')
        report = self.run_ui('100 key Escape\n600 key A\n', ())
        self.assertEqual(report['page'], 33)
        self.assertEqual(report['back_transitions'], 1)
        self.assertEqual(report['view_transitions'], 2)
        self.assertGreater(report['push_animation_frames'], 10)
        self.assert_settled(report)

    def test_push_master_off_and_unknown_style_fallback(self):
        self.enable(general=False, style='push')
        report = self.run_ui('100 key A\n')
        self.assertEqual(report['push_animation_frames'], 0)
        self.assert_settled(report)
        self.enable(style='future-style')
        report = self.run_ui('100 key Escape\n700 key A\n')
        self.assertEqual(report['view_transition_style'], 'spring')
        self.assertGreater(report['view_animation_frames'], 0)
        self.assertEqual(report['push_animation_frames'], 0)
        self.assert_settled(report)

    @unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'SDL compiler required')
    def test_renderer_without_targets_falls_back_to_instant_views(self):
        self.enable()
        source = self.root/'fallback.cpp'; library = self.root/'fallback.dylib'
        source.write_text('#include <SDL.h>\nextern "C" int SDL_SetRenderTarget(SDL_Renderer*,SDL_Texture*){return -1;}\n')
        flags = subprocess.check_output(['sdl2-config', '--cflags', '--libs'], text=True).split()
        subprocess.run(['clang++', '-shared', '-fPIC', str(source), *flags, '-o', str(library)], check=True, capture_output=True)
        for style in ('spring', 'push'):
            with self.subTest(style=style):
                self.enable(style=style)
                report = self.run_ui('100 key A\n', env_extra={'RGDS_SDL_LIBRARY': str(library)})
                self.assertEqual(report['page'], 2)
                self.assertTrue(report['view_canvas_failed'])
                self.assertEqual(report['view_animation_frames'], 0)
                self.assertFalse(report['view_animation_active'])

    def test_master_motion_off_suppresses_view_effect(self):
        self.enable(general=False)
        report = self.run_ui('100 key A\n')
        self.assertEqual(report['view_animation_frames'], 0)
        self.assert_settled(report)


if __name__ == '__main__': unittest.main()
