#!/usr/bin/env python3
"""Drive RG DS Play headless from a script, to find a profile's facts.

A companion for the Game Boy profiles: it replays a script of commands
against a game and writes what both screens show, so a new profile's facts
(when the game is in play, what a button opens, which memory says so) can be
found without a human at the handheld.

    python3 tools/play_gobot.py --rom GAME.gbc --profile gb --script steps.txt --out shots/

The script is one command per line ("#" starts a comment), the commands
play-shot's --drive mode understands:

    run 60 A          hold A for 60 frames
    run 60 -          hold nothing for 60 frames
    shot name         write shots/name.bmp (the game on top, the companion below)
    read C2EE 1       read a byte of the game's memory
    dump FILE         write the whole memory (EWRAM then IWRAM; Game Boy Advance)
    layers 1f         draw only these layers (bits 0-3 background, 4 sprites)
    poke D056 01 02   write bytes into memory from an address (set a scene up)

main() only replays the script. The helpers below (is_title, is_menu,
is_overworld, is_full_screen, press_until, cursor_on_ok) read a shot for a
script of your own that decides what to press from what the screen shows;
they need Pillow (.cache/venv/bin/python). The Game Boy screen is 160x144,
drawn at 3x in the middle of the 640x480 panel (x 80..560, y 24..456).
"""
import subprocess
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # the helpers that read a screen need Pillow; the rest does not
    Image = None

ROOT = Path(__file__).resolve().parents[1]
PLAY = ROOT / "build/play/host/play-shot"
GAME_X0, GAME_X1, GAME_Y0, GAME_Y1 = 80, 558, 24, 454


class Player:
    def __init__(self, rom, data, profiles, out):
        self.out = Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.p = subprocess.Popen(
            [
                str(PLAY), "--rom",
                str(rom), "--data",
                str(data), "--profiles",
                str(profiles), "--fonts",
                str(ROOT / "assets/fonts"), "--out",
                str(self.out), "--drive"
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1
        )
        self.frame = 0

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            line_out = self.p.stdout.readline()
            if not line_out:
                break
            line_out = line_out.strip()
            if line_out.startswith("ok"):
                break
            out.append(line_out)
        return out

    def run(self, frames, keys="-"):
        self.cmd(f"run {frames} {keys}")
        self.frame += frames

    def press(self, keys="A", frames=12):
        self.run(frames, keys)
        self.run(frames, "-")

    def wait(self, frames=120):
        self.run(frames, "-")

    def shot(self, name):
        self.cmd(f"shot {name}")
        return self.out / f"{name}.bmp"

    def read(self, address, size=1):
        # "read" answers with the value alone, not with an "ok".
        self.p.stdin.write(f"read {address} {size}\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline().strip()
        return int(line, 16) if line else None

    def image(self, name):
        return Image.open(self.out / f"{name}.bmp").crop((0, 0, 640, 480)).convert("RGB")

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.p.wait(timeout=10)


def require_image():
    if Image is None:
        raise SystemExit("Pillow is needed to read the screen: run this with .cache/venv/bin/python")


def picture_box(image):
    """Where the game drew its picture (the panel is 640x480)."""
    px = image.load()
    xs, ys = [], []
    for x in range(0, 640, 2):
        for y in range(0, 480, 2):
            r, g, b = px[x, y]
            if r > 25 or g > 25 or b > 25:
                xs.append(x)
                ys.append(y)
    return (min(xs), max(xs), min(ys), max(ys)) if xs else None


def is_full_screen(image):
    """The game shows its whole 144-line screen (not a cutscene window)."""
    require_image()
    box = picture_box(image)
    return bool(box) and box[3] - box[2] >= GAME_Y1 - GAME_Y0 - 12


def is_menu(image):
    """The menus: the full screen, mostly black, with yellow buttons."""
    require_image()
    if not is_full_screen(image):
        return False
    px = list(image.getdata())
    dark = sum(1 for r, g, b in px if r < 45 and g < 45 and b < 45)
    return dark / len(px) > 0.5


def is_overworld(image):
    """The player is out in the world: the whole screen, not a black story scene."""
    require_image()
    if not is_full_screen(image):
        return False
    px = list(image.getdata())
    dark = sum(1 for r, g, b in px if r < 45 and g < 45 and b < 45) / len(px)
    return dark < 0.5


def is_title(image):
    """The title screen: the logo on white, blue sky below.

    The boot logo is white too, but nearly all of it: the title screen is
    mostly blue and black with a white band.
    """
    require_image()
    if not is_full_screen(image):
        return False
    px = list(image.getdata())
    n = len(px)
    white = sum(1 for r, g, b in px if r > 235 and g > 235 and b > 235) / n
    blue = sum(1 for r, g, b in px if b > 150 and r < 120 and g < 160) / n
    green = sum(1 for r, g, b in px if 90 < g < 190 and r < 140 and b < 120 and g > r + 30) / n
    # The boot logo is white all over, the attract movie's last scene is green
    # below: the title screen is neither.
    return 0.02 < white < 0.3 and blue > 0.15 and green < 0.05


def press_until(player, key, predicate, label, tries=4, frames=30, wait=150):
    """Press a key until the screen looks the way we expect."""
    for attempt in range(tries):
        player.press(key, frames)
        player.wait(wait)
        name = f"{label}{attempt}"
        player.shot(name)
        image = player.image(name)
        if predicate(image):
            return True, name
    return False, f"{label}{tries - 1}"


def cursor_on_ok(image):
    """The blue underline under the name screen's OK button (24, 132, 255)."""
    require_image()
    px = image.load()
    hits = sum(1 for x in range(430, 500) for y in range(425, 445) if px[x, y] == (24, 132, 255))
    return hits > 40


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--profile", default="gb", help="the profiles folder: gb or gba")
    parser.add_argument("--script", required=True, type=Path)
    parser.add_argument("--out", default="shots", type=Path)
    parser.add_argument("--data", default="build/play-data", type=Path)
    args = parser.parse_args()
    player = Player(args.rom, args.data, ROOT / "play/profiles" / args.profile, args.out)
    try:
        for line in args.script.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 2)
            what = parts[0]
            if what == "run":
                player.run(int(parts[1]), parts[2] if len(parts) > 2 else "-")
            elif what == "shot":
                player.shot(parts[1])
            elif what == "read":
                player.read(parts[1], int(parts[2]) if len(parts) > 2 else 1)
            elif what == "dump":
                player.cmd(f"dump {parts[1]}")
            elif what == "layers":
                player.cmd(f"layers {parts[1]}")
            elif what == "poke":
                player.cmd(line)
            else:
                raise SystemExit(f"unknown command: {line}")
    finally:
        player.close()


if __name__ == "__main__":
    main()
