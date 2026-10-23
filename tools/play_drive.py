#!/usr/bin/env python3
"""Race Mario Kart: Super Circuit headless, to test companion profiles over whole races.

  python3 tools/play_drive.py --rom GAME.gba --state CHECKPOINT --out DIR [--frames 10000] [--items]

Starts build/play/host/play-shot --drive from a checkpoint taken on a course
(the player's _8.dss), steers the player's kart along the racing line the
computer karts drive (each one's trail is recorded; the player aims a few
points ahead on the nearest trail), and prints every change of the HUD's
registers and the split, with a shot of both screens at each change and every
1200 frames. DIR/inputs.txt holds the buttons it pressed: replay them through
the real player with RGDS_PLAY_INPUT (see play/src/frontend/main.cpp), on the
Mac or on the handheld.

The kart table is in IWRAM at 0x03003B98, 0x1A0 bytes a kart, the player
first: 16.16 position at +0 and +4, heading at +0x56 (65536 a turn, 0 up the
screen, turning left lowers it). Found in the European release (AMKP).
"""
import argparse
import math
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KARTS, KART_SIZE, HEADING = 0x03003B98, 0x1A0, 0x56


class Game:
    def __init__(self, rom, state, out):
        (out / "data").mkdir(parents=True, exist_ok=True)
        self.process = subprocess.Popen(
            [
                str(ROOT / "build/play/host/play-shot"), "--drive", "--rom",
                str(rom), "--data",
                str(out / "data"), "--profiles",
                str(ROOT / "play/profiles/gba"), "--fonts",
                str(ROOT / "assets/fonts"), "--out",
                str(out)
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1
        )
        self.frame = 0
        print(self.process.stdout.readline().strip())
        print(self.lines("state-load " + str(state)))

    def ask(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()
        return self.process.stdout.readline().strip()

    def lines(self, line):
        """A command that may print a line of its own before its "ok"."""
        first = self.ask(line)
        if first != "ok": self.process.stdout.readline()
        return first

    def run(self, frames, keys):
        self.frame = int(self.ask(f"run {frames} {keys}").split()[1])

    def read(self, address, size=2):
        return int(self.ask(f"read {address:08x} {size}"), 16)

    def close(self):
        self.ask("quit")
        self.process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=10000)
    parser.add_argument("--items", action="store_true", help="fire items (L) every three seconds")
    args = parser.parse_args()
    game = Game(args.rom.resolve(), args.state.resolve(), args.out.resolve())

    def position(kart):
        def signed(v):
            return v - (1 << 32) if v & 0x80000000 else v

        base = KARTS + kart * KART_SIZE
        return signed(game.read(base, 4)) / 65536, signed(game.read(base + 4, 4)) / 65536

    trails = [[] for _ in range(8)]
    watched, step, pressed = None, 0, None
    with (args.out / "inputs.txt").open("w") as inputs:
        while game.frame < args.frames:
            px, py = position(0)
            for kart in range(1, 8):
                x, y = position(kart)
                trail = trails[kart]
                if not trail or (x - trail[-1][0])**2 + (y - trail[-1][1])**2 > 16: trail.append((x, y))
            best = None
            for trail in trails[1:]:
                if len(trail) < 5: continue
                near, distance = min(((i, (px - x)**2 + (py - y)**2) for i, (x, y) in enumerate(trail)),
                                     key=lambda e: e[1])
                ahead = min(near + 6, len(trail) - 1)
                if distance <= 40 * 40 and ahead > near and (best is None or distance < best[0]):
                    best = (distance, trail[ahead])
            if best: tx, ty = best[1]
            else: tx, ty = min((position(k) for k in range(1, 8)), key=lambda c: (c[0] - px)**2 + (c[1] - py)**2)
            heading = game.read(KARTS + HEADING) * 2 * math.pi / 65536
            error = (math.atan2(tx - px, -(ty - py)) - heading + math.pi) % (2 * math.pi) - math.pi
            keys = "A+LEFT" if error < -0.08 else "A+RIGHT" if error > 0.08 else "A"
            if args.items and step % 90 < 3: keys += "+L"
            if keys != pressed:
                inputs.write(f"{game.frame} {keys}\n")
                pressed = keys
            game.run(2, keys)
            step += 1
            if step % 4 == 0:
                registers = (game.read(0x04000000), game.read(0x04000008), game.read(0x06003808))
                status = game.lines("status")
                split = ["hud=" + status.split("hud=")[1].split()[0], status.split("split=")[1].split()[0]]
                if (registers, split) != watched:
                    print(f"{game.frame:6d} DISPCNT={registers[0]:04X} BG0CNT={registers[1]:04X} "
                          f"LAP cell={registers[2]:04X} {split[0]} split={split[1]}", flush=True)
                    game.ask(f"shot change-{game.frame:06d}")
                    watched = (registers, split)
            if step % 600 == 0: game.ask(f"shot race-{game.frame:06d}")
    game.close()


if __name__ == "__main__":
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    main()
