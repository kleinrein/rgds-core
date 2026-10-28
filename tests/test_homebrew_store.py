import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))

import homebrew_store
from homebrew_store import HomebrewStore, file_name, matches
from library import game_key

GAME = b'\x00GBA homebrew bytes'


def entry(system='gba', ident='gba/tiny', **file):
    record = dict(url='https://raw.githubusercontent.com/o/r/abc/tiny.gba', name='tiny.gba', size=len(GAME))
    record.update(file or dict(git_sha1=hashlib.sha1(b'blob %d\0' % len(GAME) + GAME).hexdigest()))
    return dict(
        id=ident,
        system=system,
        title='Tiny: Game',
        developer='Someone',
        license='MIT',
        description='Small.',
        page='https://example.org',
        file=record,
        cover=''
    )


def feed(*entries):
    return dict(schema=1, entries=list(entries))


class Services:
    def __init__(self, app, data, preview=False):
        self.app, self.data, self.preview, self.fields = Path(app), Path(data), preview, {}
        self.published = []

    def publish(self, **fields):
        self.fields.update({key: str(value) for key, value in fields.items()})
        self.published.append(dict(self.fields))


class HomebrewStoreTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.app, self.data, self.roms = root / 'app', root / 'data', root / 'roms'
        (self.app / 'assets/homebrew').mkdir(parents=True)
        self.data.mkdir()
        self.services = Services(self.app, self.data)
        self.store = HomebrewStore(self.services)

    def tearDown(self):
        self.temp.cleanup()

    def test_both_hash_kinds_and_the_size_are_checked(self):
        self.assertTrue(matches(entry(), GAME))
        self.assertFalse(matches(entry(), GAME + b'!'))
        self.assertTrue(matches(entry(sha256=hashlib.sha256(GAME).hexdigest()), GAME))
        self.assertFalse(matches(entry(sha256='0' * 64), GAME))

    def test_feed_online_then_cached_then_bundled(self):
        (self.app / 'assets/homebrew/feed.json').write_text(json.dumps(feed(entry(ident='gba/bundled'))))
        online = json.dumps(feed(entry(ident='gba/online'))).encode()
        with mock.patch.object(homebrew_store, 'fetch', return_value=online):
            self.assertEqual(self.store.feed()['entries'][0]['id'], 'gba/online')
        # cached and fresh: no network at all
        with mock.patch.object(homebrew_store, 'fetch', side_effect=AssertionError('fetched')):
            self.assertEqual(self.store.feed()['entries'][0]['id'], 'gba/online')
        (self.data / 'cache/homebrew/feed.json').unlink()
        with mock.patch.object(homebrew_store, 'fetch', side_effect=OSError('offline')):
            self.assertEqual(self.store.feed()['entries'][0]['id'], 'gba/bundled')

    def test_browse_lists_the_shelf_and_marks_what_is_installed(self):
        (self.app / 'assets/homebrew/feed.json').write_text(
            json.dumps(feed(entry(), entry(ident='gba/other'), entry('nes', 'nes/x')))
        )
        self.roms.mkdir()
        (self.roms / file_name('Tiny: Game', '.gba')).write_bytes(GAME)
        with mock.patch.object(homebrew_store, 'fetch', side_effect=OSError('offline')):
            self.store.browse(dict(system='gba', folder=str(self.roms)))
        fields = self.services.fields
        self.assertEqual(fields['hb_count'], '2')
        self.assertEqual(fields['hb_0_installed'], '1')
        self.assertEqual(fields['hb_0_license'], 'MIT')
        self.assertEqual(fields['hb_0_size'], str(len(GAME)))
        with self.assertRaises(ValueError):
            self.store.browse(dict(system='pico8', folder=str(self.roms)))

    def test_get_checks_the_file_then_files_it_with_its_maker(self):
        (self.app / 'assets/homebrew/feed.json').write_text(json.dumps(feed(entry())))
        calls = []

        def fetch(url, limit=0, timeout=0, progress=None):
            calls.append(url)
            if url == homebrew_store.FEED_URL: raise OSError('offline')
            if progress: progress(len(GAME))
            return GAME

        with mock.patch.object(homebrew_store, 'fetch', fetch):
            self.store.get(dict(id='gba/tiny', folder=str(self.roms)))
        target = self.roms / 'Tiny Game.gba'
        self.assertEqual(target.read_bytes(), GAME)
        info = (self.data / 'library' / game_key(target) / 'info.ini').read_text()
        self.assertIn('developer=Someone', info)
        self.assertIn('license=MIT', info)
        self.assertEqual(self.services.fields['state'], 'ready')
        self.assertNotIn('hb_get', self.services.fields)
        self.assertTrue(any(p.get('hb_received') == str(len(GAME)) for p in self.services.published))

    def test_a_damaged_download_is_not_kept(self):
        (self.app / 'assets/homebrew/feed.json').write_text(json.dumps(feed(entry())))
        with mock.patch.object(homebrew_store, 'fetch', side_effect=[OSError('offline'), GAME + b'x']):
            with self.assertRaises(ValueError):
                self.store.get(dict(id='gba/tiny', folder=str(self.roms)))
        self.assertFalse((self.roms / 'Tiny Game.gba').exists())

    def test_only_feed_entries_and_trusted_hosts(self):
        (self.app / 'assets/homebrew/feed.json').write_text(json.dumps(feed(entry())))
        with mock.patch.object(homebrew_store, 'fetch', side_effect=OSError('offline')):
            for payload in (dict(id='gba/other', folder=str(self.roms)), dict(id='gba/tiny', folder='relative')):
                with self.assertRaises(ValueError):
                    self.store.get(payload)
        with self.assertRaises(ValueError):
            homebrew_store.fetch('https://example.org/game.gba')


class PublishedFeedTest(unittest.TestCase):
    def test_every_entry_is_pinned_hashed_and_licensed(self):
        published = json.loads((ROOT / 'assets/homebrew/feed.json').read_text())
        self.assertEqual(published['schema'], 1)
        ids = [e['id'] for e in published['entries']]
        self.assertEqual(len(ids), len(set(ids)))
        for item in published['entries']:
            with self.subTest(item['id']):
                self.assertIn(item['system'], homebrew_store.SYSTEMS)
                self.assertTrue(item['file']['url'].startswith(homebrew_store.TRUSTED))
                self.assertTrue(item['file']['name'].lower().endswith(homebrew_store.EXTENSIONS[item['system']]))
                self.assertGreater(item['file']['size'], 0)
                self.assertTrue('sha256' in item['file'] or 'git_sha1' in item['file'])
                self.assertTrue(item['license'])
                # database files at a fixed commit, never a moving branch
                if 'git_sha1' in item['file']: self.assertNotIn('/master/', item['file']['url'])


if __name__ == '__main__':
    unittest.main()
