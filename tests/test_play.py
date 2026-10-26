"""RG DS Play end to end: the test cartridge in the real libmgba, headless.

Build first: python3 tools/build_play.py host --fetch  (the cartridge needs Zig).
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHOT = ROOT / 'build/play/host/play-shot'
UNIT = ROOT / 'build/play/host/play-tests'

PANEL_W, PANEL_H = 640, 480
# The test cartridge's map (play/profiles/gba/ZRDT.ini): crop 32,24 176x112, shown 3x
# centred on the bottom panel.
MAP_SCALE, MAP_X, MAP_Y = 3, (640 - 176 * 3) // 2, (480 - 112 * 3) // 2
MAP_FIELD = (0x29, 0x00, 0x7B)
VISITED = (0x63, 0xB5, 0xFF)
UNVISITED = (0x42, 0x21, 0xA5)

# Start, walk right into room 6, down into room 10.
EXPLORE = """60 START
64 -
120 RIGHT
220 -
330 DOWN
420 -
"""


def read_bmp(path):
    data = Path(path).read_bytes()
    offset, = struct.unpack_from('<I', data, 10)
    w, h = struct.unpack_from('<ii', data, 18)
    row = (w * 3 + 3) & ~3

    def pixel(x, y):
        at = offset + (h - 1 - y) * row + x * 3
        b, g, r = data[at:at + 3]
        return r, g, b

    return w, h, pixel


def near(a, b, tolerance=10):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def cell_point(cx, cy):
    """A point inside map cell (cx, cy), away from the player marker, on the stacked shot."""
    gx, gy = 40 + cx * 40 + 3, 32 + cy * 32 + 3
    return MAP_X + (gx - 32) * MAP_SCALE, PANEL_H + MAP_Y + (gy - 24) * MAP_SCALE


@unittest.skipUnless(SHOT.exists() and shutil.which('zig'), 'Build RG DS Play first: tools/build_play.py host')
class PlayEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # By path: another suite may already have imported tools/build.py as `build`.
        spec = importlib.util.spec_from_file_location('rgds_play_testrom', ROOT / 'play/tests/testrom/build.py')
        testrom = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(testrom)
        cls.cart_dir = tempfile.TemporaryDirectory()
        cls.rom = testrom.build(Path(cls.cart_dir.name) / 'Play Test Rooms (World).gba')

    @classmethod
    def tearDownClass(cls):
        cls.cart_dir.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = Path(temp.name)
        self.data = self.dir / 'userdata/play'
        self.out = self.dir / 'shots'
        self.out.mkdir()

    def play(self, script, *extra, rom=None):
        path = self.dir / 'script.txt'
        path.write_text(script)
        result = subprocess.run(
            [
                SHOT, '--rom', rom or self.rom, '--data', self.data, '--profiles', ROOT / 'play/profiles/gba',
                '--fonts', ROOT / 'assets/fonts', '--script', path, '--out', self.out, *extra
            ],
            capture_output=True,
            text=True,
            timeout=120
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_unit_suite(self):
        result = subprocess.run([UNIT], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(' 0 failed', result.stdout)

    def test_live_map_on_the_second_screen(self):
        output = self.play('10 shot title\n' + EXPLORE + '500 shot explored\n600 quit\n')
        self.assertIn('profile', output)
        self.assertIn('ZRDT', output)
        w, h, title = read_bmp(self.out / 'title.bmp')
        self.assertEqual((w, h), (PANEL_W, PANEL_H * 2))
        self.assertFalse(near(title(4, PANEL_H + 4), MAP_FIELD))  # no map before the game starts

        _, _, px = read_bmp(self.out / 'explored.bmp')
        self.assertTrue(near(px(4, PANEL_H + 4), MAP_FIELD), px(4, PANEL_H + 4))  # edge to edge
        for room in ((1, 1), (2, 1), (2, 2)):
            self.assertTrue(near(px(*cell_point(*room)), VISITED), (room, px(*cell_point(*room))))
        for room in ((0, 0), (3, 2)):
            self.assertTrue(near(px(*cell_point(*room)), UNVISITED), (room, px(*cell_point(*room))))
        # The top panel still shows the game, not the map.
        self.assertFalse(near(px(320, 240), MAP_FIELD))

    def test_the_game_is_never_touched(self):
        script = EXPLORE + '440 SELECT\n444 -\n520 B\n524 -\n800 quit\n'
        hashes = {}
        for name, extra in (('alone', ('--no-companion',)), ('sync', ()), ('async', ('--async',))):
            path = self.dir / f'{name}.txt'
            self.play(script, '--hashes', path, *extra)
            hashes[name] = path.read_text()
            shutil.rmtree(self.data)
        self.assertEqual(len(hashes['alone'].splitlines()), 800)  # frames 0-799
        self.assertEqual(hashes['sync'], hashes['alone'])
        self.assertEqual(hashes['async'], hashes['alone'])

    def test_battery_save_round_trip(self):
        self.play(EXPLORE + '600 quit\n')
        save = self.data / 'saves/gba/Play Test Rooms (World).sav'
        data = save.read_bytes()
        self.assertEqual(data[:2], b'RG')
        visited = data[2] | data[3] << 8
        self.assertEqual(visited, (1 << 5) | (1 << 6) | (1 << 10))
        self.assertEqual(data[4], 10)
        self.assertFalse(save.with_name(save.name + '.writing').exists())

        # The next start keeps a backup and picks up where the save left off:
        # the map, captured on the first look, already shows the explored rooms.
        self.play('60 START\n64 -\n200 shot resumed\n240 quit\n')
        backup = self.data / 'saves/gba/backups/Play Test Rooms (World).sav.1'
        self.assertEqual(backup.read_bytes(), data)
        _, _, px = read_bmp(self.out / 'resumed.bmp')
        self.assertTrue(near(px(*cell_point(2, 1)), VISITED))

    def test_save_next_to_the_rom_is_copied_not_moved(self):
        rom = self.dir / 'Roms/GBA/Play Test Rooms (World).gba'
        rom.parent.mkdir(parents=True)
        shutil.copy(self.rom, rom)
        outside = rom.with_suffix('.srm')
        stock = b'RG' + bytes([1 << 3, 0, 3]) + bytes(32768 - 5)
        outside.write_bytes(stock)
        before = outside.stat()
        output = self.play('60 START\n64 -\n200 shot imported\n240 quit\n', rom=rom)
        self.assertIn('(imported)', output)
        self.assertEqual(outside.read_bytes(), stock)
        self.assertEqual(outside.stat().st_mtime_ns, before.st_mtime_ns)
        _, _, px = read_bmp(self.out / 'imported.bmp')
        self.assertTrue(near(px(*cell_point(3, 0)), VISITED))  # room 3 came from the outside save

    def test_ds_data_is_left_alone(self):
        userdata = self.data.parent
        files = {
            'preferences.ini': b'schema=1\nmode=dark\n',
            'ui.ini': b'view_transition_style=push\n',
            'library/0123456789abcdef/stats.ini': b'seconds=3600\nsessions=4\n',
            'library/0123456789abcdef/game.ini': b'core=4\n',
            'saves/drastic/Mario Kart DS.dsv': bytes(range(256)) * 64,
        }
        for name, content in files.items():
            path = userdata / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

        def snapshot():
            out = {}
            for path in userdata.rglob('*'):
                if path.is_file() and 'play' not in path.relative_to(userdata).parts[:1]:
                    out[str(path.relative_to(userdata))
                        ] = (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            return out

        before = snapshot()
        self.play(EXPLORE + '600 quit\n')
        self.assertEqual(snapshot(), before)
        written = sorted(str(p.relative_to(userdata)) for p in userdata.rglob('*') if p.is_file())
        self.assertTrue(all(name.startswith('play/') or name in files for name in written), written)

    def test_checkpoint_state_round_trip(self):
        # What the in-game menu's Save / Load do through the bridge (F5 / F7):
        # loading the state saved before frame 300 at frame 450, with the same
        # buttons held, gives frame 300's picture again.
        slot = self.dir / 'states/Play Test Rooms_8.dss'
        output = self.play(
            EXPLORE + f'300 state-save {slot}\n450 state-load {slot}\n460 quit\n', '--hashes', self.dir / 'hashes.txt'
        )
        self.assertIn('frame 300 state-save ok', output)
        self.assertIn('frame 450 state-load ok', output)
        self.assertGreater(slot.stat().st_size, 1000)
        self.assertFalse(slot.with_name(slot.name + '.writing').exists())
        hashes = dict(line.split() for line in (self.dir / 'hashes.txt').read_text().splitlines())
        self.assertEqual(hashes['450'], hashes['300'])
        self.assertNotEqual(hashes['449'], hashes['299'])  # it really went back
        missing = self.play(f'10 state-load {self.dir / "none.dss"}\n20 quit\n')
        self.assertIn('state-load failed', missing)

    def test_checkpoint_carries_the_cartridge_ram(self):
        # The auto-load puts a checkpoint into a game before its first frame,
        # beside whatever battery save is on the card. The checkpoint brings
        # the cartridge RAM it was saved with (Super Mario Land 3 keeps its
        # working variables there and hung without it), into memory only.
        slot, again = self.dir / 'first.dss', self.dir / 'again.dss'
        self.play(EXPLORE + f'300 state-save {slot}\n301 quit\n')
        # The rooms the cartridge saved: 'RG', the visited rooms, the room, then unused (FF) RAM.
        saved = slot.read_bytes()
        at = saved.find(b'RG', 0)
        while at >= 0 and saved[at + 5:at + 69] != b'\xff' * 64:
            at = saved.find(b'RG', at + 1)
        self.assertGreaterEqual(at, 0, 'no cartridge RAM in the checkpoint')
        rooms = saved[at:at + 5]
        self.data = self.dir / 'elsewhere/play'  # no battery save at all
        self.play(f'0 state-load {slot}\n10 state-save {again}\n11 quit\n')
        self.assertIn(rooms + b'\xff' * 64, again.read_bytes())
        self.assertEqual([p for p in (self.data / 'saves').rglob('*.sav') if p.read_bytes().startswith(b'RG')], [])

    def test_hud_split_moves_layers_without_touching_the_game(self):
        # The test cartridge draws everything on BG2 (a bitmap mode): a profile
        # that calls BG2 the HUD while in the rooms empties the game picture and
        # fills the HUD, and the emulator state stays the same every frame.
        profiles = self.dir / 'profiles'
        profiles.mkdir()
        text = (ROOT / 'play/profiles/gba/ZRDT.ini').read_text()
        (profiles / 'ZRDT.ini').write_text(text + '\n[hud]\non = mem8[02000000] == 1\nbg = 4\nbackdrop = #102030\n')
        script = EXPLORE + '300 status\n300 shot split\n300 hudshot hud\n440 SELECT\n444 -\n470 status\n800 quit\n'
        (self.dir / 'script.txt').write_text(script)
        states = {}
        for name, extra in (('alone', ('--no-companion',)), ('split', ())):
            path = self.dir / f'{name}-states.txt'
            result = subprocess.run(
                [
                    SHOT, '--rom', self.rom, '--data', self.dir / name, '--profiles', profiles, '--script',
                    self.dir / 'script.txt', '--out', self.out, '--state-hashes', path, *extra
                ],
                capture_output=True,
                text=True,
                timeout=120
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            states[name] = path.read_text()
            if name == 'split':
                self.assertRegex(result.stdout, r'frame 300 status .* hud=1 split=1')
                self.assertRegex(
                    result.stdout, r'frame 470 status .* hud=0 split=0'
                )  # the map screen is not "in the rooms"
        self.assertEqual(states['split'], states['alone'])
        _, _, px = read_bmp(self.out / 'split.bmp')
        self.assertEqual(px(320, 240), (0, 0, 0))  # the room left the game screen
        self.assertFalse(near(px(320, PANEL_H + 240), (0x10, 0x20, 0x30)))  # and is on the second one
        w, h, hud = read_bmp(self.out / 'hud.bmp')
        self.assertEqual((w, h), (240, 160))
        self.assertTrue(near(hud(10, 10), (0xC6, 0x63, 0x21), 12), hud(10, 10))  # room 6's colour

    @unittest.skipUnless(os.environ.get('RGDS_PLAY_ROMS'), 'Set RGDS_PLAY_ROMS to a folder of your own dumps')
    def test_super_circuit_race(self):
        roms = [p for p in Path(os.environ['RGDS_PLAY_ROMS']).glob('*.gba') if p.read_bytes()[0xAC:0xAF] == b'AMK']
        if not roms:
            self.skipTest('no Mario Kart: Super Circuit dump in RGDS_PLAY_ROMS')
        # Grand Prix, Mushroom Cup, Peach Circuit with A alone; then drive.
        script = ''.join(f'{f} A\n{f + 4} -\n' for f in range(600, 2400, 90))
        script += '2400 A\n3000 status\n3000 shot race\n3001 quit\n'
        states = {}
        for name, extra in (('alone', ('--no-companion',)), ('split', ())):
            path = self.dir / f'{name}.txt'
            output = self.play(script, '--state-hashes', path, *extra, rom=roms[0])
            states[name] = path.read_text()
        self.assertIn('hud=1 split=1', output)
        self.assertEqual(states['split'], states['alone'])
        _, _, px = read_bmp(self.out / 'race.bmp')
        # TIME is no longer on the game screen (its top right is sky) but its
        # digits are on the second screen's top right.
        top = [px(x, 90) for x in range(400, 520, 4)]
        self.assertFalse(any(r > 200 and g > 120 and b < 60 for r, g, b in top), top)
        bottom = [px(x, PANEL_H + 40) for x in range(440, 620, 2)]
        self.assertTrue(any(r > 200 and g > 120 and b < 80 for r, g, b in bottom), bottom)


if __name__ == '__main__':
    unittest.main()
