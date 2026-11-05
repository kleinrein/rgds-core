"""The PIN and the lock: every way out of a Kid profile, through real SDL events in the host preview."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import read_fields
from library import game_key

BINARY = ROOT / 'build/host/rgds-launcher'
LIBRARY, SETTINGS, KEYBOARD, PICKER, PROFILES, EDIT, CHOOSE, GAMES, RULES, PIN = 0, 1, 8, 48, 49, 50, 51, 52, 53, 54
MAINTENANCE = 17
PIN_ENTER, PIN_UNLOCK, PIN_SET, PIN_CONFIRM = 0, 1, 2, 3


def stored_pin(pin, salt='0011223344556677'):
    """profile_pin.hpp's format, by the standard library's SHA-256: so the C++ one is checked as well."""
    round_ = salt + ':' + pin
    for _ in range(2000):
        round_ = hashlib.sha256((round_ + salt).encode()).hexdigest()
    return salt + ':' + round_


def pin_keys(pin, start=0):
    """The D-pad presses that type a PIN on the pad: 1-9 in three rows, then Clear, 0, Cancel."""
    keys, at = [], start
    for digit in pin:
        target = 10 if digit == '0' else int(digit) - 1
        keys += ['Up'] * max(0, at // 3 - target // 3) + ['Down'] * max(0, target // 3 - at // 3)
        keys += ['Left'] * max(0, at % 3 - target % 3) + ['Right'] * max(0, target % 3 - at % 3) + ['a']
        at = target
    return keys


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
        rom = self.nds / 'Game.nds'
        rom.write_bytes(bytes(12) + b'ABCD' + bytes(0x200))
        self.key = game_key(rom.resolve())
        (self.data / 'library' / self.key).mkdir(parents=True)
        (self.data / 'library' / self.key / 'info.ini').write_text('rating=PEGI 3\n')

    def profiles(self, owner_pin='1234', active='p1', kid_pin='', adult=False, ask=1, extra=''):
        folder = self.data / 'profiles/p1'
        folder.mkdir(parents=True)
        pin = 'pin=' + stored_pin(kid_pin) + '\n' if kid_pin else ''
        (folder /
         'profile.ini').write_text(f'schema=1\nname=Ola\npicture=2\nrestricted=1\nage=7\nunrated=hide\n{pin}{extra}')
        if adult:
            sara = self.data / 'profiles/p2'
            sara.mkdir()
            (sara / 'profile.ini').write_text('schema=1\nname=Sara\npicture=3\nrestricted=0\nage=-1\nunrated=show\n')
        index = f'schema=1\nowner_name=Andreas\nask_at_boot={ask}\n'
        if owner_pin: index += 'owner_pin=' + stored_pin(owner_pin) + '\n'
        (self.data / 'profiles/index.ini').write_text(index)
        (self.data / 'profiles/active').write_text(active + '\n')
        return folder

    def run_ui(self, events=(), flags=('--settings',), seconds=3, gap=80):
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
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=40
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(report.read_text())

    def active(self):
        return (self.data / 'profiles/active').read_text().strip()


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class EnteringTests(Base):
    # From the picker at boot: Andreas 0, Ola 1, Sara 2
    def boot(self, events, seconds=4):
        return self.run_ui(events, flags=('--boot-startup',), seconds=seconds)

    def test_a_child_cannot_walk_into_the_owners_profile(self):
        self.profiles()
        report = self.boot(['Left', 'a'])
        self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_ENTER))
        self.assertEqual(self.active(), 'p1')

    def test_the_right_pin_goes_in_and_the_wrong_one_does_not(self):
        self.profiles()
        report = self.boot(['Left', 'a'] + pin_keys('1234'))
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'owner'))
        self.assertEqual(self.active(), 'owner')
        (self.data / 'profiles/active').write_text('p1\n')
        report = self.boot(['Left', 'a'] + pin_keys('1235'))
        self.assertEqual(report['page'], PIN)
        self.assertEqual(report['pin_digits'], 0)
        self.assertEqual(self.active(), 'p1')

    def test_cancel_goes_back_to_the_picker(self):
        self.profiles()
        report = self.boot(['Left', 'a', 'Escape'])
        self.assertEqual(report['page'], PICKER)
        report = self.boot(['Left', 'a'] + pin_keys('12') + ['Escape'])
        self.assertEqual((report['page'], report['pin_digits']), (PIN, 1))  # B takes back a digit first

    def test_a_child_cannot_walk_into_a_grown_ups_profile_that_has_no_pin(self):
        self.profiles(adult=True)
        report = self.boot(['Right', 'a'])  # from Ola to Sara
        self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_ENTER))
        report = self.boot(['Right', 'a'] + pin_keys('1234'))
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p2'))

    def test_a_kid_with_no_pin_is_open_to_anyone(self):
        self.profiles(active='owner', ask=1)
        report = self.boot(['Right', 'a'])
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))

    def test_whoever_played_last_still_needs_their_pin_at_boot(self):
        self.profiles(active='owner', ask=1)
        report = self.boot(['a'])
        self.assertEqual(report['page'], PIN)

    def test_turning_the_question_off_does_not_open_a_pin_protected_profile(self):
        self.profiles(active='owner', ask=0)
        self.assertEqual(self.boot([])['page'], PICKER)
        (self.data / 'profiles/active').write_text('p1\n')
        self.assertEqual(self.boot([])['page'], LIBRARY)  # a Kid with no PIN of its own boots straight in

    def test_the_owner_once_in_goes_anywhere_without_a_pin(self):
        self.profiles(active='owner', kid_pin='9999')
        # Settings > Profiles > Change who's playing, then Ola
        report = self.run_ui(['Down'] * 10 + ['a', 'a', 'Right', 'a'], seconds=4)
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))

    def test_a_kids_own_pin_stops_another_kid(self):
        self.profiles(active='owner', kid_pin='9999', adult=False, ask=1)
        report = self.boot(['Right', 'a'])
        self.assertEqual(report['page'], PIN)
        report = self.boot(['Right', 'a'] + pin_keys('9999'))
        self.assertEqual(report['profile'], 'p1')

    def test_no_owner_pin_nothing_to_enter(self):
        self.profiles(owner_pin='')
        report = self.boot(['Left', 'a'])
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'owner'))


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class TriesTests(Base):
    def wrong(self, times):
        return ['Left', 'a'] + pin_keys('1235') + sum((pin_keys('1235', 4) for _ in range(times - 1)), [])

    def test_five_wrong_tries_wait_and_a_restart_does_not_clear_it(self):
        self.profiles()
        self.run_ui(self.wrong(5), flags=('--boot-startup',), seconds=8, gap=60)
        state = read_fields(self.data / 'profiles/pin-state.ini')
        self.assertEqual(state['tries'], '5')
        self.assertGreater(int(state['until']), time.time())
        self.assertLessEqual(int(state['until']), time.time() + 30)
        # Restarted, and now with the right PIN: still refused.
        report = self.run_ui(['Left', 'a'] + pin_keys('1234'), flags=('--boot-startup',), seconds=4)
        self.assertEqual(report['page'], PIN)
        self.assertEqual(self.active(), 'p1')

    def test_the_wait_doubles_to_five_minutes(self):
        self.profiles()
        (self.data / 'profiles/pin-state.ini').write_text('tries=6\nuntil=0\n')
        self.run_ui(['Left', 'a'] + pin_keys('1235'), flags=('--boot-startup',), seconds=4)
        self.assertAlmostEqual(
            int(read_fields(self.data / 'profiles/pin-state.ini')['until']), time.time() + 120, delta=10
        )
        (self.data / 'profiles/pin-state.ini').write_text('tries=50\nuntil=0\n')
        self.run_ui(['Left', 'a'] + pin_keys('1235'), flags=('--boot-startup',), seconds=4)
        self.assertAlmostEqual(
            int(read_fields(self.data / 'profiles/pin-state.ini')['until']), time.time() + 300, delta=10
        )

    def test_the_right_pin_clears_the_tries(self):
        self.profiles()
        (self.data / 'profiles/pin-state.ini').write_text('tries=3\n')
        self.run_ui(['Left', 'a'] + pin_keys('1234'), flags=('--boot-startup',), seconds=4)
        self.assertFalse((self.data / 'profiles/pin-state.ini').exists())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class ForgottenTests(Base):
    def test_a_file_on_the_card_removes_only_the_owners_pin_at_boot(self):
        self.profiles(kid_pin='9999')
        (self.data / 'profiles/pin-state.ini').write_text('tries=9\nuntil=99999999999\n')
        (self.root / 'reset-pin').write_text('')
        self.run_ui(flags=('--boot-startup',))
        self.assertNotIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))
        self.assertIn('pin', read_fields(self.data / 'profiles/p1/profile.ini'))
        self.assertFalse((self.root / 'reset-pin').exists())
        self.assertFalse((self.data / 'profiles/pin-state.ini').exists())
        # It does not leave the file for someone to rely on, and the owner's profile opens.
        report = self.run_ui(['a'], flags=('--boot-startup',))
        self.assertEqual(report['page'], PIN)  # still Ola's own PIN, which was not reset

    def test_a_running_launcher_does_not_act_on_it(self):
        self.profiles()
        (self.root / 'reset-pin').write_text('')
        self.run_ui()
        self.assertIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))
        self.assertTrue((self.root / 'reset-pin').exists())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class SettingPinTests(Base):
    # Settings > Profiles, the owner's page: Name, DS name, Picture, Colour, Games, PIN
    TO_OWNER = ['Down'] * 10 + ['a', 'Down', 'a']

    def test_choosing_a_pin_asks_twice_and_keeps_no_digits_on_the_card(self):
        self.profiles(owner_pin='', active='owner')
        report = self.run_ui(self.TO_OWNER + ['Down'] * 5 + ['a'] + pin_keys('2580'), seconds=4)
        self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_CONFIRM))
        report = self.run_ui(self.TO_OWNER + ['Down'] * 5 + ['a'] + pin_keys('2580') + pin_keys('2580', 10), seconds=5)
        self.assertEqual(report['page'], EDIT)
        saved = read_fields(self.data / 'profiles/index.ini')['owner_pin']
        salt, _, digest = saved.partition(':')
        self.assertEqual(stored_pin('2580', salt), saved)
        self.assertNotIn('2580', (self.data / 'profiles/index.ini').read_text())

    def test_two_different_tries_start_over(self):
        self.profiles(owner_pin='', active='owner')
        report = self.run_ui(self.TO_OWNER + ['Down'] * 5 + ['a'] + pin_keys('2580') + pin_keys('2581', 10), seconds=5)
        self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_SET))
        self.assertNotIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))

    def test_the_same_pin_is_stored_differently_each_time(self):
        self.profiles(owner_pin='', active='owner')
        self.run_ui(self.TO_OWNER + ['Down'] * 5 + ['a'] + pin_keys('2580') + pin_keys('2580', 10), seconds=5)
        first = read_fields(self.data / 'profiles/index.ini')['owner_pin']
        self.run_ui(self.TO_OWNER + ['Down'] * 5 + ['a'] + pin_keys('2580') + pin_keys('2580', 10), seconds=5)
        self.assertNotEqual(read_fields(self.data / 'profiles/index.ini')['owner_pin'], first)

    def test_it_can_be_removed(self):
        self.profiles(active='owner')
        report = self.run_ui(self.TO_OWNER + ['Down'] * 6 + ['a'], seconds=4)
        self.assertEqual(report['page'], EDIT)
        self.assertNotIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))

    def test_a_new_kid_asks_for_the_owners_pin_once_and_it_can_be_skipped(self):
        new_kid = ['Down'] * 10 + ['a', 'Down', 'a', 'Right', 'a', 'text Ola', 'Return', 'a']
        report = self.run_ui(new_kid, seconds=4)
        self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_SET))
        # Skipped: Cancel on the pad, and on to its games, with nothing locked.
        shutil.rmtree(self.data / 'profiles')
        report = self.run_ui(new_kid + pin_keys('') + ['Escape'], seconds=4)
        self.assertEqual((report['page'], report['games_tab']), (GAMES, 3))
        self.assertNotIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))
        self.assertEqual(read_fields(self.data / 'profiles/p1/profile.ini')['restricted'], '1')

    def test_a_new_kid_with_the_pin_given_ends_on_its_games(self):
        new_kid = ['Down'] * 10 + ['a', 'Down', 'a', 'Right', 'a', 'text Ola', 'Return', 'a']
        report = self.run_ui(new_kid + pin_keys('2580') + pin_keys('2580', 10), seconds=5)
        self.assertEqual((report['page'], report['games_tab']), (GAMES, 3))
        self.assertIn('owner_pin', read_fields(self.data / 'profiles/index.ini'))


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class LockTests(Base):
    # The root list: Theme, Display, Power, Library, Sound, Connectivity, System, Updates, Controls, Statistics, Profiles,
    # Power off
    def row(self, index):
        return ['Down'] * index + ['a']

    def test_each_settings_row_is_open_or_asks(self):
        self.profiles()
        for name, index, locked in (
            ('Theme', 0, False), ('Display', 1, False), ('Power', 2, True), ('Library', 3, True), ('Sound', 4, False),
            ('Connectivity', 5, True), ('System', 6, True), ('Updates', 7, True), ('Controls', 8, True),
            ('Statistics', 9, False), ('Profiles', 10, False)
        ):
            with self.subTest(row=name):
                report = self.run_ui(self.row(index), seconds=3)
                if locked: self.assertEqual((report['page'], report['pin_purpose']), (PIN, PIN_UNLOCK))
                else: self.assertNotEqual(report['page'], PIN)

    def test_the_owners_pin_opens_it_and_going_back_to_the_games_locks_it_again(self):
        self.profiles()
        report = self.run_ui(self.row(3) + pin_keys('1234'), seconds=4)
        self.assertEqual((report['page'], report['settings_unlocked']), (6, 1))  # Page::LibraryOptions
        report = self.run_ui(self.row(3) + pin_keys('1234') + ['Escape', 'Escape'], seconds=4)
        self.assertEqual((report['page'], report['settings_unlocked']), (LIBRARY, 0))

    def test_a_wrong_pin_opens_nothing_and_cancel_goes_back(self):
        self.profiles()
        report = self.run_ui(self.row(3) + pin_keys('1235'), seconds=4)
        self.assertEqual((report['page'], report['settings_unlocked']), (PIN, 0))
        report = self.run_ui(self.row(3) + ['Escape'], seconds=3)
        self.assertEqual(report['page'], SETTINGS)

    def test_a_kids_pin_is_not_the_owners(self):
        self.profiles(kid_pin='9999')
        report = self.run_ui(self.row(3) + pin_keys('9999'), seconds=4)
        self.assertEqual((report['page'], report['settings_unlocked']), (PIN, 0))

    def test_keys_nobody_planned_for_stop_at_the_pin(self):
        self.profiles()
        for key in ('Tab', 'c'):
            with self.subTest(key=key):
                report = self.run_ui(['Escape', key], seconds=3)
                self.assertEqual(report['page'], PIN)
                self.assertNotEqual(report['page'], 6)

    def test_a_restored_page_stops_at_the_pin(self):
        self.profiles()
        state = self.root / 'state.txt'
        state.write_text('1 0 4 ""\n17 9 0\n')  # Page::Maintenance
        report = self.run_ui(flags=('--state', str(state)))
        self.assertEqual(report['page'], PIN)

    def test_a_section_named_on_the_command_line_stops_at_the_pin(self):
        self.profiles()
        report = self.run_ui(flags=('--settings', '--section', 'updates'))
        self.assertEqual(report['page'], PIN)

    def test_a_game_can_be_played_with_and_its_options_are_mostly_closed(self):
        self.profiles()
        # Favourite, Shader, Controls, Artwork, 3D resolution, Game info, Save gallery, DS controls
        opts = ['x']
        self.assertEqual(self.run_ui(opts, flags=('--library',))['page'], 19)  # Page::GameOptions
        self.assertEqual(self.run_ui(opts + ['Down', 'a'], flags=('--library',))['page'], 20)  # Shaders
        self.assertEqual(self.run_ui(opts + ['Down', 'Down', 'a'], flags=('--library',))['page'], PIN)  # Controls
        self.assertEqual(self.run_ui(opts + ['Down'] * 3 + ['a'], flags=('--library',))['page'], PIN)  # Artwork
        self.assertEqual(self.run_ui(opts + ['Down'] * 6 + ['a'], flags=('--library',))['page'], PIN)  # Save gallery

    def test_a_game_cannot_be_removed_from_a_kid_profile(self):
        self.profiles()
        # The list ends at DS controls: no Remove game, no Visible to…
        report = self.run_ui(['x'] + ['Down'] * 12 + ['a'], flags=('--library',))
        self.assertNotEqual(report['page'], 18)  # Page::Confirm
        self.assertTrue((self.nds / 'Game.nds').exists())

    def test_managing_profiles_needs_the_pin_but_switching_does_not(self):
        self.profiles()
        # Change who's playing, Andreas, Ola, Add, the question
        to_page = ['Down'] * 10 + ['a']
        self.assertEqual(self.run_ui(to_page + ['a'])['page'], PICKER)
        self.assertEqual(self.run_ui(to_page + ['Down', 'a'])['page'], PIN)
        self.assertEqual(self.run_ui(to_page + ['Down', 'Down', 'Down', 'a'])['page'], PIN)
        report = self.run_ui(to_page + ['Down'] * 4 + ['a'])
        self.assertEqual(report['page'], PIN)
        self.assertEqual(read_fields(self.data / 'profiles/index.ini').get('ask_at_boot'), '1')  # not changed
        report = self.run_ui(to_page + ['Down'] * 4 + ['a'] + pin_keys('1234'), seconds=4)
        self.assertEqual(report['page'], PROFILES)

    def test_a_grown_ups_settings_are_open_but_not_the_profiles(self):
        self.profiles(adult=True, active='p2')
        self.assertEqual(self.run_ui(self.row(3))['page'], 6)  # Library
        report = self.run_ui(self.row(10) + ['Down', 'a'])
        self.assertEqual(report['page'], PIN)

    def test_with_no_owner_pin_a_kid_profile_is_filtered_but_not_locked(self):
        self.profiles(owner_pin='')
        report = self.run_ui(self.row(3))
        self.assertEqual(report['page'], 6)
        self.assertEqual(report['all_games'], 1)

    def test_searching_still_works(self):
        self.profiles()
        report = self.run_ui(['/'], flags=('--library',))
        self.assertEqual(report['page'], KEYBOARD)

    def test_the_owner_is_never_stopped(self):
        self.profiles(active='owner')
        self.assertEqual(self.run_ui(self.row(3))['page'], 6)
        self.assertEqual(self.run_ui(['Escape', 'Tab'])['page'], 6)

    def test_update_prompts_are_for_the_owner(self):
        self.profiles()
        (self.data / 'update').mkdir()
        (self.data / 'update/status.ini').write_text('state=available\noffer=9.9.9\nversion=9.9.9\n')
        report = self.run_ui(['y'], flags=('--home',))
        self.assertNotEqual(report['page'], PIN)
        self.assertNotEqual(report['page'], MAINTENANCE)
        (self.data / 'profiles/active').write_text('owner\n')
        report = self.run_ui(['y'], flags=('--home',))
        self.assertEqual(report['page'], MAINTENANCE)

    def test_cheats_in_a_game_need_a_permission_with_or_without_a_pin(self):
        menu = ('--game-menu', '--game-core', 'rgds-experimental', '--game-rom', str(self.nds / 'Game.nds'))
        to_cheats = ['Down'] * 4 + ['Right', 'a']
        folder = self.profiles()
        self.assertEqual(self.run_ui(to_cheats, flags=menu)['page'], 12)  # Page::QuickMenu, no PIN pad over a game
        (self.data / 'profiles/index.ini').write_text('schema=1\nask_at_boot=1\n')
        self.assertEqual(self.run_ui(to_cheats, flags=menu)['page'], 12)  # no owner's PIN: still off
        (folder / 'profile.ini').write_text('schema=1\nname=Ola\nrestricted=1\nage=7\ncheats=1\n')
        self.assertEqual(self.run_ui(to_cheats, flags=menu)['page'], 38)  # Page::Cheats
        (self.data / 'profiles/active').write_text('owner\n')
        (folder / 'profile.ini').write_text('schema=1\nname=Ola\nrestricted=1\nage=7\n')
        self.assertEqual(self.run_ui(to_cheats, flags=menu)['page'], 38)

    def test_the_rest_of_the_game_menu_stays_open_to_a_kid(self):
        menu = ('--game-menu', '--game-core', 'rgds-experimental', '--game-rom', str(self.nds / 'Game.nds'))
        self.profiles()
        self.assertEqual(self.run_ui([], flags=menu)['page'], 12)
        # The grid's second row, first: Sound. Whatever is there opens without a PIN.
        report = self.run_ui(['Down', 'Down', 'a'], flags=menu)
        self.assertNotEqual(report['page'], PIN)
        self.assertNotEqual(report['page'], 12)

    def test_pictochat_needs_a_permission(self):
        folder = self.profiles()
        # Home: from Games, down to Recents, across to PictoChat
        to_chat = ['Down', 'Right', 'a']
        self.assertEqual(self.run_ui(to_chat, flags=('--home',))['chat_open'], 0)
        (folder / 'profile.ini').write_text('schema=1\nname=Ola\nrestricted=1\nage=7\npictochat=1\n')
        self.assertEqual(self.run_ui(to_chat, flags=('--home',))['chat_open'], 1)


class WirelessTests(unittest.TestCase):
    def setUp(self):
        import emulators
        self.emulators = emulators
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / 'userdata'
        self.kid = self.data / 'profiles/p1'
        self.kid.mkdir(parents=True)
        (self.data / 'profiles/active').write_text('p1\n')
        self.app = self.root / 'app'
        binary = self.app / 'emulators/rgds-experimental/rgds-experimental'
        binary.parent.mkdir(parents=True)
        binary.write_text('#!/bin/sh\n')
        binary.chmod(0o755)
        self.rom = self.root / 'Game.nds'
        self.rom.write_bytes(bytes(12) + b'ABCD' + bytes(0x200))
        game = self.kid / 'library' / game_key(self.rom.resolve())
        game.mkdir(parents=True)
        (game / 'ds-input.ini').write_text('wireless=1\n')

    def mode(self):
        self.emulators.launch_plan('rgds-experimental', self.app, self.data, self.rom)
        return (self.kid / 'emulators/rgds-experimental/config.ini').read_text()

    def test_local_wireless_stays_off_on_a_kid_profile_unless_permitted(self):
        (self.kid / 'profile.ini').write_text('restricted=1\n')
        self.assertIn('mode = off', self.mode())
        (self.kid / 'profile.ini').write_text('restricted=1\nwireless=1\n')
        self.assertIn('mode = auto', self.mode())
        (self.kid / 'profile.ini').write_text('restricted=0\n')
        self.assertIn('mode = auto', self.mode())


@unittest.skipUnless(shutil.which('clang++'), 'Requires host C++')
class HashTests(unittest.TestCase):
    def test_sha256_matches_the_standard_and_the_format_is_what_is_stored(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'driver.cpp'
            source.write_text(
                r'''#include "launcher/profile_pin.hpp"
#include <iostream>
#include <vector>
using namespace rgds;
int main() {
    for (const std::string& text : std::vector<std::string>{"", "abc", std::string(55, 'a'), std::string(56, 'a'), std::string(1000, 'x')}) {
        const auto digest = sha256(text);
        std::cout << pin_hex(digest.data(), digest.size()) << "\n";
    }
    std::cout << pin_digest("0011223344556677", "1234") << "\n";
    std::cout << pin_matches("0011223344556677:" + pin_digest("0011223344556677", "1234"), "1234")
              << pin_matches("0011223344556677:" + pin_digest("0011223344556677", "1234"), "1235")
              << pin_matches("nonsense", "1234") << pin_matches("", "") << "\n";
    const auto one = pin_store("1234"), two = pin_store("1234");
    std::cout << (one != two) << pin_matches(one, "1234") << pin_matches(two, "1234") << "\n";
    for (int tries : {0, 4, 5, 6, 7, 8, 9, 50}) std::cout << pin_wait_after(tries) << " ";
    std::cout << "\n";
}
'''
            )
            binary = Path(folder) / 'driver'
            subprocess.run(
                ['clang++', '-std=c++17', '-I' + str(ROOT / 'src'),
                 str(source), '-o', str(binary)], check=True
            )
            lines = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.splitlines()
        for text, line in zip(('', 'abc', 'a' * 55, 'a' * 56, 'x' * 1000), lines):
            self.assertEqual(line, hashlib.sha256(text.encode()).hexdigest())
        self.assertEqual('0011223344556677:' + lines[5], stored_pin('1234'))
        self.assertEqual(lines[6], '1000')
        self.assertEqual(lines[7], '111')
        self.assertEqual(lines[8].split(), ['0', '0', '30', '60', '120', '240', '300', '300'])


if __name__ == '__main__':
    unittest.main()
