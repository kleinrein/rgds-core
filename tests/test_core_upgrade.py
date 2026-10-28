"""RG DS Core catalog, large collection cancellation and snapshot regressions."""
from pathlib import Path
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from menu_snapshot import top_panel


class SnapshotTests(unittest.TestCase):
    def bmp(self, w, h):
        stride = (w * 3 + 3) & ~3
        return (
            struct.pack('<2sIHHI', b'BM', 54 + stride * h, 0, 0, 54) +
            struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 24, 0, stride * h, 0, 0, 0, 0) +
            b''.join(bytes((y % 256, 10, 20)) * w + bytes(stride - w * 3) for y in range(h))
        )

    def test_stacked_capture_crops_top_and_bounds_texture(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'capture'
            out = Path(tmp) / 'out.bmp'
            source.write_bytes(self.bmp(640, 960))
            top_panel(source, out)
            data = out.read_bytes()
            self.assertEqual(struct.unpack_from('<ii', data, 18), (640, -480))
            self.assertEqual(data[54:57], bytes((0, 10, 20)))
            self.assertEqual(data[-3:], bytes((479 % 256, 10, 20)))

    def test_truncation_and_bad_masks_do_not_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'capture'
            out = Path(tmp) / 'out.bmp'
            source.write_bytes(self.bmp(2, 2)[:-1])
            with self.assertRaises(ValueError):
                top_panel(source, out)
            self.assertFalse(out.exists())

    def test_side_by_side_capture_uses_top_panel(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'capture'
            out = Path(tmp) / 'out.bmp'
            source.write_bytes(self.bmp(1280, 480))
            top_panel(source, out)
            self.assertEqual(struct.unpack_from('<ii', out.read_bytes(), 18), (640, -480))

    def test_bgra_rows_preserve_channels_and_orientation(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'capture'
            out = Path(tmp) / 'out.bmp'
            pixels = bytes((1, 2, 3, 255, 4, 5, 6, 255))
            source.write_bytes(
                struct.pack('<2sIHHI', b'BM', 62, 0, 0, 54) +
                struct.pack('<IiiHHIIiiII', 40, 1, 2, 1, 32, 0, 8, 0, 0, 0, 0) + pixels
            )
            top_panel(source, out)
            self.assertEqual(out.read_bytes()[54:57], bytes((4, 5, 6)))


class CatalogTests(unittest.TestCase):
    def test_catalogs_have_equal_nonempty_keys_and_placeholders(self):
        catalogs = []
        for language in ('en-US', 'nb-NO'):
            lines = [
                line.split('\t') for line in (ROOT / 'assets/locales' / f'{language}.tsv').read_text().splitlines()
                if line and not line.startswith('#')
            ]
            self.assertTrue(all(len(line) == 2 and all(line) for line in lines))
            values = dict(lines)
            self.assertEqual(len(values), len(lines))
            catalogs.append(values)
            for key, value in lines:
                self.assertEqual(key.count('{0}'), value.count('{0}'), key)
        self.assertEqual(catalogs[0].keys(), catalogs[1].keys())
        self.assertGreater(len(catalogs[0]), 500)

    @unittest.skipUnless(shutil.which('clang++'), 'C++ required')
    def test_locale_fallback_templates_and_large_collection_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            source.write_text(
                r'''
#include "launcher/i18n.hpp"
#include "launcher/collection_filter.hpp"
#include <cassert>
#include <thread>
int main(int argc,char** argv){
 assert(argc==3);rgds::Locale locale;locale.load(argv[1],"nb-NO");
 assert(locale.text("Games")=="Spill");assert(locale.text("Catalog fallback probe")=="Catalog fallback probe");
 assert(locale.text("Use default (DSperate)")=="Bruk standard (DSperate)");
 for(const auto& sample:std::vector<std::vector<std::string>>{
     {"nn-NO","Norsk nynorsk","Innstillingar","Bruk standard (DSperate)","3 spel"},
     {"sv-SE","Svenska","Inställningar","Använd standard (DSperate)","3 spel"},
     {"da-DK","Dansk","Indstillinger","Brug standard (DSperate)","3 spil"},
     {"nl-NL","Nederlands","Instellingen","Standaard gebruiken (DSperate)","3 spellen"}}){
   locale.load(argv[1],sample[0]);assert(locale.name()==sample[1]);
   assert(locale.text("Settings")==sample[2]);assert(locale.text("Use default (DSperate)")==sample[3]);
   assert(locale.text("3 games")==sample[4]);assert(locale.text("Uncatalogued ROM title")=="Uncatalogued ROM title");
 }
 locale.load(argv[1],"../../bad");assert(locale.text("Favourites")=="Favorites");
 rgds::CollectionFilter filter;std::vector<rgds::Game> games;
 for(int i=0;i<10000;i++)games.push_back({"Game "+std::to_string(i),std::filesystem::path(argv[2])/std::to_string(i),false});
 rgds::LibraryCache cache(argv[2]);rgds::GameOptions options;options.favourite=true;
 rgds::save_game_options(cache.folder(games[9999])/"game.ini",options);
 filter.request(argv[2],games,2);filter.poll();filter.request(argv[2],games,1);
 std::optional<rgds::CollectionFilter::Result> result;
 for(int i=0;i<20000 && !result;i++){result=filter.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 assert(result && result->games.size()==1 && result->games[0].title=="Game 9999");
 filter.request(argv[2],games,1);filter.poll();filter.cancel();
 while(filter.pending()){assert(!filter.poll());std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}'''
            )
            subprocess.run(
                [
                    'clang++', '-std=c++17', '-pthread',
                    *shlex.split(subprocess.check_output(['sdl2-config', '--cflags'], text=True)),
                    '-I' + str(ROOT / 'src'),
                    str(source), '-o',
                    str(binary)
                ],
                check=True
            )
            subprocess.run([str(binary), str(ROOT / 'assets/locales'), tmp], check=True, timeout=30)


class SnapshotHandoffTests(unittest.TestCase):
    def test_capture_timeout_opens_menu_without_blocking_or_stale_backdrop(self):
        from game_menu import GameMenu
        from unittest.mock import patch
        from test_game_menu import Process, Inputs
        import signal
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / 'data'
            engine = Process()
            menu_process = Process()
            clock = [10.0]
            with patch('game_menu.time.monotonic', side_effect=lambda: clock[0]
                       ), patch('game_menu.subprocess.Popen', return_value=menu_process) as spawn:
                menu = GameMenu(
                    engine,
                    Path(tmp) / 'app', data, 'dsperate',
                    Path(tmp) / 'game.nds', {'RGDS_CONTROL_FILE': str(data / 'session/emulator.control')}, Inputs()
                )
                backdrop = data / 'session/menu-background.bmp'
                backdrop.write_bytes(b'stale')
                try:
                    menu.open()
                    self.assertFalse(backdrop.exists())
                    self.assertEqual(engine.signals, [])
                    spawn.assert_not_called()
                    menu.poll_snapshot()
                    spawn.assert_not_called()
                    clock[0] += .19
                    menu.poll_snapshot()
                    spawn.assert_called_once()
                    self.assertEqual(engine.signals, [signal.SIGSTOP])
                    self.assertIsNone(menu.snapshot_deadline)
                    self.assertFalse(Path(str(menu.control) + '.capture').exists())
                finally:
                    menu.close()

    def test_exit_request_during_capture_stays_busy_and_engine_running(self):
        from game_menu import GameMenu
        from unittest.mock import patch
        from test_game_menu import Process, Inputs
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / 'data'
            engine = Process()
            clock = [10.0]
            with patch('game_menu.time.monotonic',
                       side_effect=lambda: clock[0]), patch('game_menu.subprocess.Popen', return_value=Process()):
                menu = GameMenu(
                    engine,
                    Path(tmp) / 'app', data, 'dsperate',
                    Path(tmp) / 'game.nds', {'RGDS_CONTROL_FILE': str(data / 'session/emulator.control')}, Inputs()
                )
                try:
                    menu.open()
                    menu.send('save-quit')
                    clock[0] += .19
                    menu.poll_snapshot()
                    self.assertFalse(menu.paused)
                    self.assertIn('state=busy', menu.status.read_text())
                finally:
                    menu.close()


@unittest.skipUnless((ROOT / 'build/host/rgds-launcher').exists(), 'Build host first')
class CoreUITests(unittest.TestCase):
    def run_ui(self, data, script='', extra=()):
        import json
        (data / 'input').write_text(script)
        if not (data / 'ui.ini').exists():
            (data /
             'ui.ini').write_text("schema=1\nhome_layout=carousel\nbackground=plain\n")  # the carousel home and rail
        result = subprocess.run(
            [
                str(ROOT / 'build/host/rgds-launcher'), '--preview', '--data',
                str(data), '--input-script',
                str(data / 'input'), '--state',
                str(data / 'state'), '--seconds', '1', '--cache-report',
                str(data / 'report.json'), *extra
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=8
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((data / 'report.json').read_text())

    def test_language_picker_persists_and_game_menu_loads_same_preference(self):
        languages = [line.split('\t')[0] for line in (ROOT / 'assets/locales/languages.tsv').read_text().splitlines()]
        for language in (
            'nb-NO', 'nn-NO', 'sv-SE', 'da-DK', 'nl-NL', 'fi-FI', 'pt-PT', 'vi-VN', 'pl-PL', 'ru-RU', 'uk-UA', 'ja-JP',
            'zh-CN', 'ko-KR'
        ):
            with self.subTest(language=language), tempfile.TemporaryDirectory() as tmp:
                data = Path(tmp)
                steps = languages.index(language)
                script = '20 key A\n40 key-up A\n' + ''.join(
                    f'{100+i*40} key Down\n{120+i*40} key-up Down\n' for i in range(steps)
                )
                # Every language, the last included, is chosen well inside the one-second run.
                script += f'{120+steps*40} key A\n'
                self.run_ui(data, script, ('--settings', '--section', 'system'))
                self.assertIn(f'language={language}\n', (data / 'ui.ini').read_text())
                before = (data / 'ui.ini').read_text()
                report = self.run_ui(data, extra=('--game-menu', '--state', str(data / 'game-menu-state')))
                self.assertEqual(report['page'], 12)
                self.assertEqual((data / 'ui.ini').read_text(), before)
                self.assertEqual(report['render_file_reads'], 0)

    def test_last_system_row_survives_launcher_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            (data / 'state').write_text('1 0 0 ""\n9 5 5\n')
            report = self.run_ui(data)
            self.assertEqual(report['page'], 9)
            self.assertIn('9 5 5\n', (data / 'state').read_text())

    def test_last_settings_entry_opens_power_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            report = self.run_ui(
                data, ''.join(f'{20+i*20} key Down\n' for i in range(10)) + '240 key A\n', ('--settings',)
            )
            self.assertEqual(report['page'], 18)

    def test_back_button_waits_for_release_and_rail_reaches_screen_bottom(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            report = self.run_ui(data, '100 tap 46 46\n', ('--library',))
            self.assertNotIn('home ', (data / 'state').read_text())
            report = self.run_ui(data, '100 tap 46 46\n200 release\n', ('--library',))
            self.assertIn('home ', (data / 'state').read_text())
            # Last physical row belongs to the rail, including the outermost edge.
            report = self.run_ui(data, '100 click 639 479\n', ('--home',))
            self.assertIn('home 2', (data / 'state').read_text())


if __name__ == '__main__': unittest.main()
