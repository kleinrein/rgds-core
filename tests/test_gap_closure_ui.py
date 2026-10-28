"""Real SDL navigation for slot management, DS input and library discovery."""
import json, os, subprocess, tempfile, unittest, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import write_fields, read_fields
from library import game_key


class GapClosureUiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rom = self.root / 'Mario.nds'
        self.rom.write_bytes(bytes(512))
        self.data = self.root / 'data'
        self.data.mkdir()
        self.prefs = self.data / 'preferences.ini'
        write_fields(self.prefs, dict(schema=1, core=1, font='rounded', mode='dark'))
        self.game = self.data / 'library' / game_key(self.rom)

    def run_ui(self, keys, mode='game', text=''):
        script = self.root / 'input'
        # Settings > Library opens on Scan for games, then Show all systems: step onto the rows these tests count from.
        if mode == 'settings': keys = ['Down', 'Down', *keys]
        script.write_text(
            ''.join(f'{60+i*65} key {key}\n{80+i*65} key-up {key}\n' for i, key in enumerate(keys)) + text
        )
        report = self.root / 'report.json'
        args = ['--game-menu', '--game-core', 'dsperate', '--game-rom',
                str(self.rom)] if mode == 'game' else ['--settings', '--section', 'system'] if mode == 'system' else [
                    '--settings', '--section', 'library'
                ] if mode == 'settings' else ['--library', '--rom-dir', str(self.root)]
        r = subprocess.run(
            [
                str(ROOT / 'build/host/rgds-launcher'), '--preview', *args, '--rom-dir',
                str(self.root), '--data',
                str(self.data), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(script), '--seconds', '2', '--cache-report',
                str(report), '--screenshot',
                str(self.root / 'screen.bmp')
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=6
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(report.read_text())

    def test_save_checkpoint_asks_before_replacing_an_occupied_slot(self):
        write_fields(self.game / 'slots/dsperate/index.ini', dict(slot_0='2026-10-12 12:30'))
        r = self.run_ui(['Down', 'A', 'A'])
        self.assertEqual(r['page'], 28)
        self.assertEqual(r['picker_title'], 'Replace slot 1?')
        self.assertEqual(r['picker_focus'], 0)
        self.assertEqual(r['render_file_reads'], 0)

    def test_load_checkpoint_asks_on_an_occupied_slot_and_ignores_an_empty_one(self):
        write_fields(self.game / 'slots/dsperate/index.ini', dict(slot_0='2026-10-12 12:30'))
        r = self.run_ui(['Down', 'Right', 'A', 'A'])
        self.assertEqual(r['page'], 28)
        self.assertEqual(r['picker_title'], 'Load slot 1?')
        self.assertEqual(r['picker_focus'], 0)
        r = self.run_ui(['Down', 'Right', 'A', 'Down', 'A'])
        self.assertEqual(r['page'], 28)
        self.assertFalse(r['picker_open'])

    def test_x_deletes_an_occupied_slot_after_asking_and_y_opens_more(self):
        write_fields(self.game / 'slots/dsperate/index.ini', dict(slot_0='2026-10-12 12:30'))
        r = self.run_ui(['Down', 'A', 'X'])
        self.assertEqual(r['picker_title'], 'Delete slot 1?')
        self.assertEqual(r['picker_focus'], 0)
        r = self.run_ui(['Down', 'A', 'Down', 'X'])
        self.assertFalse(r['picker_open'])
        r = self.run_ui(['Down', 'A', 'Y'])
        self.assertEqual(r['picker_title'], 'Slot 1')
        self.assertTrue(r['picker_open'])
        # Game options: ... Artwork, Game info, Save gallery (the sixth Down).
        r = self.run_ui(['X'] + ['Down'] * 6 + ['A', 'X'], mode='library')
        self.assertEqual(r['page'], 28)
        self.assertEqual(r['picker_title'], 'Delete slot 1?')

    def test_ds_language_and_real_microphone_persist(self):
        r = self.run_ui(['X'] + ['Down'] * 8 + ['A', 'Down', 'Down', 'A', 'Down', 'Down', 'A'])
        self.assertEqual(read_fields(self.game / 'ds-input.ini')['language'], '3')
        self.assertEqual(r['page'], 31)

    def test_discovery_and_filter_remove_played_games(self):
        write_fields(self.game / 'stats.ini', dict(sessions=1, seconds=100, last_played='2026-10-12'))
        # Y opens the Filter sheet: three rows of tiles, then Unplayed.
        r = self.run_ui(['Y', 'Down', 'Down', 'Down', 'A'], mode='library')
        self.assertEqual(r['discovery_filter'], 1)
        self.assertEqual(r['filtered_games'], 0)
        self.assertEqual(r['render_file_reads'], 0)

    def test_first_run_help_is_reachable_from_library_settings(self):
        r = self.run_ui(['Down'] * 7 + ['A'], mode='settings')
        self.assertEqual(r['page'], 30)

    def test_automatic_download_setting_is_persisted(self):
        self.run_ui(['Down'] * 6 + ['A'] + ['Down'] * 6 + ['A', 'Down', 'A'], mode='system')
        self.assertEqual(read_fields(self.data / 'experience.ini')['auto_update'], '2')

    def test_export_import_picker_has_no_duplicate_and_lists_archive(self):
        archive = self.data / 'imports/test.rgdssave.zip'
        archive.parent.mkdir()
        archive.touch()
        r = self.run_ui(['X'] + ['Down'] * 6 + ['A', 'Y', 'A'], mode='library')
        self.assertEqual(r['page'], 28)
        self.assertEqual(r['picker_title'], 'Import slot')
        self.assertTrue(r['picker_open'])

    def test_search_from_the_library_opens_results_and_trims_query(self):
        (self.root / 'Zelda.nds').write_bytes(bytes(512))
        r = self.run_ui(['E'], mode='library', text='800 text   MARIO  \n900 key Return\n')
        self.assertEqual(r['search_query'], 'MARIO')
        self.assertEqual(r['filtered_games'], 1)
        self.assertFalse(r['home_menu'])
        self.assertEqual(r['page'], 0)

    def test_search_matches_artwork_title(self):
        write_fields(self.game / 'info.ini', dict(artwork_title='Super Plumber'))
        r = self.run_ui(['E'], mode='library', text='800 text plumber\n900 key Return\n')
        self.assertEqual(r['filtered_games'], 1)
        self.assertFalse(r['home_menu'])

    def test_empty_search_can_be_cleared(self):
        # Y opens the sheet; two rows down and two across is the Clear filters tile.
        r = self.run_ui(
            ['E'],
            mode='library',
            text='800 text missing\n900 key Return\n1000 key Y\n1100 key Down\n1200 key Down\n'
            '1300 key Right\n1400 key Right\n1500 key Return\n'
        )
        self.assertEqual(r['filtered_games'], 1)
        self.assertEqual(r['search_query'], '')
        self.assertFalse(r['home_menu'])

    def test_reset_requires_confirmation_and_can_be_cancelled(self):
        r = self.run_ui(['X'] + ['Down'] * 9 + ['A'])
        self.assertEqual(r['picker_title'], 'Restart without loading a state?')
        self.assertEqual(r['picker_focus'], 0)
        self.assertFalse((self.data / 'session/game-menu.request').exists())

    def test_microphone_test_opens_and_returns_to_sound(self):
        r = self.run_ui(['B', 'Down', 'A', 'Down', 'Down', 'A'], mode='settings')
        self.assertEqual(r['page'], 32)
        r = self.run_ui(['B', 'Down', 'A', 'Down', 'Down', 'A', 'B'], mode='settings')
        self.assertEqual(r['page'], 11)


if __name__ == '__main__': unittest.main()
