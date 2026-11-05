"""A handheld updating from 0.8.0 keeps every save exactly where it is, whatever it does with profiles afterwards. A gate
for releases: tools/own_os_release.py runs it before anything goes out."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import io
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import emulators
from library import game_key
import play_options
from profiles import player_root
from save_library import backup_all

RELEASE = 'v0.8.0'
ROMS = ('Game.nds', 'Game.gba', 'Game.gb', 'Game.sfc', 'Game.z64')
PATHS = r'''
import json, sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
import emulators, play_options
app, data, roms = Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
found = {}
with patch.object(play_options, 'libretro_core', return_value=Path('/core.so')):
    for name in sys.argv[5:]:
        core = 'rgds-experimental' if name.endswith('.nds') else 'rgds-play'
        plan = emulators.launch_plan(core, app, data, roms / name, prepare=False)
        values = [str(part) for part in plan['command']] + [plan['cwd']]
        for key, value in plan['env'].items():
            values += [key + '=' + part for part in str(value).split(':') if part.startswith(str(data))]
        found[name] = sorted({value for value in values if str(data) in value})
print(json.dumps(found))
'''


def fill_as_on_0_8_0(data, roms):
    """Files in the layout read from a handheld on 0.8.0, with contents that differ from one another."""
    for name in ROMS:
        (roms / name).write_bytes(bytes(12) + b'ABCD' + name.encode() + bytes(0x200))
    keys = {name: game_key(roms / name) for name in ROMS}
    files = [
        'preferences.ini', 'ui.ini', 'system.ini', 'volume.ini', 'experience.ini', 'ds.ini', 'pictochat.ini',
        'setup.ini', 'boot.ini', 'performance.ini', 'controls.ini', 'system-defaults/gba.ini',
        'emulators/rgds-experimental/saves/Game.sav', 'emulators/rgds-experimental/states/ABCD.auto.dss',
        'emulators/rgds-experimental/states/ABCD.0.dss', 'emulators/rgds-experimental/config.ini',
        'emulators/rgds-experimental/cheats/usrcheat.dat', 'emulators/rgds-experimental/panvk.json',
        'emulators/rgds-experimental/home/.config/dsperate/games/Game.ini', 'emulators/rgds-play/saves/gba/Game.sav',
        'emulators/rgds-play/saves/gba/backups/Game.sav.1', 'emulators/rgds-play/saves/gb/Game.sav',
        'emulators/rgds-play/saves/snes/Game.sav', 'emulators/rgds-play/saves/n64/Game.sav',
        'emulators/rgds-play/saves/psx/Game.sav', 'emulators/rgds-play/states/Game_8.dss',
        'emulators/rgds-play/pcsx-card2.mcd', 'emulators/rgds-play/second-screen.ini',
        'emulators/rgds-play/PSP/SAVEDATA/ULES00262/PARAM.SFO', 'emulators/rgds-play/PSP/PPSSPP_STATE/a.ppst',
        'emulators/rgds-play/3ds/.local/share/Tanuki3DS/ctremu.ini',
        'emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/savedata/MARIO KART 7/GameData.bin',
        'emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/extdata/MARIO KART 7/x',
        'emulators/rgds-play/3ds/.local/share/Tanuki3DS/shaders/mk7.list', 'resume/candidate.json', 'exports/a.zip',
        'frames/gba/mine.png'
    ]
    for key in keys.values():
        files += [
            'library/' + key + '/' + name for name in (
                'box.bmp', 'info.ini', 'info-status.ini', 'stats.ini', 'sessions.tsv', 'game.ini', 'play.ini',
                'auto-rgds-play.dss', 'auto-rgds-experimental.dss', 'slots/rgds-play/catalog.json',
                'slots/rgds-play/index.ini', 'slots/rgds-play/objects/abc/checkpoint.dss'
            )
        ]
    for name in files:
        path = data / name
        path.parent.mkdir(parents=True, exist_ok=True)
        # DSCore's two are section files; the rest are key=value
        sections = name.endswith(('config.ini', 'games/Game.ini'))
        path.write_text(('[emu]\nautosave = true\n' if sections else 'schema=1\n') + '# from 0.8.0: ' + name + '\n')


def snapshot(data):
    return {
        str(path.relative_to(data)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(data.rglob('*')) if path.is_file()
    }


class UpgradeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / 'userdata'
        self.roms = self.root / 'roms'
        self.app = self.root / 'app'
        self.data.mkdir()
        self.roms.mkdir()
        for core in ('rgds-experimental', 'rgds-play'):
            binary = self.app / 'emulators' / core / core
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\n')
            binary.chmod(0o755)
        hook = self.app / 'emulators/hooks/librgdsbridge.so'
        hook.parent.mkdir(parents=True)
        hook.write_bytes(b'bridge')
        fill_as_on_0_8_0(self.data, self.roms)

    def plans(self):
        with patch.object(play_options, 'libretro_core', return_value=Path('/core.so')):
            return [
                emulators.launch_plan(
                    'rgds-experimental' if name.endswith('.nds') else 'rgds-play', self.app, self.data, self.roms / name
                ) for name in ROMS
            ]

    def test_updating_changes_nothing_on_disk_and_adds_no_profiles_folder(self):
        before = snapshot(self.data)
        self.assertEqual(player_root(self.data), self.data)
        self.assertFalse((self.data / 'profiles').exists())
        self.assertEqual(snapshot(self.data), before)

    def test_preparing_a_launch_as_the_owner_rewrites_only_what_every_launch_rewrites(self):
        before = snapshot(self.data)
        self.plans()
        after = snapshot(self.data)
        missing = set(before) - set(after)
        self.assertEqual(missing, set())
        changed = {name for name in before if before[name] != after[name]}
        # Regenerated at every launch since long before profiles; neither holds progress.
        allowed = {
            'emulators/rgds-experimental/config.ini', 'emulators/rgds-experimental/home/.config/dsperate/games/Game.ini'
        }
        self.assertEqual(changed - allowed - {'emulators/rgds-play/states/Game_8.dss'}, set())
        # Auto-load puts the resume point in the engine's slot, and the slot's old contents are kept.
        kept = list((self.data / 'library').glob('*/manual-state-backups/rgds-play/Game_8-*.dss'))
        self.assertTrue(any('# from 0.8.0: emulators/rgds-play/states/Game_8.dss' in k.read_text() for k in kept))

    def test_creating_and_removing_a_profile_leaves_every_file_of_the_owners(self):
        before = snapshot(self.data)
        kid = self.data / 'profiles/p1'
        kid.mkdir(parents=True)
        (self.data / 'profiles/active').write_text('p1\n')
        self.plans()
        saved = kid / 'emulators/rgds-experimental/saves/Game.sav'
        saved.parent.mkdir(parents=True, exist_ok=True)
        saved.write_bytes(b'the child\'s')
        after = snapshot(self.data)
        for name, digest in before.items():
            self.assertEqual(after.get(name), digest, name)
        shutil.rmtree(self.data / 'profiles')
        self.assertEqual(snapshot(self.data), before)
        self.assertEqual(player_root(self.data), self.data)

    def test_a_backup_still_names_each_save_as_it_did(self):
        with zipfile.ZipFile(backup_all(self.data)) as archive:
            names = set(archive.namelist())
        self.assertIn('emulators/rgds-experimental/saves/Game.sav', names)
        self.assertIn('emulators/rgds-play/saves/gba/Game.sav', names)
        self.assertIn('emulators/rgds-play/PSP/SAVEDATA/ULES00262/PARAM.SFO', names)
        self.assertIn('emulators/rgds-play/3ds/.local/share/Tanuki3DS/3ds/savedata/MARIO KART 7/GameData.bin', names)
        self.assertFalse(any(name.startswith('profiles/') for name in names))

    def test_a_build_that_knows_nothing_of_profiles_reads_the_owners_tree_as_before(self):
        """The fallback after a failed update: the previous build, on this data, with a profiles folder in it."""
        old = self.release_runtime()
        (self.data / 'profiles/p1/emulators/rgds-play/saves/gba').mkdir(parents=True)
        (self.data / 'profiles/active').write_text('p1\n')
        paths = self.paths_with(old)
        self.assertTrue(paths)
        for name, values in paths.items():
            self.assertFalse([value for value in values if 'profiles' in value], name)

    def test_every_save_path_is_the_one_0_8_0_uses(self):
        old = self.paths_with(self.release_runtime())
        new = self.paths_with(ROOT / 'runtime')
        for name in ROMS:
            with self.subTest(game=name):
                added = set(new[name]) - set(old[name])
                removed = set(old[name]) - set(new[name])
                # The bridge now reads ui.ini through its own variable; volume.ini stays where it was.
                self.assertEqual(added, {'RGDS_UI_FILE=' + str(self.data / 'ui.ini')})
                self.assertEqual(removed, set())

    def release_runtime(self):
        archive = subprocess.run(
            ['git', 'archive', RELEASE, 'runtime', 'assets/shaders'], cwd=ROOT, capture_output=True
        )
        if archive.returncode: self.skipTest('The ' + RELEASE + ' tag is not in this checkout')
        folder = self.root / 'release'
        folder.mkdir(exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(folder)
        return folder / 'runtime'

    def paths_with(self, runtime):
        result = subprocess.run(
            [sys.executable, '-B', '-c', PATHS,
             str(runtime),
             str(self.app),
             str(self.data),
             str(self.roms), *ROMS],
            capture_output=True,
            text=True,
            env={
                key: value
                for key, value in os.environ.items() if key != 'PYTHONPATH'
            },
            cwd=str(self.root),
            timeout=60
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)


if __name__ == '__main__':
    unittest.main()
