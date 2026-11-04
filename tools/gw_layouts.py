#!/usr/bin/env python3
"""Writes play/profiles/gw/ from the Game & Watch simulators themselves.

Each simulator's picture is a scan of the whole machine. Nothing says where its LCD and its buttons are, but the
simulator shows them: every LCD segment is lit until a game starts, and a button is drawn pressed while its key is
held. So this runs each game headless in play-shot with every key in turn, and compares the pictures.

  python3 tools/gw_layouts.py --core gw_libretro.dylib --sims DIR_OF_MGW [--out play/profiles/gw] [--only NAME]

Work files go to a temporary folder. The sims themselves are not kept anywhere in the repository.
"""
import argparse
import re
import struct
import subprocess
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The power switch and Game A first: on many machines nothing else shows until the game is on.
KEYS = "L R L2 UP DOWN LEFT RIGHT A B X Y R2".split()
# LCDs read off the scans by hand, where lighting them shows too little to measure: dark screens with few segments.
LCD_BY_HAND = {
    "Armor Battle": [(119, 150, 379, 440, 0)],
    "Las Vegas": [(166, 62, 381, 252, 0)],
    "Sub Chase": [(109, 180, 384, 460, 0)],
    "Penguin Land": [(135, 115, 410, 318, 0), (672, 122, 948, 318, 0)],
    "Motor Cross": [(133, 105, 345, 262, 0)],
    "Tron": [(138, 118, 623, 292, 0)],
}
CELL = 8


def read_bmp(path):
    d = path.read_bytes()
    off = struct.unpack("<I", d[10:14])[0]
    w, h = struct.unpack("<ii", d[18:26])
    bytes_per = struct.unpack("<H", d[28:30])[0] // 8
    row = ((w * bytes_per * 8 + 31) // 32) * 4
    rows = [d[off + (abs(h) - 1 - y if h > 0 else y) * row:][:w * bytes_per] for y in range(abs(h))]
    return w, abs(h), bytes_per, rows


def changed_cells(a, b):
    w, _, n, rows_a = a
    cells = set()
    for y, (la, lb) in enumerate(zip(rows_a, b[3])):
        if la == lb: continue
        for x in range(w):
            if la[x * n:x * n + 3] != lb[x * n:x * n + 3]: cells.add((x // CELL, y // CELL))
    return cells


def clusters(cells, reach=2):
    """Boxes (x0, y0, x1, y1, cells) around cells no further than `reach` cells apart."""
    out, seen = [], set()
    for start in cells:
        if start in seen: continue
        todo, xs, ys = [start], [], []
        seen.add(start)
        while todo:
            x, y = todo.pop()
            xs.append(x)
            ys.append(y)
            for dx in range(-reach, reach + 1):
                for dy in range(-reach, reach + 1):
                    near = (x + dx, y + dy)
                    if near in cells and near not in seen:
                        seen.add(near)
                        todo.append(near)
        out.append((min(xs) * CELL, min(ys) * CELL, (max(xs) + 1) * CELL, (max(ys) + 1) * CELL, len(xs)))
    return out


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def crc_of(path):
    return "%08X" % (zlib.crc32(path.read_bytes()) & 0xFFFFFFFF)


def probe(sim, play_shot, core, work):
    """{key: boxes that key changed outside the LCD}, the LCD boxes, the picture's size."""
    data = work / "data"
    data.mkdir(parents=True)
    commands = ["run 30 -", "gameshot boot"]
    for key in KEYS:
        commands += ["gameshot b%s" % key, "run 6 %s" % key, "gameshot k%s" % key, "run 30 -"]
    commands += ["run 15 L", "run 300 -", "gameshot game", "quit"]
    subprocess.run([
        str(play_shot), "--rom", str(sim), "--data", str(data), "--profiles", str(work), "--fonts",
        str(ROOT / "assets/fonts"), "--core", str(core), "--system", "gw", "--drive", "--out", str(work),
        "--no-companion"
    ],
                   input="\n".join(commands) + "\n",
                   capture_output=True,
                   text=True,
                   timeout=300,
                   check=True)
    boot = read_bmp(work / "boot.bmp")
    w, h = boot[0], boot[1]
    # Whatever has changed once a key is let go (its button is up again), in large clusters, is the LCD or each of
    # two: a machine with a power switch only lights it then.
    lit = changed_cells(boot, read_bmp(work / "game.bmp"))
    for key in KEYS:
        lit |= changed_cells(boot, read_bmp(work / ("b%s.bmp" % key)))
    pressed = {}
    for key in KEYS:
        cells = changed_cells(read_bmp(work / ("b%s.bmp" % key)), read_bmp(work / ("k%s.bmp" % key)))
        pressed[key] = clusters(cells)
    lcds = clusters(lit, reach=5)
    biggest = max((c[4] for c in lcds), default=0)
    lcds = [c for c in lcds if c[4] * 4 >= biggest and (c[2] - c[0]) >= 80 and (c[3] - c[1]) >= 60]
    for name, boxes in LCD_BY_HAND.items():
        if sim.stem.startswith(name): lcds = boxes
    for key in KEYS:
        pressed[key] = [c for c in pressed[key] if not any(overlaps(c, l) for l in lcds) and c[4] >= 4]
    return w, h, lcds, pressed


def four_three(w):
    return w * 3 // 4


def layout(w, h, lcds):
    """(top, bottom), each (x, y, w, h[, keep_from, keep_to]): a panel's rectangle of the picture."""
    if len(lcds) >= 2:
        a, b = sorted(lcds, key=lambda c: (c[1], c[0]))[:2]
        if b[1] >= a[3]: # one over the other: the machine's own two halves, meeting at the hinge
            hinge = (a[3] + b[1]) // 2
            return (0, hinge - four_three(w), w, four_three(w)), (0, hinge, w, four_three(w))
        a, b = sorted(lcds, key=lambda c: c[0])[:2]
        # side by side: left half above, right half below, each whole; the other half's edge kept off by keep
        middle = (a[2] + b[0]) // 2
        panel_w = h * 4 // 3
        return ((middle // 2 - panel_w // 2, 0, panel_w, h, 0, middle), (middle + (w - middle) // 2 - panel_w // 2, 0, panel_w, h,
                                                                         middle, w))
    if not lcds: return (0, (h - four_three(w)) // 2, w, four_three(w)), (0, (h - four_three(w)) // 2, w, four_three(w))
    x0, y0, x1, y1, _ = lcds[0]
    # The LCD large on the top screen, a little of the machine's frame round it.
    pad = 14
    tw = max(x1 - x0 + pad * 2, (y1 - y0 + pad * 2) * 4 // 3)
    th = four_three(tw)
    top = ((x0 + x1 - tw) // 2, (y0 + y1 - th) // 2, tw, th)
    if h * 5 <= w * 4: # a machine lying flat: all of it on the second screen, its buttons round its LCD
        return top, (0, (h - four_three(w)) // 2, w, four_three(w))
    # A tall one: what is under the LCD, its controls, on the second screen.
    from_y = min(y1 + pad, h - 1)
    rest = h - from_y
    bw = max(w, rest * 4 // 3)
    bh = four_three(bw)
    return top, ((w - bw) // 2, from_y + (rest - bh) // 2, bw, bh)


def zones(pressed):
    """touch lines: (x, y, w, h, BUTTONS). A D-pad or rocker draws one picture for all its directions: split by arm."""
    lines, by_box = [], {}
    for key in KEYS:
        for box in pressed[key]:
            by_box.setdefault(box[:4], []).append(key)
    for (x0, y0, x1, y1), keys in by_box.items():
        w, h = x1 - x0, y1 - y0
        third_w, third_h = w // 3, h // 3
        arms = {
            "UP": (x0, y0, w, third_h),
            "DOWN": (x0, y1 - third_h, w, third_h),
            "LEFT": (x0, y0 + third_h, third_w, h - 2 * third_h),
            "RIGHT": (x1 - third_w, y0 + third_h, third_w, h - 2 * third_h),
        }
        directions = [k for k in keys if k in arms]
        if len(directions) == 4:
            for key in directions:
                lines.append((*arms[key], key))
        elif set(keys) >= {"UP", "DOWN"}:
            lines.append((x0, y0, w, h // 2, "UP"))
            lines.append((x0, y0 + h // 2, w, h - h // 2, "DOWN"))
        elif set(keys) >= {"X", "B"}: # the second rocker (Mario's)
            lines.append((x0, y0, w, h // 2, "X"))
            lines.append((x0, y0 + h // 2, w, h - h // 2, "B"))
        else:
            lines.append((x0, y0, w, h, keys[0]))
    return lines


def same_action(pressed):
    """A and B alike on a machine with one action button of its own."""
    def own(key, other):
        boxes = {b[:4] for b in pressed[key]}
        return boxes and not any(boxes & {b[:4] for b in pressed[k]} for k in KEYS if k != key and k != other)

    a, b = own("A", "B"), own("B", "A")
    return (a and not b) or (b and not a)


def slug(name):
    return re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_")


def profile_text(title, crc, w, h, lcds, pressed):
    top, bottom = layout(w, h, lcds)
    lines = [
        "# %s: written by tools/gw_layouts.py from the simulator itself." % title, "# Picture %d x %d, LCD %s." %
        (w, h, " and ".join("%d %d %d %d" % c[:4] for c in lcds) or "not found"), "[game]", "system = gw",
        "title = %s" % title.split(" (")[0], "codes = CRC_%s" % crc, "", "[screens]",
        "top = %s" % " ".join(map(str, top)), "bottom = %s" % " ".join(map(str, bottom))
    ]
    if same_action(pressed): lines.append("same = A+B")
    lines += ["touch = %d %d %d %d %s" % z for z in zones(pressed)]
    return "\n".join(lines) + "\n"


def one(sim, args, out):
    with tempfile.TemporaryDirectory() as folder:
        w, h, lcds, pressed = probe(sim, args.play_shot, args.core, Path(folder))
    (out / (slug(sim.stem) + ".ini")).write_text(profile_text(sim.stem, crc_of(sim), w, h, lcds, pressed))
    return "%s: %dx%d, %d LCD, %d keys" % (sim.stem, w, h, len(lcds), sum(1 for k in KEYS if pressed[k]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--sims", required=True, type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "play/profiles/gw")
    parser.add_argument("--play-shot", type=Path, default=ROOT / "build/play/host/play-shot")
    parser.add_argument("--only")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sims = [s for s in sorted(args.sims.glob("*.mgw")) if not args.only or args.only in s.stem]
    with ThreadPoolExecutor(args.jobs) as pool:
        for line in pool.map(lambda s: one(s, args, args.out), sims):
            print(line, flush=True)


if __name__ == "__main__":
    main()
