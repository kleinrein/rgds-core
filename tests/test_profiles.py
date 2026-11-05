"""Profiles: whose files a launch uses, and that a game in one profile never touches another's saves (phase 1 of
docs/PROFILES_PLAN.md). The stand-in engines write where the launch plan tells them to, as the real ones do."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import controls
import emulators
from library import game_key, playtime, read_fields, record_playtime, write_fields
import play_options
import profiles
from profiles import ProfileError, check_plan, device_root, is_owner, look_root, player_root, player_roots
import quick_resume
from save_guard import protect
from save_library import SaveLibrary, backup_all

BINARY = ROOT / 'build/host/rgds-launcher'
PLAYER_FILES = ('library', 'emulators', 'resume', 'save-history', 'system-defaults', 'cache/disks')
ENGINE = r'''#!/bin/sh
# Writes its save and its resume point where the plan says.
while [ $# -gt 0 ]; do
    case "$1" in --data) DATA=$2 ;; --config) CONFIG=$2 ;; esac
    shift
done
if [ -n "$CONFIG" ]; then
    SAVES=$(sed -n 's/^saves *= *//p' "$CONFIG" | head -n 1)
    mkdir -p "$SAVES" && echo "$MARK" > "$SAVES/Game.sav"
else
    mkdir -p "$DATA/saves/gba" "$DATA/PSP/SAVEDATA" && echo "$MARK" > "$DATA/saves/gba/Game.sav"
    echo "$MARK" > "$DATA/PSP/SAVEDATA/Game.bin"
fi
mkdir -p "$(dirname "$RGDS_AUTOSTATE_FILE")" && echo "$MARK" > "$RGDS_AUTOSTATE_FILE"
'''


def hashes(root, folders=PLAYER_FILES):
    found = {}
    for folder in folders:
        for path in sorted((root / folder).rglob('*')):
            if path.is_file(): found[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ('preferences.ini', 'ui.ini', 'ds.ini', 'pictochat.ini'):
        if (root / name).is_file(): found[name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
    return found


class Tree(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / 'userdata'
        self.data.mkdir()
        self.app = self.root / 'app'
        self.roms = self.root / 'roms'
        self.roms.mkdir()

    def add_profile(self, name='p1', active=True):
        folder = self.data / 'profiles' / name
        folder.mkdir(parents=True)
        if active: (self.data / 'profiles/active').write_text(name + '\n')
        return folder

    def rom(self, name='Game.gba'):
        path = self.roms / name
        path.write_bytes(bytes(12) + b'ABCD' + bytes(0x200))
        return path

    def binary(self, core):
        path = self.app / 'emulators' / core / core
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(ENGINE)
        path.chmod(0o755)
        hook = self.app / 'emulators/hooks/librgdsbridge.so'
        hook.parent.mkdir(parents=True, exist_ok=True)
        hook.write_bytes(b'bridge')
        return path


class PlayerRootTests(Tree):
    def test_without_profiles_everything_is_where_it_always_was(self):
        self.assertEqual(player_root(self.data), self.data)
        self.assertTrue(is_owner(self.data))
        self.assertEqual(player_roots(self.data), [self.data])

    def test_a_profile_is_its_folder_and_the_owner_is_userdata(self):
        kid = self.add_profile()
        self.assertEqual(player_root(self.data), kid)
        self.assertFalse(is_owner(kid))
        self.assertEqual(device_root(kid), self.data)
        self.assertEqual(player_root(kid), kid)
        (self.data / 'profiles/active').write_text('owner\n')
        self.assertEqual(player_root(self.data), self.data)
        self.assertEqual(player_root(kid), self.data)

    def test_an_unclear_profile_never_falls_back_to_the_owners_saves(self):
        self.add_profile()
        for text in ('p9', '../..', '/etc', 'p1/../..', ''):
            with self.subTest(active=text):
                (self.data / 'profiles/active').write_text(text)
                with self.assertRaises(ProfileError):
                    player_root(self.data)
                self.assertEqual(look_root(self.data), self.data)
        (self.data / 'profiles/active').unlink()
        (self.data / 'profiles/index.ini').write_text('schema=1\n')
        with self.assertRaises(ProfileError):
            player_root(self.data)

    def test_a_folder_named_like_profiles_elsewhere_is_not_one(self):
        other = self.root / 'profiles/game'
        other.mkdir(parents=True)
        self.assertEqual(device_root(other), other)

    def test_every_players_folder_is_listed(self):
        kid = self.add_profile()
        removed = self.data / 'profiles/.removed/p2-123'
        removed.mkdir(parents=True)
        (self.data / 'profiles/stray').mkdir()
        self.assertEqual(player_roots(self.data), [self.data, kid, removed])


class PlanTests(Tree):
    SYSTEMS = (
        ('Game.nds', 'rgds-experimental'), ('Game.gba', 'rgds-play'), ('Game.gb', 'rgds-play'),
        ('Game.gbc', 'rgds-play'), ('Game.nes', 'rgds-play'), ('Game.sfc', 'rgds-play'), ('Game.md', 'rgds-play'),
        ('Game.sms', 'rgds-play'), ('Game.cue', 'rgds-play'), ('Game.z64', 'rgds-play'), ('Game.tzx', 'rgds-play'),
        ('Game.d64', 'rgds-play'), ('Game.mgw', 'rgds-play'), ('psp/Game.iso', 'rgds-play'), ('Game.cci', 'rgds-play')
    )

    def plan(self, name, core):
        (self.roms / name).parent.mkdir(exist_ok=True)
        rom = self.rom(name)
        self.binary('rgds-experimental')
        self.binary('rgds-play')
        engine = self.root / 'tanuki'
        engine.mkdir(exist_ok=True)
        (engine / 'run.sh').write_text('exit 0\n')
        real = emulators.tanuki_plan
        with patch.object(play_options, 'libretro_core', return_value=self.root / 'core.so'), \
                patch.object(emulators, 'tanuki_plan', lambda *a, **k: real(*a, engine=engine, **k)):
            return emulators.launch_plan(core, self.app, self.data, rom)

    def owner_paths(self, plan):
        """Every path in a plan that lies in the owner's own files."""
        found = []
        for key, text in profiles.plan_paths(plan):
            path = Path(text)
            if not path.is_absolute() or not path.is_relative_to(self.data): continue
            relative = path.relative_to(self.data)
            if not relative.parts or relative.parts[0] == 'profiles' or any(
                relative.is_relative_to(s) for s in profiles.SHARED_FILES
            ):
                continue
            if any(relative.is_relative_to(o) for o in profiles.OWNERS_FILES): found.append((key, text))
        return found

    def test_no_system_is_given_the_owners_files_on_a_childs_profile(self):
        kid = self.add_profile()
        for name, core in self.SYSTEMS:
            with self.subTest(game=name):
                plan = self.plan(name, core)
                self.assertEqual(self.owner_paths(plan), [])
                check_plan(plan, self.data, kid, core)
                self.assertTrue(Path(plan['env']['HOME']).is_relative_to(kid), plan['env']['HOME'])
                self.assertTrue(Path(plan['env']['RGDS_AUTOSTATE_FILE']).is_relative_to(kid))
                self.assertEqual(plan['env']['RGDS_UI_FILE'], str(kid / 'ui.ini'))
                self.assertEqual(plan['env']['RGDS_VOLUME_DATA'], str(self.data))
                self.assertEqual(plan['env']['RGDS_CONTROL_FILE'], str(self.data / 'session/emulator.control'))

    def test_the_owner_keeps_the_same_paths_with_and_without_profiles(self):
        for name, core in self.SYSTEMS:
            with self.subTest(game=name):
                before = self.plan(name, core)
                self.assertTrue(Path(before['env']['RGDS_AUTOSTATE_FILE']).is_relative_to(self.data / 'library'))
                self.assertNotIn('--no-save-import', before['command'])
                (self.data / 'profiles').mkdir(exist_ok=True)
                (self.data / 'profiles/active').write_text('owner\n')
                after = self.plan(name, core)
                self.assertEqual(before['command'], after['command'])
                self.assertEqual(before['env'].get('HOME'), after['env'].get('HOME'))
                shutil.rmtree(self.data / 'profiles')

    def test_box_art_and_frames_stay_shared_and_a_child_gets_no_save_from_beside_the_game(self):
        kid = self.add_profile()
        command = self.plan('Game.gba', 'rgds-play')['command']
        rom = self.roms / 'Game.gba'
        self.assertEqual(command[command.index('--art') + 1], str(self.data / 'library' / game_key(rom)))
        self.assertEqual(command[command.index('--frames') + 1], str(self.data / 'frames/gba'))
        self.assertEqual(command[command.index('--data') + 1], str(kid / 'emulators/rgds-play'))
        self.assertEqual(command[command.index('--preferences') + 1], str(kid / 'preferences.ini'))
        self.assertEqual(command[command.index('--shared-cheats') + 1], str(self.data / 'emulators/rgds-play'))
        self.assertIn('--no-save-import', command)

    def test_a_childs_ds_config_saves_in_its_folder_with_its_name(self):
        kid = self.add_profile()
        write_fields(self.data / 'ds.ini', {'nickname': 'Andreas'})
        write_fields(kid / 'ds.ini', {'nickname': 'Ola'})
        self.plan('Game.nds', 'rgds-experimental')
        config = (kid / 'emulators/rgds-experimental/config.ini').read_text()
        self.assertIn('nickname = Ola', config)
        self.assertIn('saves = ' + str(kid / 'emulators/rgds-experimental/saves'), config)
        self.assertIn('states = ' + str(kid / 'emulators/rgds-experimental/states'), config)
        self.assertFalse((self.data / 'emulators/rgds-experimental/config.ini').exists())

    def test_a_games_own_name_is_that_players(self):
        kid = self.add_profile()
        rom = self.rom('Game.nds')
        write_fields(kid / 'ds.ini', {'nickname': 'Ola'})
        write_fields(self.data / 'library' / game_key(rom) / 'ds-input.ini', {'nickname': 'NotOla'})
        self.plan('Game.nds', 'rgds-experimental')
        self.assertIn('nickname = Ola', (kid / 'emulators/rgds-experimental/config.ini').read_text())

    def test_the_guard_refuses_a_plan_that_would_mix_saves(self):
        kid = self.add_profile()
        good = {'command': ['engine'], 'cwd': str(kid), 'env': {'HOME': str(kid / 'home')}}
        check_plan(good, self.data, kid, 'rgds-play')
        for bad in (
            {
                'HOME': str(self.data / 'emulators/rgds-play/home')
            },
            {
                'HOME': str(kid),
                'RGDS_AUTOSTATE_FILE': str(self.data / 'library/x/auto-rgds-play.dss')
            },
            {
                'HOME': str(kid),
                'RGDS_STATE_DIR': str(self.data / 'emulators/rgds-play/states')
            },
            {
                'HOME': str(kid),
                'ANY': '/x:' + str(self.data / 'emulators/rgds-play/saves')
            },
            {
                'HOME': str(kid),
                'ANY': str(self.data / 'ui.ini')
            },
        ):
            with self.subTest(env=bad), self.assertRaises(ProfileError):
                check_plan({'command': ['engine'], 'cwd': str(kid), 'env': bad}, self.data, kid, 'rgds-play')
        with self.assertRaises(ProfileError):
            check_plan(
                {
                    'command': ['engine', '--data', str(self.data / 'emulators/rgds-play')],
                    'cwd': '',
                    'env': {}
                }, self.data, kid, 'rgds-play'
            )
        # The owner, and a port (its saves are its own folder, for every profile), are not checked.
        owners = {'command': ['engine'], 'cwd': '', 'env': {'HOME': str(self.data / 'emulators/rgds-play/home')}}
        check_plan(owners, self.data, self.data, 'rgds-play')
        check_plan(owners, self.data, kid, 'portmaster')
        # What is shared by design passes.
        shared = {
            'command': [
                'engine', '--art',
                str(self.data / 'library/x'), '--rom',
                str(self.data / 'cache/roms/a/b.nds')
            ],
            'cwd': '',
            'env': {
                'A': str(self.data / 'session/emulator.control'),
                'B': str(self.data / 'emulators/rgds-experimental/panvk.json'),
                'C': str(self.data / 'emulators/rgds-experimental/cheats/usrcheat.dat'),
                'D': str(self.data / 'controls.ini')
            }
        }
        check_plan(shared, self.data, kid, 'rgds-experimental')


class SeedTests(Tree):
    def test_a_new_profile_gets_the_3ds_lists_and_firmware_but_no_save(self):
        share = self.data / 'emulators/rgds-play/3ds/.local/share/Tanuki3DS'
        (share / 'shaders').mkdir(parents=True)
        (share / 'jit').mkdir()
        (share / 'shaders/mk7.list').write_text('shaders')
        (share / 'jit/mk7.list').write_text('blocks')
        (share / '3ds/sdmc/3ds').mkdir(parents=True)
        (share / '3ds/sdmc/3ds/dspfirm.cdc').write_text('firmware')
        (share / '3ds/savedata/MARIO KART 7').mkdir(parents=True)
        (share / '3ds/savedata/MARIO KART 7/GameData.bin').write_text('the owner\'s save')
        (share / '3ds/extdata/MARIO KART 7').mkdir(parents=True)
        (share / '3ds/extdata/MARIO KART 7/x').write_text('the owner\'s')
        before = hashes(self.data)
        kid = self.add_profile()
        engine = self.root / 'tanuki'
        engine.mkdir()
        (engine / 'run.sh').write_text('exit 0\n')
        rom = self.rom('Game.cci')
        emulators.tanuki_plan(self.data, rom, engine=engine)
        own = kid / 'emulators/rgds-play/3ds/.local/share/Tanuki3DS'
        self.assertEqual((own / 'shaders/mk7.list').read_text(), 'shaders')
        self.assertEqual((own / 'jit/mk7.list').read_text(), 'blocks')
        self.assertEqual((own / '3ds/sdmc/3ds/dspfirm.cdc').read_text(), 'firmware')
        self.assertFalse((own / '3ds/savedata').exists())
        self.assertFalse((own / '3ds/extdata').exists())
        self.assertEqual(hashes(self.data), before)
        # What the profile's own game has added to its lists stays.
        (own / 'shaders/mk7.list').write_text('more shaders')
        emulators.tanuki_plan(self.data, rom, engine=engine)
        self.assertEqual((own / 'shaders/mk7.list').read_text(), 'more shaders')


class SaveToolTests(Tree):
    def test_save_history_stays_in_the_profile_and_a_save_beside_the_game_is_not_imported(self):
        kid = self.add_profile()
        rom = self.rom('Game.nds')
        (self.roms / 'Game.sav').write_bytes(b'the owner\'s save, beside the game')
        ds = 'rgds-experimental'
        protect(kid, ds, rom, rom)
        self.assertFalse((kid / 'emulators' / ds / 'saves/Game.sav').exists())
        self.assertFalse((self.data / 'emulators' / ds / 'saves/Game.sav').exists())
        (kid / 'emulators' / ds / 'saves').mkdir(parents=True)
        (kid / 'emulators' / ds / 'saves/Game.sav').write_bytes(b'kid')
        protect(kid, ds, rom, rom)
        self.assertTrue(any((kid / 'save-history').rglob('Game.sav')))
        self.assertFalse((self.data / 'save-history').exists())
        # The owner still gets it, as on every earlier build.
        protect(self.data, ds, rom, rom)
        self.assertEqual(
            (self.data / 'emulators' / ds / 'saves/Game.sav').read_bytes(), b'the owner\'s save, beside the game'
        )

    def test_playtime_and_the_game_view_are_the_players(self):
        kid = self.add_profile()
        rom = self.rom()
        record_playtime(kid, rom, 60, 1, '2026-11-05T08:00:00+00:00', 60)
        self.assertEqual(playtime(kid, rom)[0], 60)
        self.assertEqual(playtime(self.data, rom)[0], 0)
        self.assertTrue((kid / 'library' / game_key(rom) / 'sessions.tsv').is_file())
        play_options.save_view(kid, rom, 1, 2, 'black')
        self.assertEqual(play_options.view(kid, rom)['fit'], 'integer')
        self.assertNotEqual(play_options.view(self.data, rom)['fit'], 'integer')

    def test_a_picture_frame_is_the_handhelds_whichever_root_asks(self):
        kid = self.add_profile()
        rom = self.rom()
        (self.data / 'frames/gba').mkdir(parents=True)
        (self.data / 'frames/gba/mine.png').write_bytes(b'png')
        self.assertTrue(play_options.frame_known(kid, rom, 'mine'))
        self.assertEqual(play_options.frames_folder(kid, rom), self.data / 'frames/gba')

    def test_a_zipped_c64_games_disks_hold_high_scores_so_they_are_the_players(self):
        kid = self.add_profile()
        (self.roms / 'c64').mkdir()
        rom = self.roms / 'c64/Game.zip'
        with zipfile.ZipFile(rom, 'w') as archive:
            archive.writestr('Game [Side A].d64', b'a')
            archive.writestr('Game [Side B].d64', b'b')
        resolved = emulators.resolve_rom(rom, self.data, 'rgds-play')
        self.assertTrue(resolved.is_relative_to(kid / 'cache/disks'), resolved)
        self.assertFalse((self.data / 'cache/disks').exists())

    def test_controls_buttons_are_the_handhelds_and_a_games_own_the_players(self):
        kid = self.add_profile()
        rom = self.rom()
        for root in (self.data, kid):
            shared, own = controls.paths(root, rom)
            self.assertEqual(shared, self.data / 'controls.ini')
            self.assertEqual(own, kid / 'library' / game_key(rom) / 'controls.ini')

    def test_slots_are_the_players_and_exports_land_in_the_shared_folder(self):
        kid = self.add_profile()
        rom = self.rom('Game.nds')
        vault = SaveLibrary(player_root(self.data), rom, 'rgds-experimental', 'build')
        self.assertTrue(vault.root.is_relative_to(kid / 'library'))
        self.assertFalse((self.data / 'library').exists())
        source = Path(emulators.__file__).parent.joinpath('save_library.py').read_text()
        self.assertIn("device_root(self.data) / 'exports'", source)

    def test_resume_after_power_off_keeps_the_ticket_with_the_player(self):
        kid = self.add_profile()
        rom = self.rom('Game.nds')
        write_fields(self.data / 'experience.ini', {'quick_resume': '1'})
        state = kid / 'library' / game_key(rom) / 'auto-rgds-experimental.dss'
        state.parent.mkdir(parents=True)
        state.write_bytes(b'state')
        quick_resume.record_exit(kid, rom, 'rgds-experimental', 0)
        self.assertTrue((kid / 'resume/candidate.json').is_file())
        self.assertTrue(quick_resume.arm(kid))
        self.assertTrue((kid / 'resume/armed.json').is_file())
        self.assertFalse((self.data / 'resume').exists())
        self.assertTrue(quick_resume.consume(kid))
        self.assertEqual(read_fields(self.data / 'session/resume-launch.ini')['rom'], str(rom))

    def test_back_up_all_saves_includes_every_profile(self):
        kid = self.add_profile()
        removed = self.data / 'profiles/.removed/p2-1'
        for root, text in ((self.data, b'owner'), (kid, b'kid'), (removed, b'gone')):
            save = root / 'emulators/rgds-experimental/saves/Game.sav'
            save.parent.mkdir(parents=True)
            save.write_bytes(text)
            psp = root / 'emulators/rgds-play/PSP/SAVEDATA/GAME/x.bin'
            psp.parent.mkdir(parents=True)
            psp.write_bytes(text)
            state = root / 'library/key/auto-rgds-play.dss'
            state.parent.mkdir(parents=True)
            state.write_bytes(text)
            (root / 'library/key/box.bmp').write_bytes(b'art is not a save')
        with zipfile.ZipFile(backup_all(self.data)) as archive:
            names = set(archive.namelist())
            self.assertEqual(archive.read('emulators/rgds-experimental/saves/Game.sav'), b'owner')
            self.assertEqual(archive.read('profiles/p1/emulators/rgds-experimental/saves/Game.sav'), b'kid')
            self.assertEqual(archive.read('profiles/.removed/p2-1/emulators/rgds-experimental/saves/Game.sav'), b'gone')
            self.assertEqual(archive.read('profiles/p1/library/key/auto-rgds-play.dss'), b'kid')
            self.assertEqual(archive.read('profiles/p1/emulators/rgds-play/PSP/SAVEDATA/GAME/x.bin'), b'kid')
            self.assertEqual(json.loads(archive.read('backup.json'))['schema'], 1)
        self.assertNotIn('profiles/p1/library/key/box.bmp', names)


class Session:
    """The in-game menu, without its helper or windows."""
    def __init__(self, child, app, data, core, rom, environment, inputs=None):
        self.child, self.restart = child, False

    def poll(self):
        pass

    def close(self):
        pass

    def played_seconds(self):
        return 7

    def completed_switch(self):
        return None


class LaunchTests(Tree):
    """Real launches through emulators._run_once with stand-in engines."""
    def launch(self, core, rom, mark):
        handler = signal.getsignal(signal.SIGTERM)
        engine = self.root / 'tanuki'
        engine.mkdir(exist_ok=True)
        (engine / 'run.sh').write_text(
            'S="$HOME/.local/share/Tanuki3DS/3ds/savedata/Game"\n'
            'mkdir -p "$S" && echo "$MARK" > "$S/GameData.bin"\n'
            'mkdir -p "$(dirname "$RGDS_AUTOSTATE_FILE")" && echo "$MARK" > "$RGDS_AUTOSTATE_FILE"\n'
        )
        self.binary('rgds-experimental')
        self.binary('rgds-play')
        menu_input = MagicMock(descriptors=[])
        real = emulators.tanuki_plan
        with patch.object(emulators, 'GameMenu', Session), patch.object(emulators, 'VolumeWatch'), \
                patch.object(emulators, 'MenuInput', return_value=menu_input), \
                patch.object(emulators, 'release_launcher_display'), \
                patch.object(play_options, 'libretro_core', return_value=self.root / 'core.so'), \
                patch.object(emulators, 'tanuki_plan', lambda *a, **k: real(*a, engine=engine, **k)), \
                patch.dict(os.environ, MARK=mark):
            try:
                result = emulators._run_once(core, self.app, self.data, rom)
            finally:
                signal.signal(signal.SIGTERM, handler)
        self.assertEqual(result, 0)
        self.assertFalse((self.data / 'session/core.json').exists())

    def everything(self, name):
        rom = self.rom(name)
        return rom, ('rgds-experimental' if name.endswith('.nds') else 'rgds-play')

    def test_a_game_on_a_childs_profile_never_touches_the_owners_saves(self):
        # The owner has played all of them, and left a save beside each game.
        for name, saved in (
            ('Game.nds',
             'emulators/rgds-experimental/saves/Game.sav'), ('Game.gba', 'emulators/rgds-play/saves/gba/Game.sav'),
            ('Game.cci', 'emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/savedata/Game/GameData.bin')
        ):
            rom, core = self.everything(name)
            self.launch(core, rom, 'owner')
            self.assertEqual((self.data / saved).read_text().strip(), 'owner')
            self.assertEqual(
                (self.data / 'library' / game_key(rom) / ('auto-' + core + '.dss')).read_text().strip(), 'owner'
            )
            (self.roms / 'Game.sav').write_bytes(b'beside')
        before = hashes(self.data)
        kid = self.add_profile()
        for name, saved in (
            ('Game.nds',
             'emulators/rgds-experimental/saves/Game.sav'), ('Game.gba', 'emulators/rgds-play/saves/gba/Game.sav'),
            ('Game.cci', 'emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/savedata/Game/GameData.bin')
        ):
            with self.subTest(game=name):
                rom, core = self.everything(name)
                self.launch(core, rom, 'kid')
                self.assertEqual((kid / saved).read_text().strip(), 'kid')
                self.assertEqual(
                    (kid / 'library' / game_key(rom) / ('auto-' + core + '.dss')).read_text().strip(), 'kid'
                )
                self.assertEqual(playtime(kid, rom)[1], 1)
                self.assertEqual(hashes(self.data), before)
        # And back: the owner's next game is the owner's, and the child's saves are left alone.
        kids = hashes(kid)
        (self.data / 'profiles/active').write_text('owner\n')
        rom, core = self.everything('Game.nds')
        self.launch(core, rom, 'owner again')
        self.assertEqual((self.data / 'emulators/rgds-experimental/saves/Game.sav').read_text().strip(), 'owner again')
        self.assertEqual(hashes(kid), kids)

    def test_a_save_beside_the_game_is_not_a_childs_first_save(self):
        kid = self.add_profile()
        rom = self.rom('Game.nds')
        (self.roms / 'Game.sav').write_bytes(b'beside')
        with patch.object(emulators, 'launch_plan', side_effect=ValueError('stop here')):
            with self.assertRaises(ValueError):
                emulators._run_once('rgds-experimental', self.app, self.data, rom)
        self.assertFalse((kid / 'emulators/rgds-experimental/saves/Game.sav').exists())

    def test_no_game_starts_while_the_active_profile_is_unclear(self):
        self.add_profile()
        (self.data / 'profiles/active').write_text('p9')
        rom = self.rom()
        self.binary('rgds-play')
        with self.assertRaises(ProfileError):
            emulators._run_once('rgds-play', self.app, self.data, rom)
        self.assertEqual(hashes(self.data), {})

    def test_a_plan_that_breaks_the_rule_starts_nothing(self):
        self.add_profile()
        rom = self.rom()
        bad = {'command': ['/bin/sh', '-c', 'touch "$MARK"'], 'cwd': str(self.root), 'env': {
            'HOME': str(self.data / 'emulators/rgds-play/home'),
            'MARK': str(self.root / 'started')
        }}  # yapf: disable
        with patch.object(emulators, 'launch_plan', return_value=bad):
            with self.assertRaises(ProfileError):
                emulators._run_once('rgds-play', self.app, self.data, rom)
        self.assertFalse((self.root / 'started').exists())


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'Requires host C++ and SDL headers')
class LauncherCacheTests(unittest.TestCase):
    def test_the_launcher_reads_the_same_folders(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'driver.cpp'
            source.write_text(
                r'''#include "launcher/library_cache.hpp"
#include <cassert>
using namespace rgds;
void write(fs::path path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value;
}
int main(int, char** argv) {
    fs::path data = argv[1];
    bool unclear = true;
    assert(player_root(data, &unclear) == data && !unclear);
    Game game{"Fixture", data / "Fixture.nds", false};
    const auto key = game_key(game.path);
    write(data / "library" / key / "info.ini", "title=About the game\n");
    write(data / "library" / key / "stats.ini", "seconds=100\nsessions=1\n");
    write(data / "library" / key / "game.ini", "favourite=1\n");
    {
        LibraryCache owner(data);
        assert(owner.stats(game).seconds == 100 && owner.options(game).favourite);
        assert(owner.folder(game) == owner.player_folder(game));
    }
    write(data / "profiles/p1/library" / key / "stats.ini", "seconds=7\nsessions=2\n");
    write(data / "profiles/active", "p1\n");
    assert(player_root(data, &unclear) == data / "profiles/p1" && !unclear);
    LibraryCache kid(data);
    assert(kid.folder(game) == data / "library" / key);
    assert(kid.player_folder(game) == data / "profiles/p1/library" / key);
    assert(kid.metadata(game).info["title"] == "About the game");
    assert(kid.stats(game).seconds == 7);
    assert(!kid.options(game).favourite);
    GameOptions next;
    next.favourite = true;
    save_game_options(kid.player_folder(game) / "game.ini", next);
    assert(kid.reload_options(game).favourite);
    assert(fields(data / "library" / key / "stats.ini")["seconds"] == "100");
    write(data / "profiles/active", "p9\n");
    assert(player_root(data, &unclear) == data && unclear);
    write(data / "profiles/active", "../..\n");
    assert(player_root(data, &unclear) == data && unclear);
    write(data / "profiles/active", "owner\n");
    assert(player_root(data, &unclear) == data && !unclear);
    fs::remove(data / "profiles/active");
    assert(player_root(data, &unclear) == data && !unclear);
    write(data / "profiles/index.ini", "schema=1\n");
    assert(player_root(data, &unclear) == data && unclear);
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


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class LauncherLookTests(unittest.TestCase):
    def report(self, data):
        (data / 'input.txt').write_text('')
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(data), '--input-script',
                str(data / 'input.txt'), '--cache-report',
                str(data / 'report.json'), '--seconds', '1'
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads((data / 'report.json').read_text())

    def test_the_look_is_the_players_and_system_settings_the_handhelds(self):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder).resolve()
            (data / 'setup.ini').write_text('done=1\n')
            (data / 'ui.ini').write_text('menu_music=1\n')
            (data / 'system.ini').write_text('schema=1\npower=2\n')
            self.assertEqual(self.report(data)['menu_music'], 1)
            kid = data / 'profiles/p1'
            kid.mkdir(parents=True)
            (data / 'profiles/active').write_text('p1\n')
            (kid / 'ui.ini').write_text('menu_music=0\n')
            self.assertEqual(self.report(data)['menu_music'], 0)
            self.assertEqual((data / 'ui.ini').read_text(), 'menu_music=1\n')
            self.assertFalse((kid / 'system.ini').exists())


if __name__ == '__main__':
    unittest.main()
