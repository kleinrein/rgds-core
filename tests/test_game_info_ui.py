"""The Game info page: reached from Game options > Artwork, with and without downloaded information."""
import json, os, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))
from fields import write_fields
from library import game_key

ARTWORK, GAME_INFO = 26, 40  # Page enum values in src/launcher/settings.hpp
OPEN_ARTWORK = ['X', 'Down', 'Down', 'Down', 'Down', 'A']


class GameInfoUiTests(unittest.TestCase):
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

    def run_ui(self, keys, *mode):
        script = self.root / 'input'
        script.write_text(''.join(f'{300+i*180} key {key}\n{340+i*180} key-up {key}\n' for i, key in enumerate(keys)))
        report = self.root / 'report.json'
        result = subprocess.run(
            [
                str(ROOT / 'build/host/rgds-launcher'), '--preview', *(mode or ('--library',)), '--rom-dir',
                str(self.root), '--data',
                str(self.data), '--preferences',
                str(self.prefs), '--state',
                str(self.root / 'state'), '--input-script',
                str(script), '--seconds', '3', '--cache-report',
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

    def test_artwork_page_has_a_game_info_row_that_opens_the_page_and_back_returns(self):
        self.assertEqual(self.run_ui(OPEN_ARTWORK + ['Down', 'Down', 'Down', 'A'])['page'], GAME_INFO)
        self.assertEqual(self.run_ui(OPEN_ARTWORK + ['Down', 'Down', 'Down', 'A', 'B'])['page'], ARTWORK)

    def test_page_works_for_a_game_with_nothing_downloaded_and_for_one_with_everything(self):
        keys = OPEN_ARTWORK + ['Down', 'Down', 'Down', 'A', 'Right', 'Down', 'Up', 'Left']
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
        self.assertEqual(self.run_ui(OPEN_ARTWORK + ['Down', 'Down', 'Down', 'A'])['page'], GAME_INFO)

    def test_artwork_sources_can_switch_game_info_off_and_it_is_remembered(self):
        keys = ['Down', 'Down', 'A', 'Down', 'Down', 'Down', 'A']
        self.run_ui(keys, '--settings', '--section', 'library')
        self.assertIn('game_info=0\n', (self.data / 'ui.ini').read_text())
        self.assertIn('extra_pictures=0\n', (self.data / 'ui.ini').read_text())


if __name__ == '__main__':
    unittest.main()
