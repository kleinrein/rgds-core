#!/usr/bin/env python3
"""RG DS Play's icons: the launcher's 12 x 12 pixel glyphs plus the companion's own.

The companion screen draws the same symbols as the launcher (Save, Load, Sun,
...), taken from tools/build_pixel_icons.py so the two never drift, and adds
the things games count: coins, lives, treasures, stars, keys and so on. Simple
pixel draws each glyph at whole scales; Rich draws the same shape smoothed.
Profiles name them in lower case (`icon coin`).

    python3 tools/build_play_icons.py              # write play/src/ui/icons.hpp
    python3 tools/build_play_icons.py --sheet out.png
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_pixel_icons as launcher  # noqa: E402

HEADER = ROOT / "play/src/ui/icons.hpp"

# From the launcher, under the same names.
SHARED = [
    "Save", "Load", "Sun", "Moon", "Sleep", "Power", "Info", "Clock", "Heart", "Key", "Touch", "Check", "Back", "Home",
    "Play", "Sliders", "Spark", "Controller", "Cartridge", "Battery", "Library"
]

OWN = {
    "Coin": [
        "...######...", "..#++++++#..", ".#++####++#.", "#++#++++#++#", "#+#++##++#+#", "#+#++##++#+#", "#+#++##++#+#",
        "#+#++##++#+#", "#++#++++#++#", ".#++####++#.", "..#++++++#..", "...######..."
    ],
    "Life": [
        "....####....", "...######...", "..########..", "..########..", "...######...", "....####....", "............",
        "...######...", "..########..", ".##########.", ".##########.", "............"
    ],
    "Star": [
        ".....##.....", ".....##.....", "....####....", "....####....", "############", ".##########.", "..########..",
        "...######...", "...######...", "..###..###..", "..##....##..", ".#........#."
    ],
    "Treasure": [
        "............", "..########..", ".#++++++++#.", "#++++++++++#", "############", "#+++####+++#", "#+++#..#+++#",
        "#++++##++++#", "#++++++++++#", "#++++++++++#", "############", "............"
    ],
    "Bank": [
        "............", "...######...", "..#++++++#..", "...######...", "..#++++++#..", "...######...", "..#++++++#..",
        "...######...", "..#++++++#..", "...######...", "..#++++++#..", "...######..."
    ],
    "Flag": [
        ".##.........", ".#######....", ".#++++####..", ".#++++++++#.", ".#++++####..", ".#######....", ".#..........",
        ".#..........", ".#..........", ".#..........", "###.........", "............"
    ],
    "Map": [
        "............", "##...##...##", "#+###++###+#", "#+#+#++#+#+#", "#+#+#++#+#+#", "#+#+#++#+#+#", "#+#+#++#+#+#",
        "#+#+#++#+#+#", "#+#+#++#+#+#", "#+###++###+#", "##...##...##", "............"
    ],
    "Gem": [
        ".....##.....", "....####....", "...##++##...", "..##++++##..", "..#++++++#..", "..#++++++#..", "..#++++++#..",
        "..#++++++#..", "..##++++##..", "...##++##...", "....####....", ".....##....."
    ],
    "Sword": [
        "..........##", ".........###", "........###.", ".......###..", "......###...", ".....###....", ".#..###.....",
        ".####.......", "..###.......", ".##.##......", "##..........", "#..........."
    ],
    "Bag": [
        "....####....", "...#....#...", "....####....", "..########..", ".##++++++##.", "#++++++++++#", "#+++####+++#",
        "#+++#++#+++#", "#++++##++++#", "#++++++++++#", ".##########.", "............"
    ],
    "More": [
        "............", "............", "............", "............", "............", ".##..##..##.", ".##..##..##.",
        "............", "............", "............", "............", "............"
    ],
    "Close": [
        "............", ".##......##.", ".###....###.", "..###..###..", "...######...", "....####....", "....####....",
        "...######...", "..###..###..", ".###....###.", ".##......##.", "............"
    ],
    "Trophy": [
        ".##########.", "#.########.#", "#.########.#", ".#.######.#.", "..########..", "...######...", "....####....",
        ".....##.....", ".....##.....", "....####....", "...######...", "............"
    ],
    "Shield": [
        "############", "#++++##++++#", "#++++##++++#", "#++++##++++#", "############", "#++++##++++#", ".#+++##+++#.",
        ".#+++##+++#.", "..#++##++#..", "...#+##+#...", "....####....", ".....##....."
    ],
}


def glyphs():
    out = {name: launcher.GLYPHS[name] for name in SHARED}
    out.update(OWN)
    for name, rows in out.items():
        if len(rows) != 12 or any(len(row) != 12 or set(row) - set(".#+") for row in rows):
            raise ValueError("Glyph %s is not 12 x 12 of .#+" % name)
    return out


def header():
    table = glyphs()
    names = list(table)
    enum = ", ".join(names)
    cases = "\n".join(
        "    case Icon::%s: return\n%s;" % (n, "\n".join('        "%s"' % r for r in table[n])) for n in names
    )
    lookup = "\n".join('    if (name == "%s") return Icon::%s;' % (n.lower(), n) for n in names)
    return f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Generated by tools/build_play_icons.py; edit the glyphs there.
#pragma once
#include <string>
#include <string_view>

namespace play {{

// 12 x 12 glyphs: '#' ink, '+' a half-tone of it, '.' empty.
enum class Icon {{ None, {enum} }};

inline std::string_view icon_glyph(Icon icon) {{
    switch (icon) {{
    case Icon::None: return {{}};
{cases}
    }}
    return {{}};
}}

// A profile's lower-case name for an icon (Icon::None when unknown).
inline Icon icon_named(const std::string& name) {{
{lookup}
    return Icon::None;
}}

}} // namespace play
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheet", type=Path)
    args = parser.parse_args()
    if args.sheet:
        launcher.GLYPHS.update(OWN)
        launcher.SYMBOLS[:] = list(glyphs())
        launcher.STATUS[:] = []
        launcher.sheet(args.sheet)
    else:
        HEADER.write_text(header())
        print(HEADER.relative_to(ROOT))


if __name__ == "__main__":
    main()
