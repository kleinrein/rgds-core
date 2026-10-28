#!/usr/bin/env python3
"""Check every companion profile against docs/COMPANION.md, then draw them all.

The static half reads play/profiles/*/*.ini:

  sources   a profile starts with a comment: what the screen shows, where its facts come from
  badge     every board tile has an icon or the game's art
  label     labels of at most 18 characters; at most 8 tiles a page, 3 pages a board
  on        a profile with several views gives each its `on`
  words     headings, labels, pages and step names are in assets/locales/en-US.tsv
            (so the launcher's catalogs can translate them), and translated in every catalog;
            a view's `proper = A; B` lists the game's own names, which stay as written

The drawing half is play-lint (play/src/tools/play_lint.cpp): sizes, edges,
contrast, overlap, the system corners and touch targets, in four looks.

    python3 tools/lint_companions.py [--no-draw] [--shots DIR]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / "play/profiles"
LOCALES = ROOT / "assets/locales"
LINT = ROOT / "build/play/host/play-lint"
MAX_LABEL, MAX_TILES, MAX_PAGES = 18, 8, 3


def sections(text):
    """[(name, {key: value}, first line)] in file order; comments and blanks dropped."""
    out, current = [], None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        line = re.sub(r"\s#(\s.*)?$", "", line).strip()
        if line.startswith("["):
            current = (line[1:-1].strip(), {}, number)
            out.append(current)
        elif "=" in line and current:
            key, value = line.split("=", 1)
            current[1][key.strip()] = value.strip()
    return out


def catalog(path):
    table = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or "\t" not in line:
            continue
        source, text = line.split("\t", 1)
        table[source] = text
    return table


def board_words(values):
    """The words a board shows, and its tiles as (name, label, rest)."""
    words, tiles = [], []
    if values.get("heading"):
        words.append(values["heading"])
    for key, value in values.items():
        match = re.fullmatch(r"tile(\d+)", key)
        if match:
            label, _, rest = value.partition("|")
            tiles.append((int(match.group(1)), label.strip(), rest.split()))
            words.append(label.strip())
            page = rest.split()
            if "page" in page and page.index("page") + 1 < len(page):
                words.append(page[page.index("page") + 1])
        elif re.fullmatch(r"tiers\d+", key):
            for step in value.split(";"):
                name = step.strip().partition(" ")[2].strip()
                if name:
                    words.append(name)
    return words, sorted(tiles)


def check_profile(path, english, catalogs):
    problems = []
    text = path.read_text(encoding="utf-8")
    where = str(path.relative_to(ROOT))
    head = [line for line in text.splitlines()[:3] if line.startswith("#")]
    if len(head) < 2:
        problems.append(f"{where}: sources: start with a comment on what the screen shows and where its facts are from")
    parsed = sections(text)
    views = [(name, values, line) for name, values, line in parsed if name == "view" or name.startswith("view.")]
    if len(views) > 1:
        for name, values, line in views:
            if "on" not in values:
                problems.append(f"{where}:{line}: on: [{name}] needs an `on` when a profile has several views")
    for name, values, line in views:
        if values.get("format") != "board":
            continue
        words, tiles = board_words(values)
        # The game's own names (places, items, characters) stay as the top screen writes them.
        proper = {w.strip() for w in values.get("proper", "").split(";") if w.strip()}
        words = [w for w in words if w not in proper]
        pages, page, count = [], None, {}
        for number, label, rest in tiles:
            # A tile of the HUD itself is the game's own drawing: it is its own badge.
            if "icon" not in rest and "art" not in rest and rest[:1] != ["hud"]:
                problems.append(f"{where}: badge: [{name}] tile{number} '{label}' has neither an icon nor art")
            if len(label) > MAX_LABEL:
                problems.append(f"{where}: label: [{name}] tile{number} '{label}' is longer than {MAX_LABEL}")
            if "page" in rest:
                page = rest[rest.index("page") + 1]
                if page not in pages:
                    pages.append(page)
            count[page] = count.get(page, 0) + 1
        if len(pages) > MAX_PAGES:
            problems.append(f"{where}: label: [{name}] has {len(pages)} pages (at most {MAX_PAGES})")
        for page_name, n in count.items():
            if n > MAX_TILES:
                problems.append(
                    f"{where}: label: [{name}] page {page_name or 'one'} has {n} tiles (at most {MAX_TILES})"
                )
        for word in words:
            if word not in english:
                problems.append(f"{where}: words: '{word}' is not in assets/locales/en-US.tsv")
                continue
            missing = [lang for lang, table in catalogs.items() if word not in table]
            if missing:
                problems.append(f"{where}: words: '{word}' is not translated in {', '.join(sorted(missing))}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-draw", action="store_true", help="only the static checks")
    parser.add_argument("--shots", help="play-lint writes every screen it draws here")
    args = parser.parse_args()
    english = catalog(LOCALES / "en-US.tsv")
    catalogs = {p.stem: catalog(p) for p in sorted(LOCALES.glob("*.tsv")) if p.stem not in ("en-US", "languages")}
    problems = []
    for path in sorted(PROFILES.glob("*/*.ini")):
        problems += check_profile(path, english, catalogs)
    for line in problems:
        print(line)
    print(f"lint_companions: {len(list(PROFILES.glob('*/*.ini')))} profiles, {len(problems)} problems")
    drawn = 0
    if not args.no_draw:
        if not LINT.exists():
            subprocess.run(
                [sys.executable, str(ROOT / "tools/build_play.py"), "host", "--only", "play-lint"], check=True
            )
        command = [str(LINT), "--fonts", str(ROOT / "assets/fonts")]
        for system in sorted(p for p in PROFILES.iterdir() if p.is_dir()):
            command += ["--profiles", str(system)]
        if args.shots:
            Path(args.shots).mkdir(parents=True, exist_ok=True)
            command += ["--shots", args.shots]
        drawn = subprocess.run(command).returncode
    return 1 if problems or drawn else 0


if __name__ == "__main__":
    sys.exit(main())
