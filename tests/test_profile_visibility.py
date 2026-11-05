"""What a profile sees, through the real launcher: every list comes from the same filter, and a hidden game stays hidden."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import read_fields
from library import game_key

BINARY = ROOT / 'build/host/rgds-launcher'
LIBRARY, CONFIRM, CHOOSE, GAMES, RULES, EDIT = 0, 18, 51, 52, 53, 50
TO_PROFILES = ['Down'] * 10 + ['a']
RATED = {'Young': 'PEGI 3', 'Teen': 'ESRB T', 'Grown': 'PEGI 18', 'Board': 'PEGI'}
UNRATED = ('Board', 'Plain')
ALL = ('Young', 'Teen', 'Grown', 'Board', 'Plain')


class Base(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / 'userdata'
        self.data.mkdir()
        (self.data / 'setup.ini').write_text('done=1\n')
        (self.data / 'ds.ini').write_text('nickname=Andreas\n')
        self.nds = self.root / 'roms/nds'
        self.nds.mkdir(parents=True)
        self.keys = {}
        for name in ALL:
            rom = self.nds / (name + '.nds')
            rom.write_bytes(bytes(12) + b'ABCD' + bytes(0x200))
            self.keys[name] = game_key(rom.resolve())
            if name in RATED:
                folder = self.data / 'library' / self.keys[name]
                folder.mkdir(parents=True)
                (folder / 'info.ini').write_text('rating=' + RATED[name] + '\n')

    def add_system(self, system, *files):
        folder = self.root / 'roms' / system
        folder.mkdir(parents=True, exist_ok=True)
        for name in files:
            (folder / name).write_bytes(b'game')
        return folder

    def kid(self, name='p1', age=7, unrated='hide', restricted=1, active=True, extra=''):
        folder = self.data / 'profiles' / name
        folder.mkdir(parents=True)
        (folder / 'profile.ini'
         ).write_text(f'schema=1\nname=Ola\npicture=2\nrestricted={restricted}\nage={age}\nunrated={unrated}\n{extra}')
        (self.data / 'profiles/index.ini').write_text('schema=1\nowner_name=Andreas\nask_at_boot=0\n')
        (self.data / 'profiles/active').write_text((name if active else 'owner') + '\n')
        return folder

    def run_ui(self, events=(), flags=('--library',), seconds=3, gap=110, systems=()):
        script = self.root / 'input.txt'
        script.write_text(''.join(f'{300 + i * gap} {e if " " in e else "key " + e}\n' for i, e in enumerate(events)))
        report = self.root / 'report.json'
        command = [
            str(BINARY), '--preview', '--data',
            str(self.data), '--rom-dir',
            str(self.nds), '--input-script',
            str(script), '--cache-report',
            str(report), '--seconds',
            str(seconds), *flags
        ]
        for system in systems:
            command += ['--system-dir', system + '=' + str(self.root / 'roms' / system)]
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(report.read_text())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class RuleTests(Base):
    def seen(self, **values):
        folder = self.kid(**values)
        report = self.run_ui()
        shutil.rmtree(self.data / 'profiles')
        return report['all_games'], report['shown_games'], report['catalog_games'], folder

    def test_nobody_filtered_sees_everything_as_before(self):
        report = self.run_ui()
        self.assertEqual((report['all_games'], report['catalog_games'], report['shown_games']), (5, 5, 5))
        self.assertFalse((self.data / 'visibility.ini').exists())

    def test_the_age_decides_for_rated_games_and_unrated_stay_hidden(self):
        self.assertEqual(self.seen(age=3)[:3], (1, 1, 5))  # Young
        self.assertEqual(self.seen(age=7)[:3], (1, 1, 5))
        self.assertEqual(self.seen(age=13)[:3], (2, 2, 5))  # and Teen
        self.assertEqual(self.seen(age=12)[:3], (1, 1, 5))  # T is 13
        self.assertEqual(self.seen(age=16)[:3], (2, 2, 5))
        self.assertEqual(self.seen(age=18)[:3], (3, 3, 5))  # and Grown

    def test_unrated_can_be_shown_and_a_board_with_no_value_counts_as_unrated(self):
        self.assertEqual(self.seen(age=7, unrated='show')[:2], (3, 3))  # Young, Board, Plain

    def test_no_limit_shows_everything_and_only_picked_shows_nothing(self):
        self.assertEqual(self.seen(age=-1)[:2], (5, 5))
        self.assertEqual(self.seen(age=-2, unrated='show')[:2], (0, 0))

    def test_a_choice_by_hand_beats_the_age_both_ways(self):
        folder = self.kid(age=7)
        (folder / 'visibility.ini').write_text(
            'schema=1\n' + self.keys['Grown'] + '=show\n' + self.keys['Plain'] + '=show\n' + self.keys['Young'] +
            '=hide\n'
        )
        self.assertEqual(self.run_ui()['all_games'], 2)  # Grown and Plain; Young gone

    def test_only_picked_shows_what_was_picked(self):
        folder = self.kid(age=-2)
        (folder / 'visibility.ini').write_text(self.keys['Teen'] + '=show\n')
        self.assertEqual(self.run_ui()['all_games'], 1)

    def test_the_owner_can_hide_from_themself_with_no_profiles(self):
        (self.data / 'visibility.ini').write_text(self.keys['Plain'] + '=hide\n')
        report = self.run_ui()
        self.assertEqual((report['all_games'], report['shown_games'], report['catalog_games']), (4, 4, 5))

    def test_one_profiles_choices_are_not_anothers(self):
        folder = self.kid(age=-1, restricted=0, active=False)
        (folder / 'visibility.ini').write_text(self.keys['Plain'] + '=hide\n')
        self.assertEqual(self.run_ui()['all_games'], 5)
        (self.data / 'profiles/active').write_text('p1\n')
        self.assertEqual(self.run_ui()['all_games'], 4)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class RouteTests(Base):
    def test_search_finds_nothing_hidden(self):
        self.kid(age=7)
        report = self.run_ui(['/', 'text Grown', 'Return'], seconds=4)
        self.assertEqual(report['shown_games'], 0)
        shutil.rmtree(self.data / 'profiles')
        self.assertEqual(self.run_ui(['/', 'text Grown', 'Return'], seconds=4)['shown_games'], 1)

    def test_favourites_and_recents_keep_only_what_may_be_seen(self):
        folder = self.kid(age=7)
        for name in ALL:
            game = folder / 'library' / self.keys[name]
            game.mkdir(parents=True)
            (game / 'game.ini').write_text('schema=1\nfavourite=1\n')
            (game / 'stats.ini').write_text('seconds=60\nsessions=1\nlast_played=2026-11-05T08:00:00+00:00\n')
        (folder / 'ui.ini').write_text('view=1\n')
        self.assertEqual(self.run_ui()['shown_games'], 1)
        (folder / 'ui.ini').write_text('view=2\n')
        self.assertEqual(self.run_ui()['shown_games'], 1)

    def test_the_home_counts_and_the_game_switcher_list_only_what_may_be_seen(self):
        folder = self.kid(age=7)
        for name in ALL:
            game = folder / 'library' / self.keys[name]
            game.mkdir(parents=True)
            (game / 'stats.ini').write_text('seconds=60\nsessions=1\nlast_played=2026-11-05T08:00:00+00:00\n')
        report = self.run_ui(flags=('--home',))
        self.assertEqual(report['all_games'], 1)
        self.assertEqual(report['recent_games'], 1)  # what Last played and the switcher draw from
        shutil.rmtree(self.data / 'profiles')
        for name in ALL:
            game = self.data / 'library' / self.keys[name]
            game.mkdir(parents=True, exist_ok=True)
            (game / 'stats.ini').write_text('seconds=60\nsessions=1\nlast_played=2026-11-05T08:00:00+00:00\n')
        self.assertEqual(self.run_ui(flags=('--home',))['recent_games'], 5)

    def test_the_hidden_games_chip_is_not_offered_on_a_kid_profile(self):
        self.kid(age=7)
        # Where the chip would be: the same keys as the owner's test below, one chip further than there are.
        report = self.run_ui(['y', 'Down', 'Down', 'Down', 'Right', 'Right', 'Right', 'a'], seconds=3, gap=90)
        self.assertNotEqual(report['discovery_filter'], 4)
        self.assertLessEqual(report['shown_games'], 1)

    def test_systems_with_nothing_visible_leave_the_list(self):
        self.add_system('gba', 'Rated.gba', 'Plain.gba')
        self.add_system('snes', 'Plain.sfc')
        (self.data / 'ui.ini').write_text('show_systems=1\n')
        self.assertEqual(self.run_ui(systems=('gba', 'snes'))['listed_systems'], 4)  # DS, GBA, SNES and Homebrew
        folder = self.kid(age=7)
        (folder / 'ui.ini').write_text('show_systems=1\n')
        self.assertEqual(self.run_ui(systems=('gba', 'snes'))['listed_systems'], 1)  # DS stays, the way home
        key = game_key((self.root / 'roms/gba/Plain.gba').resolve())
        (folder / 'visibility.ini').write_text(key + '=show\n')
        report = self.run_ui(systems=('gba', 'snes'))
        self.assertEqual((report['listed_systems'], report['all_games']), (2, 2))

    def test_ports_need_a_permission_on_a_kid_profile_and_no_rating_lets_them_in(self):
        self.add_system('ports', 'Game.sh')
        (self.data / 'ui.ini').write_text('show_systems=1\n')
        self.assertEqual(self.run_ui(systems=('ports',))['all_games'], 6)
        folder = self.kid(age=-1)
        (folder / 'ui.ini').write_text('show_systems=1\n')
        report = self.run_ui(systems=('ports',))
        self.assertEqual((report['all_games'], report['listed_systems']), (5, 1))
        key = game_key((self.root / 'roms/ports/Game.sh').resolve())
        (folder / 'visibility.ini').write_text(key + '=show\n')
        self.assertEqual(self.run_ui(systems=('ports',))['all_games'], 5)
        (folder / 'profile.ini').write_text('schema=1\nname=Ola\nrestricted=1\nage=-1\nports=1\n')
        self.assertEqual(self.run_ui(systems=('ports',))['all_games'], 6)
        # A grown-up's profile needs no permission.
        (folder / 'profile.ini').write_text('schema=1\nname=Sara\nrestricted=0\nage=-1\n')
        self.assertEqual(self.run_ui(systems=('ports',))['all_games'], 6)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class PageTests(Base):
    # Change who's playing, Andreas, Ola
    def to_kid(self):
        return TO_PROFILES + ['Down', 'Down', 'a']

    def to_games(self):
        # Who it's for, Name, DS name, Picture, Colour, Age limit, Games with no rating, Games
        return self.to_kid() + ['Down'] * 7 + ['a']

    def visibility(self, folder):
        return read_fields(folder / 'visibility.ini')

    def test_a_new_kid_ends_on_the_games_waiting_for_a_decision(self):
        report = self.run_ui(
            TO_PROFILES + ['Down', 'a', 'Right', 'a', 'text Ola', 'Return', 'a', 'Escape'], flags=('--settings',), seconds=4
        )
        self.assertEqual((report['page'], report['games_tab']), (GAMES, 3))
        self.assertEqual(report['games_rows'], 3)  # a shelf, Board and Plain
        info = read_fields(self.data / 'profiles/p1/profile.ini')
        self.assertEqual((info['restricted'], info['age'], info['unrated']), ('1', '7', 'hide'))

    def test_the_tabs_sort_the_games(self):
        self.kid(age=7, active=False)
        counts = {}
        for tab, keys in enumerate(([], [']'], [']', ']'], [']', ']', ']'])):
            report = self.run_ui(self.to_games() + keys, flags=('--settings',), seconds=4, gap=90)
            self.assertEqual(report['page'], GAMES)
            self.assertEqual(report['games_tab'], tab)
            counts[tab] = report['games_rows']
        self.assertEqual(counts, {0: 6, 1: 2, 2: 5, 3: 3})  # with one shelf row each

    def test_a_cycles_automatic_show_hide(self):
        folder = self.kid(age=7, active=False)
        key = self.keys['Board']  # All: the shelf, then Board, Grown, Plain, Teen, Young
        self.run_ui(self.to_games() + ['Down', 'a'], flags=('--settings',), seconds=4, gap=90)
        self.assertEqual(self.visibility(folder)[key], 'show')
        self.run_ui(self.to_games() + ['Down', 'a'], flags=('--settings',), seconds=4, gap=90)
        self.assertEqual(self.visibility(folder)[key], 'hide')
        self.run_ui(self.to_games() + ['Down', 'a'], flags=('--settings',), seconds=4, gap=90)
        self.assertNotIn(key, self.visibility(folder))

    def test_a_shelf_is_approved_at_once_for_what_has_no_rating_only(self):
        folder = self.kid(age=7, active=False)
        (folder / 'visibility.ini').write_text(self.keys['Plain'] + '=hide\n')
        report = self.run_ui(self.to_games() + ['a', 'Right', 'a'], flags=('--settings',), seconds=4, gap=90)
        self.assertEqual(report['page'], GAMES)
        saved = self.visibility(folder)
        self.assertEqual(saved[self.keys['Board']], 'show')
        self.assertEqual(saved[self.keys['Plain']], 'hide')  # a choice already made stands
        for name in ('Young', 'Teen', 'Grown'):
            self.assertNotIn(self.keys[name], saved)

    def test_a_shelf_is_not_approved_without_saying_yes(self):
        folder = self.kid(age=7, active=False)
        report = self.run_ui(self.to_games() + ['a', 'a'], flags=('--settings',), seconds=4, gap=90)
        self.assertEqual(report['page'], GAMES)
        self.assertFalse((folder / 'visibility.ini').exists())

    def test_the_page_lists_other_systems_even_with_them_switched_off_here(self):
        self.kid(age=7, active=False)
        self.add_system('gba', 'Other.gba')
        report = self.run_ui(self.to_games(), flags=('--settings',), seconds=4, gap=90, systems=('gba',))
        self.assertEqual(report['games_rows'], 8)  # two shelves, six games

    def test_the_age_limit_and_the_switches_are_saved(self):
        folder = self.kid(age=7, active=False)
        report = self.run_ui(self.to_kid() + ['Down'] * 5 + ['a', 'Right', 'a'], flags=('--settings',))
        self.assertEqual(report['page'], EDIT)
        self.assertEqual(read_fields(folder / 'profile.ini')['age'], '8')
        self.run_ui(self.to_kid() + ['Down'] * 6 + ['a'], flags=('--settings',))
        self.assertEqual(read_fields(folder / 'profile.ini')['unrated'], 'show')
        self.run_ui(self.to_kid() + ['Down'] * 8 + ['a'], flags=('--settings',), gap=90)
        self.assertEqual(read_fields(folder / 'profile.ini')['ports'], '1')
        self.run_ui(self.to_kid() + ['Down'] * 9 + ['a'], flags=('--settings',), gap=90)
        self.assertEqual(read_fields(folder / 'profile.ini')['homebrew'], '1')

    def test_only_picked_and_no_limit_are_the_last_two_ages(self):
        folder = self.kid(age=16, active=False)  # the grid's last age, row 3 column 2
        self.run_ui(self.to_kid() + ['Down'] * 5 + ['a', 'Right', 'a'], flags=('--settings',))
        self.assertEqual(read_fields(folder / 'profile.ini')['age'], '-2')
        self.run_ui(self.to_kid() + ['Down'] * 5 + ['a', 'Right', 'a'], flags=('--settings',))
        self.assertEqual(read_fields(folder / 'profile.ini')['age'], '-1')

    def test_turning_a_kid_into_an_adult_and_back_starts_safe(self):
        folder = self.kid(age=12, active=False)
        self.run_ui(self.to_kid() + ['a'], flags=('--settings',))
        info = read_fields(folder / 'profile.ini')
        self.assertEqual((info['restricted'], info['age'], info['unrated']), ('0', '-1', 'show'))
        self.run_ui(self.to_kid() + ['a'], flags=('--settings',))
        info = read_fields(folder / 'profile.ini')
        self.assertEqual((info['restricted'], info['age'], info['unrated']), ('1', '7', 'hide'))

    def test_a_change_to_who_is_playing_shows_when_back_at_the_list(self):
        folder = self.kid(age=7)
        # Settings > Profiles > Ola's page: to 13, and back out to the games.
        report = self.run_ui(
            self.to_kid()[:-2] + ['Down', 'a'] + ['Down'] * 5 +
            ['a', 'Right', 'Right', 'Right', 'Right', 'Right', 'Right', 'a'] + ['Escape', 'Escape', 'Escape'],
            flags=('--settings',),
            seconds=5,
            gap=90
        )
        self.assertEqual(read_fields(folder / 'profile.ini')['age'], '13')
        self.assertEqual(report['page'], LIBRARY)
        self.assertEqual(report['all_games'], 2)


class SourceTests(unittest.TestCase):
    def test_every_list_page_names_its_own_title(self):
        # The header's list ends in a default; a page that is not named there shows another page's title.
        source = (ROOT / 'src/launcher/settings_screen.hpp').read_text()
        for page in ('ProfileGames', 'ProfileGameRules'):
            self.assertIn('settings.page == Page::' + page + ' ', source)
        self.assertIn('settings.page == Page::ProfileEdit && edited_profile()) title =', source)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class GameOptionsTests(Base):
    def test_one_person_can_hide_a_game_and_bring_it_back(self):
        # Options on the first game (Board), to the last row: other rows differ with the system, the end does not.
        report = self.run_ui(['x'] + ['Down'] * 9 + ['a', 'Escape'], seconds=4, gap=90)
        saved = read_fields(self.data / 'visibility.ini')
        self.assertEqual(saved[self.keys['Board']], 'hide')
        self.assertEqual(report['all_games'], 4)
        self.assertEqual(report['shown_games'], 4)

    def test_with_profiles_the_row_says_who_sees_it(self):
        folder = self.kid(age=7, active=False)
        report = self.run_ui(['x'] + ['Down'] * 9 + ['a'], seconds=4, gap=90)
        self.assertEqual(report['page'], RULES)
        report = self.run_ui(['x'] + ['Down'] * 9 + ['a', 'Down', 'a'], seconds=4, gap=90)
        self.assertEqual(report['page'], RULES)
        self.assertEqual(read_fields(folder / 'visibility.ini')[self.keys['Board']], 'show')
        self.assertFalse((self.data / 'visibility.ini').exists())
        self.assertEqual(report['all_games'], 5)  # the owner's own list is not changed by that

    def test_a_kid_has_no_such_row(self):
        self.kid(age=-1)
        report = self.run_ui(['x'] + ['Down'] * 9 + ['a'], seconds=4, gap=90)
        self.assertNotEqual(report['page'], RULES)
        self.assertFalse((self.data / 'profiles/p1/visibility.ini').exists())

    def test_the_hidden_games_filter_brings_a_game_back_and_it_does_not_start_from_there(self):
        (self.data / 'visibility.ini').write_text(self.keys['Plain'] + '=hide\n')
        to_filter = ['y', 'Down', 'Down', 'Down', 'Right', 'Right', 'Right', 'a', 'Escape']
        report = self.run_ui(to_filter, seconds=4, gap=120)
        self.assertEqual((report['discovery_filter'], report['shown_games']), (4, 1))
        # The last check: a game a list holds but the rules refuse is not started.
        report = self.run_ui(to_filter + ['a'], seconds=5, gap=250)  # the list has to be there first
        self.assertEqual(report['launching'], 0)
        self.assertEqual(report['status'], 'This game is not available on this profile')
        # From the same list, its options bring it back.
        report = self.run_ui(to_filter + ['x'] + ['Down'] * 9 + ['a', 'Escape'], seconds=5, gap=110)
        self.assertNotIn(self.keys['Plain'], read_fields(self.data / 'visibility.ini'))
        self.assertEqual(report['all_games'], 5)


if __name__ == '__main__':
    unittest.main()
