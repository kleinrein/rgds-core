"""Catalog coverage, historical totals and public artwork migration."""
from pathlib import Path
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RefinementTests(unittest.TestCase):
    def test_every_registered_language_has_complete_keys_and_shipped_glyphs(self):
        catalogs = ROOT / 'assets/locales'
        languages = [line.split('\t')[0] for line in (catalogs / 'languages.tsv').read_text().splitlines()]
        self.assertTrue(
            {
                "en-US", "nb-NO", "nn-NO", "da-DK", "sv-SE", "nl-NL", "de-DE", "fr-FR", "es-ES", "pt-BR", "it-IT",
                "fi-FI", "pt-PT", "vi-VN", "id-ID", "pl-PL", "tr-TR", "ru-RU", "uk-UA"
            }.issubset(languages)
        )
        self.assertEqual(len(languages), len(set(languages)))
        glyphs = None
        for path in sorted((ROOT / 'assets/fonts').glob('rounded-*.rgf')):
            font = path.read_bytes()
            count = struct.unpack_from('<I', font, 16)[0]
            current = {struct.unpack_from('<I', font, 20 + i * 32)[0] for i in range(count)}
            glyphs = current if glyphs is None else glyphs & current
        base = None
        for language in languages:
            rows = [
                line.split('\t') for line in (catalogs / (language + '.tsv')).read_text().splitlines()
                if line and not line.startswith('#')
            ]
            self.assertTrue(all(len(row) == 2 and all(row) for row in rows), language)
            values = dict(rows)
            self.assertEqual(len(values), len(rows))
            if base is None: base = values.keys()
            self.assertEqual(base, values.keys())
            for key, value in rows:
                self.assertEqual(key.count('{0}'), value.count('{0}'), (language, key))
                self.assertFalse(set(map(ord, value)) - glyphs, (language, value))

    def test_public_cli_rejects_account_provider_and_authentication(self):
        with tempfile.TemporaryDirectory() as tmp:
            for args in [('status', '--provider', 'screenscraper'), ('login',)]:
                result = subprocess.run(
                    [sys.executable, str(ROOT / 'runtime/scraper.py'), *args, '--data', tmp],
                    capture_output=True,
                    text=True
                )
                self.assertEqual(result.returncode, 2)
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_summary_counts_existing_records_and_migrates_old_provider(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            source.write_text(
                r'''
#include "launcher/library_summary.hpp"
#include "launcher/settings.hpp"
#include <cassert>
#include <thread>
int main(int argc,char** argv){
 assert(argc==2);rgds::fs::path data=argv[1];
 std::vector<rgds::Game> games={{"First",data/"a.nds",false},{"Last",data/"b.nds",false},{"Unplayed",data/"c.nds",false}};
 rgds::LibraryCache cache(data);
 for(size_t i=0;i<games.size();i++){auto folder=cache.folder(games[i]);rgds::fs::create_directories(folder);
  std::ofstream(folder/"stats.ini")<<"seconds="<<(i==0?"3600":i==1?"120":"-10")<<"\nsessions="<<(i==0?"2":i==1?"3":"-1")<<"\nlast_played="<<(i==0?"2026-10-11":i==1?"2026-10-12":"")<<'\n';
 }
 rgds::GameOptions options;options.favourite=true;rgds::save_game_options(cache.folder(games[1])/"game.ini",options);
 rgds::LibrarySummary summary;summary.request(data,games);std::optional<rgds::LibrarySummary::Result> result;
 for(int i=0;i<3000 && !result;i++){result=summary.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 assert(result);const auto& all=result->groups[0];assert(all.games==3 && all.sessions==5 && all.played==2 && all.seconds==3720 && all.last_title=="Last");
 assert(all.last_game && all.last_game->path==games[1].path);
 assert(result->groups[1].last_game && result->groups[1].last_game->path==games[1].path);
 assert(result->groups[1].games==1 && result->groups[1].sessions==3);assert(result->groups[2].games==2);
 assert(result->most_played.size()==2);
 assert(result->most_played[0].game.path==games[0].path && result->most_played[0].seconds==3600);
 assert(result->most_played[1].game.path==games[1].path && result->most_played[1].sessions==3);
 auto prefs=data/"preferences.ini";rgds::save_preferences(prefs,rgds::Preferences{});std::ofstream(data/"ui.ini")<<"schema=1\nartwork_provider=3\n";
 auto loaded=rgds::load_preferences(prefs);assert(loaded.artwork_provider==0);
}'''
            )
            flags = shlex.split(subprocess.check_output(['sdl2-config', '--cflags'], text=True))
            subprocess.run(
                ['clang++', '-std=c++17', '-pthread', *flags, '-I' + str(ROOT / 'src'),
                 str(source), '-o',
                 str(binary)],
                check=True
            )
            subprocess.run([str(binary), tmp], check=True, timeout=10)

    def test_generated_icons_keep_alpha_and_fixed_texture_budget(self):
        icons = list((ROOT / 'assets/icons').glob('*.bmp'))
        self.assertEqual(len(icons), 21)
        for path in icons:
            data = path.read_bytes()
            self.assertEqual(struct.unpack_from('<ii', data, 18), (128, -128))
            self.assertEqual(struct.unpack_from('<I', data, 66)[0], 0xff000000)
            alpha = data[125::4]
            self.assertIn(0, alpha)
            self.assertTrue(any(a > 200 for a in alpha))


if __name__ == '__main__': unittest.main()
