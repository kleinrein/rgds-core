"""Game information and extra pictures: parsing, matching, merging and what is skipped on a second run."""
import io
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
import game_info
import public_artwork as art
import scraper
from library import read_fields, write_fields

DATABASE = b'''<?xml version="1.0" encoding="UTF-8"?>
<datafile>
  <game name="Puzzle (USA)">
    <id>ABCE</id><type>DS</type><region>NTSC-U</region><languages>EN,FR</languages>
    <locale lang="EN"><title>Puzzle Quest</title><synopsis>Match gems.

Win the day.</synopsis></locale>
    <locale lang="FR"><title>Puzzle Quete</title><synopsis>Alignez des gemmes.</synopsis></locale>
    <developer>Acme</developer><publisher>Acme Games</publisher>
    <date year="2007" month="5" day="9"/><genre>puzzle,strategy</genre>
    <rating type="ESRB" value="E"/>
    <wi-fi players="4"><feature>online</feature></wi-fi>
    <input players-multi-cart="2" players-single-cart="1"/>
  </game>
  <game name="Other (USA)"><id>ZZZE</id><locale lang="EN"><title>Other</title></locale></game>
</datafile>'''

SERIALS = '''clrmamepro (
\tname "Nintendo - Game Boy Advance"
)

game (
\tname "Hero Saga (USA)"
\tregion "USA"
\tserial "AHRE"
\trom ( name "Hero Saga (USA).gba" size 8 crc {crc} md5 AA sha1 BB serial "AHRE" )
)
'''


def dat(key, value, crc, name="Hero Saga (USA)"):
    return 'clrmamepro (\n\tname "x"\n)\n\ngame (\n\tcomment "%s"\n\t%s\n\trom ( crc %s )\n)\n' % (
        name, key + ' ' + value, crc
    )


class Client:
    """Stands in for PublicClient: catalogues come from a dict and nothing touches the network."""
    def __init__(self, catalogs=None, fetched=None, cache=None):
        self.catalogs, self.fetched, self.cache = catalogs or {}, fetched or {}, cache
        self.requests = []

    def catalog(self, filename, url, parse):
        self.requests.append(filename)
        if filename not in self.catalogs: raise scraper.ScraperError('catalog unavailable')
        return parse(self.catalogs[filename])

    def fetch(self, url, limit=0):
        self.requests.append(url)
        for fragment, content in self.fetched.items():
            if fragment in url: return content
        raise scraper.NotFound()

    def names_for_code(self, code, system='nds'):
        return []


class GameInfoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.item = self.root / 'item'
        self.item.mkdir()

    def archive(self):
        path = self.root / 'dstdb.zip'
        with zipfile.ZipFile(path, 'w') as packed:
            packed.writestr('dstdb.xml', DATABASE)
        return path

    def ds_rom(self, name='Puzzle.nds'):
        path = self.root / name
        path.write_bytes(b'\0' * 12 + b'ABCE' + b'\0' * 500)
        return path

    def test_parse_kinds_ignores_unknown_words_and_keeps_order(self):
        self.assertEqual(game_info.parse_kinds('title, info,bogus'), ('info', 'title'))
        self.assertEqual(game_info.parse_kinds(''), ())

    def test_parse_dat_reads_crc_name_and_value(self):
        raw = dat('genre', '"Shooter"', 'abcdef12') + dat('users', '2', '00000001', 'Two')
        self.assertEqual(game_info.parse_dat(raw, 'genre'), {'ABCDEF12': ('Hero Saga (USA)', 'Shooter')})
        self.assertEqual(game_info.parse_dat(raw, 'users'), {'00000001': ('Two', '2')})
        with self.assertRaises(scraper.ScraperError):
            game_info.parse_dat('nothing here', 'genre')

    def test_no_intro_list_gives_names_by_crc(self):
        names = game_info.parse_dat(SERIALS.format(crc='0000ABCD'), '')
        self.assertEqual(names['0000ABCD'][0], 'Hero Saga (USA)')

    def test_gametdb_records_stream_only_wanted_codes(self):
        records = game_info.gametdb_records(self.archive(), {'ABCE'})
        self.assertEqual(set(records), {'ABCE'})
        record = records['ABCE']
        self.assertEqual(record['genre'], 'Puzzle, Strategy')
        self.assertEqual(record['release'], '2007-05-09')
        self.assertEqual(record['rating'], 'ESRB E')
        self.assertEqual(record['players'], 4)
        self.assertEqual(record['features'], 'online')
        self.assertEqual(set(record['synopses']), {'EN', 'FR'})

    def test_ds_information_keeps_library_title_and_existing_developer(self):
        rom = self.ds_rom()
        write_fields(self.item / 'info.ini', {'title': 'My own title', 'developer': 'Mine', 'provider': 'Libretro'})
        info = game_info.GameInfo(Client(), ('info',))
        info.prepare([rom], lambda _: self.item)
        info.records = game_info.gametdb_records(self.archive(), {'ABCE'})
        self.assertEqual(info.process(rom, self.item), {'info': 'ok'})
        saved = read_fields(self.item / 'info.ini')
        self.assertEqual(saved['title'], 'My own title')
        self.assertEqual(saved['developer'], 'Mine')
        self.assertEqual(saved['info_title'], 'Puzzle Quest')
        self.assertEqual(saved['publisher'], 'Acme Games')
        self.assertEqual(saved['year'], '2007')
        self.assertEqual(saved['players'], '4')
        self.assertEqual(saved['synopsis_languages'], 'EN,FR')
        self.assertEqual((self.item / 'synopsis.EN.txt').read_text(), 'Match gems.\n\nWin the day.\n')
        self.assertEqual(read_fields(self.item / 'info-status.ini'), {'info': 'ok', 'version': '1'})

    def test_second_run_skips_found_and_missing_entries_but_force_retries(self):
        rom = self.ds_rom()
        info = game_info.GameInfo(Client(), ('info',))
        info.records = {}
        self.assertEqual(info.process(rom, self.item), {'info': 'missing'})
        self.assertEqual(info.process(rom, self.item), {'info': 'cached'})
        self.assertEqual(info.process(rom, self.item, force=True), {'info': 'missing'})

    def test_unavailable_database_is_an_error_that_is_retried(self):
        rom = self.ds_rom()
        info = game_info.GameInfo(Client(), ('info',))
        info.records_error = 'The game information database was unreadable.'
        self.assertTrue(info.process(rom, self.item)['info'].startswith('error'))
        self.assertNotIn('info', read_fields(self.item / 'info-status.ini'))

    def test_prepare_downloads_once_and_only_when_a_ds_game_needs_it(self):
        rom = self.ds_rom()
        client = Client(fetched={'dstdb.zip': self.archive().read_bytes()}, cache=self.root / 'cache')
        info = game_info.GameInfo(client, ('info',))
        info.prepare([rom], lambda _: self.item)
        self.assertIn('ABCE', info.records)
        self.assertTrue((self.root / 'cache/gametdb-ds.zip').exists())
        write_fields(self.item / 'info-status.ini', {'info': 'ok', 'version': '1'})
        client.requests.clear()
        game_info.GameInfo(client, ('info',)).prepare([rom], lambda _: self.item)
        self.assertEqual(client.requests, [])

    def test_gba_information_matches_by_rom_crc(self):
        rom = self.root / 'Hero Saga.gba'
        rom.write_bytes(b'hero' * 64)
        crc = '%08X' % zlib.crc32(rom.read_bytes())
        client = Client(
            catalogs={
                'gba-developer.dat': dat('developer', '"Acme"', crc),
                'gba-genre.dat': dat('genre', '"rpg"', crc),
                'gba-releaseyear.dat': dat('releaseyear', '"2004"', crc),
                'gba-releasemonth.dat': dat('releasemonth', '"6"', crc),
                'gba-maxusers.dat': dat('users', '2', crc),
                'gba-esrb.dat': dat('esrb_rating', '"T"', crc),
                'gba-publisher.dat': dat('publisher', '"Acme Games"', '00000000'),
                'gba-franchise.dat': dat('franchise', '"Hero"', '00000000'),
                'gba-nointro.dat': SERIALS.format(crc=crc)
            }
        )
        info = game_info.GameInfo(client, ('info',))
        self.assertEqual(info.process(rom, self.item), {'info': 'ok'})
        saved = read_fields(self.item / 'info.ini')
        self.assertEqual(saved['developer'], 'Acme')
        self.assertEqual(saved['release'], '2004-06')
        self.assertEqual(saved['rating'], 'ESRB T')
        self.assertEqual(saved['players'], '2')
        self.assertEqual(saved['info_provider'], 'Libretro database')
        self.assertNotIn('publisher', saved)
        self.assertEqual(saved['info_title'], 'Hero Saga')

    def test_zip_crc_comes_from_the_directory_without_unpacking(self):
        inner = b'gb' * 100
        packed = self.root / 'game.zip'
        with zipfile.ZipFile(packed, 'w') as archive:
            archive.writestr('Game.gb', inner)
        self.assertEqual(game_info.rom_crc(packed), '%08X' % zlib.crc32(inner))

    def test_picture_uses_exact_no_intro_name_then_stores_a_bmp(self):
        rom = self.root / 'renamed.gb'
        rom.write_bytes(b'tetris' * 20)
        crc = '%08X' % zlib.crc32(rom.read_bytes())
        names = SERIALS.format(crc=crc).replace('Hero Saga (USA)', 'Tetris (World)')
        page = '<a href="Tetris%20(World).png">x</a><a href="Other.png">y</a>'
        client = Client(
            catalogs={
                'gb-nointro.dat': names,
                'gb-snap.html': page,
                'gb-title.html': page
            },
            fetched={'Named_Snaps/Tetris%20%28World%29.png': b'png'}
        )
        info = game_info.GameInfo(client, ('snap', 'title'))
        with patch.object(game_info, 'image_to_bmp') as save:
            outcome = info.process(rom, self.item)
        self.assertEqual(outcome, {'snap': 'ok', 'title': 'missing'})
        save.assert_called_once_with(b'png', self.item / 'snap.bmp')
        self.assertEqual(read_fields(self.item / 'info-status.ini')['title'], 'missing')

    def test_pick_name_prefers_the_roms_region_and_never_crosses_sequels(self):
        index = ['Puzzle (Europe)', 'Puzzle (USA)', 'Puzzle 2 (USA)']
        self.assertEqual(game_info.pick_name(index, ['Puzzle'], 'ABCE'), 'Puzzle (USA)')
        self.assertEqual(game_info.pick_name(index, ['Puzzle'], 'ABCP'), 'Puzzle (Europe)')
        self.assertEqual(game_info.pick_name(index, ['Puzzle (USA)'], 'ABCP'), 'Puzzle (USA)')
        self.assertEqual(game_info.pick_name(index, ['Puzzle 3'], 'ABCE'), '')

    def test_scrape_runs_extras_without_touching_the_cover_result(self):
        rom = self.ds_rom()
        data = self.root / 'data'
        item = data / 'library' / scraper.game_key(rom)
        item.mkdir(parents=True)
        (item / 'box.bmp').write_bytes(b'BM')
        info = game_info.GameInfo(Client(), ('info',))
        info.records = game_info.gametdb_records(self.archive(), {'ABCE'})
        with patch.object(info, 'prepare'):
            scraper.scrape(data, art.PublicClient(), [rom], extras=info)
        self.assertEqual(read_fields(data / 'scraper/status.ini')['state'], 'done')
        self.assertEqual(read_fields(item / 'info.ini')['publisher'], 'Acme Games')
        self.assertEqual((item / 'box.bmp').read_bytes(), b'BM')


if __name__ == '__main__':
    unittest.main()
