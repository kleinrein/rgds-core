"""The second card in the launcher (src/launcher/games_card.hpp): a stock games card's games beside the main card's,
gone when the card is taken out, back as they were when it returns."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/host/rgds-launcher'
sys.path.insert(0, str(ROOT / 'runtime'))
from library import game_key, playtime, record_playtime


@unittest.skipUnless(BINARY.exists(), 'Build host launcher first')
class GamesCardUITests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / 'userdata'
        self.data.mkdir()
        self.roms = self.root / 'roms'  # the system card's /data/roms
        (self.roms / 'nds').mkdir(parents=True)
        (self.roms / 'snes').mkdir()
        (self.roms / 'nds/Home.nds').write_bytes(bytes(512))
        self.card = self.root / 'tf2'  # /media/tf2, a stock Anbernic games card
        for folder, name in (
            ('NDS', 'Kart.nds'), ('NDS', 'Zelda Hourglass.nds'), ('SFC', 'Zelda.sfc'), ('SFC', 'Mario World.sfc')
        ):
            (self.card / 'Roms' / folder).mkdir(parents=True, exist_ok=True)
            (self.card / 'Roms' / folder / name).write_bytes(bytes(512))
        (self.card / 'Roms/MDCD').mkdir()  # a system the session has no folder for: not listed
        (self.card / 'Roms/MDCD/Sonic CD.cue').write_text('FILE "Sonic CD.bin" BINARY\n')
        self.state = self.root / 'games-card.ini'

    def insert(self):
        self.state.write_text(f'point={self.card}\ndevice=/dev/mmcblk2p1\nfs=exfat\nid=0353\nlabel=GAMES\n')

    def launch(self, seconds=1, show_systems=False):
        ui = 'schema=1\nhome_layout=carousel\nbackground=plain\n' + ('show_systems=1\n' if show_systems else '')
        (self.data / 'ui.ini').write_text(ui)
        (self.data / 'input').write_text('')
        return subprocess.Popen(
            [
                str(BINARY), '--preview', '--data',
                str(self.data), '--rom-dir',
                str(self.roms / 'nds'), '--system-dir', 'snes=' + str(self.roms / 'snes'), '--games-card',
                str(self.state), '--input-script',
                str(self.data / 'input'), '--seconds',
                str(seconds), '--cache-report',
                str(self.data / 'report.json')
            ],
            cwd=ROOT,
            env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

    def report(self, process):
        output, _ = process.communicate(timeout=40)
        self.assertEqual(process.returncode, 0, output)
        return json.loads((self.data / 'report.json').read_text())

    def test_the_cards_games_join_the_library(self):
        self.insert()
        report = self.report(self.launch())
        self.assertTrue(report['games_card'])
        self.assertEqual(report['library_count'], 3)  # the DS games of both cards
        report = self.report(self.launch(show_systems=True))
        self.assertEqual(report['all_games'], 5)  # and the Super Nintendo's, from the card's SFC folder

    def test_no_card_no_change(self):
        report = self.report(self.launch())
        self.assertFalse(report['games_card'])
        self.assertEqual(report['library_count'], 1)

    def test_taken_out_its_games_leave_and_come_back_with_their_play_time(self):
        kart = (self.card / 'Roms/NDS/Kart.nds').resolve()
        record_playtime(self.data, kart, 3600, 4, '2026-11-20T10:00:00+00:00')
        self.insert()
        process = self.launch(seconds=7)
        time.sleep(2)  # the first scan has listed the card's games; the next look (every 4 s) finds it gone
        self.state.unlink()
        report = self.report(process)
        self.assertFalse(report['games_card'])
        self.assertEqual(report['library_count'], 1)
        # nothing of the card's games was dropped from userdata
        self.assertEqual(playtime(self.data, kart), (3600, 4))
        self.assertTrue((self.data / 'library' / game_key(kart) / 'stats.ini').is_file())
        # put back in a running menu: the games return
        process = self.launch(seconds=7)
        time.sleep(2)
        self.insert()
        report = self.report(process)
        self.assertTrue(report['games_card'])
        self.assertEqual(report['library_count'], 3)


if __name__ == '__main__':
    unittest.main()
