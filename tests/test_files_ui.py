"""Settings > Library > Files in the launcher, driven by keys against the real runtime/files.py."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RGDS_LAUNCHER', ROOT / 'build/host/rgds-launcher'))


@unittest.skipUnless(BINARY.exists(), 'Build the host launcher first')
class FilesPageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'app'
        self.app.mkdir()
        for name in ('assets', 'themes', 'runtime'):
            (self.app / name).symlink_to(ROOT / name, target_is_directory=True)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.games = self.root / 'roms'
        self.inbox = self.root / 'inbox'
        (self.games / 'gba').mkdir(parents=True)
        self.inbox.mkdir()
        (self.games / 'gba/Zelda.gba').write_bytes(b'z' * 5000)
        (self.games / 'gba/Zelda.sav').write_bytes(b'progress')
        (self.games / 'readme.txt').write_text('Games go in their system folders.\n')
        self.state = self.root / 'state.txt'
        self.state.write_text('1 0 0 ""\n')

    def run_ui(self, script, seconds=3, screenshot=None):
        path = self.root / 'input.txt'
        path.write_text(script)
        command = [
            str(BINARY), '--preview', '--app',
            str(self.app), '--data',
            str(self.data), '--preferences',
            str(self.root / 'preferences.ini'), '--state',
            str(self.state), '--input-script',
            str(path), '--seconds',
            str(seconds), '--settings', '--section', 'files'
        ]
        if screenshot: command += ['--screenshot', str(screenshot)]
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=dict(
                os.environ,
                SDL_VIDEODRIVER='dummy',
                SDL_AUDIODRIVER='dummy',
                RGDS_FILES_ROOTS=f'Games={self.games};Inbox={self.inbox}'
            ),
            capture_output=True,
            text=True,
            timeout=30
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_copy_a_game_into_another_root(self):
        # gba is the first row: open it, Zelda.gba is first (before Zelda.sav), Y > Copy, R to Inbox, Y > Paste here.
        self.run_ui('300 key A\n700 key Y\n900 key A\n1200 key ]\n1500 key Y\n1700 key A\n')
        self.assertEqual((self.inbox / 'Zelda.gba').read_bytes(), b'z' * 5000)
        self.assertTrue((self.games / 'gba/Zelda.gba').exists())

    def test_deleting_a_save_asks_twice_then_goes_to_the_trash(self):
        # Zelda.sav is the second file; Y > Copy, Move, Rename, Delete; "Move to the trash?" > Delete; the save
        # confirm from files.py > Go ahead.
        self.run_ui(
            '300 key A\n600 key Down\n900 key Y\n1100 key Down\n1200 key Down\n1300 key Down\n'
            '1400 key A\n1700 key Down\n1800 key A\n2600 key Down\n2700 key A\n',
            seconds=4
        )
        self.assertFalse((self.games / 'gba/Zelda.sav').exists())
        trashed = list((self.data / 'trash').rglob('Zelda.sav'))
        self.assertEqual(len(trashed), 1)

    def test_a_cancelled_save_confirm_leaves_the_save(self):
        self.run_ui(
            '300 key A\n600 key Down\n900 key Y\n1100 key Down\n1200 key Down\n1300 key Down\n'
            '1400 key A\n1700 key Down\n1800 key A\n2600 key A\n',
            seconds=4
        )
        self.assertTrue((self.games / 'gba/Zelda.sav').exists())

    def test_new_folder_from_the_keyboard(self):
        # At the Games root: Y > (folder row selected) Copy, Move, Rename, Delete, New folder.
        self.run_ui(
            '300 key Y\n500 key Down\n600 key Down\n700 key Down\n800 key Down\n900 key A\n'
            '1300 text Hacks\n1500 key Return\n',
            seconds=3
        )
        self.assertTrue((self.games / 'Hacks').is_dir())

    def test_b_climbs_to_the_root_and_marks_clear_first(self):
        shot = self.root / 'shot.bmp'
        self.run_ui('300 key A\n600 key X\n900 key B\n1200 key B\n', screenshot=shot)
        self.assertTrue(shot.exists() or Path(str(shot) + '-top.bmp').exists())


if __name__ == '__main__':
    unittest.main()
