#!/usr/bin/env python3
"""How many games in a real library have an age rating? Reads only (docs/PROFILES_PLAN.md, phase 0).

    tools/rating_audit.py --device root@rgds.local        what the handheld has saved in each game's info.ini
    tools/rating_audit.py --roms DIR [--roms gba=DIR]     what the catalogs hold for these files, with the plan's fixes

--device runs find and grep at the lowest priority over SSH and nothing else there. --roms reads the files' headers and
checksums on this computer and keeps the catalogs in memory.
"""
import argparse, collections, io, re, shlex, subprocess, sys, urllib.parse, urllib.request, zipfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from age_rating import minimum_age

GAMETDB = {'nds': 'https://www.gametdb.com/dstdb.zip', '3ds': 'https://www.gametdb.com/3dstdb.zip'}
ESRB = 'https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/esrb/'
# The plan's fix: the launcher asks for "Nintendo - Nintendo 64", where the list holds one game.
ESRB_NAMES = {'n64': 'Nintendo - Nintendo 64 (BigEndian)'}
USERDATA = '/data/rgds/userdata'
ROMS = '/data/roms'
FOLDERS = {'genesis': 'md'}
DS_SUFFIXES = ('.nds', '.dsi')  # runtime's SYSTEMS lists every other system


class Row:
    def __init__(self):
        self.games, self.empty_board, self.unrated = 0, 0, []
        self.ages = collections.Counter()

    def add(self, title, rating):
        self.games += 1
        age = minimum_age(rating)
        if age is not None: self.ages[age] += 1
        else:
            self.unrated.append(title)
            if rating: self.empty_board += 1


def report(rows, names):
    ages = sorted({age for row in rows.values() for age in row.ages})
    print('%-10s %6s %6s %8s  %s' % ('system', 'games', 'rated', 'unrated', '  '.join('%3d+' % a for a in ages)))
    total = Row()
    for system, row in sorted(rows.items()):
        rated = sum(row.ages.values())
        print(
            '%-10s %6d %6d %8d  %s' %
            (system, row.games, rated, len(row.unrated), '  '.join('%4d' % row.ages[a] for a in ages))
        )
        total.games += row.games
        total.ages.update(row.ages)
        total.unrated += row.unrated
        total.empty_board += row.empty_board
    rated = sum(total.ages.values())
    print(
        '%-10s %6d %6d %8d  %s' %
        ('all', total.games, rated, len(total.unrated), '  '.join('%4d' % total.ages[a] for a in ages))
    )
    print(
        '\n%d%% rated. %d of the unrated name a board with no value.' %
        (100 * rated / max(1, total.games), total.empty_board)
    )
    if names:
        for system, row in sorted(rows.items()):
            for title in sorted(row.unrated):
                print('unrated  %-8s %s' % (system, title))


def is_game(path, system):
    from play_options import SYSTEMS
    suffixes = DS_SUFFIXES if system == 'nds' else SYSTEMS.get(system)
    return suffixes is not None and path.suffix.lower() in (*suffixes, '.zip')


def device_lines(host, command):
    script = 'nice -n 19 sh -c ' + shlex.quote(command)
    result = subprocess.run(
        ['ssh', '-o', 'ConnectTimeout=5', '-o', 'BatchMode=yes', host, script],
        capture_output=True,
        text=True,
        timeout=120
    )
    if result.returncode > 1: raise SystemExit('ssh: ' + result.stderr.strip())
    return result.stdout.splitlines()


def audit_device(host):
    from library import game_key
    ratings, fetched = {}, set()
    for line in device_lines(
        host, 'find %s/library -maxdepth 2 -name info.ini -exec grep -H "^rating=" {} +' % shlex.quote(USERDATA)
    ):
        path, _, value = line.partition(':rating=')
        ratings[Path(path).parent.name] = value
    for line in device_lines(
        host, 'find %s/library -maxdepth 2 -name info-status.ini -exec grep -H "^info=ok" {} +' % shlex.quote(USERDATA)
    ):
        fetched.add(Path(line.partition(':')[0]).parent.name)
    rows = collections.defaultdict(Row)
    never = collections.Counter()
    for line in device_lines(host, 'find %s -mindepth 2 -type f ! -name "._*"' % shlex.quote(ROMS)):
        path = Path(line)
        folder = path.relative_to(ROMS).parts[0]
        system = FOLDERS.get(folder.lower(), folder.lower())
        if system == 'scummvm' or not is_game(path, system): continue
        key = game_key(path)
        rows[system].add(path.stem, ratings.get(key, ''))
        if key not in fetched: never[system] += 1
    for line in device_lines(host, 'find %s/scummvm -mindepth 1 -maxdepth 1 -type d ! -name ".*"' % shlex.quote(ROMS)):
        rows['scummvm'].add(Path(line).name, '')
    for line in device_lines(host, 'find /data/portmaster/ports -maxdepth 1 -name "*.sh"'):
        rows['ports'].add(Path(line).stem, '')
    return rows, never


def fetch(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'rgds-rating-audit'})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def gametdb(system):
    """{game code: its rating}, and {first three letters: the strictest rating of any region}."""
    archive = zipfile.ZipFile(io.BytesIO(fetch(GAMETDB[system])))
    own, strictest = {}, {}
    with archive.open(next(n for n in archive.namelist() if n.lower().endswith('.xml'))) as stream:
        for _, element in ElementTree.iterparse(stream, events=('end',)):
            if element.tag != 'game': continue
            code = (element.findtext('id') or '').strip().upper()
            found = element.find('rating')
            rating = (found.get('type', '') + ' ' + found.get('value', '')).strip() if found is not None else ''
            own[code] = rating
            age = minimum_age(rating)
            if age is not None and age > (minimum_age(strictest.get(code[:3])) or 0): strictest[code[:3]] = rating
            element.clear()
    return own, strictest


def esrb_list(system):
    """{ROM CRC32: 'ESRB x'} from Libretro's list, and {title: the strictest rating of any dump of it}."""
    from public_artwork import CATALOGS, title_key
    name = ESRB_NAMES.get(system, CATALOGS.get(system))
    if not name: return {}, {}
    try:
        raw = fetch(ESRB + urllib.parse.quote(name) + '.dat').decode('utf-8', 'replace')
    except OSError:
        return {}, {}
    table, strictest = {}, {}
    for block in re.findall(r'^game \(\s*$(.*?)^\)', raw, re.M | re.S):
        crc = re.search(r'\bcrc ([0-9A-Fa-f]{8})\b', block)
        value = re.search(r'^\s*esrb_rating\s+"?([^"\n]*?)"?\s*$', block, re.M)
        title = re.search(r'^\s*(?:comment|name) "([^"\n]+)"', block, re.M)
        if not crc or not value: continue
        rating = 'ESRB ' + value[1].strip()
        table[crc[1].upper()] = rating
        age = minimum_age(rating)
        if title and age is not None and age > (minimum_age(strictest.get(title_key(title[1]))) or 0):
            strictest[title_key(title[1])] = rating
    return table, strictest


def audit_files(arguments):
    from game_info import rom_crc
    from play_options import system_of
    from public_artwork import rom_identity, title_key
    from scraper import NotFound
    catalogs, rows, borrowed = {}, collections.defaultdict(Row), collections.Counter()
    for argument in arguments:
        forced, _, folder = argument.rpartition('=')
        for path in sorted(Path(folder).expanduser().rglob('*')):
            if not path.is_file() or path.name.startswith('.'): continue
            system = forced or system_of(path)
            if not is_game(path, system): continue
            if system not in catalogs: catalogs[system] = gametdb(system) if system in GAMETDB else esrb_list(system)
            rating = ''
            try:
                if system in GAMETDB:
                    code = rom_identity(path)[2]
                    own, strictest = catalogs[system]
                    rating = own.get(code, '')
                    if minimum_age(rating) is None and code[:3] in strictest:
                        rating = strictest[code[:3]]
                        borrowed[system] += 1
                elif catalogs[system][0]:
                    own, strictest = catalogs[system]
                    rating = own.get(rom_crc(path), '')
                    if minimum_age(rating) is None and title_key(path.stem) in strictest:
                        rating = strictest[title_key(path.stem)]
                        borrowed[system] += 1
            except (NotFound, OSError, ValueError, zipfile.BadZipFile) as error:
                print('could not read %s: %s' % (path.name, error), file=sys.stderr)
            rows[system].add(path.stem, rating)
    return rows, borrowed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', help='what a handheld has saved, for example root@rgds.local')
    parser.add_argument('--roms', action='append', help='a folder of games, or system=folder')
    parser.add_argument('--names', action='store_true', help='also list the unrated games')
    args = parser.parse_args()
    if bool(args.device) == bool(args.roms): parser.error('give --device or --roms')
    if args.device:
        rows, never = audit_device(args.device)
        report(rows, args.names)
        print('Information never fetched: ' + (', '.join('%s %d' % item for item in sorted(never.items())) or 'none'))
    else:
        rows, borrowed = audit_files(args.roms)
        report(rows, args.names)
        print(
            'Borrowed from another release: ' +
            (', '.join('%s %d' % item for item in sorted(borrowed.items())) or 'none')
        )


if __name__ == '__main__':
    main()
