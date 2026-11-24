#!/usr/bin/env python3
"""Write assets/arcade/sets.tsv: every arcade set RG DS Play can start, by its short name.

FBNeo (os/buildroot/package/libretro-fbneo) runs the arcade; its sets are named as its own DAT names them
(dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat), which mostly matches current MAME. The twelve sets of
Nintendo's two-monitor cabinets go to mame2003-plus instead (package/libretro-mame2003-plus, play/profiles/arcade),
named as MAME 0.78 named them.

    python3 tools/arcade_sets.py [FBNEO_SOURCE]     # default .cache/fbneo-src, at the package's pinned commit

Columns: name, title, parent (a clone's), flags. Flags: b = a board's BIOS, no game (neogeo); v = drawn on a
vertical monitor (the player stands it over both panels, the tall screen); x = FBNeo marks it not working;
m = one of the two-monitor cabinets, played through mame2003-plus. The launcher lists the sets in this file
(src/launcher/model.hpp arcade_catalog), the runtime picks the core by the m flag (runtime/play_options.py).
"""
import html
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets/arcade/sets.tsv"
DAT = "dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat"

# The two-monitor cabinets (runtime/play_options.py ARCADE_SETS, two_monitors.py), with the games' own names.
TWO_MONITORS = {
    "punchout": ("Punch-Out!!", ""),
    "spnchout": ("Super Punch-Out!!", ""),
    "spnchotj": ("Super Punch-Out!! (Japan)", "spnchout"),
    "armwrest": ("Arm Wrestling", ""),
    "vstennis": ("Vs. Tennis", ""),
    "wrecking": ("Vs. Wrecking Crew", ""),
    "balonfgt": ("Vs. Balloon Fight", ""),
    "vsmahjng": ("Vs. Mahjong (Japan)", ""),
    "vsbball": ("Vs. Baseball", ""),
    "vsbballj": ("Vs. Baseball (Japan, set 1)", "vsbball"),
    "vsbbalja": ("Vs. Baseball (Japan, set 2)", "vsbball"),
    "iceclmrj": ("Vs. Ice Climber Dual (Japan)", ""),
}

STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def fields(body):
    """A BurnDriver initialiser's top-level fields, strings unquoted."""
    out, depth, current, i = [], 0, "", 0
    while i < len(body):
        c = body[i]
        if c == '"':
            m = STRING.match(body, i)
            current += m.group(0)
            i = m.end()
            continue
        if c in "({": depth += 1
        if c in ")}": depth -= 1
        if c == "," and depth == 0:
            out.append(current.strip())
            current = ""
        else:
            current += c
        i += 1
    out.append(current.strip())
    return out


def driver_flags(source):
    """Each built driver's name and its BDF_ flags, from the driver sources."""
    built = set(re.findall(r"DRV\s+(\w+);", (source / "src/dep/generated/driverlist.h").read_text(errors="replace")))
    flags = {}
    for path in sorted((source / "src/burn/drv").rglob("*.cpp")):
        text = path.read_text(errors="replace")
        for m in re.finditer(r"struct\s+BurnDriver\s+(\w+)\s*=\s*\{(.*?)\};", text, re.S):
            if built and m.group(1) not in built: continue
            f = fields(m.group(2))
            name = STRING.match(f[0])
            if name and len(f) > 13: flags[name.group(1)] = f[13]
    return flags


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / ".cache/fbneo-src"
    commit = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dat = (source / DAT).read_text(encoding="utf-8")
    flags = driver_flags(source)
    rows = {}
    for m in re.finditer(r'<game ([^>]*)>(.*?)</game>', dat, re.S):
        attrs, body = m.group(1), m.group(2)
        name = re.search(r'\bname="([^"]+)"', attrs).group(1)
        title = html.unescape(re.search(r"<description>(.*?)</description>", body, re.S).group(1)).strip()
        parent = re.search(r'cloneof="([^"]+)"', attrs)
        bdf = flags.get(name, "")
        f = ("b" if 'isbios="yes"' in attrs else "") + ("v" if "BDF_ORIENTATION_VERTICAL" in bdf else "") + \
            ("" if "BDF_GAME_WORKING" in bdf or not bdf else "x")
        rows[name] = (title, parent.group(1) if parent else "", f)
    for name, (title, parent) in TWO_MONITORS.items():
        rows[name] = (title, parent, "m")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8") as out:
        out.write(
            f"# Arcade sets: tools/arcade_sets.py from FBNeo {commit[:12]}. name, title, parent, flags (b v x m)\n"
        )
        for name in sorted(rows):
            title, parent, f = rows[name]
            out.write(f"{name}\t{title.replace(chr(9), ' ')}\t{parent}\t{f}\n")
    vertical = sum("v" in r[2] for r in rows.values())
    print(f"{OUT.relative_to(ROOT)}: {len(rows)} sets, {vertical} vertical, {OUT.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
