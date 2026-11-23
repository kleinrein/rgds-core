"""The play journal and "Where you left off": the builder under sanitizers (tests/play_journal_test.cpp), and the
runner keeping the resume note only for a launch that really resumes."""
import time
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime'))


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'C++ compiler and SDL2 required')
class JournalBuilderTests(unittest.TestCase):
    def test_the_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'play_journal_test'
            flags = shlex.split(subprocess.check_output(['sdl2-config', '--cflags'], text=True))
            build = subprocess.run(
                [
                    'clang++', '-std=c++17', '-O1', '-g', '-pthread', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', *flags, '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/play_journal_test.cpp'), '-o',
                    str(binary)
                ],
                capture_output=True,
                text=True
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            data = Path(tmp) / 'data'
            data.mkdir()
            run = subprocess.run([str(binary), str(data)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('journal: ok', run.stdout)


class ResumeNoteTests(unittest.TestCase):
    def test_note_only_when_the_game_resumes(self):
        from emulators import keep_resume_note
        note = {'RGDS_RESUME_NOTE': 'Where you left off\tMetroid II\t2 days ago'}
        plan = {'command': ['engine', 'game.gb'], 'env': dict(note, RGDS_NATIVE_AUTOLOAD='1')}
        keep_resume_note(plan)
        self.assertIn('RGDS_RESUME_NOTE', plan['env'])
        plan = {'command': ['dscore', 'game.nds', '--load-state', '/x/auto.dss'], 'env': dict(note)}
        keep_resume_note(plan)
        self.assertIn('RGDS_RESUME_NOTE', plan['env'])
        plan = {'command': ['engine', 'game.gb'], 'env': dict(note)}
        keep_resume_note(plan)
        self.assertNotIn('RGDS_RESUME_NOTE', plan['env'])

    def test_last_frame_is_kept_only_from_this_session(self):
        from emulators import keep_last_frame
        from library import game_key
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            rom = data / 'game.gb'
            rom.write_bytes(b'x')
            (data / 'session').mkdir()
            capture = data / 'session/menu-background.bmp'
            capture.write_bytes(b'BMframe')
            kept = keep_last_frame(data, data, rom, time.time() - 60)
            self.assertEqual(kept, data / 'library' / game_key(rom) / 'last-frame.bmp')
            self.assertEqual(kept.read_bytes(), b'BMframe')
            kept.unlink()
            # A capture from before this session belongs to another game.
            self.assertIsNone(keep_last_frame(data, data, rom, time.time() + 60))
            self.assertFalse((data / 'library' / game_key(rom) / 'last-frame.bmp').exists())

    def test_engine_frame_wins_and_ds_checkpoint_stands_in(self):
        import os
        from emulators import keep_last_frame
        from library import game_key
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            rom = data / 'game.nds'
            rom.write_bytes(b'x')
            folder = data / 'library' / game_key(rom)
            folder.mkdir(parents=True)
            (data / 'session').mkdir()
            (data / 'session/menu-background.bmp').write_bytes(b'BMmenu')
            since = time.time() - 60
            # RG DS Play wrote its own as it quit: kept, the menu's capture not copied over it.
            (folder / 'last-frame.bmp').write_bytes(b'BMengine')
            self.assertEqual(keep_last_frame(data, data, rom, since, 'rgds-play').read_bytes(), b'BMengine')
            # No engine frame and no capture: the 128x96 top screen inside a DSCore checkpoint.
            (folder / 'last-frame.bmp').unlink()
            (data / 'session/menu-background.bmp').unlink()
            head = bytearray(64 + 128 * 96 * 2)
            head[:4], head[8:12] = b'DSST', b'HEAD'
            head[56:60], head[60:64] = (128).to_bytes(4, 'little'), (96).to_bytes(4, 'little')
            head[64:66] = b'\xff\xff'
            (folder / 'auto-rgds-experimental.dss').write_bytes(bytes(head))
            kept = keep_last_frame(data, data, rom, since, 'rgds-experimental')
            self.assertEqual(kept.read_bytes()[:2], b'BM')
            # An old checkpoint is an earlier session's.
            kept.unlink()
            os.utime(folder / 'auto-rgds-experimental.dss', (since - 600, since - 600))
            self.assertIsNone(keep_last_frame(data, data, rom, since, 'rgds-experimental'))


if __name__ == '__main__':
    unittest.main()
