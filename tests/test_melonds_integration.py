"""Fourth-engine launch isolation, automatic saves and live-setting ownership."""
import os
import subprocess
import sys
from pathlib import Path
import unittest
from unittest.mock import patch
from test_runtime import Fixture
import test_game_menu as fixtures
import emulators
from library import game_key, read_fields, write_fields
from emulator_profiles import game_options


class MelonDSLaunchTests(Fixture):
    def test_package_tool_can_import_runtime_catalog_without_runtime_on_sys_path(self):
        root = Path(__file__).resolve().parents[1]
        code = "import sys;sys.path.insert(0," + repr(
            str(root)
        ) + ");from runtime.shaders import supported;assert supported('melonds',20)"
        subprocess.run([sys.executable, '-I', '-c', code], check=True, capture_output=True, text=True)

    def setup_engine(self):
        binary = self.binary('melonds')
        hook = self.app / 'emulators/hooks/librgdsbridge.so'
        hook.parent.mkdir(parents=True)
        hook.write_bytes(b'fixture bridge')
        return binary

    def test_native_adapter_uses_no_stock_files_and_respects_threading(self):
        binary = self.setup_engine()
        rom = self.root / 'game.nds'
        write_fields(self.data / 'library' / game_key(rom) / 'game.ini', dict(core=3, threaded_3d=0))
        with patch.object(emulators, 'stock_file', side_effect=AssertionError('stock lookup')
                          ), patch.dict(os.environ, {'RGDS_NATIVE_AUTOLOAD': '1'}, clear=True):
            plan = emulators.launch_plan('melonds', self.app, self.data, rom)
        self.assertEqual(plan['command'][0], str(binary.resolve()))
        self.assertEqual(plan['env']['RGDS_THREADED_3D'], '0')
        self.assertEqual(plan['env']['RGDS_NATIVE_AUTOLOAD'], '0')
        self.assertEqual(plan['env']['RGDS_AUTOLOAD'], '0')
        self.assertEqual(plan['env']['RGDS_MENU_LIVE'], '0')
        self.assertNotIn('/mnt/', plan['env']['LD_LIBRARY_PATH'])
        self.assertEqual(game_options(self.data, rom)['core'], 3)

    def test_game_identity_separates_equal_basenames_and_survives_archive_extraction(self):
        self.setup_engine()
        rom = self.root / 'cache/game.nds'
        original = self.root / 'collection/game.zip'
        one = emulators.launch_plan('melonds', self.app, self.data, rom, identity=original)
        two = emulators.launch_plan('melonds', self.app, self.data, rom, identity=self.root / 'other/game.zip')
        self.assertNotEqual(one['env']['RGDS_STATE_SLOT'], two['env']['RGDS_STATE_SLOT'])
        self.assertEqual(Path(one['env']['RGDS_STATE_SLOT']).stem, game_key(original))
        self.assertIn(game_key(original) + '.sav', one['command'][-3])

    def test_autoload_is_native_immediate_and_preserves_existing_manual_checkpoint(self):
        self.setup_engine()
        rom = self.root / 'game.nds'
        key = game_key(rom)
        slot = self.data / 'emulators/melonds/states' / f'{key}.dss'
        slot.parent.mkdir(parents=True)
        slot.write_bytes(b'manual checkpoint')
        automatic = self.data / 'library' / key / 'auto-melonds.dss'
        automatic.parent.mkdir(parents=True)
        automatic.write_bytes(b'automatic checkpoint')
        plan = emulators.launch_plan('melonds', self.app, self.data, rom)
        self.assertEqual(plan['env']['RGDS_NATIVE_AUTOLOAD'], '1')
        self.assertEqual(plan['env']['RGDS_AUTOLOAD'], '0')
        self.assertEqual(slot.read_bytes(), automatic.read_bytes())
        self.assertEqual(
            next((automatic.parent / 'manual-state-backups/melonds').iterdir()).read_bytes(), b'manual checkpoint'
        )


class LiveThreadingTests(unittest.TestCase):
    setUp = fixtures.GameMenuTests.setUp
    request = fixtures.GameMenuTests.request
    result = fixtures.GameMenuTests.result

    def test_live_threading_persists_only_after_matching_success(self):
        self.menu.core = 'melonds'
        self.menu.threaded = True
        path = self.menu.data / 'library' / game_key(self.menu.rom) / 'game.ini'
        write_fields(path, dict(core=3, filter=4, threaded_3d=1))
        self.request('threaded:0')
        self.assertEqual(read_fields(path)['threaded_3d'], '1')
        self.assertFalse(self.menu.paused)
        self.result('threaded-off', token='stale')
        self.assertTrue(self.menu.threaded)
        self.result('threaded-off')
        self.assertTrue(self.menu.paused)
        self.assertFalse(self.menu.threaded)
        self.assertEqual(read_fields(path), dict(core='3', filter='4', threaded_3d='0'))
        self.assertIn('No restart', read_fields(self.menu.status)['message'])

    def test_failed_threading_retains_game_preference(self):
        self.menu.core = 'melonds'
        self.menu.threaded = True
        self.request('threaded:0')
        self.result('threaded-failed')
        self.assertTrue(self.menu.threaded)
        self.assertEqual(read_fields(self.menu.status)['state'], 'error')

    def test_closed_engine_does_not_receive_invented_live_command(self):
        self.menu.core = 'drastic'
        self.request('threaded:0')
        self.assertIsNone(self.menu.pending)
        self.assertFalse(self.menu.control.exists())
