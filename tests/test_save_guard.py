import re, sys, tempfile, unittest, zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
import save_guard
from library import game_key
from save_guard import DS, TANUKI, copies, import_old_ds_save, protect, restore


class SaveGuardTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.rom = self.root / 'roms/Game.nds'
        self.rom.parent.mkdir()
        self.rom.write_bytes(b'rom')
        self.sav = self.data / 'emulators' / DS / 'saves/Game.sav'
        self.key = game_key(self.rom)

    def write(self, path, bytes_):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes_)
        return path

    def test_each_different_save_is_copied_once_and_the_oldest_goes_last(self):
        self.write(self.sav, b'one')
        protect(self.data, DS, self.rom)
        protect(self.data, DS, self.rom)
        history = self.data / 'save-history' / self.key
        self.assertEqual(len(copies(history)), 1)
        for n in range(save_guard.KEEP_SAVES + 3):
            self.sav.write_bytes(b'save %d' % n)
            protect(self.data, DS, self.rom)
        kept = copies(history)
        self.assertEqual(len(kept), save_guard.KEEP_SAVES)
        newest = kept[-1] / self.sav.relative_to(self.data)
        self.assertEqual(newest.read_bytes(), b'save %d' % (save_guard.KEEP_SAVES + 2))

    def test_a_save_wiped_later_can_be_restored_and_the_restore_undone(self):
        self.write(self.sav, b'hours of play')
        protect(self.data, DS, self.rom)
        self.sav.write_bytes(b'blank')
        restore(self.data, copies(self.data / 'save-history' / self.key)[0])
        self.assertEqual(self.sav.read_bytes(), b'hours of play')
        kept = copies(self.data / 'save-history' / self.key)
        self.assertEqual((kept[-1] / self.sav.relative_to(self.data)).read_bytes(), b'blank')

    def test_an_empty_save_never_replaces_a_copy(self):
        self.write(self.sav, b'')
        protect(self.data, DS, self.rom)
        self.assertEqual(copies(self.data / 'save-history' / self.key), [])

    def test_the_resume_point_is_kept_apart_from_the_save(self):
        state = self.write(self.data / 'library' / self.key / f'auto-{DS}.dss', b'state one')
        protect(self.data, DS, self.rom)
        self.assertEqual(state.read_bytes(), b'state one')
        history = self.data / 'save-history' / self.key / 'resume'
        kept = copies(history)
        self.assertEqual(len(kept), 1)
        self.assertEqual((kept[0] / state.relative_to(self.data)).read_bytes(), b'state one')

    def test_the_resume_point_an_update_found_is_never_pushed_out(self):
        state = self.write(self.data / 'library' / self.key / f'auto-{DS}.dss', b'from the old engine')
        protect(self.data, DS, self.rom, build='old')
        protect(self.data, DS, self.rom, build='new')
        history = self.data / 'save-history' / self.key / 'resume'
        for n in range(save_guard.KEEP_STATES + 3):
            state.write_bytes(b'new engine %d' % n)
            protect(self.data, DS, self.rom, build='new')
        kept = copies(history / 'before-new')
        self.assertEqual(len(kept), 1)
        self.assertEqual((kept[0] / state.relative_to(self.data)).read_bytes(), b'from the old engine')
        self.assertEqual(len(copies(history)), save_guard.KEEP_STATES)

    def test_only_the_last_few_updates_keep_a_resume_point(self):
        state = self.write(self.data / 'library' / self.key / f'auto-{DS}.dss', b'state')
        protect(self.data, DS, self.rom, build='0')
        for n in range(1, save_guard.KEEP_UPDATES + 3):
            state.write_bytes(b'state %d' % n)
            protect(self.data, DS, self.rom, build=str(n))
        history = self.data / 'save-history' / self.key / 'resume'
        self.assertEqual(len(list(history.glob('before-*'))), save_guard.KEEP_UPDATES)

    def test_3ds_saves_and_extra_data_are_copied(self):
        rom = self.root / 'roms/3ds/Mario.cci'
        self.write(rom, b'rom')
        saved = self.write(self.data / TANUKI / 'savedata/MARIO/save.dat', b'progress')
        extra = self.write(self.data / TANUKI / 'extdata/MARIO/ghosts.dat', b'ghosts')
        protect(self.data, 'rgds-play', rom)
        kept = copies(self.data / 'save-history/3ds')
        self.assertEqual(len(kept), 1)
        self.assertEqual((kept[0] / saved.relative_to(self.data)).read_bytes(), b'progress')
        self.assertEqual((kept[0] / extra.relative_to(self.data)).read_bytes(), b'ghosts')

    def test_a_failure_never_stops_the_launch(self):
        self.write(self.sav, b'save')
        with patch('save_guard.shutil.copyfile', side_effect=OSError('card full')):
            protect(self.data, DS, self.rom)
        self.assertEqual(self.sav.read_bytes(), b'save')

    def test_no_copy_when_the_card_is_nearly_full(self):
        self.write(self.sav, b'save')
        with patch('save_guard.shutil.disk_usage') as usage:
            usage.return_value.free = 1024
            protect(self.data, DS, self.rom)
        self.assertEqual(copies(self.data / 'save-history' / self.key), [])

    def test_a_desmume_save_beside_the_game_loses_its_footer(self):
        raw = bytes(range(256)) * 32
        footer = b'x' * (save_guard.DESMUME_FOOTER - len(save_guard.DESMUME_MAGIC)) + save_guard.DESMUME_MAGIC
        old = self.write(self.rom.with_suffix('.dsv'), raw + footer)
        self.assertEqual(import_old_ds_save(self.data, self.rom), old)
        self.assertEqual(self.sav.read_bytes(), raw)
        self.assertTrue(old.is_file())

    def test_an_existing_dscore_save_is_never_replaced(self):
        self.write(self.sav, b'mine')
        self.write(self.rom.with_suffix('.sav'), b'older emulator')
        self.assertIsNone(import_old_ds_save(self.data, self.rom))
        self.assertEqual(self.sav.read_bytes(), b'mine')

    def test_the_newest_old_save_wins_and_a_blank_one_is_skipped(self):
        import os
        blank = self.write(self.rom.with_suffix('.sav'), b'\xff' * 512)
        older = self.write(self.data / 'emulators/drastic/backup/Game.dsv', b'older save')
        newer = self.write(self.data / 'emulators/melonds/saves/Game.sav', b'newer save')
        os.utime(older, (1000, 1000))
        os.utime(newer, (2000, 2000))
        os.utime(blank, (3000, 3000))
        self.assertEqual(import_old_ds_save(self.data, self.rom), newer)
        self.assertEqual(self.sav.read_bytes(), b'newer save')

    def test_a_retroarch_save_only_counts_when_nothing_else_does(self):
        import os
        srm = self.write(self.rom.with_suffix('.srm'), b'padded save')
        os.utime(srm, (9000, 9000))
        dsv = self.write(self.rom.with_suffix('.dsv'), b'real save')
        os.utime(dsv, (1000, 1000))
        self.assertEqual(import_old_ds_save(self.data, self.rom), dsv)

    def test_back_up_all_saves_includes_3ds_and_psp(self):
        from save_library import backup_all
        expected = [
            self.write(self.data / TANUKI / 'savedata/MARIO/save.dat', b'3ds'),
            self.write(self.data / TANUKI / 'extdata/MARIO/ghosts.dat', b'3ds extra'),
            self.write(self.data / 'emulators/rgds-play/PSP/SAVEDATA/GAME/DATA.BIN', b'psp'),
            self.write(self.data / 'emulators/rgds-play/pcsx-card2.mcd', b'card'),
            self.write(self.sav, b'ds'),
        ]
        with patch('emulators.core_running', return_value=False):
            out = backup_all(self.data)
        with zipfile.ZipFile(out) as z:
            names = z.namelist()
        for path in expected:
            self.assertIn(path.relative_to(self.data).as_posix(), names)


class SourceRuleTests(unittest.TestCase):
    def test_launch_copies_saves_before_the_engine_starts(self):
        source = (ROOT / 'runtime/emulators.py').read_text()
        self.assertLess(source.index('protect(player, core, rom, original_rom, build=app.name)'), source.index('subprocess.Popen'))

    def test_dscore_writes_the_save_before_the_state(self):
        # the launcher skips a resume point older than the .sav
        source = (ROOT / 'dscore/src/frontend/sdl/main.cpp').read_text()
        for name in ('auto_state_path(nds, session.states_dir)', 'state_path(nds, session.states_dir, state_slot), display'):
            write = source.index('save_state_file(nds, ' + name)
            self.assertIn('flush_save()', source[write - 200:write])
            self.assertNotIn('flush_save()', source[write:write + 90])

    def test_resume_check_follows_the_state_format(self):
        # a format bump the launcher does not know makes it skip everyone's resume point after an update
        state = (ROOT / 'dscore/src/core/state/state.h').read_text()
        newest = int(re.search(r'u32 FORMAT_VERSION = (\d+);', state).group(1))
        oldest = int(re.search(r'u32 OLDEST_READABLE_VERSION = (\d+);', state).group(1))
        import emulators
        self.assertEqual(emulators.DS_STATE_FORMATS, (oldest, newest))

    def test_a_release_checks_the_resume_points_first(self):
        source = (ROOT / 'tools/own_os_release.py').read_text()
        self.assertLess(source.index('tools/resume_check.py'), source.index('archive, unpacked = package('))
        self.assertTrue(list((ROOT / 'tests/resume-fixtures/ds').glob('*.dss.gz')))

    def test_a_refused_resume_point_starts_the_game_without_it(self):
        import emulators
        from types import SimpleNamespace as Log
        plan = {'command': ['engine', '--load-state', 'auto.dss']}
        self.assertTrue(emulators.refused_resume(1, plan, Log(state_refused=1)))
        self.assertFalse(emulators.refused_resume(1, plan, Log(state_refused=0)))
        self.assertFalse(emulators.refused_resume(0, plan, Log(state_refused=1)))
        self.assertFalse(emulators.refused_resume(1, {'command': ['engine']}, Log(state_refused=1)))


if __name__ == '__main__':
    unittest.main()
