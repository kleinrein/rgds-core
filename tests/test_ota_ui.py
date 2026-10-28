"""UI acceptance for updates/statistics and large-library rail containment."""
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import test_visual_options as helpers

ROOT = Path(__file__).resolve().parents[1]


class OTAUITests(unittest.TestCase):
    setUp = helpers.VisualOptionsTests.setUp
    run_ui = helpers.VisualOptionsTests.run_ui
    pixel = staticmethod(helpers.VisualOptionsTests.pixel)

    def test_statistics_entry_and_back(self):
        self.run_ui(extra=('--settings', '--section', 'statistics'))
        self.assertIn('27 7 0', (self.root / 'state').read_text())
        self.run_ui('200 key Escape\n', ('--settings', '--section', 'statistics'))
        self.assertIn('1 7 0', (self.root / 'state').read_text())

    def test_settings_home_tile_has_no_play_summary(self):
        data = self.run_ui('20 key Left\n40 key-up Left\n', ('--home',))
        self.assertIn('home 0', (self.root / 'state').read_text())
        self.assertEqual(
            sum(self.pixel(data, x, y) == (45, 57, 68) for x in range(50, 592) for y in range(239, 262)), 0
        )

    def test_updates_are_reachable_and_unconfigured_is_explicit(self):
        script = '20 key Down\n40 key-up Down\n80 key Down\n100 key-up Down\n140 key Down\n160 key-up Down\n180 key Down\n190 key-up Down\n200 key Down\n210 key-up Down\n220 key Down\n230 key-up Down\n260 key A\n'  # Updates & recovery is the seventh System row
        self.run_ui(script, ('--settings', '--section', 'system'))
        self.assertIn('17 5 0', (self.root / 'state').read_text())
        self.assertIn('state=unconfigured', (self.root / 'data/update/status.ini').read_text())

    def test_update_progress_renders_from_async_worker(self):
        binaries = self.root / 'bin'
        binaries.mkdir()
        helper = binaries / 'python3'
        helper.write_text(
            '#!' + sys.executable + '\n' + '''import os,sys,time
from pathlib import Path
if 'update-check' in sys.argv:
 d=Path(sys.argv[sys.argv.index('--data')+1])/'update';d.mkdir(parents=True,exist_ok=True)
 (d/'status.ini').write_text('state=downloading\\nmessage=Downloading update…\\nprogress=50\\nversion=0.1.0\\n')
 time.sleep(10)
else:os.execv(sys.executable,[sys.executable,*sys.argv[1:]])
'''
        )
        helper.chmod(0o755)
        (self.root / 'state').write_text('1 0 0 ""\n17 5 0\n')
        with patch.dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH']):
            data = self.run_ui('400 key A\n')
        # The glitter bar's tube (x 51..589, middle row 291) is lit to half: blue on the
        # left, the groove's grey floor on the right.
        blue = [x for x in range(51, 590) if self.bluish(self.pixel(data, x, 291))]
        self.assertTrue(250 <= len(blue) <= 300, len(blue))
        self.assertLess(max(blue), 340)

    @staticmethod
    def bluish(p):
        return p[2] > p[0] + 60

    def fake_worker(self, status=''):
        # A stand-in python3 that records each device action and holds a download open.
        binaries = self.root / 'bin'
        binaries.mkdir(exist_ok=True)
        helper = binaries / 'python3'
        self.actions = self.root / 'actions'
        helper.write_text(
            '#!' + sys.executable + '\n' + f'''import os,sys,time
from pathlib import Path
action=next((a for a in sys.argv if a.startswith('update-')),'')
if action:
 open({str(self.actions)!r},'a').write(action+'\\n')
 if action=='update-download':
  d=Path(sys.argv[sys.argv.index('--data')+1])/'update';(d/'status.ini').write_text({status!r})
 time.sleep(10)
else:os.execv(sys.executable,[sys.executable,*sys.argv[1:]])
'''
        )
        helper.chmod(0o755)
        return patch.dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'])

    def offer(self, **extra):
        folder = self.root / 'data/update'
        folder.mkdir(parents=True, exist_ok=True)
        values = dict(
            state='available',
            message='A new version is ready to download.',
            version='0.8.0',
            offer='0.8.0',
            size='262995716',
            received='0'
        )
        values.update(extra)
        (folder / 'status.ini').write_text(''.join(f'{k}={v}\n' for k, v in values.items()))

    def test_home_banner_appears_only_with_an_offer(self):
        def banner(data):
            return sum(self.bluish(self.pixel(data, x, 458)) for x in range(100, 380))  # below the banner's text

        (self.root / 'ui.ini').write_text('schema=1\nhome_layout=column\nbackground=plain\n')
        self.assertLess(banner(self.run_ui(extra=('--home',))), 20)
        self.offer()
        self.assertGreater(banner(self.run_ui(extra=('--home',))), 250)

    def test_y_on_home_starts_the_download_and_back_returns_home(self):
        self.offer()
        with self.fake_worker(
            'state=downloading\nmessage=Downloading update…\nversion=0.8.0\noffer=0.8.0\nsize=1000\nreceived=500\nprogress=50\n'
        ):
            self.run_ui('500 key Y\n540 key-up Y\n', ('--home',))
        self.assertIn('17 9 1', (self.root / 'state').read_text())  # Updates, on its download row
        self.assertIn('update-download', self.actions.read_text())
        self.offer(state='cancelled', received='1000')
        with self.fake_worker():
            self.run_ui('500 key Y\n540 key-up Y\n800 key Escape\n840 key-up Escape\n', ('--home',))
        state = (self.root / 'state').read_text()
        self.assertNotIn('17 9', state)
        self.assertIn('home ', state)  # B goes straight back to Home

    def test_y_on_a_ready_update_asks_to_restart(self):
        self.offer(state='staged', prepared='1')
        with self.fake_worker():
            self.run_ui('500 key Y\n540 key-up Y\n', ('--home',))
        self.assertIn('18 9 0', (self.root / 'state').read_text())  # the Restart and install confirmation
        self.assertFalse(self.actions.exists() and 'update-download' in self.actions.read_text())

    def test_updates_is_a_root_entry_below_system(self):
        # Section ids stay stable; Updates (9) is listed between System (5) and Controls (6).
        (self.root / 'state').write_text('1 0 0 ""\n1 5 0\n')
        self.run_ui('300 key Down\n340 key-up Down\n')
        self.assertIn('1 9 0', (self.root / 'state').read_text())
        self.run_ui('300 key Down\n340 key-up Down\n')
        self.assertIn('1 6 0', (self.root / 'state').read_text())
        (self.root / 'state').write_text('1 0 0 ""\n1 9 0\n')
        self.run_ui('300 key A\n340 key-up A\n')
        self.assertIn('17 9 0', (self.root / 'state').read_text())
        self.run_ui('300 key Escape\n340 key-up Escape\n')
        self.assertIn('1 9 0', (self.root / 'state').read_text())  # B lands on Updates again

    def test_footer_key_resumes_a_paused_download(self):
        self.offer(state='cancelled', received='5000', progress='40')
        (self.root / 'state').write_text('1 0 0 ""\n17 5 0\n')
        with self.fake_worker():
            self.run_ui('500 click 320 452\n')
        self.assertIn('update-download', self.actions.read_text())

    def test_large_library_thumb_stays_inside_viewport_at_every_position(self):
        source = self.root / 'rail.cpp'
        binary = self.root / 'rail'
        source.write_text(
            '''#include "launcher/carousel.hpp"
#include <cassert>
int main(){for(size_t count:{0,1,4,5,62,100,1000,100000})for(int i=0;i<=1000;i++){
 auto r=rgds::RailGeometry::at(count,count>1?double(count-1)*i/1000:0);
 constexpr int left=rgds::RailGeometry::left,right=left+rgds::RailGeometry::width;
 assert(r.thumb_left>=left && r.thumb_left+r.thumb_width<=right);   // the grabber stays on the track, flush at its ends
 assert(r.window_left>=left && r.window_left+r.window_width<=right);
 assert(r.thumb_width==rgds::RailGeometry::grab);   // the DSi grabber keeps one size whatever the count
}}
'''
        )
        subprocess.run(
            ['clang++', '-std=c++17', '-I' + str(ROOT / 'src'),
             str(source), '-o', str(binary)],
            check=True,
            capture_output=True
        )
        subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__': unittest.main()
