"""Show all systems: the Systems step, other systems' libraries, and RG DS Play's in-game Graphics."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key, read_fields

PAGE_LIBRARY, PAGE_LIBRARY_OPTIONS, PAGE_QUICK_MENU, PAGE_GAME_OPTIONS = 0, 6, 12, 19
PAGE_PLAY_GRAPHICS, PAGE_SHADER_PICKER = 39, 20
PLAY_CORE = 5


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class SystemsUITests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.nds = self.root / 'Roms/NDS'
        self.gba = self.root / 'Roms/GBA'
        self.nds.mkdir(parents=True)
        self.gba.mkdir(parents=True)
        for name in ('Alpha.nds', 'Beta.nds', 'Gamma.nds'):
            (self.nds / name).write_bytes(bytes(512))
        for name in ('Metroid Fusion (USA).gba', 'Mario Kart - Super Circuit (Europe).gba'):
            (self.gba / name).write_bytes(bytes(0xC0))
        # A GBA game dropped in rgds/games/gba stays out of the DS list.
        (self.nds / 'gba').mkdir()
        (self.nds / 'gba/Stray.zip').write_bytes(b'PK')

    def run_ui(self, script='', extra=(), show_systems=None, home=None, seconds=1):
        ui = 'schema=1\nhome_layout=carousel\nbackground=plain\n'
        if show_systems is not None: ui += f'show_systems={int(show_systems)}\n'
        (self.data / 'ui.ini').write_text(ui)
        state = self.data / 'state'
        if home is not None: state.write_text(f'1 0 4 ""\nhome {home}\n')
        (self.data / 'input').write_text(script)
        result = subprocess.run(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--rom-dir',
                str(self.nds), '--system-dir', 'gba=' + str(self.gba), '--input-script',
                str(self.data / 'input'), '--state',
                str(state), '--seconds',
                str(seconds), '--cache-report',
                str(self.data / 'report.json'), *extra
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.stdout = result.stdout
        return json.loads((self.data / 'report.json').read_text())

    def test_off_by_default_games_opens_the_ds_library_as_before(self):
        report = self.run_ui('100 key A\n', home=1)
        self.assertFalse(report['show_systems'])
        self.assertEqual(report['page'], PAGE_LIBRARY)
        self.assertEqual(report['library_count'], 3)  # the DS games only, nothing from GBA folders
        self.assertEqual(report['listed_systems'], 1)

    def test_games_opens_systems_then_a_system_library_and_back(self):
        report = self.run_ui('100 key A\n', show_systems=True, home=1)
        self.assertEqual(report['page'], PAGE_LIBRARY)
        self.assertTrue(report['systems_menu'])  # the library's carousel, its tiles the systems
        self.assertEqual(report['listed_systems'], 2)  # the DS and Game Boy Advance
        report = self.run_ui('100 key A\n200 key Right\n300 key A\n', show_systems=True, home=1)
        self.assertEqual(report['page'], PAGE_LIBRARY)
        self.assertEqual(report['shown_system'], 'gba')
        self.assertEqual(report['library_count'], 2)
        report = self.run_ui('100 key A\n200 key A\n', show_systems=True, home=1)
        self.assertEqual(report['shown_system'], 'nds')
        self.assertEqual(report['library_count'], 3)
        # Back from a system's games returns to Systems, Back from Systems to Home.
        report = self.run_ui('100 key A\n200 key Right\n300 key A\n400 key B\n', show_systems=True, home=1)
        self.assertTrue(report['systems_menu'])
        self.assertEqual(report['systems_selected'], 1)  # back on the Game Boy Advance tile

    def test_a_gba_game_launches_with_rgds_play(self):
        report = self.run_ui('100 key A\n200 key Right\n300 key A\n400 key A\n', show_systems=True, home=1)
        self.assertEqual(report['launching_core'], PLAY_CORE)

    def test_library_setting_turns_systems_on_and_is_saved(self):
        script = '100 key A\n'  # the first row of Settings > Library
        report = self.run_ui(script, extra=('--settings', '--section', 'library'))
        self.assertEqual(report['page'], PAGE_LIBRARY_OPTIONS)
        self.assertTrue(report['show_systems'])
        self.assertIn('show_systems=1', (self.data / 'ui.ini').read_text())

    def test_gba_game_options_keep_the_shared_rows_and_no_ds_engine(self):
        report = self.run_ui('100 key A\n200 key Right\n300 key A\n400 key X\n', show_systems=True, home=1)
        self.assertEqual(report['page'], PAGE_GAME_OPTIONS)
        self.assertEqual(report['default_core'], 4)  # the DS default stays a DS engine
        # The emulator row is not a picker for other systems.
        report = self.run_ui(
            '100 key A\n200 key Right\n300 key A\n400 key X\n500 key Down\n600 key A\n', show_systems=True, home=1
        )
        self.assertFalse(report.get('picker_open', False))
        self.assertEqual(report['game_core_override'], -1)

    def test_in_game_graphics_for_rgds_play(self):
        rom = self.gba / 'Metroid Fusion (USA).gba'
        menu = ('--game-menu', '--game-core', 'rgds-play', '--game-rom', str(rom))
        report = self.run_ui('100 key Down\n150 key Right\n200 key Right\n300 key A\n', extra=menu)
        self.assertEqual(report['page'], PAGE_PLAY_GRAPHICS)
        # Game screen: Whole pixels; then Second screen: Swap screens (preview saves play.ini directly).
        script = '100 key Down\n150 key Right\n200 key Right\n300 key A\n400 key A\n500 key Down\n600 key A\n'
        script += '700 key Down\n800 key A\n900 key Down\n1000 key Down\n1100 key A\n'
        report = self.run_ui(script, extra=menu, seconds=2)
        view = read_fields(self.data / 'library' / game_key(rom) / 'play.ini')
        self.assertEqual(view.get('fit'), 'integer')
        self.assertEqual(view.get('second'), 'swap')
        self.assertEqual((report['play_fit'], report['play_second']), (1, 2))

    def test_in_game_frame_and_shader_for_rgds_play(self):
        rom = self.gba / 'Metroid Fusion (USA).gba'
        menu = ('--game-menu', '--game-core', 'rgds-play', '--game-rom', str(rom))
        # Graphics > Frame (the third row): Black, the fourth shell.
        script = '100 key Down\n150 key Right\n200 key Right\n300 key A\n400 key Down\n500 key Down\n600 key A\n'
        script += '700 key Down\n750 key Down\n800 key Down\n900 key A\n'
        report = self.run_ui(script, extra=menu, seconds=2)
        self.assertEqual(read_fields(self.data / 'library' / game_key(rom) / 'play.ini').get('frame'), 'black')
        self.assertEqual(report['play_frame'], 'black')
        # Graphics > Shader (the fourth row) opens the same shader carousel as the DS engines.
        script = '100 key Down\n150 key Right\n200 key Right\n300 key A\n400 key Down\n500 key Down\n600 key Down\n'
        script += '700 key A\n'
        (self.data / 'state').unlink(missing_ok=True)
        report = self.run_ui(script, extra=menu, seconds=2)
        self.assertEqual(report['page'], PAGE_SHADER_PICKER)
        (self.data / 'state').unlink(missing_ok=True)  # each menu open starts afresh
        report = self.run_ui(script + '800 key B\n', extra=menu, seconds=2)
        self.assertEqual(report['page'], PAGE_PLAY_GRAPHICS)

    def test_each_system_is_offered_only_its_own_screen_shaders(self):
        ds_screens, gba_screens = {4, 6, 8, 9, 11, 12, 14, 21, 22}, {29, 30}
        rom = self.gba / 'Metroid Fusion (USA).gba'
        script = '100 key Down\n150 key Right\n200 key Right\n300 key A\n400 key Down\n500 key Down\n600 key Down\n'
        report = self.run_ui(
            script + '700 key A\n', extra=('--game-menu', '--game-core', 'rgds-play', '--game-rom', str(rom)), seconds=2
        )
        self.assertEqual(report['page'], PAGE_SHADER_PICKER)
        offered = set(report['shader_choices'])
        self.assertTrue(gba_screens <= offered)
        self.assertFalse(ds_screens & offered)
        # A DS game's carousel (its Graphics tile opens it directly) has the DS screens and no GBA ones.
        (self.data / 'state').unlink(missing_ok=True)
        nds = self.nds / 'Alpha.nds'
        report = self.run_ui(
            '100 key Down\n150 key Right\n200 key Right\n300 key A\n',
            extra=('--game-menu', '--game-core', 'melonds', '--game-rom', str(nds)),
            seconds=2
        )
        self.assertEqual(report['page'], PAGE_SHADER_PICKER)
        offered = set(report['shader_choices'])
        self.assertTrue(ds_screens <= offered)
        self.assertFalse(gba_screens & offered)


if __name__ == '__main__':
    unittest.main()
