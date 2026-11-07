"""Who's playing?, Settings > Profiles and a profile's page, through real SDL events in the host preview."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import read_fields
from profiles import erase_removed, player_root, remove_profile

BINARY = ROOT / 'build/host/rgds-launcher'
PICKER, PROFILES, EDIT, CHOOSE = 48, 49, 50, 51  # Page::ProfilePicker and after: stored numbers, appended
LIBRARY = 0
# From --settings: the Profiles row in the root list.
TO_PROFILES = ['Down'] * 10 + ['a']


class Base(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name).resolve() / 'userdata'
        self.data.mkdir()
        (self.data / 'setup.ini').write_text('done=1\n')
        (self.data / 'ds.ini').write_text('nickname=Andreas\n')
        (self.data / 'ui.ini').write_text('menu_music=1\naccent=2\n')

    def add(self, name='p1', title='Ola', restricted=1, active=False):
        folder = self.data / 'profiles' / name
        folder.mkdir(parents=True)
        (folder / 'profile.ini').write_text(f'schema=1\nname={title}\npicture=2\nrestricted={restricted}\n')
        (folder / 'ui.ini').write_text('menu_music=0\naccent=10\n')
        (folder / 'ds.ini').write_text(f'nickname={title}\n')
        (self.data / 'profiles/index.ini').write_text('schema=1\nowner_name=Andreas\n')
        if active or not (self.data / 'profiles/active').exists():
            (self.data / 'profiles/active').write_text((name if active else 'owner') + '\n')
        return folder

    def run_ui(self, events=(), boot=False, settings=False, seconds=3, gap=110):
        script = self.data.parent / 'input.txt'
        lines = []
        for i, event in enumerate(events):
            lines.append(f'{300 + i * gap} {event if " " in event else "key " + event}\n')
        script.write_text(''.join(lines))
        report = self.data.parent / 'report.json'
        command = [
            str(BINARY), '--preview', '--data',
            str(self.data), '--input-script',
            str(script), '--cache-report',
            str(report), '--seconds',
            str(seconds)
        ]
        if boot: command.append('--boot-startup')
        if settings: command.append('--settings')
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

    def active(self):
        return (self.data / 'profiles/active').read_text().strip()


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class WithoutProfilesTests(Base):
    def test_one_person_sees_no_new_screen_and_nothing_is_written(self):
        report = self.run_ui(['Down'], boot=True)
        self.assertEqual(report['page'], LIBRARY)
        self.assertEqual(report['profile_count'], 1)
        self.assertFalse((self.data / 'profiles').exists())

    def test_opening_the_page_and_leaving_writes_nothing(self):
        report = self.run_ui(TO_PROFILES + ['Escape'], settings=True)
        self.assertEqual(report['page'], 1)  # Page::Settings
        self.assertFalse((self.data / 'profiles').exists())

    def test_cancelling_a_new_profile_leaves_nothing_behind(self):
        report = self.run_ui(TO_PROFILES + ['Down', 'a', 'a', 'Escape'], settings=True)
        self.assertEqual(report['page'], PROFILES)
        self.assertFalse((self.data / 'profiles').exists())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class PickerTests(Base):
    def test_boot_asks_with_the_last_profile_chosen_and_a_goes_on(self):
        self.add()
        self.assertEqual(self.run_ui(boot=True)['page'], PICKER)
        (self.data / 'profiles/active').write_text('p1\n')
        report = self.run_ui(boot=True)
        self.assertEqual((report['page'], report['profile_focus'], report['profile']), (PICKER, 1, 'p1'))
        report = self.run_ui(['a'], boot=True)
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))
        self.assertEqual(self.active(), 'p1')

    def test_choosing_someone_else_changes_whose_look_and_files_are_used(self):
        self.add()
        report = self.run_ui(['Right', 'a'], boot=True)
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))
        self.assertEqual(self.active(), 'p1')
        self.assertEqual(report['menu_music'], 0)  # the child's ui.ini, not the owner's
        report = self.run_ui(['a'], boot=True)
        self.assertEqual(report['menu_music'], 0)

    def test_there_is_no_way_past_it_at_boot_but_choosing(self):
        self.add()
        report = self.run_ui(['Escape', 'b', 'Tab'], boot=True)
        self.assertEqual(report['page'], PICKER)

    def test_the_owner_can_turn_the_question_off(self):
        self.add()
        (self.data / 'profiles/index.ini').write_text('schema=1\nask_at_boot=0\n')
        self.assertEqual(self.run_ui(boot=True)['page'], LIBRARY)

    def test_an_unclear_profile_always_asks_with_no_way_out(self):
        self.add()
        (self.data / 'profiles/active').write_text('p9\n')
        (self.data / 'profiles/index.ini').write_text('schema=1\nask_at_boot=0\n')
        report = self.run_ui(['Escape'], boot=True)
        self.assertEqual(report['page'], PICKER)
        self.assertEqual(report['profile_unclear'], 1)
        report = self.run_ui(['a'], boot=True)
        self.assertEqual((report['page'], report['profile_unclear']), (LIBRARY, 0))
        self.assertEqual(self.active(), 'owner')

    def test_a_tile_can_be_tapped(self):
        self.add()
        report = self.run_ui(['tap 400 210', 'release 0 0'], boot=True)
        self.assertEqual(report['profile'], 'p1')
        self.assertEqual(report['page'], LIBRARY)

    def test_four_tiles_wrap_and_the_arrows_stay_on_them(self):
        for i, title in enumerate(('Ola', 'Sara', 'Theo'), 1):
            self.add('p%d' % i, title)
        self.assertEqual(self.run_ui(['Down'], boot=True)['profile_focus'], 3)
        self.assertEqual(self.run_ui(['Right', 'Right', 'Right', 'Right'], boot=True)['profile_focus'], 3)
        self.assertEqual(self.run_ui(['Left'], boot=True)['profile_focus'], 0)
        self.assertEqual(self.run_ui(['Up'], boot=True)['profile_focus'], 0)
        self.assertEqual(self.run_ui(['Right', 'Down'], boot=True)['profile_focus'], 3)

    def test_the_chip_on_home_opens_it_and_back_returns(self):
        self.add()
        to_chip = ['a', 'Down', 'Down', 'Down', 'Down', 'Right']
        report = self.run_ui(to_chip + ['a'], boot=True)
        self.assertEqual(report['page'], PICKER)
        report = self.run_ui(to_chip + ['a', 'Escape'], boot=True)
        self.assertEqual(report['page'], LIBRARY)
        self.assertEqual(report['profile'], 'owner')
        report = self.run_ui(to_chip + ['a', 'Right', 'a'], boot=True, seconds=4)
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))

    def test_the_chip_is_not_there_for_one_person(self):
        report = self.run_ui(['tap 180 444', 'release 0 0'], boot=True)
        self.assertEqual(report['page'], LIBRARY)


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class ManageTests(Base):
    def new_profile(self, *kind, text=('Ola',)):
        return TO_PROFILES + ['Down', 'a', *kind, 'a', *('text ' + part for part in text), 'Return']

    def test_a_new_adult_gets_its_own_folder_and_the_owner_is_untouched(self):
        before = {p.name: p.read_bytes() for p in self.data.iterdir() if p.is_file()}
        report = self.run_ui(self.new_profile(), settings=True)
        self.assertEqual((report['page'], report['profile_count']), (EDIT, 2))
        folder = self.data / 'profiles/p1'
        info = read_fields(folder / 'profile.ini')
        self.assertEqual((info['name'], info['restricted']), ('Ola', '0'))
        self.assertEqual(read_fields(folder / 'ds.ini')['nickname'], 'Ola')
        self.assertEqual(read_fields(folder / 'pictochat.ini')['name'], 'Ola')
        self.assertEqual(self.active(), 'owner')
        self.assertEqual(read_fields(self.data / 'profiles/index.ini')['owner_name'], 'Andreas')
        self.assertEqual(player_root(self.data), self.data)
        for name, content in before.items():
            if name != 'preferences.ini': self.assertEqual((self.data / name).read_bytes(), content, name)
        # It starts from the owner's look, in a colour of its own.
        self.assertIn('menu_music=1', (folder / 'ui.ini').read_text())
        self.assertNotIn('accent=2\n', (folder / 'ui.ini').read_text())

    def test_a_new_kid_is_marked_and_cancelling_the_name_adds_nothing(self):
        self.run_ui(self.new_profile('Right'), settings=True)
        self.assertEqual(read_fields(self.data / 'profiles/p1/profile.ini')['restricted'], '1')
        self.run_ui(TO_PROFILES + ['Down', 'Down', 'Down', 'a', 'Right', 'a', 'Escape'], settings=True)
        self.assertFalse((self.data / 'profiles/p2').exists())

    def test_the_name_being_typed_is_readable(self):
        # Found on the handheld: the name fields fell through to the password's stars, which look the same for any name.
        pictures = []
        for name in ('wwww', 'iiii'):
            script = self.data.parent / 'input.txt'
            script.write_text(
                ''.join(
                    f'{300 + i * 110} {e if " " in e else "key " + e}\n'
                    for i, e in enumerate(TO_PROFILES + ['Down', 'a', 'a', 'text ' + name])
                )
            )
            shot = self.data.parent / name
            subprocess.run(
                [
                    str(BINARY), '--preview', '--settings', '--data',
                    str(self.data), '--input-script',
                    str(script), '--seconds', '3', '--screenshot',
                    str(shot)
                ],
                cwd=ROOT,
                env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
                capture_output=True,
                timeout=30,
                check=True
            )
            # The start of the name field on the bottom panel: left of the caret, which blinks.
            picture = shot.read_bytes()
            offset = int.from_bytes(picture[10:14], 'little')
            width = int.from_bytes(picture[18:22], 'little')
            height = int.from_bytes(picture[22:26], 'little', signed=True)
            depth = int.from_bytes(picture[28:30], 'little') // 8
            step = (width * depth + 3) // 4 * 4
            rows = [480 + 109 + i if height < 0 else abs(height) - 1 - (480 + 109 + i) for i in range(28)]
            pictures.append(
                [picture[offset + row * step + 38 * depth:offset + row * step + 70 * depth] for row in rows]
            )
        self.assertNotEqual(pictures[0], pictures[1])

    def test_the_ds_name_keeps_to_ten_characters(self):
        self.run_ui(self.new_profile(text=('Maximiliana', 'Fredr', 'ikke')), settings=True)
        folder = self.data / 'profiles/p1'
        self.assertEqual(read_fields(folder / 'profile.ini')['name'], 'MaximilianaFredr')  # the name field's 16
        self.assertEqual(read_fields(folder / 'ds.ini')['nickname'], 'Maximilian')
        self.assertEqual(read_fields(folder / 'pictochat.ini')['name'], 'Maximilian')

    def edit_page(self, row=2):
        # Change who's playing, Andreas, Ola, ...: the row's profile, entered
        return TO_PROFILES + ['Down'] * row + ['a']

    def test_a_profiles_name_and_ds_name_can_be_changed_while_someone_else_plays(self):
        folder = self.add()
        (folder / 'pictochat.ini').write_text('name=Ola\ncolour=auto\n')
        report = self.run_ui(self.edit_page() + ['Down', 'Down', 'a', 'text x', 'Return'], settings=True)
        self.assertEqual((report['page'], report['profile_focus']), (EDIT, 2))
        self.assertEqual(read_fields(folder / 'ds.ini')['nickname'], 'Olax')
        self.assertEqual(read_fields(folder / 'pictochat.ini')['name'], 'Olax')
        self.assertEqual(read_fields(folder / 'pictochat.ini')['colour'], 'auto')
        self.assertEqual(read_fields(self.data / 'ds.ini')['nickname'], 'Andreas')
        self.run_ui(self.edit_page() + ['Down', 'a', 'text y', 'Return'], settings=True)
        self.assertEqual(read_fields(folder / 'profile.ini')['name'], 'Olay')
        self.assertEqual(read_fields(folder / 'ds.ini')['nickname'], 'Olax')

    def test_the_active_profiles_ds_name_also_reaches_the_running_chat_name(self):
        folder = self.add(active=True)
        self.run_ui(self.edit_page() + ['Down', 'Down', 'a', 'text x', 'Return'], settings=True)
        self.assertEqual(read_fields(folder / 'ds.ini')['nickname'], 'Olax')
        self.assertEqual(read_fields(folder / 'pictochat.ini')['name'], 'Olax')
        self.assertEqual(read_fields(self.data / 'ds.ini')['nickname'], 'Andreas')

    def test_who_its_for_picture_and_colour_are_saved(self):
        folder = self.add()
        self.run_ui(self.edit_page() + ['a'], settings=True)
        self.assertEqual(read_fields(folder / 'profile.ini')['restricted'], '0')
        # A picture or a colour is applied at once and the page stays, to try another; B goes back.
        report = self.run_ui(self.edit_page() + ['Down', 'Down', 'Down', 'a', 'Right', 'a'], settings=True)
        self.assertEqual(report['page'], CHOOSE)
        self.assertEqual(read_fields(folder / 'profile.ini')['picture'], '3')
        report = self.run_ui(
            self.edit_page() + ['Down', 'Down', 'Down', 'a', 'Down', 'Down', 'a', 'Escape'], settings=True
        )
        self.assertEqual((report['page'], report['profile_focus']), (EDIT, 3))
        self.assertEqual(read_fields(folder / 'profile.ini')['picture'], '15')  # from 3, two rows down: in colour
        report = self.run_ui(self.edit_page() + ['Down', 'Down', 'Down', 'Down', 'a', 'Left', 'a'], settings=True)
        self.assertEqual(report['page'], CHOOSE)
        self.assertIn('accent=9', (folder / 'ui.ini').read_text())
        report = self.run_ui(self.edit_page() + ['Down', 'Down', 'Down', 'Down', 'a', 'Escape'], settings=True)
        self.assertEqual((report['page'], report['profile_focus']), (EDIT, 4))
        self.assertIn('accent=2', (self.data / 'ui.ini').read_text())

    def test_the_owner_has_no_kind_and_cannot_be_removed(self):
        self.add()
        report = self.run_ui(self.edit_page(1) + ['Down'] * 8, settings=True)
        self.assertEqual(
            (report['page'], report['profile_focus']), (EDIT, 7)
        )  # Name, DS name, Picture, Colour, Language, Games, Games opens, PIN
        self.run_ui(self.edit_page(1) + ['Down', 'a', 'text x', 'Return'], settings=True)
        self.assertEqual(read_fields(self.data / 'ds.ini')['nickname'], 'Andreasx')
        self.run_ui(self.edit_page(1) + ['a', 'text x', 'Return'], settings=True)
        self.assertEqual(read_fields(self.data / 'profiles/index.ini')['owner_name'], 'Andreasx')

    def test_six_is_the_limit(self):
        for i in range(1, 6):
            self.add('p%d' % i, 'P%d' % i)
        # Change who's playing, six profiles, then straight to the question: no Add row
        report = self.run_ui(TO_PROFILES + ['Down'] * 12 + ['a'], settings=True)
        self.assertEqual(report['page'], PROFILES)
        self.assertEqual(report['profile_focus'], 7)
        self.assertEqual(read_fields(self.data / 'profiles/index.ini').get('ask_at_boot'), '0')

    def test_the_question_at_boot_can_be_switched_off_on_the_page(self):
        self.add()
        # Change who's playing, Andreas, Ola, Add a profile, the question
        self.run_ui(TO_PROFILES + ['Down'] * 4 + ['a'], settings=True)
        self.assertEqual(read_fields(self.data / 'profiles/index.ini')['ask_at_boot'], '0')
        self.assertEqual(self.run_ui(boot=True)['page'], LIBRARY)

    def test_changing_profile_from_the_page_goes_home_as_them(self):
        self.add()
        report = self.run_ui(TO_PROFILES + ['a', 'Right', 'a'], settings=True)
        self.assertEqual((report['page'], report['profile']), (LIBRARY, 'p1'))

    def test_removing_keeps_the_saves_and_makes_a_backup(self):
        folder = self.add()
        save = folder / 'emulators/rgds-experimental/saves/Game.sav'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'the child\'s')
        # The page's last row, then Confirm's second choice
        report = self.run_ui(self.edit_page() + ['Down'] * 16 + ['a', 'Right', 'a'], settings=True, seconds=5, gap=90)
        self.assertEqual(report['page'], PROFILES)
        self.assertFalse(folder.exists())
        removed = list((self.data / 'profiles/.removed').iterdir())
        self.assertEqual(len(removed), 1)
        self.assertEqual((removed[0] / 'emulators/rgds-experimental/saves/Game.sav').read_bytes(), b'the child\'s')
        self.assertEqual(report['profile_count'], 1)
        exports = list((self.data / 'exports').glob('save-backup-*.zip'))
        self.assertEqual(len(exports), 1)
        with zipfile.ZipFile(exports[0]) as archive:
            self.assertIn('profiles/p1/emulators/rgds-experimental/saves/Game.sav', archive.namelist())

    def test_the_one_playing_cannot_be_removed(self):
        folder = self.add(active=True)
        report = self.run_ui(self.edit_page() + ['Down'] * 16 + ['a'], settings=True, seconds=4, gap=90)
        self.assertEqual(report['page'], EDIT)
        self.assertTrue(folder.exists())

    def test_a_removed_profiles_number_is_never_used_again(self):
        folder = self.add()
        (self.data / 'profiles/.removed').mkdir()
        folder.rename(self.data / 'profiles/.removed/p1-123')
        self.run_ui(self.new_profile(text=('Sara',)), settings=True)
        self.assertTrue((self.data / 'profiles/p2/profile.ini').is_file())
        self.assertFalse((self.data / 'profiles/p1').exists())

    def test_pictochat_leaves_home_on_a_kid_profile_without_it(self):
        folder = self.add(active=True)
        report = self.run_ui(['Down', 'Right'], boot=False)
        self.assertEqual(report['profile'], 'p1')
        self.assertNotEqual(report['home_selected'], 4)  # Recents spans the row: Right finds no PictoChat
        report_tiles = report['home_tiles']
        with open(folder / 'profile.ini', 'a') as file:
            file.write('pictochat=1\n')
        report = self.run_ui(['Down', 'Right'], boot=False)
        self.assertEqual(report['home_selected'], 4)
        self.assertEqual(report['home_tiles'], report_tiles)  # the column keeps its six tiles either way

    def test_a_new_profiles_empty_recents_offers_the_games(self):
        self.add(restricted=0, active=True)  # has played nothing
        report = self.run_ui(['Down', 'a'])
        self.assertEqual((report['profile'], report['library_view'], report['shown_games']), ('p1', 2, 0))
        report = self.run_ui(['Down', 'a', 'a'])
        self.assertEqual(report['library_view'], 0)

    def test_games_opens_is_chosen_per_profile(self):
        folder = self.add()
        # Who it's for, Name, DS name, Picture, Colour, Language, Age limit, Games with no rating, Games, Games opens
        report = self.run_ui(self.edit_page() + ['Down'] * 9 + ['a'], settings=True)
        self.assertEqual(report['page'], EDIT)
        look = (folder / 'ui.ini').read_text()
        self.assertIn('all_games=1', look)
        self.assertIn('show_systems=1', look)
        self.assertNotIn('all_games=1', (self.data / 'ui.ini').read_text())  # the owner's stays as it was
        self.run_ui(self.edit_page() + ['Down'] * 9 + ['a'], settings=True)
        self.assertIn('all_games=0', (folder / 'ui.ini').read_text())

    def test_language_is_chosen_per_profile(self):
        folder = self.add()
        # Who it's for, Name, DS name, Picture, Colour, Language; the picker's first language is English (US)
        report = self.run_ui(self.edit_page() + ['Down'] * 5 + ['a', 'Down', 'a'], settings=True)
        self.assertEqual(report['page'], EDIT)
        chosen = read_fields(folder / 'ui.ini').get('language')
        self.assertNotIn(chosen, (None, '', 'en-US'))
        self.assertNotIn('language=' + chosen, (self.data / 'ui.ini').read_text())  # the owner's stays

    def test_the_rows_answer_touch(self):
        self.add()
        # Row 0 is Change who's playing
        report = self.run_ui(TO_PROFILES + ['tap 320 100', 'release 0 0'], settings=True)
        self.assertEqual(report['page'], PICKER)
        report = self.run_ui(TO_PROFILES + ['tap 320 146', 'release 0 0'], settings=True)
        self.assertEqual(report['page'], EDIT)


class RemoveRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name).resolve() / 'userdata'
        (self.data / 'profiles/p1').mkdir(parents=True)
        (self.data / 'profiles/p2').mkdir()
        (self.data / 'profiles/active').write_text('p2\n')

    def test_bad_names_and_the_active_profile_are_refused(self):
        for name in ('', 'owner', '..', '../..', '.removed', 'p9'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                remove_profile(self.data, name)
        with self.assertRaises(ValueError):
            remove_profile(self.data, 'p2')
        self.assertTrue((self.data / 'profiles/p2').is_dir())

    def test_a_failed_backup_removes_nothing(self):
        from unittest.mock import patch
        with patch('save_library.backup_all', side_effect=ValueError('Not enough space for a save backup.')):
            with self.assertRaises(ValueError):
                remove_profile(self.data, 'p1')
        self.assertTrue((self.data / 'profiles/p1').is_dir())
        self.assertFalse((self.data / 'profiles/.removed').exists())

    def test_it_steps_aside_and_stays_in_every_later_backup(self):
        save = self.data / 'profiles/p1/emulators/rgds-play/saves/gba/Game.sav'
        save.parent.mkdir(parents=True)
        save.write_bytes(b'kept')
        _, kept = remove_profile(self.data, 'p1')
        self.assertEqual((kept / 'emulators/rgds-play/saves/gba/Game.sav').read_bytes(), b'kept')
        from save_library import backup_all
        with zipfile.ZipFile(backup_all(self.data)) as archive:
            self.assertEqual(
                archive.read(str(kept.relative_to(self.data)) + '/emulators/rgds-play/saves/gba/Game.sav'), b'kept'
            )


class EraseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name).resolve() / 'userdata'
        self.removed = self.data / 'profiles/.removed/p1-123'
        (self.removed / 'emulators').mkdir(parents=True)
        (self.removed / 'emulators/Game.sav').write_bytes(b'gone')
        self.kept = self.data / 'profiles/p2/emulators/Game.sav'
        self.kept.parent.mkdir(parents=True)
        self.kept.write_bytes(b'kept')
        self.owner = self.data / 'emulators/rgds-experimental/saves/Game.sav'
        self.owner.parent.mkdir(parents=True)
        self.owner.write_bytes(b'kept')

    def test_only_removed_profiles_go_and_nothing_else(self):
        (self.data / 'profiles/.removed/stray').mkdir()
        self.assertEqual(erase_removed(self.data), 1)
        self.assertFalse(self.removed.exists())
        self.assertTrue((self.data / 'profiles/.removed/stray').is_dir())  # not a name this code ever wrote
        self.assertEqual(self.kept.read_bytes(), b'kept')
        self.assertEqual(self.owner.read_bytes(), b'kept')

    def test_only_the_owner_can_erase(self):
        (self.data / 'profiles/active').write_text('p2\n')
        with self.assertRaises(ValueError):
            erase_removed(self.data)
        self.assertTrue(self.removed.exists())

    def test_not_while_a_game_runs(self):
        from unittest.mock import patch
        with patch('emulators.core_running', return_value=True), self.assertRaises(ValueError):
            erase_removed(self.data)
        self.assertTrue(self.removed.exists())


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class EraseUiTests(Base):
    TO_OWNER = TO_PROFILES + ['Down', 'a']

    def setUp(self):
        super().setUp()
        self.add()
        self.removed = self.data / 'profiles/.removed/p3-1'
        self.removed.mkdir(parents=True)
        (self.removed / 'Game.sav').write_bytes(b'gone')

    def test_it_is_the_owners_last_row_asks_first_and_cancel_is_the_default(self):
        # Name, DS name, Picture, Colour, Language, Games, Games opens, PIN, Erase
        rows = self.TO_OWNER + ['Down'] * 8 + ['a']
        self.assertEqual(self.run_ui(rows, settings=True, gap=90)['page'], 18)  # Page::Confirm
        self.run_ui(rows + ['a'], settings=True, gap=90)  # A on the default
        self.assertTrue(self.removed.exists())
        report = self.run_ui(rows + ['Right', 'a'], settings=True, seconds=4, gap=90)
        self.assertFalse(self.removed.exists())
        self.assertEqual(report['page'], EDIT)

    def test_there_is_no_row_with_nothing_to_erase(self):
        shutil.rmtree(self.removed)
        report = self.run_ui(self.TO_OWNER + ['Down'] * 9, settings=True, gap=90)
        self.assertEqual(report['profile_focus'], 7)  # stops at PIN


class PictureTests(unittest.TestCase):
    def test_every_character_has_its_smooth_drawing(self):
        files = (ROOT / 'src/launcher/profile_pictures.hpp').read_text()
        for name in (
            'fox', 'panda', 'frog', 'penguin', 'owl', 'bear', 'unicorn', 'dino', 'alien', 'astronaut', 'octopus',
            'bunny'
        ):
            self.assertIn(f'"{name}"', files)
            picture = (ROOT / 'assets/profiles' / (name + '.bmp')).read_bytes()
            self.assertEqual((picture[:2], int.from_bytes(picture[28:30], 'little')), (b'BM', 32), name)  # with alpha


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'Requires host C++ and SDL headers')
class StoreTests(unittest.TestCase):
    def test_the_store(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'driver.cpp'
            source.write_text(
                r'''#include "launcher/profile_store.hpp"
#include "launcher/profile_pictures.hpp"
#include <cassert>
using namespace rgds;
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value;
}
int main(int, char** argv) {
    fs::path data = argv[1];
    for (size_t i = 0; i < profile_picture_count; i++) assert(profile_picture(int(i)).size() == 256);
    assert(profile_ds_name("Andreas") == "Andreas");
    assert(profile_ds_name("Maximiliana") == "Maximilian");
    assert(profile_ds_name("ÆØÅæøåÆØÅæøå") == "ÆØÅæøåÆØÅæ");
    assert(profile_ds_name("🙂🙂🙂🙂🙂🙂") == "🙂🙂🙂🙂🙂"); // two units each, and never half of one
    assert(profile_clean_name(" a\nb ") == "a b");
    write(data / "ds.ini", "nickname=RG DS Core\n");
    ProfileStore store;
    store.load(data);
    assert(!store.enabled() && store.profiles().size() == 1 && store.profiles()[0].name == "Owner");
    assert(store.active() == "owner" && !store.active_unclear());
    assert(!fs::exists(data / "profiles"));
    write(data / "ds.ini", "nickname=Andreas\n");
    write(data / "ui.ini", "accent=2\nmenu_music=1\n");
    store.load(data);
    assert(store.profiles()[0].name == "Andreas" && store.profiles()[0].colour == 2);
    const auto id = store.add("  Ola\n", true, 3);
    assert(id == "p1" && store.enabled() && store.profiles().size() == 2);
    const auto& ola = store.profiles()[1];
    assert(ola.id == "p1" && ola.name == "Ola" && ola.restricted && ola.picture == 3 && ola.ds_name == "Ola");
    assert(ola.root == data / "profiles/p1" && ola.colour == 2);
    Preferences look;
    load_ui_preferences(ola.root / "ui.ini", look);
    assert(look.all_games && look.menu_music); // the owner's look, with Games opening every game
    assert(profile_picture_rich(12) && !profile_picture_rich(11) && profile_palette('k') && !profile_palette('.'));
    write(ola.root / "last-game.txt", "Kart Racers\n/roms/nds/Kart Racers.nds\nnds\n");
    write(data / "last-game.txt", "Old Title\n"); // written by an older build: the title only
    store.load(data);
    assert(store.profiles()[1].last_game == "Kart Racers" && store.profiles()[1].last_system == "nds");
    assert(store.profiles()[1].last_path == "/roms/nds/Kart Racers.nds");
    assert(store.profiles()[0].last_game == "Old Title" && store.profiles()[0].last_path.empty());
    assert(fs::exists(data / "profiles/index.ini") && fs::exists(data / "profiles/active"));
    assert(store.active() == "owner" && player_root(data) == data);
    assert(store.select("p1") && store.active() == "p1" && player_root(data) == data / "profiles/p1");
    assert(!store.select("p9") && store.active() == "p1");
    assert(store.select("owner") && player_root(data) == data);
    assert(store.add("", false, 0).empty() && store.add("  ", false, 0).empty());
    assert(store.next_id() == "p2");
    fs::create_directories(data / "profiles/.removed/p7-123");
    assert(store.next_id() == "p8");
    for (int i = 0; i < 4; i++) assert(!store.add("n", false, 0).empty());
    assert(store.profiles().size() == 6 && !store.can_add() && store.add("n", false, 0).empty());
    // In number order, not as text: p9 before p10.
    store.load(data);
    assert(store.profiles()[1].id == "p1" && store.profiles()[3].id == "p9" && store.profiles().back().id == "p11");
    auto changed = store.profiles()[1];
    changed.name = "Sara";
    changed.restricted = false;
    assert(store.save(changed) && store.profiles()[1].name == "Sara" && !store.profiles()[1].restricted);
    auto owner = store.profiles()[0];
    owner.name = "Dad";
    owner.picture = 4;
    assert(store.save(owner) && store.profiles()[0].name == "Dad" && store.profiles()[0].picture == 4);
    assert(store.ask_at_boot() && store.set_ask_at_boot(false) && !store.ask_at_boot());
    store.load(data);
    assert(!store.ask_at_boot());
    assert(profile_fields(data / "profiles/index.ini")["owner_name"] == "Dad");
    write(data / "profiles/active", "p99\n");
    store.load(data);
    assert(store.active_unclear());
    return 0;
}
'''
            )
            flags = subprocess.run(['sdl2-config', '--cflags'], capture_output=True, text=True,
                                   check=True).stdout.split()
            binary = root / 'driver'
            subprocess.run(
                ['clang++', '-std=c++17', '-pthread', *flags, '-I' + str(ROOT / 'src'),
                 str(source), '-o',
                 str(binary)],
                check=True,
                capture_output=True
            )
            (root / 'data').mkdir()
            subprocess.run([str(binary), str(root / 'data')], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
