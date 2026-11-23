#!/usr/bin/env python3
"""Flash games from Flashpoint Archive (https://flashpointarchive.org) into the Flash shelf.

Flashpoint keeps each game as a zip of the web files it loaded (Data/Games/<id>-<time>.zip, content/<host>/...),
its details in Data/flashpoint.sqlite and its pictures in Data/Images. An import names each pack after its game and
writes a `<title>.flashpoint` beside it: the address the game starts from (the player answers every web request from
the zip), and the details the Game info page shows. Nothing is downloaded: only the games already in the copied
Flashpoint folder are taken.

    flashpoint.py import FLASHPOINT [FLASH_FOLDER] [--copy] [--extreme] [--search TEXT] [--limit N]
        on a computer, from a Flashpoint install to a card's roms/flash (copies), or on the handheld
    flashpoint.py scan FLASH_FOLDER
        on the handheld at start: imports any Flashpoint folder copied into the Flash folder (moves the packs)

Games marked extreme in Flashpoint are left out unless --extreme is given. Games for other plugins (Shockwave,
Unity, HTML5, Java) are never taken: only the Flash platform.
"""
import argparse
import os
import re
import shutil
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fields import read_fields, write_fields  # noqa: E402

SIDECAR = ".flashpoint"
IMAGES = ".flashpoint-images"  # pictures copied beside the shelf (hidden from the library: a dot folder)
DONE = ".flashpoint-imported"  # in a scanned Flashpoint folder: which database was imported, when
# Flashpoint's play modes as the library's player counts.
PLAYERS = {"single player": "1", "multiplayer": "2+", "cooperative": "2+"}


def database_in(folder):
    """The flashpoint.sqlite of a Flashpoint install (its Data folder, or the folder itself)."""
    folder = Path(folder)
    for candidate in (folder / "Data" / "flashpoint.sqlite", folder / "flashpoint.sqlite"):
        if candidate.is_file(): return candidate
    return None


def columns(db, table):
    return {row[1] for row in db.execute("PRAGMA table_info(%s)" % table)}


def safe_name(title):
    """A file name from a title: no path characters, no trailing dots or spaces (exFAT refuses them)."""
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', " ", title)
    name = re.sub(r"\s+", " ", name).strip().rstrip(". ")
    return name[:120] or "Untitled"


def unique(folder, stem, suffix, taken):
    name, n = stem, 2
    while (folder / (name + suffix)).exists() or (name + suffix).lower() in taken:
        name = "%s (%d)" % (stem, n)
        n += 1
    taken.add((name + suffix).lower())
    return folder / (name + suffix)


def picture(data, kind, game_id):
    """Flashpoint's Logos or Screenshots picture for a game: Images/<kind>/ab/cd/<id>.png."""
    folder = data / "Images" / kind / game_id[:2] / game_id[2:4]
    for ext in (".png", ".jpg", ".jpeg", ".webp"):
        path = folder / (game_id + ext)
        if path.is_file(): return path
    return None


def games(db, search="", extreme=False):
    """Flash games, with the zip that holds each one where there is one: (game row, data row or None)."""
    db.row_factory = sqlite3.Row
    game_cols = columns(db, "game")
    platform = "platformName" if "platformName" in game_cols else "platform"
    where = ["%s = 'Flash'" % platform]
    params = []
    if "extreme" in game_cols and not extreme: where.append("(extreme IS NULL OR extreme = 0)")
    if search:
        where.append("title LIKE ?")
        params.append("%" + search + "%")
    rows = db.execute("SELECT * FROM game WHERE " + " AND ".join(where) + " ORDER BY title", params).fetchall()
    data_rows = {}
    if "game_data" in {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}:
        for row in db.execute("SELECT * FROM game_data ORDER BY dateAdded"):
            data_rows[row["gameId"]] = row  # the newest pack wins
    for row in rows:
        yield row, data_rows.get(row["id"])


def value(row, *keys):
    for key in keys:
        try:
            v = row[key]
        except (IndexError, KeyError):
            continue
        if v not in (None, ""): return str(v)
    return ""


def details(game):
    """The sidecar's details from Flashpoint's game row."""
    tags = [t.strip() for t in value(game, "tagsStr").split(";") if t.strip()]
    release = value(game, "releaseDate")
    year = release[:4] if re.match(r"\d{4}", release) else ""
    modes = [m.strip().lower() for m in value(game, "playMode").split(";")]
    players = next((PLAYERS[m] for m in modes if m in PLAYERS), "")
    genre = ", ".join(t for t in tags[:3])
    return {
        "id": value(game, "id"),
        "title": value(game, "title"),
        "developer": value(game, "developer"),
        "publisher": value(game, "publisher"),
        "genre": genre,
        "tags": ", ".join(tags),  # all of them: the player picks the stick's keys for racing, platform... games
        "release": release,
        "year": year,
        "players": players,
        "series": value(game, "series"),
        "library": value(game, "library"),
        "description": value(game, "originalDescription"),
        "notes": value(game, "notes"),
    }


def copy_or_move(source, target, copy):
    target.parent.mkdir(parents=True, exist_ok=True)
    if copy:
        shutil.copyfile(source, target)
    else:
        try:
            os.replace(source, target)
        except OSError:  # another file system: copy, then remove
            shutil.copyfile(source, target)
            source.unlink()


def import_flashpoint(source, dest, copy=True, extreme=False, search="", limit=0, log=print):
    """Imports every Flash game whose files the Flashpoint folder holds; returns (imported, skipped)."""
    source, dest = Path(source), Path(dest)
    database = database_in(source)
    if not database: raise FileNotFoundError("No flashpoint.sqlite in %s (or its Data folder)." % source)
    data = database.parent
    dest.mkdir(parents=True, exist_ok=True)
    # Games imported before (by Flashpoint id): an import again only adds what is new.
    known = {}
    for side in dest.glob("*" + SIDECAR):
        game_id = read_fields(side).get("id")
        if game_id: known[game_id] = side
    taken, imported, skipped = set(), 0, 0
    db = sqlite3.connect("file:%s?mode=ro" % database, uri=True)
    try:
        for game, pack in games(db, search, extreme):
            if limit and imported >= limit: break
            info = details(game)
            if info["id"] in known:
                skipped += 1
                continue
            launch = value(pack, "launchCommand") if pack is not None else ""
            launch = launch or value(game, "launchCommand")
            flashvars = ""
            if "?" in launch and launch.lower().split("?")[0].endswith(".swf"):
                launch, flashvars = launch.split("?", 1)
            if not re.match(r"https?://", launch):
                skipped += 1  # a local file Flashpoint runs itself: not a web game
                continue
            stem = safe_name(info["title"])
            zipped = data / "Games" / value(pack, "path") if pack is not None and value(pack, "path") else None
            if zipped and zipped.is_file():
                target = unique(dest, stem, ".zip", taken)
                copy_or_move(zipped, target, copy)
                fields = {"launch": launch}
            else:
                # A legacy game: its files loose under Legacy/htdocs, the movie copied out and the rest served from
                # there (only while the Flashpoint folder stays on the card).
                htdocs = source / "Legacy" / "htdocs"
                if not htdocs.is_dir(): htdocs = data.parent / "Legacy" / "htdocs"
                movie = re.sub(r"^https?://", "", launch)
                movie_path = htdocs / movie
                if not movie_path.is_file() or not movie.lower().endswith(".swf"):
                    skipped += 1
                    continue
                target = unique(dest, stem, ".swf", taken)
                copy_or_move(movie_path, target, True)
                fields = {"launch": launch, "htdocs": str(htdocs)}
            if flashvars: fields["flashvars"] = flashvars
            images = dest / IMAGES
            for kind, key in (("Logos", "logo"), ("Screenshots", "screenshot")):
                found = picture(data, kind, info["id"])
                if found:
                    keep = images / (info["id"] + "-" + key + found.suffix.lower())
                    keep.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(found, keep)
                    fields[key] = str(keep.relative_to(dest))
            fields.update({k: v for k, v in info.items() if v})
            write_fields(target.with_suffix(SIDECAR), fields)
            imported += 1
            log("flashpoint: %s" % target.name)
    finally:
        db.close()
    return imported, skipped


def scan(flash):
    """On the handheld: imports a Flashpoint folder copied into the Flash folder, once per database version."""
    flash = Path(flash)
    if not flash.is_dir(): return 0
    total = 0
    for child in sorted(flash.iterdir()):
        if not child.is_dir() or child.name.startswith("."): continue
        database = database_in(child)
        if not database: continue
        stamp = child / DONE
        stamp_text = "%d:%d" % (database.stat().st_size, int(database.stat().st_mtime))
        if read_fields(stamp).get("database") == stamp_text: continue
        imported, skipped = import_flashpoint(child, flash, copy=False, log=lambda _: None)
        write_fields(stamp, {"database": stamp_text, "imported": imported, "skipped": skipped, "at": int(time.time())})
        total += imported
    return total


# ---- for the library's Game info and covers ----


def sidecar(rom):
    return read_fields(Path(rom).with_suffix(SIDECAR))


def image(rom, key):
    """The bytes of a game's Flashpoint picture ("logo" or "screenshot"), or None."""
    rom = Path(rom)
    path = sidecar(rom).get(key)
    if not path: return None
    try:
        return (rom.parent / path).read_bytes()
    except OSError:
        return None


def information(rom):
    """({info.ini fields}, {language: synopsis}) from a game's sidecar."""
    info = sidecar(rom)
    if not info.get("id"): return None
    fields = {
        "info_title": info.get("title", ""),
        "developer": info.get("developer", ""),
        "publisher": info.get("publisher", ""),
        "genre": info.get("genre", ""),
        "release": info.get("release", ""),
        "players": info.get("players", ""),
        "franchise": info.get("series", ""),
    }
    synopses = {"EN": info["description"]} if info.get("description") else {}
    return fields, synopses


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("import", help="import from a Flashpoint folder")
    one.add_argument("flashpoint")
    one.add_argument("dest", nargs="?", default="/data/roms/flash")
    one.add_argument("--copy", action="store_true", help="copy the packs (default when importing from a computer)")
    one.add_argument("--move", action="store_true", help="move the packs instead of copying")
    one.add_argument("--extreme", action="store_true", help="include games Flashpoint marks extreme")
    one.add_argument("--search", default="", help="only titles containing this text")
    one.add_argument("--limit", type=int, default=0, help="at most this many games")
    every = sub.add_parser("scan", help="import any Flashpoint folder inside the Flash folder (on the handheld)")
    every.add_argument("flash", nargs="?", default="/data/roms/flash")
    args = parser.parse_args(argv)
    try:
        if args.command == "scan":
            print("flashpoint: %d games imported" % scan(args.flash))
        else:
            imported, skipped = import_flashpoint(
                args.flashpoint,
                args.dest,
                copy=not args.move,
                extreme=args.extreme,
                search=args.search,
                limit=args.limit
            )
            print("flashpoint: %d games imported, %d skipped" % (imported, skipped))
    except (OSError, sqlite3.Error) as error:
        print("flashpoint: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
