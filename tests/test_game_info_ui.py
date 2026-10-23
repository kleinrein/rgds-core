"""The Game info page (Game options > Game info) and the library filters built on game information."""
import json, os, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import write_fields
from library import game_key

GAME_OPTIONS, DISCOVER, GAME_INFO = 19, 29, 40  # Page enum values in src/launcher/settings.hpp
# Game options > Game info: the row above Save gallery (DSperate here: Favourite ... Artwork, Game info).
OPEN_INFO = ['X', 'Down', 'Down', 'Down', 'Down', 'Down', 'A']


class LauncherFixture(unittest.TestCase):
    """A launcher run in the dummy video driver, driven by an input script."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rom = self.root / 'Puzzle.nds'
        self.rom.write_bytes(bytes(512))
        self.data = self.root / 'data'
        self.data.mkdir()
        self.prefs = self.data / 'preferences.ini'
        write_fields(self.prefs, dict(schema=1, core=1, font='rounded', mode='light'))
        self.item = self.data / 'library' / game_key(self.rom.resolve())

    def run_ui(self, keys, *mode, touches=()):
        """Keys press one by one; `touches` (script lines like 'tap 56 352') follow, 150 ms apart."""
        script = self.root / 'input'
        end = 300 + len(keys) * 180
        script.write_text(
            ''.join(f'{300+i*180} key {key}\n{340+i*180} key-up {key}\n'
                    for i, key in enumerate(keys)) + ''.join(f'{end+i*150} {line}\n' for i, line in enumerate(touches))
        )
        report = self.root / 'report.json'
        result = subprocess.run(
            [
                str(ROOT / 'build/host/rgds-launcher'), '--preview', *(mode or ('--library',)), '--rom-dir',
                str(self.root), '--data',
                str(self.data), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(script), '--seconds',
                str(max(3, (len(keys) + len(touches)) * 0.2 + 2)), '--cache-report',
                str(report)
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(report.read_text())


class GameInfoUiTests(LauncherFixture):
    def test_game_options_has_a_game_info_row_that_opens_the_page_and_back_returns(self):
        self.assertEqual(self.run_ui(OPEN_INFO)['page'], GAME_INFO)
        self.assertEqual(self.run_ui(OPEN_INFO + ['B'])['page'], GAME_OPTIONS)

    def test_page_works_for_a_game_with_nothing_downloaded_and_for_one_with_everything(self):
        keys = OPEN_INFO + ['Right', 'Down', 'Up', 'Left']
        self.assertEqual(self.run_ui(keys)['page'], GAME_INFO)
        write_fields(
            self.item / 'info.ini',
            dict(
                info_version=1,
                info_title='Puzzle Quest',
                publisher='Acme',
                genre='Puzzle',
                release='2007-05-09',
                synopsis_languages='EN'
            )
        )
        (self.item / 'synopsis.EN.txt').write_text('Match gems.\n\nWin the day. ' * 40)
        self.assertEqual(self.run_ui(keys)['page'], GAME_INFO)

    def test_a_game_without_information_keeps_the_page_reachable_while_fetching_is_off(self):
        write_fields(self.data / 'ui.ini', dict(schema=1, game_info=0, extra_pictures=3))
        self.assertEqual(self.run_ui(OPEN_INFO)['page'], GAME_INFO)

    def test_artwork_sources_can_switch_game_info_off_and_it_is_remembered(self):
        keys = ['Down', 'Down', 'A', 'Down', 'Down', 'Down', 'A']
        self.run_ui(keys, '--settings', '--section', 'library')
        self.assertIn('game_info=0\n', (self.data / 'ui.ini').read_text())
        self.assertIn('extra_pictures=0\n', (self.data / 'ui.ini').read_text())


class FacetFilterTests(LauncherFixture):
    """Search and filters > More filters: library filters from the saved game information."""
    def setUp(self):
        super().setUp()
        self.rom.unlink()
        games = [
            ('Puzzle One', 'Puzzle, Strategy', 'Acme', '2', 'ESRB E', '2007-05-09'),
            ('Puzzle Two', 'Puzzle', 'Nintendo', '1', 'ESRB E', '2011-01-01'),
            ('Quest', 'RPG', 'Acme', '1', 'ESRB T', '2004-06-01'),
            ('Homebrew', '', '', '', '', ''),
        ]
        for title, genre, publisher, players, rating, release in games:
            rom = self.root / (title + '.nds')
            rom.write_bytes(bytes(512))
            if genre:
                item = self.data / 'library' / game_key(rom.resolve())
                write_fields(
                    item / 'info.ini',
                    dict(
                        info_version=1,
                        genre=genre,
                        publisher=publisher,
                        players=players,
                        rating=rating,
                        release=release
                    )
                )

    # Home opens Search and filters; its third row opens the filter sheet. In the sheet the first chip (Unplayed) has
    # focus; Down moves to the nearest item on the next row: Genre chips, the Players slider, the Released slider,
    # Rating, More (With info / Without info), Publisher. Library: players 1/1/2, released 2007/2011/2004.
    OPEN_FILTERS = ['Home', 'Down', 'Down', 'A']

    def test_toggling_a_genre_chip_filters_the_library_and_says_so(self):
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'A'])
        self.assertEqual(report['page'], DISCOVER)
        self.assertTrue(report['discovery_active'])
        self.assertEqual(report['discovery_description'], 'Puzzle')
        self.assertEqual(report['filtered_games'], 2)

    def test_a_chip_toggles_off_again_and_x_resets_everything(self):
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'A', 'A'])
        self.assertFalse(report['discovery_active'])
        self.assertEqual(report['filtered_games'], 4)
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'A', 'Down', 'Right', 'X'])
        self.assertFalse(report['discovery_active'])
        self.assertEqual(report['filtered_games'], 4)

    def test_the_players_slider_narrows_by_range_with_the_arrow_keys(self):
        # Right moves the low handle from 1 to 2 players: only the two-player game is left.
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'Down', 'Right'])
        self.assertEqual(report['discovery_description'], '2 players')
        self.assertEqual(report['filtered_games'], 1)
        # Moving it back to the full extent clears the filter again.
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'Down', 'Right', 'Left'])
        self.assertFalse(report['discovery_active'])

    def test_the_release_slider_moves_either_end_and_a_picks_which(self):
        # A switches to the high handle; one step left drops 2011, leaving 2004 and 2007 (the game without a date
        # never matches a narrowed range).
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'Down', 'Down', 'A', 'Left'])
        self.assertEqual(report['discovery_description'], '2004–2010')
        self.assertEqual(report['filtered_games'], 2)
        report = self.run_ui(self.OPEN_FILTERS + ['Down', 'Down', 'Down', 'Right', 'Right', 'Right', 'Right'])
        self.assertEqual(report['discovery_description'], '2008–2011')
        self.assertEqual(report['filtered_games'], 1)

    def test_range_and_chip_filters_combine(self):
        keys = self.OPEN_FILTERS + ['Down', 'A', 'Down', 'Right']  # Puzzle, and two players or more
        report = self.run_ui(keys)
        self.assertEqual(report['discovery_description'], 'Puzzle  ·  2 players')
        self.assertEqual(report['filtered_games'], 1)

    def test_games_without_information_can_be_listed(self):
        # Five rows down is More: the nearest chip is Without info (1 game); With info has 3.
        report = self.run_ui(self.OPEN_FILTERS + ['Down'] * 5 + ['A'])
        self.assertEqual(report['discovery_description'], 'Without info')
        self.assertEqual(report['filtered_games'], 1)

    def test_played_state_chip_and_back_closes_the_sheet_keeping_the_filter(self):
        report = self.run_ui(self.OPEN_FILTERS + ['Right', 'A', 'B'])
        self.assertEqual(report['page'], DISCOVER)
        self.assertEqual(report['discovery_filter'], 2)
        self.assertEqual(report['discovery_description'], 'Played')

    def test_touch_taps_a_chip_and_drags_a_slider_handle(self):
        # The Genre chip "Puzzle" is the first of its row; the players track sits near the bottom of the first view.
        report = self.run_ui(self.OPEN_FILTERS, touches=['tap 80 220', 'release'])
        self.assertEqual(report['discovery_description'], 'Puzzle')
        report = self.run_ui(self.OPEN_FILTERS, touches=['tap 56 352', 'drag 300 352', 'drag 544 352', 'release'])
        self.assertEqual(report['discovery_description'], '2 players')
        self.assertEqual(report['filtered_games'], 1)


if __name__ == '__main__':
    unittest.main()
