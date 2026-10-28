"""Homebrew shelves: free DS, GBA and NES homebrew from our own curated feed (assets/homebrew/feed.json, built by
tools/build_homebrew_feed.py), played by DSCore and RG DS Play like any other game.

The feed is read from where we publish it (FEED_URL, the repository's copy) and kept in cache/homebrew; with no
network, the cached copy, then the one inside this release, still lists the shelf. Every download is checked against
the size and hash the feed records (a database file's git blob SHA-1 or a release file's SHA-256) before it is put
in the system's folder; its cover goes to library/<game key>/box.bmp and its author and license to info.ini.

Status fields (device/status.ini): hb_system, hb_count, hb_<n>_{id,title,developer,license,size,installed,cover,
about}; while a download runs hb_get (its id), hb_received and hb_size. Actions: homebrew-browse {system, folder},
homebrew-get {id, folder}."""
import hashlib
import json
from pathlib import Path
import re
import time
import urllib.request

from fields import read_fields, write_fields
from library import game_key

FEED_URL = 'https://raw.githubusercontent.com/kleinrein/rgds-core/main/assets/homebrew/feed.json'
AGENT = 'RG-DS-Core homebrew shelves (+https://github.com/kleinrein/rgds-core)'
SYSTEMS = ('nds', 'gba', 'nes')
EXTENSIONS = {'nds': '.nds', 'gba': '.gba', 'nes': '.nes'}
FRESH = 6 * 3600  # a cached feed younger than this is used without asking the network again
TRUSTED = ('https://raw.githubusercontent.com/', 'https://github.com/')


def fetch(url, limit=64 << 20, timeout=20, progress=None):
    if not url.startswith(TRUSTED): raise ValueError('That download is not from a source the feed allows.')
    request = urllib.request.Request(url, headers={'User-Agent': AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        chunks, got = [], 0
        while True:
            chunk = response.read(64 << 10)
            if not chunk: break
            got += len(chunk)
            if got > limit: raise ValueError('That download is larger than the feed says.')
            chunks.append(chunk)
            if progress: progress(got)
    return b''.join(chunks)


def file_name(title, suffix):
    """A game's file name from its title: no path separators or characters exFAT refuses."""
    clean = re.sub(r'[\\/:*?"<>|\x00-\x1f]+', ' ', title).strip(' .')
    return (re.sub(r'\s+', ' ', clean)[:80] or 'game') + suffix


def matches(entry, body):
    record = entry['file']
    if len(body) != int(record['size']): return False
    if 'sha256' in record: return hashlib.sha256(body).hexdigest() == record['sha256']
    if 'git_sha1' in record:
        return hashlib.sha1(b'blob %d\0' % len(body) + body).hexdigest() == record['git_sha1']
    return False


def valid(feed):
    return isinstance(feed, dict) and feed.get('schema') == 1 and isinstance(feed.get('entries'), list)


class HomebrewStore:
    def __init__(self, services):
        self.services = services
        self.preview = services.preview
        self.cache = Path(services.data) / 'cache/homebrew'
        self.bundled = Path(services.app) / 'assets/homebrew/feed.json'

    # -- the feed --
    def feed(self):
        cached = self.cache / 'feed.json'
        try:
            local = json.loads(cached.read_text())
            fresh = time.time() - cached.stat().st_mtime < FRESH
        except (OSError, ValueError):
            local, fresh = None, False
        if valid(local) and (fresh or self.preview): return local
        if not self.preview:
            try:
                online = json.loads(fetch(FEED_URL, limit=4 << 20))
                if valid(online):
                    self.cache.mkdir(parents=True, exist_ok=True)
                    pending = cached.with_suffix('.part')
                    pending.write_text(json.dumps(online))
                    pending.replace(cached)
                    return online
            except (OSError, ValueError):
                pass  # offline, or not published yet: the cached or bundled copy
        if valid(local): return local
        try:
            bundled = json.loads(self.bundled.read_text())
        except (OSError, ValueError):
            bundled = None
        if valid(bundled): return bundled
        raise ValueError('The homebrew list is not available. Check Wi-Fi and try again.')

    def shelf(self, system):
        return [e for e in self.feed()['entries'] if e.get('system') == system]

    def target(self, entry, folder):
        return Path(folder) / file_name(entry['title'], EXTENSIONS[entry['system']])

    def cover(self, entry):
        """The entry's cover as a BMP in the cache (the launcher draws BMPs), fetched once."""
        if not entry.get('cover'): return ''
        path = self.cache / 'covers' / (re.sub(r'[^A-Za-z0-9_-]+', '_', entry['id']) + '.bmp')
        if path.is_file(): return str(path)
        if self.preview: return ''
        try:
            from scraper import image_to_bmp
            path.parent.mkdir(parents=True, exist_ok=True)
            image_to_bmp(fetch(entry['cover'], limit=4 << 20), path)
            return str(path)
        except Exception as error:  # a shelf without a picture still works
            print(f'Homebrew cover not saved for {entry["id"]}: {error}', flush=True)
            return ''

    # -- actions --
    def browse(self, payload):
        system = payload.get('system') if isinstance(payload, dict) else None
        if system not in SYSTEMS: raise ValueError('Choose a system with homebrew.')
        folder = Path(payload.get('folder') or '.')
        # the previous shelf's rows go at once: the launcher shows only rows tagged with the shelf it asked for
        fields = self.services.fields
        for key in [k for k in fields if k.startswith('hb_')]:
            fields.pop(key)
        self.services.publish(state='busy', message='', hb_system=system, hb_count=0)
        entries = self.shelf(system)
        for index, entry in enumerate(entries):
            fields.update(
                {
                    f'hb_{index}_id': entry['id'],
                    f'hb_{index}_title': entry['title'],
                    f'hb_{index}_developer': entry.get('developer', ''),
                    f'hb_{index}_license': entry.get('license', ''),
                    f'hb_{index}_size': str(entry['file']['size']),
                    f'hb_{index}_about': ' '.join(entry.get('description', '').split()),
                    f'hb_{index}_installed': str(int(self.target(entry, folder).is_file())),
                }
            )
        # the list first, the pictures as they arrive
        self.services.publish(state='ready', hb_system=system, hb_count=len(entries))
        for index, entry in enumerate(entries):
            path = self.cover(entry)
            if path:
                fields[f'hb_{index}_cover'] = path
                self.services.publish()

    def get(self, payload):
        if not isinstance(payload, dict): raise ValueError('Choose a game from the shelf.')
        wanted, folder = str(payload.get('id', '')), Path(payload.get('folder') or '')
        if not folder.is_absolute(): raise ValueError('The games folder for this system is missing.')
        entry = next((e for e in self.feed()['entries'] if e.get('id') == wanted), None)
        if entry is None or entry.get('system') not in SYSTEMS: raise ValueError('Choose a game from the shelf.')
        title, size = entry['title'], int(entry['file']['size'])
        self.services.publish(state='busy', message='', hb_get=wanted, hb_received=0, hb_size=size)
        last = [0.0]

        def progress(got):
            if time.monotonic() - last[0] >= .2:
                last[0] = time.monotonic()
                self.services.publish(hb_received=got)

        if self.preview:
            body = b'preview'
        else:
            try:
                body = fetch(entry['file']['url'], limit=size, timeout=30, progress=progress)
            except (OSError, ValueError) as error:
                raise ValueError(f'{title} did not download. Check Wi-Fi and try again.') from error
            if not matches(entry, body): raise ValueError(f'{title} did not arrive intact. Try again.')
        self.services.publish(hb_received=size)
        target = self.target(entry, folder)
        folder.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name + '.part')
        pending.write_bytes(body)
        pending.replace(target)
        self.artwork(target, entry)
        for key, value in list(self.services.fields.items()):
            if key.startswith('hb_') and key.endswith('_id') and value == wanted:
                self.services.fields[key[:-3] + '_installed'] = '1'
        self.services.fields.pop('hb_get', None)
        self.services.publish(state='ready', message=f'{title} is in your library.')

    def artwork(self, rom, entry):
        item = Path(self.services.data) / 'library' / game_key(rom)
        item.mkdir(parents=True, exist_ok=True)
        info = read_fields(item / 'info.ini')
        info.update(
            title=entry['title'],
            developer=entry.get('developer', ''),
            provider='Homebrew shelves',
            license=entry.get('license', ''),
            source_url=entry.get('page', '')
        )
        cover = self.cover(entry)
        if cover:
            (item / 'box.bmp').write_bytes(Path(cover).read_bytes())
        write_fields(item / 'info.ini', info)
