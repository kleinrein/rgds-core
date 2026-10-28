"""Vibration: the motor's patterns, the Vibration settings page, menu cues on the
motor (also with sounds off) and the game rumble setting handed to engines."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
HAPTICS_PAGE = 41
# Haptic indices (platform/haptics.hpp): the first seventeen follow MenuCue.
OPEN, BACK, EDGE = 2, 3, 17


class HapticEngineTests(unittest.TestCase):
    def test_patterns_and_drop_rules(self):
        source = r'''
#include "platform/haptics.hpp"
#include <cassert>
#include <thread>
using namespace rgds;
int main() {
    for (const auto& p : haptic_patterns) {
        assert(p.count >= 1 && p.count <= 4);
        int total = 0;
        for (int i = 0; i < p.count; i++) {
            assert(p.pulses[i].drive > 0 && p.pulses[i].drive <= 1);
            assert(p.pulses[i].ms >= 8);
            total += p.pulses[i].ms + p.pulses[i].gap;
        }
        assert(total <= 400);
    }
    Haptics h;
    h.set_strength(0);
    assert(!h.play(Haptic::Select) && h.plays == 0);
    h.set_strength(60);
    assert(h.play(Haptic::Tick));
    assert(!h.play(Haptic::Tick));            // ticks closer than 28 ms are dropped
    assert(h.play(Haptic::Select));
    assert(h.play(Haptic::Error));
    assert(!h.play(Haptic::Tick));            // a tick never cuts an error short
    assert(h.play(Haptic::Back, false) == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    assert(h.play(Haptic::Back));
    assert(h.play(Haptic::Notify, true));
    assert(!h.play(Haptic::Save));            // a pinned preview plays out
    assert(h.counts[size_t(Haptic::Error)] == 1 && h.plays == 5);
    h.set_rumble_strength(50);
    h.rumble(1);
    h.rumble(1);
    h.rumble(0);
    h.rumble(.5f);
    assert(h.rumbles == 2);
    h.set_rumble_strength(0);
    h.rumble(1);
    assert(h.rumbles == 2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'test.cpp').write_text(source)
            subprocess.run(
                [
                    os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT / 'src'),
                    str(Path(tmp) / 'test.cpp'), '-o',
                    str(Path(tmp) / 'test')
                ],
                check=True
            )
            subprocess.run([str(Path(tmp) / 'test')], check=True)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class VibrationUITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'preferences.ini').write_text('schema=1\nmode=light\nfont=rounded\n')

    def run_ui(self, script='', prefs='', entry=('--settings', '--section', 'vibration'), seconds=2):
        (self.root / 'ui.ini').write_text('menu_music=0\nanimations=0\n' + prefs)
        (self.root / 'input').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.root), '--state',
                str(self.root / 'state'), '--input-script',
                str(self.root / 'input'), '--cache-report',
                str(self.root / 'report'), '--seconds',
                str(seconds), *entry
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=8
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report').read_text())

    def test_on_by_default_and_strengths_step_and_persist(self):
        r = self.run_ui('100 key Down\n200 key Left\n300 key Down\n400 key Down\n500 key Right\n')
        self.assertEqual(r['page'], HAPTICS_PAGE)
        self.assertEqual((r['menu_haptics'], r['game_rumble']), (1, 1))
        self.assertEqual((r['haptic_strength'], r['rumble_strength']), (50, 90))
        saved = (self.root / 'ui.ini').read_text()
        for line in ('menu_haptics=1', 'haptic_strength=50', 'game_rumble=1', 'rumble_strength=90'):
            self.assertIn(line + '\n', saved)

    def test_switches_turn_off_and_a_switched_off_strength_does_not_move(self):
        r = self.run_ui('100 key A\n200 key Down\n300 key Right\n400 key Down\n500 key A\n')
        self.assertEqual((r['menu_haptics'], r['haptic_strength'], r['game_rumble']), (0, 60, 0))
        self.assertIn('menu_haptics=0\n', (self.root / 'ui.ini').read_text())
        self.assertIn('game_rumble=0\n', (self.root / 'ui.ini').read_text())

    def test_menu_cues_vibrate_with_sounds_off_and_not_with_haptics_off(self):
        r = self.run_ui('100 key A\n300 key Escape\n', 'menu_sounds=0\n', ('--settings',))
        self.assertEqual(r['sound_plays'], 0)
        self.assertGreaterEqual(r['haptic_counts'][OPEN], 1)
        self.assertGreaterEqual(r['haptic_counts'][BACK], 1)
        r = self.run_ui('100 key A\n300 key Escape\n', 'menu_haptics=0\n', ('--settings',))
        self.assertEqual(r['haptic_plays'], 0)
        self.assertGreater(r['sound_plays'], 0)

    def test_end_of_a_list_bumps_once_per_press_not_per_repeat(self):
        r = self.run_ui('100 key Up\n150 key-up Up\n200 key Up\n250 key-up Up\n')
        self.assertEqual(r['haptic_counts'][EDGE], 1)
        r = self.run_ui('100 key Up\n150 key-up Up\n700 key Up\n750 key-up Up\n')
        self.assertEqual(r['haptic_counts'][EDGE], 2)

    def test_sound_page_vibration_row_opens_the_page(self):
        r = self.run_ui('100 click 300 381\n', entry=('--settings', '--section', 'sound'))
        self.assertEqual(r['page'], HAPTICS_PAGE)
        r = self.run_ui(
            '100 key Down\n200 key Down\n300 key Down\n400 key Down\n500 key A\n',
            entry=('--settings', '--section', 'sound')
        )
        self.assertEqual(r['page'], HAPTICS_PAGE)


class RumbleLaunchTests(unittest.TestCase):
    def test_engines_get_the_rumble_strength_or_zero(self):
        import sys
        sys.path.insert(0, str(ROOT / 'runtime'))
        import emulators
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            for ui, expected in (
                ('', '80'), ('rumble_strength=40\n', '40'), ('game_rumble=0\nrumble_strength=40\n', '0'),
                ('rumble_strength=x\n', '80')
            ):
                (data / 'ui.ini').write_text(ui)
                self.assertEqual(emulators.rumble_setting(data), expected, ui)


if __name__ == '__main__': unittest.main()
