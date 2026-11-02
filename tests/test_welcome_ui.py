"""Setup on a new card: when it appears, that it can be walked and skipped, and that choices reach the normal files."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
WELCOME_PAGE = 47  # Page::Welcome
TIMEZONE_PAGE = 10  # Page::Timezone


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class WelcomeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name).resolve()

    def run_ui(self, script='', first_run=True, seconds=2):
        (self.data / 'input.txt').write_text(script)
        command = [
            str(BINARY), '--preview', '--boot-startup', '--data',
            str(self.data), '--input-script',
            str(self.data / 'input.txt'), '--cache-report',
            str(self.data / 'report.json'), '--seconds',
            str(seconds)
        ]
        if first_run:
            command.append('--first-run')
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            capture_output=True,
            text=True,
            timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.data / 'report.json').read_text())

    @staticmethod
    def keys(*names, start=300, gap=120):
        return ''.join(f'{start + i * gap} key {name}\n' for i, name in enumerate(names))

    def setup_file(self):
        return (self.data / 'setup.ini').read_text()

    def ini(self, name):
        path = self.data / name
        return path.read_text() if path.exists() else ''

    def test_a_new_card_opens_setup_at_the_first_step(self):
        report = self.run_ui()
        self.assertEqual(report['page'], WELCOME_PAGE)
        self.assertEqual(self.setup_file(), 'step=0\n')

    def test_only_with_first_run(self):
        self.assertNotEqual(self.run_ui(first_run=False)['page'], WELCOME_PAGE)
        self.assertFalse((self.data / 'setup.ini').exists())

    def test_a_used_card_is_left_alone_for_good(self):
        (self.data / 'ui.ini').write_text('schema=1\n')
        self.assertNotEqual(self.run_ui()['page'], WELCOME_PAGE)
        self.assertEqual(self.setup_file(), 'done=1\n')

    def test_skip_goes_to_the_end_and_start_finishes_for_good(self):
        report = self.run_ui(self.keys('x', 'a'))
        self.assertNotEqual(report['page'], WELCOME_PAGE)
        self.assertEqual(self.setup_file(), 'done=1\n')
        self.assertNotEqual(self.run_ui()['page'], WELCOME_PAGE)

    def test_it_comes_back_at_the_step_it_stopped_on(self):
        (self.data / 'setup.ini').write_text('step=4\n')
        self.assertEqual(self.run_ui()['page'], WELCOME_PAGE)
        self.assertEqual(self.setup_file(), 'step=4\n')

    def test_choosing_a_language_sets_time_defaults_and_advances(self):
        # Norsk bokmål: the ninth language, at the top of the second column.
        self.run_ui(self.keys('Right', 'a'))
        self.assertIn('language=nb-NO', self.ini('ui.ini') + self.ini('preferences.ini'))
        self.assertIn('timezone=Europe/Oslo', self.ini('system.ini'))
        self.assertEqual(self.setup_file(), 'step=1\n')

    def test_the_name_reaches_ds_games_and_pictochat(self):
        (self.data / 'setup.ini').write_text('step=2\n')
        self.run_ui(
            self.keys('a') + ''.join(f'{700 + i * 60} text {c}\n' for i, c in enumerate('Ada')) + '1100 key Return\n'
        )
        self.assertIn('nickname=Ada', self.ini('ds.ini'))
        self.assertIn('name=Ada', self.ini('pictochat.ini'))
        self.assertEqual(self.setup_file(), 'step=3\n')

    def test_home_choices_are_saved(self):
        (self.data / 'setup.ini').write_text('step=3\n')
        # Next is focused: Up lands on the typeface row, Up again on Animations.
        self.run_ui(self.keys('Up', 'Up', 'Left', 'a'))
        text = self.ini('ui.ini')
        self.assertIn('view_animations=', text)

    def test_systems_is_a_big_choice_of_its_own(self):
        (self.data / 'setup.ini').write_text('step=4\n')
        self.run_ui(self.keys('Left', 'a'))
        self.assertIn('show_systems=0', self.ini('ui.ini'))
        self.assertEqual(self.setup_file(), 'step=5\n')

    def test_a_new_card_shows_all_systems_unless_told_otherwise(self):
        self.run_ui()
        self.assertIn('show_systems=1', self.ini('ui.ini'))

    def test_coming_back_keeps_a_ds_only_choice(self):
        (self.data / 'setup.ini').write_text('step=7\n')
        (self.data / 'ui.ini').write_text('schema=1\nshow_systems=0\n')
        self.run_ui()
        self.assertIn('show_systems=0', self.ini('ui.ini'))

    def test_timezone_is_a_detour_back_into_setup(self):
        (self.data / 'setup.ini').write_text('step=5\n')
        report = self.run_ui(self.keys('Up', 'Up', 'a'))
        self.assertEqual(report['page'], TIMEZONE_PAGE)
        report = self.run_ui(self.keys('Up', 'Up', 'a', 'b'))
        self.assertEqual(report['page'], WELCOME_PAGE)

    def test_touch_presses_next(self):
        report = self.run_ui('300 finger-down 0.85 0.969\n360 finger-up 0.85 0.969\n')
        self.assertEqual(report['page'], WELCOME_PAGE)
        self.assertEqual(self.setup_file(), 'step=1\n')

    def test_every_setup_string_is_translated_everywhere(self):
        source = (ROOT / 'src/launcher/welcome_screen.hpp').read_text()
        english = {line.split('\t')[0] for line in (ROOT / 'assets/locales/en-US.tsv').read_text().splitlines()}
        used = {key for key in english if len(key) > 8 and '"' + key + '"' in source}
        self.assertIn('Skip setup', used)
        self.assertIn('What do you want to play?', used)
        for path in (ROOT / 'assets/locales').glob('*-*.tsv'):
            have = {line.split('\t')[0] for line in path.read_text().splitlines()}
            self.assertEqual(used - have, set(), path.name)


class OwnerNameTests(unittest.TestCase):
    def test_ds_games_fall_back_to_the_owner_name_and_a_game_still_wins(self):
        source = (ROOT / 'runtime/emulators.py').read_text()
        self.assertIn("ds_input.get('nickname') or read_fields(data / 'ds.ini').get('nickname')", source)


if __name__ == '__main__':
    unittest.main()
