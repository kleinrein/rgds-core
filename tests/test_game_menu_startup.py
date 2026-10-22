"""Real launcher first-frame reads and opt-in startup timing with dummy SDL."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from library import game_key

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'


@unittest.skipUnless(BINARY.exists(), 'Build the host launcher first')
class GameMenuStartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.data = self.root / 'data'
        self.rom = self.root / 'fixture.nds'
        self.rom.write_bytes(bytes(0x640))
        self.item = self.data / 'library' / game_key(self.rom)
        self.item.mkdir(parents=True)
        (self.item / 'info.ini').write_text('title=Original displayed title\n')
        self.report = self.root / 'report.json'
        self.image = self.root / 'screen.bmp'

    def run_ui(self, extra=(), profile=False):
        command = [
            str(BINARY), '--preview', '--game-menu', '--game-rom',
            str(self.rom), '--app',
            str(ROOT), '--data',
            str(self.data), '--cache-report',
            str(self.report), *extra
        ]
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', RGDS_LAUNCH_PROFILE='1' if profile else '0'),
            capture_output=True,
            text=True,
            timeout=10
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(self.report.read_text()), [
            json.loads(line) for line in result.stderr.splitlines() if line.startswith('{"type":"rgds-launch-profile"')
        ]

    def test_quick_menu_reads_title_once_and_profile_emits_once(self):
        counts, profiles = self.run_ui(('--frames', '20'), True)
        self.assertEqual(counts['page'], 12)
        self.assertEqual(counts['metadata_reads'], 1, 'The game identity is loaded once, not every frame')
        self.assertEqual(counts['banner_reads'], 0)
        self.assertEqual(len(profiles), 1)
        report = profiles[0]
        self.assertTrue(report['game_menu'])
        stages = report['milestones']
        names = [row['stage'] for row in stages]
        self.assertEqual(names[0], 'arguments-ready')
        self.assertEqual(names[-1], 'first-present')
        for stage in (
            'sdl-library-loaded', 'configuration-ready', 'sdl-init-start', 'sdl-init-done', 'window-created',
            'renderer-created', 'assets-start', 'assets-ready'
        ):
            self.assertIn(stage, names)
        times = [row['elapsed_us'] for row in stages]
        self.assertEqual(times, sorted(times))
        _, disabled = self.run_ui(('--frames', '1'))
        self.assertEqual(disabled, [])

    def test_artwork_page_still_loads_and_displays_game_title(self):
        state = self.root / 'navigation'
        script = self.root / 'input'
        script.write_text('20 key A\n')

        def pixels(title):
            (self.item / 'info.ini').write_text('title=' + title + '\n')
            state.write_text('1 0 1 ""\n19 0 4\n')
            counts, _ = self.run_ui(
                (
                    '--state', str(state), '--input-script', str(script), '--seconds', '1', '--screenshot',
                    str(self.image)
                )
            )
            self.assertEqual(counts['page'], 26)
            self.assertGreater(counts['metadata_reads'], 0)
            data = self.image.read_bytes()
            offset = struct.unpack_from('<I', data, 10)[0]
            width, height = struct.unpack_from('<ii', data, 18)
            self.assertEqual(width, 640)
            # Only the title region; clock/status changes cannot satisfy this.
            return b''.join(
                data[offset + (height - 1 - y) * width * 4:offset + (height - y) * width * 4] for y in range(314, 348)
            )

        self.assertNotEqual(pixels('Original displayed title'), pixels('Replacement displayed title'))


if __name__ == '__main__': unittest.main()
