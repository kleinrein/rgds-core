"""Exercise engine settings with real SDL input and actual preference files."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
import emulators
from library import game_key, read_fields, write_fields


@unittest.skipUnless(BINARY.exists(), 'Build the host launcher first')
class EmulatorSettingsUiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.rom = self.root / 'game.nds'
        self.rom.write_bytes(b'fixture')
        self.prefs = self.root / 'preferences.ini'
        self.prefs.write_text('schema=1\nmode=light\nfont=rounded\ncore=1\n')
        self.game = self.root / 'library' / game_key(self.rom) / 'game.ini'
        write_fields(self.game, dict(schema=1, core=0, filter=4, favourite=1, threaded_3d=1, hires_3d=0, frameskip=0))
        self.original = self.game.read_bytes()

    def run_ui(self, keys, core='rgds-experimental', library=False, touch='', game_library=False):
        script = self.root / 'input'
        # Settings > Library opens on Scan for games, then Show all systems: step onto the rows these tests count from.
        if library: keys = ['Down', 'Down', *keys]
        script.write_text(''.join(f'{30 + i * 30} key {key}\n' for i, key in enumerate(keys)) + touch)
        state = self.root / 'state'
        state.unlink(missing_ok=True)
        mode = ['--settings', '--section', 'library'
                ] if library else ['--game-menu', '--game-core', core, '--game-rom',
                                   str(self.rom)]
        if game_library: mode = ['--library', '--rom-dir', str(self.root)]
        result = subprocess.run(
            [
                str(BINARY), '--preview', *mode, '--data',
                str(self.root), '--state',
                str(state), '--input-script',
                str(script), '--seconds', '1', '--cache-report',
                str(self.root / 'report.json')
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=6
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / 'report.json').read_text())

    @staticmethod
    def row(index, *keys):
        return ['X', *(['Down'] * index), *keys]

    def test_ds_nickname_edit_preserves_other_ds_settings(self):
        path = self.game.parent / 'ds-input.ini'
        write_fields(path, dict(language=3, microphone=0, nickname='Old'))
        keys = self.row(7, 'A', 'Down', 'Down', 'Down', 'Down', 'A', 'Backspace', 'Backspace', 'Backspace')
        self.run_ui(keys, 'rgds-experimental', touch='800 text Andreas\n900 key Return\n')
        self.assertEqual(read_fields(path), dict(language='3', microphone='0', nickname='Andreas'))

    def test_ds_nickname_cancel_and_utf16_limit(self):
        path = self.game.parent / 'ds-input.ini'
        write_fields(path, dict(nickname='NineChars'))
        keys = self.row(7, 'A', 'Down', 'Down', 'Down', 'Down', 'A')
        # A supplementary character needs two DS UTF-16 units, so it cannot
        # fit after nine characters. The following ASCII character can.
        self.run_ui(keys, 'rgds-experimental', touch='750 text 😀\n800 text X\n900 key Return\n')
        self.assertEqual(read_fields(path)['nickname'], 'NineCharsX')
        self.run_ui(keys + ['Backspace'], 'rgds-experimental', touch='850 key Escape\n')
        self.assertEqual(read_fields(path)['nickname'], 'NineCharsX')

    def test_dscore_is_the_default_and_capabilities_match_fork(self):
        report = self.run_ui([], library=True)
        self.assertEqual(report['default_core'], 4)  # the stored DSperate (1) is retired
        report = self.run_ui(self.row(5, 'A', 'Down', 'A'), 'rgds-experimental')
        self.assertEqual(report['game_frameskip'], 1)
        report = self.run_ui(self.row(4, 'A'), 'rgds-experimental')
        self.assertTrue(report['picker_open'])

    def test_experimental_resolution_picker_persists_from_both_menus(self):
        self.run_ui(self.row(4, 'A', 'Down', 'Down', 'Down', 'Down', 'A'), 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_scale'], '5')
        fields = read_fields(self.game)
        fields['core'] = '4'
        write_fields(self.game, fields)
        self.run_ui(self.row(4, 'A', 'Up', 'Up', 'A'), game_library=True)
        self.assertEqual(read_fields(self.game)['rgds_scale'], '3')
        self.assertEqual(read_fields(self.game)['core'], '4')
        self.assertEqual(read_fields(self.game)['filter'], '4')
        self.run_ui(self.row(4, 'A', 'Down', 'Escape'), game_library=True)
        self.assertEqual(read_fields(self.game)['rgds_scale'], '3')

    def test_graphics_tile_controls_resolution_smoothing_and_returns_to_game_menu(self):
        # Resume -> Save slots -> across to the new Graphics tile.
        graphics = ['Down', 'Right', 'Right', 'A']
        self.run_ui(graphics + ['A', 'Down', 'Down', 'Down', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_scale'], '5')
        self.run_ui(graphics + ['Down', 'A', 'Up', 'A'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_smooth'], '0')
        self.assertEqual(read_fields(self.game)['rgds_scale'], '5')
        # A cancelled resolution change cannot overwrite the saved choice.
        self.run_ui(graphics + ['A', 'Up', 'Escape', 'Escape'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_scale'], '5')
        self.assertEqual(read_fields(self.game)['filter'], '4')

    def test_graphics_tile_2d_scaling_persists_and_keeps_other_settings(self):
        graphics = ['Down', 'Right', 'Right', 'A']
        self.run_ui(graphics + ['A', 'Down', 'Down', 'A'], 'rgds-experimental')  # 3D resolution 3x
        scale = read_fields(self.game)['rgds_scale']
        # Row 3 is 2D scaling: Native pixels -> Sharp, then Smooth, then back to native.
        self.run_ui(graphics + ['Down', 'Down', 'Down', 'A', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_2d'], '1')
        self.assertEqual(read_fields(self.game)['rgds_scale'], scale)
        self.run_ui(graphics + ['Down', 'Down', 'Down', 'A', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_2d'], '2')
        # Cancelling leaves the saved choice alone.
        self.run_ui(graphics + ['Down', 'Down', 'Down', 'A', 'Up', 'Escape', 'Escape'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_2d'], '2')
        self.run_ui(graphics + ['Down', 'Down', 'Down', 'A', 'Up', 'Up', 'A'], 'rgds-experimental')
        self.assertNotIn('rgds_2d', read_fields(self.game))
        self.assertEqual(read_fields(self.game)['rgds_scale'], scale)

    def test_graphics_tile_heavy_scenes_choice_persists_and_keeps_other_settings(self):
        graphics = ['Down', 'Right', 'Right', 'A']
        report = self.run_ui(graphics + ['Down', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(report['picker_title'], 'Heavy 3D scenes (2x and above)')
        self.assertEqual(report['picker_focus'], 0)  # Adaptive (holds 60 fps) is the default
        self.run_ui(graphics + ['A', 'Down', 'Down', 'A'], 'rgds-experimental')  # 3D resolution 3x
        scale = read_fields(self.game)['rgds_scale']
        # Row 2: Always sharp is stored; the default never is.
        self.run_ui(graphics + ['Down', 'Down', 'A', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_adaptive'], '0')
        self.assertEqual(read_fields(self.game)['rgds_scale'], scale)
        self.assertEqual(read_fields(self.game)['filter'], '4')
        report = self.run_ui(graphics + ['Down', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual(report['picker_focus'], 1)
        # Cancelling leaves the saved choice alone.
        self.run_ui(graphics + ['Down', 'Down', 'A', 'Up', 'Escape', 'Escape'], 'rgds-experimental')
        self.assertEqual(read_fields(self.game)['rgds_adaptive'], '0')
        self.run_ui(graphics + ['Down', 'Down', 'A', 'Up', 'A'], 'rgds-experimental')
        self.assertNotIn('rgds_adaptive', read_fields(self.game))
        self.assertEqual(read_fields(self.game)['rgds_scale'], scale)

    def test_graphics_tile_rows_follow_heavy_scenes_row(self):
        graphics = ['Down', 'Right', 'Right', 'A']
        report = self.run_ui(graphics + ['Down'] * 5 + ['A'], 'rgds-experimental')
        self.assertEqual(report['picker_title'], 'Frameskip')
        # Leaving the screen filter list lands back on its own row, not 2D scaling.
        shader = self.run_ui(graphics + ['Down'] * 4 + ['A'], 'rgds-experimental')
        report = self.run_ui(graphics + ['Down'] * 4 + ['A', 'Escape', 'A'], 'rgds-experimental')
        self.assertEqual(report['page'], shader['page'])
        self.assertFalse(report['picker_open'])

    def test_unsupported_controls_do_not_open_pickers_or_write_preferences(self):
        # DSCore threads its 3D itself; RG DS Play has no 3D resolution or frameskip.
        for core, row in [('rgds-experimental', 3), ('rgds-play', 3), ('rgds-play', 4), ('rgds-play', 5)]:
            with self.subTest(core=core, row=row):
                report = self.run_ui(self.row(row, 'A', 'Right', 'Left'), core)
                self.assertFalse(report['picker_open'])
                self.assertEqual(self.game.read_bytes(), self.original)

    def test_supported_pickers_open_without_mutating_on_a_or_arrow(self):
        for core, row, key in [('rgds-experimental', 4, 'A'), ('rgds-experimental', 5, 'Right')]:
            with self.subTest(core=core, row=row, key=key):
                report = self.run_ui(self.row(row, key), core)
                self.assertTrue(report['picker_open'])
                self.assertEqual(self.game.read_bytes(), self.original)

    def test_failed_save_keeps_displayed_and_stored_game_tuning(self):
        Path(str(self.game) + '.pending').mkdir()
        report = self.run_ui(self.row(5, 'A', 'Down', 'A'))
        self.assertEqual(report['game_frameskip'], 0)
        self.assertIn('Previous values kept', report['settings_notice'])
        self.assertEqual(self.game.read_bytes(), self.original)

    def test_system_defaults_page_saves_a_systems_choice_once(self):
        # Third Library row: System defaults. Down to Game screen, open its picker, step to Smooth fill.
        self.run_ui(['A', 'Down', 'A', 'Down', 'Down', 'A'], library=True)
        saved = read_fields(self.root / 'system-defaults' / 'gba.ini')
        self.assertEqual(saved['fit'], 'smooth')
        self.assertEqual(saved['second'], 'companion')
        self.assertNotIn('filter', saved)  # the shader stays on the system's own screen until chosen

    def test_partial_ui_save_failure_reopens_picker_at_stored_layout(self):
        (self.root / 'ui.ini').write_text('schema=1\nlayout=0\n')
        (self.root / 'ui.ini.pending').mkdir()
        # run_ui steps past Scan and Systems; System defaults is the third row, so one more Down than before.
        report = self.run_ui(['Down'] * 5 + ['A', 'Down', 'A', 'A'], library=True)
        self.assertTrue(report['picker_open'])
        self.assertEqual(report['picker_title'], 'Browser layout')
        self.assertEqual(report['picker_focus'], 0)
        self.assertEqual(read_fields(self.root / 'ui.ini')['layout'], '0')
        self.assertIn('Could not save', report['settings_notice'])

    # The preview's sample database: Infinite lives and a note outside any group,
    # the exclusive Coins group (99 coins on, Max coins) and Movement (two codes).
    # Page 38 is Page::Cheats, 12 the game menu; views are Groups 0, Group 1, All 2, On 3.
    # The header's tabs (All, Groups, On) stand at x 272..488, 72 px each; the X search key at 500..616.
    def test_cheats_bar_opens_the_groups_overview_and_toggles_inside_a_group(self):
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A'], 'rgds-experimental')
        self.assertEqual(
            (report['page'], report['cheat_view'], report['cheat_rows'], report['cheats_on']), (38, 0, 3, 1)
        )
        self.assertTrue(report['cheats_available'])
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual((report['cheat_view'], report['cheat_group'], report['cheat_rows']), (1, 0, 2))
        # Max coins on turns 99 coins off: one code at a time in that group.
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'Down', 'A', 'Down', 'A'], 'rgds-experimental')
        self.assertEqual((report['cheats_on'], report['cheat_rows']), (1, 2))
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'Down', 'A', 'B'], 'rgds-experimental')
        self.assertEqual((report['page'], report['cheat_view']), (38, 0))
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'Down', 'A', 'B', 'B'], 'rgds-experimental')
        self.assertEqual(report['page'], 12)

    def test_cheats_right_steps_through_groups_and_shoulders_change_tabs(self):
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A'], 'rgds-experimental')
        self.assertEqual((report['cheat_view'], report['cheat_group'], report['cheat_rows']), (1, -1, 2))
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A', 'Right', 'Right', 'Right'], 'rgds-experimental')
        self.assertEqual((report['cheat_view'], report['cheat_group'], report['cheat_rows']), (1, 1, 2))
        # R from a group opens the On tab; L twice from there crosses Groups to All, which lists every code.
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A', 'Right', ']'], 'rgds-experimental')
        self.assertEqual((report['cheat_view'], report['cheat_rows'], report['cheats_on']), (3, 1, 1))
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A', 'Right', ']', '['], 'rgds-experimental')
        self.assertEqual(report['cheat_view'], 0)
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A', 'Right', ']', '[', '[', '['], 'rgds-experimental')
        self.assertEqual((report['cheat_view'], report['cheat_rows']), (2, 6))

    def test_cheats_search_lists_matches_across_groups(self):
        keys = ['Down'] * 4 + ['Right'] + ['A', 'X']
        report = self.run_ui(keys, 'rgds-experimental', touch='800 text coin\n900 key Return\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 2, 2))
        report = self.run_ui(keys, 'rgds-experimental', touch='800 text coin\n900 key Return\n1000 key A\n')
        self.assertEqual(report['cheats_on'], 0)
        # Escape cancels and leaves the overview as it was; an empty search lists every code flat.
        report = self.run_ui(keys, 'rgds-experimental', touch='800 text coin\n900 key Escape\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 0, 3))
        report = self.run_ui(keys, 'rgds-experimental', touch='900 key Return\n')
        self.assertEqual((report['cheat_view'], report['cheat_rows']), (2, 6))
        # B clears a search but stays on All; B again leaves for the menu. Changing tabs drops the search too.
        report = self.run_ui(keys, 'rgds-experimental', touch='400 text coin\n500 key Return\n600 key B\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 2, 6))
        report = self.run_ui(keys, 'rgds-experimental', touch='400 text coin\n500 key Return\n600 key B\n700 key B\n')
        self.assertEqual(report['page'], 12)
        report = self.run_ui(keys, 'rgds-experimental', touch='400 text coin\n500 key Return\n600 key ]\n700 key [\n')
        self.assertEqual((report['cheat_view'], report['cheat_rows']), (2, 6))

    def test_cheats_bar_and_rows_answer_to_touch(self):
        # The bar under the grid opens the overview; a row opens its group; a code row toggles.
        report = self.run_ui([], 'rgds-experimental', touch='300 click 470 380\n500 click 300 130\n700 click 300 130\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheats_on']), (38, 1, 2))
        # The header's tabs: On lists what is on, Groups returns, All lists every code; the X key opens the keyboard.
        report = self.run_ui([], 'rgds-experimental', touch='300 click 470 380\n500 click 452 45\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 3, 1))
        report = self.run_ui([], 'rgds-experimental', touch='300 click 470 380\n500 click 452 45\n700 click 380 45\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 0, 3))
        report = self.run_ui([], 'rgds-experimental', touch='300 click 470 380\n500 click 308 60\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 2, 6))
        report = self.run_ui([], 'rgds-experimental', touch='300 click 470 380\n500 click 560 45\n')
        self.assertEqual(report['page'], 8)  # Page::Keyboard, the cheat search

    def test_cheats_empty_views_keep_their_header_and_the_shoulders_change_tabs(self):
        # Nothing matches 'zzz': the page stays, its tabs still answer, and the pad's R (button 5) opens On.
        keys = ['Down'] * 4 + ['Right'] + ['A', 'X']
        report = self.run_ui(keys, 'rgds-experimental', touch='400 text zzz\n500 key Return\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 2, 0))
        report = self.run_ui(keys, 'rgds-experimental', touch='400 text zzz\n500 key Return\n600 click 380 45\n')
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 0, 3))
        report = self.run_ui(
            keys, 'rgds-experimental', touch='400 text zzz\n500 key Return\n600 joy-down 5\n700 joy-down 5\n'
        )
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 3, 1))
        # With 99 coins switched off, the On tab is empty but still the Cheats page; L (button 4) goes back to Groups.
        report = self.run_ui(keys[:-1] + ['Down', 'A', 'A'], 'rgds-experimental', touch='400 joy-down 5\n')
        self.assertEqual(
            (report['page'], report['cheat_view'], report['cheat_rows'], report['cheats_on']), (38, 3, 0, 0)
        )
        report = self.run_ui(
            keys[:-1] + ['Down', 'A', 'A'], 'rgds-experimental', touch='400 joy-down 5\n500 joy-down 4\n'
        )
        self.assertEqual((report['page'], report['cheat_view'], report['cheat_rows']), (38, 0, 3))

    def test_cheats_page_lists_the_engine_file_flat_when_it_has_no_groups(self):
        write_fields(
            self.root / 'session/emulator.control.cheats',
            dict(
                count=2,
                groups=0,
                cheat_0='Moon jump',
                cheat_0_description='',
                cheat_0_group=-1,
                cheat_0_state=1,
                cheat_1='Notes',
                cheat_1_description='',
                cheat_1_group=-1,
                cheat_1_state=-1
            )
        )
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'Down', 'A'], 'rgds-experimental')
        # No overview for a flat list, so it opens on All; the note cannot be toggled, so the one code stays on.
        self.assertEqual(
            (report['page'], report['cheat_view'], report['cheat_rows'], report['cheats_on']), (38, 2, 2, 1)
        )
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'A'], 'rgds-experimental')
        self.assertEqual(report['cheats_on'], 0)
        report = self.run_ui(['Down'] * 4 + ['Right'] + ['A', 'B'], 'rgds-experimental')
        self.assertEqual(report['page'], 12)

    def test_fast_forward_preview_tracks_toggle_state(self):
        self.assertTrue(self.run_ui(self.row(1, 'A'))['menu_fast_forward'])
        self.assertFalse(self.run_ui(self.row(1, 'A', 'A'))['menu_fast_forward'])

    def test_frameskip_choice_persists(self):
        report = self.run_ui(self.row(5, 'A', 'Down', 'Down', 'A'))
        self.assertEqual(report['game_frameskip'], 2)
        self.assertEqual(read_fields(self.game)['frameskip'], '2')

    def test_restart_always_resolves_to_dscore(self):
        # Any stored choice, a retired engine's included, restarts a DS game in DSCore.
        for selection in (-1, 0, 1, 3, 4):
            with self.subTest(selection=selection):
                write_fields(self.game, dict(core=selection))
                with patch.object(emulators, '_run_once', side_effect=[75, 0]) as run:
                    self.assertEqual(emulators.run('rgds-experimental', ROOT, self.root, self.rom), 0)
                self.assertEqual([call.args[0] for call in run.call_args_list], ['rgds-experimental'] * 2)


if __name__ == '__main__':
    unittest.main()
