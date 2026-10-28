"""Build the Homebrew shelves feed (assets/homebrew/feed.json): free DS, GBA and NES homebrew that RG DS Core plays.

The feed is ours: this script picks the entries, pins every download to an immutable URL and records its size and
hash, and the handheld (runtime/homebrew_store.py) only offers what is in it. Sources and what is taken from them:

  GBA  Homebrew Hub's database (github.com/gbadev-org/games), files served from the repository at a fixed commit
  NES  Homebrew Hub's database (github.com/nesdev-org/homebrew-db), the same way
  DS   original open-source games that publish .nds files on GitHub releases (the DS list below, chosen by hand)

An entry from a database is taken when its rights are clear: an explicit license in the entry, an open license on
the GitHub repository the entry names, or a GBA Jam 2026 entry (its rules have every entrant agree that the jam
binary stays available on Homebrew Hub). Games built on other companies' characters or brands are left out.
Database files are checked on the handheld against their git blob hash, release files against GitHub's SHA-256.

  python3 tools/build_homebrew_feed.py           write assets/homebrew/feed.json
  python3 tools/build_homebrew_feed.py --check   say what would change, write nothing

GitHub API calls use `gh auth token` (or GITHUB_TOKEN) when there is one; without, the 60-an-hour limit is close."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
FEED = ROOT / 'assets/homebrew/feed.json'
DATABASES = (('gba', 'gbadev-org/games', '.gba'), ('nes', 'nesdev-org/homebrew-db', '.nes'))
OPEN = {
    'MIT', 'Apache-2.0', 'BSD-2-Clause', 'BSD-3-Clause', 'ISC', 'Zlib', 'Unlicense', 'CC0-1.0', 'MPL-2.0', 'GPL-2.0',
    'GPL-2.0-only', 'GPL-2.0-or-later', 'GPL-3.0', 'GPL-3.0-only', 'GPL-3.0-or-later', 'LGPL-2.1', 'LGPL-3.0',
    'AGPL-3.0', 'CC-BY-4.0', 'CC-BY-SA-4.0', 'CC-BY-NC-4.0', 'CC-BY-NC-SA-4.0', '0BSD'
}
CONSENT_TAG, CONSENT = 'event:gbajam26', 'GBA Jam 2026 (archived on Homebrew Hub with the entrant\'s consent)'
MAX_SIZE = 32 << 20
# Original DS games only: (repository, release tag, file, title, developer, one line about it)
DS = (
    (
        'pandymic/delve-nds', 'v2.5.1', 'delve.nds', 'Delve', 'pandymic',
        'An endless room-to-room action roguelike in isometric 3D.'
    ),
    (
        'Warioware64/Traffic-Escape-DS', 'v1.2', 'Traffic_Escape_DS.nds', 'Traffic Escape DS', 'Warioware64',
        'Slide the cars to clear the exit: a sliding-block traffic puzzle.'
    ),
    (
        'Jonatan6/Pistol-Pong-DS', 'v1.0', 'Pistol-Pong-DS.nds', 'Pistol Pong DS', 'Jonatan6',
        'The classic paddle game, but with guns.'
    ),
    ('mdmrk/2048-nds', 'v1.0.2', '2048-nds.nds', '2048', 'mdmrk', 'Slide and merge the tiles to reach 2048.'),
    (
        'kkzero241/bunnygarden', '1.0', 'bunnygarden.nds', 'Bunny Garden', 'kkzero241',
        'Hop a bunny around floating 3D platforms.'
    ),
    (
        'Warioware64/Balloon-Pop-DS', '1.0.0', 'balloon-touch-ds.nds', 'Balloon Pop DS', 'Warioware64',
        'Pop the balloons with the stylus.'
    ),
)
# Database entries left out by hand (2026-10-28 review): open code but someone else's game, name or brand, or a tool
EXCLUDE = {
    'gbatris',
    'bugtris',
    'apotris',  # "-tris" block games: the Tetris Company enforces its name and look
    'minicraft-gba',  # a port of Mojang's Minicraft
    'thedarknessuser_spelunky-gba',  # Spelunky is Mossmouth's
    'dungeon-master',  # FTL's Dungeon Master
    'blackrookgames_super-granny-gba',  # Super Granny is a commercial series
    'hero-core-gba-port',  # a port of Daniel Remar's freeware game
    'piugba',  # a Pump It Up simulator
    'songoku2000_butano-sprite-tool',
    'codename-b_gba-gb-maps',
    'breadmakesyoufull_gbadev-containers',  # tools
}


def token():
    if os.environ.get('GITHUB_TOKEN'): return os.environ['GITHUB_TOKEN']
    try:
        return subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True, timeout=10).stdout.strip()
    except OSError:
        return ''


AUTH = token()


def get(url, api=False):
    headers = {'User-Agent': 'rgds-homebrew-feed'}
    if api and AUTH: headers['Authorization'] = 'Bearer ' + AUTH
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
                return json.load(response)
        except OSError:
            if attempt == 2: raise
            time.sleep(2)


def repository_license(url, cache={}):
    """The SPDX id of a GitHub repository's license, when the entry names one."""
    prefix = 'https://github.com/'
    if not url or not url.startswith(prefix): return None
    name = '/'.join(url[len(prefix):].strip('/').split('/')[:2])
    if name not in cache:
        try:
            cache[name] = ((get(f'https://api.github.com/repos/{name}', api=True).get('license') or {}).get('spdx_id'))
        except OSError:
            cache[name] = None
    return cache[name]


def database(system, repo, extension):
    tree = get(f'https://api.github.com/repos/{repo}/git/trees/master?recursive=1', api=True)
    commit, blobs = tree['sha'], {t['path']: t for t in tree['tree'] if t['type'] == 'blob'}
    raw = f'https://raw.githubusercontent.com/{repo}/{commit}/'
    paths = sorted(p for p in blobs if p.endswith('/game.json'))
    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        games = list(pool.map(lambda p: get(raw + p), paths))
    entries = []
    for path, game in zip(paths, games):
        folder = path.rsplit('/', 1)[0]
        if game.get('typetag') != 'game' or game.get('slug') in EXCLUDE: continue
        files = [f for f in game.get('files', []) if f.get('filename', '').lower().endswith(extension)]
        chosen = next((f for f in files if f.get('default')), files[0] if files else None)
        blob = blobs.get(f'{folder}/{chosen["filename"]}') if chosen else None
        if not blob or blob['size'] > MAX_SIZE: continue
        license = game.get('license') or repository_license(game.get('repository'))
        if license not in OPEN:
            if CONSENT_TAG not in game.get('tags', []): continue
            license = CONSENT
        shots = [s for s in game.get('screenshots', []) if f'{folder}/{s}' in blobs]
        entries.append(
            dict(
                id=f'{system}/{game["slug"]}',
                system=system,
                title=game.get('title') or game['slug'],
                developer=game.get('developer', ''),
                license=license,
                description=(game.get('description') or '').strip()[:280],
                page=game.get('website') or f'https://hh.gbdev.io/game/{game["slug"]}',
                source=f'Homebrew Hub ({repo})',
                file=dict(
                    url=raw + f'{folder}/{chosen["filename"]}',
                    name=chosen['filename'],
                    size=blob['size'],
                    git_sha1=blob['sha']
                ),
                cover=raw + f'{folder}/{shots[0]}' if shots else ''
            )
        )
    return entries


def ds():
    entries = []
    for repo, tag, name, title, developer, about in DS:
        release = get(f'https://api.github.com/repos/{repo}/releases/tags/{tag}', api=True)
        asset = next(a for a in release['assets'] if a['name'] == name)
        license = repository_license(f'https://github.com/{repo}')
        if license not in OPEN: raise SystemExit(f'{repo}: license {license} is not an open one')
        digest = asset.get('digest') or ''
        if not digest.startswith('sha256:'):
            # releases from before GitHub kept digests: hash the file once here
            with urllib.request.urlopen(asset['browser_download_url'], timeout=60) as response:
                digest = 'sha256:' + hashlib.sha256(response.read()).hexdigest()
        entries.append(
            dict(
                id='nds/' + repo.split('/')[1].lower(),
                system='nds',
                title=title,
                developer=developer,
                license=license,
                description=about,
                page=f'https://github.com/{repo}',
                source=f'GitHub release {repo} {tag}',
                file=dict(url=asset['browser_download_url'], name=name, size=asset['size'], sha256=digest[7:]),
                cover=''
            )
        )
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    entries = ds()
    for system, repo, extension in DATABASES:
        entries += database(system, repo, extension)
    entries.sort(key=lambda e: (e['system'], e['title'].lower()))
    feed = dict(
        schema=1,
        updated=time.strftime('%Y-%m-%d'),
        shelves=[
            dict(system='nds', title='Nintendo DS'),
            dict(system='gba', title='Game Boy Advance'),
            dict(system='nes', title='NES')
        ],
        entries=entries
    )
    counts = {s: sum(e['system'] == s for e in entries) for s in ('nds', 'gba', 'nes')}
    old = json.loads(FEED.read_text()) if FEED.is_file() else {'entries': []}
    added = sorted({e['id'] for e in entries} - {e['id'] for e in old['entries']})
    gone = sorted({e['id'] for e in old['entries']} - {e['id'] for e in entries})
    print(f'{len(entries)} entries {counts}; added {len(added)}, removed {len(gone)}')
    for item in added[:40]:
        print('  +', item)
    for item in gone:
        print('  -', item)
    if args.check: return 1 if (added or gone) else 0
    FEED.parent.mkdir(parents=True, exist_ok=True)
    FEED.write_text(json.dumps(feed, indent=1, ensure_ascii=False) + '\n')
    print('wrote', FEED.relative_to(ROOT))
    return 0


if __name__ == '__main__': sys.exit(main())
