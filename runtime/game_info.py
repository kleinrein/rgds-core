"""Game information and extra pictures from public catalogs, for every system.

Added on top of the cover job and never instead of it: each piece lands in the
game's library folder as its own file, so a library without any of them looks
exactly as before. Only game codes, CRC32 values and titles are used to look
things up; ROM contents are never uploaded.

  info.ini (merged)       publisher, genre, release, rating, players, ...
  synopsis.<LANG>.txt     the description, one file per language
  snap.bmp, title.bmp     Libretro screenshot and title screen
  info-status.ini         what was tried, so a missing entry is not retried
"""
import os
import re
import tempfile
import time
import xml.etree.ElementTree as ElementTree
import zipfile
import zlib
from pathlib import Path

import public_artwork as art
from library import read_fields, write_fields
from scraper import ScraperError, NotFound, image_to_bmp

VERSION = 1
GAMETDB = "https://www.gametdb.com/dstdb.zip"
METADATA = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/"
# Libretro metadata lists (keyed by ROM CRC32): directory -> the key inside a game block.
LISTS = {
    "developer": "developer",
    "publisher": "publisher",
    "genre": "genre",
    "releaseyear": "releaseyear",
    "releasemonth": "releasemonth",
    "franchise": "franchise",
    "maxusers": "users",
    "esrb": "esrb_rating"
}
KINDS = ("info", "snap", "title")
PICTURES = {"snap": ("Named_Snaps", "snap.bmp"), "title": ("Named_Titles", "title.bmp")}
# Region words in Libretro file names, by the last letter of a DS/GBA game code.
REGIONS = {"E": "USA", "J": "Japan", "K": "Korea", "D": "Germany", "F": "France", "I": "Italy", "S": "Spain"}
MAX_ZIP = 24 * 1024 * 1024
BLOCK = re.compile(r'^game \(\s*$(.*?)^\)', re.M | re.S)


def parse_kinds(value):
    """'info,snap' -> ('info', 'snap'); unknown words are ignored."""
    return tuple(kind for kind in KINDS if kind in {word.strip() for word in (value or "").split(",")})


def parse_dat(raw, key):
    """{CRC: (game name, value)} from a Libretro clrmamepro list. The value is empty for the plain No-Intro list."""
    result = {}
    for block in BLOCK.findall(raw):
        crc = re.search(r'\bcrc ([0-9A-Fa-f]{8})\b', block)
        name = re.search(r'^\s*(?:comment|name) "([^"\n]+)"', block, re.M)
        if not crc or not name: continue
        value = ""
        if key:
            found = re.search(r'^\s*' + re.escape(key) + r'\s+"?([^"\n]*?)"?\s*$', block, re.M)
            if not found: continue
            value = found[1].strip()
        result[crc[1].upper()] = (name[1], value)
    if not result: raise ScraperError("The game information list was unreadable.")
    return result


def rom_crc(path):
    """CRC32 of the game inside a ROM file or a single-game ZIP (the ZIP already stores it)."""
    from play_options import SYSTEMS
    path = Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            entries = [
                i for i in archive.infolist()
                if any(Path(i.filename).suffix.lower() in suffixes for suffixes in SYSTEMS.values())
            ]
            if len(entries) != 1: raise NotFound("ZIP must contain one game.")
            return "%08X" % entries[0].CRC
    value = 0
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk: break
            value = zlib.crc32(chunk, value)
    return "%08X" % value


def clean_genre(value):
    """'simulation,life simulation' -> 'Simulation, Life simulation'."""
    parts = [part.strip() for part in re.split(r"[,/]", value or "") if part.strip()]
    return ", ".join(part[:1].upper() + part[1:] for part in parts)


def release_text(year, month="", day=""):
    year, month, day = (str(x or "").strip() for x in (year, month, day))
    if not year.isdigit(): return ""
    text = year.zfill(4)
    if month.isdigit() and 1 <= int(month) <= 12:
        text += "-%02d" % int(month)
        if day.isdigit() and 1 <= int(day) <= 31: text += "-%02d" % int(day)
    return text


def game_record(game):
    """One GameTDB <game> element as plain data: scalar fields plus {language: text} maps."""
    def text(tag):
        found = game.find(tag)
        return (found.text or "").strip() if found is not None else ""

    record = {"titles": {}, "synopses": {}}
    for locale in game.findall("locale"):
        language = (locale.get("lang") or "").upper()
        if not re.fullmatch(r"[A-Z]{2,3}", language): continue
        title = (locale.findtext("title") or "").strip()
        synopsis = (locale.findtext("synopsis") or "").strip()
        if title: record["titles"][language] = title
        if synopsis: record["synopses"][language] = synopsis
    date = game.find("date")
    rating = game.find("rating")
    wifi = game.find("wi-fi")
    controls = game.find("input")
    players = [0]
    for node, attribute in ((controls, "players-single-cart"), (controls, "players-multi-cart"), (wifi, "players")):
        value = node.get(attribute, "") if node is not None else ""
        if value.isdigit(): players.append(int(value))
    record.update(
        developer=text("developer"),
        publisher=text("publisher"),
        genre=clean_genre(text("genre")),
        release=release_text(*(date.get(k) for k in ("year", "month", "day"))) if date is not None else "",
        rating=(rating.get("type", "") + " " + rating.get("value", "")).strip() if rating is not None else "",
        players=max(players),
        online=1 if wifi is not None else 0,
        features=", ".join(f.text.strip() for f in (wifi.findall("feature") if wifi is not None else []) if f.text),
        languages=text("languages"),
        region=text("region")
    )
    return record


def gametdb_records(source, wanted):
    """{game code: record} for the codes in `wanted`, streamed from the GameTDB zip so memory stays small."""
    found = {}
    with zipfile.ZipFile(source) as archive:
        names = [n for n in archive.namelist() if n.lower().endswith(".xml")]
        if not names: raise ScraperError("The game information database was unreadable.")
        with archive.open(names[0]) as stream:
            try:
                for _, element in ElementTree.iterparse(stream, events=("end",)):
                    if element.tag != "game": continue
                    code = (element.findtext("id") or "").strip().upper()
                    if code in wanted and code not in found: found[code] = game_record(element)
                    element.clear()
            except ElementTree.ParseError:
                raise ScraperError("The game information database was unreadable.") from None
    return found


def pick_name(index, candidates, code=""):
    """The Libretro picture name for the first candidate that matches: exact, else the same title in the ROM's region."""
    names = set(index)
    for candidate in candidates:
        if candidate in names: return candidate
    keys = {}
    for name in index:
        keys.setdefault(art.title_key(name), []).append(name)
    preferred = REGIONS.get(code[-1:], "Europe") if code else ""
    for candidate in candidates:
        matches = keys.get(art.title_key(candidate), [])
        if not matches: continue
        regional = [m for m in matches if preferred and "(" + preferred in m]
        return sorted(regional or matches, key=lambda x: ("(Rev " in x, len(x), x))[0]
    return ""


class GameInfo:
    """Fetches the optional pieces named in `kinds` for each game, through a PublicClient's throttled fetch."""
    def __init__(self, client, kinds):
        self.client, self.kinds = client, tuple(kinds)
        self.records = {}  # GameTDB: game code -> record
        self.records_error = ""
        self.lists = {}  # system -> {directory: {crc: (name, value)}}
        self.names = {}  # system -> {crc: No-Intro name}
        self.indexes = {}  # (system, kind) -> picture names

    # --- skipping -----------------------------------------------------------------------------------------------
    def needs(self, item, kind, force):
        if force: return True
        if kind == "info" and "info" in self.kinds:
            status = read_fields(item / "info-status.ini")
            return not (status.get("info") in ("ok", "missing") and status.get("version") == str(VERSION))
        if kind in PICTURES and kind in self.kinds:
            if (item / PICTURES[kind][1]).exists(): return False
            return read_fields(item / "info-status.ini").get(kind) != "missing"
        return False

    def prepare(self, roms, data_item, force=False):
        """Download GameTDB once if some DS game still needs information. `data_item(rom)` is its library folder."""
        if "info" not in self.kinds: return
        from play_options import system_of
        wanted = set()
        for rom in roms:
            if system_of(rom) != "nds" or not self.needs(data_item(rom), "info", force): continue
            try:
                code = art.rom_details(rom)[1]
            except (NotFound, OSError, ValueError, zipfile.BadZipFile):
                continue
            if code: wanted.add(code)
        if not wanted: return
        try:
            self.records = gametdb_records(self.gametdb_file(), wanted)
        except ScraperError as error:
            self.records_error = str(error)

    def gametdb_file(self):
        cache = self.client.cache
        path = cache / "gametdb-ds.zip" if cache else None
        if path:
            try:
                if time.time() - path.stat().st_mtime < 7 * 86400: return path
            except OSError:
                pass
        try:
            raw = self.client.fetch(GAMETDB, limit=MAX_ZIP)
            if raw[:2] != b"PK": raise ScraperError("The game information database was unreadable.")
        except ScraperError:
            if path and path.exists(): return path  # an older copy beats none
            raise
        if not path:
            import io
            return io.BytesIO(raw)
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, pending = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(raw)
            os.replace(pending, path)
        finally:
            Path(pending).unlink(missing_ok=True)
        return path

    # --- Libretro lists -----------------------------------------------------------------------------------------
    def table(self, system, directory):
        tables = self.lists.setdefault(system, {})
        if directory not in tables:
            name = art.CATALOGS[system]
            url = METADATA + art.urllib.parse.quote(directory) + "/" + art.urllib.parse.quote(name) + ".dat"
            key = LISTS[directory]
            tables[directory] = self.client.catalog(
                "%s-%s.dat" % (system, directory), url, lambda raw, key=key: parse_dat(raw, key)
            )
        return tables[directory]

    def crc_names(self, system):
        if system not in self.names:
            raw = self.client.catalog(system + "-nointro.dat", art.serials_url(system), lambda raw: parse_dat(raw, ""))
            self.names[system] = {crc: value[0] for crc, value in raw.items()}
        return self.names[system]

    # --- information ----------------------------------------------------------------------------------------------
    def information(self, rom, system, code):
        """({info.ini fields}, {language: synopsis}, provider) for one game."""
        if system == "nds":
            if self.records_error: raise ScraperError(self.records_error)
            record = self.records.get(code)
            if not record: raise NotFound("No entry in the game information database.")
            language = {
                "E": "EN",
                "J": "JA",
                "D": "DE",
                "F": "FR",
                "I": "IT",
                "S": "ES",
                "K": "KO",
                "N": "NO",
                "H": "NL",
                "V": "SE"
            }.get(code[-1:], "EN")
            titles = record["titles"]
            fields = {
                "info_title": titles.get(language) or titles.get("EN") or next(iter(titles.values()), ""),
                "developer": record["developer"],
                "publisher": record["publisher"],
                "genre": record["genre"],
                "release": record["release"],
                "rating": record["rating"],
                "players": record["players"] or "",
                "online": record["online"],
                "features": record["features"],
                "languages": record["languages"],
                "region": record["region"]
            }
            return fields, record["synopses"], "GameTDB"
        crc = rom_crc(rom)
        found = {directory: self.table(system, directory).get(crc) for directory in LISTS}
        if not any(found.values()):
            raise NotFound("No entry in the game information lists.")
        value = lambda directory: (found[directory] or ("", ""))[1]
        year, month = value("releaseyear"), value("releasemonth")
        users = value("maxusers")
        fields = {
            "info_title": self.crc_names(system).get(crc, "") and art.display_title(self.crc_names(system)[crc]),
            "developer": value("developer"),
            "publisher": value("publisher"),
            "genre": clean_genre(value("genre")),
            "release": release_text(year, month),
            "rating": ("ESRB " + value("esrb")) if value("esrb") else "",
            "players": users if users.isdigit() else "",
            "franchise": value("franchise")
        }
        return fields, {}, "Libretro database"

    def save_information(self, item, fields, synopses, provider):
        info = read_fields(item / "info.ini")
        # The library's own title stays: info_title is separate, and an existing developer or year is kept.
        year = fields.get("release", "")[:4]
        merged = dict(info)
        for key, value in fields.items():
            if value in ("", None): continue
            if key == "developer" and info.get("developer"): continue
            merged[key] = value
        if year and not info.get("year"): merged["year"] = year
        previous = info.get("info_provider", "")
        merged["info_provider"] = provider if not previous or provider in previous else previous + ", " + provider
        merged["info_version"] = VERSION
        merged["synopsis_languages"] = ",".join(sorted(synopses))
        for language, text in synopses.items():
            write_fields_text(item / ("synopsis.%s.txt" % language), text)
        write_fields(item / "info.ini", merged)

    # --- pictures ---------------------------------------------------------------------------------------------------
    def index(self, system, kind):
        key = (system, kind)
        if key not in self.indexes:
            base = art.libretro_base(system).replace("Named_Boxarts", PICTURES[kind][0])

            def parse(raw):
                parser = art.CoverIndex()
                parser.feed(raw)
                if not parser.names: raise ScraperError("The picture catalog was unreadable.")
                return parser.names

            self.indexes[key] = (base, self.client.catalog("%s-%s.html" % (system, kind), base, parse))
        return self.indexes[key]

    def picture(self, item, kind, rom, system, name, code):
        base, index = self.index(system, kind)
        info = read_fields(item / "info.ini")
        override = read_fields(item / "artwork.ini").get("match_title", "").strip()
        candidates = [override] if override else []
        if info.get("provider") == "Libretro" and info.get("provider_id"): candidates.append(info["provider_id"])
        if system != "nds":
            # The ROM's CRC names it exactly in No-Intro, which is how Libretro names its pictures.
            try:
                candidates.append(self.crc_names(system).get(rom_crc(rom), ""))
            except (NotFound, OSError, zipfile.BadZipFile):
                pass
        candidates.append(name)
        if code and system in ("nds", "gba"): candidates.extend(self.client.names_for_code(code, system))
        selected = pick_name(index, [c for c in candidates if c], code)
        if not selected: raise NotFound("No matching picture in the public catalog.")
        raw = self.client.fetch(base + art.urllib.parse.quote(selected, safe="") + ".png")
        image_to_bmp(raw, item / PICTURES[kind][1])

    # --- one game ---------------------------------------------------------------------------------------------------
    def process(self, rom, item, force=False):
        """Run every requested piece for one game; returns {kind: 'ok'|'missing'|'cached'|'error: message'}."""
        outcome = {}
        status = read_fields(item / "info-status.ini")
        system = code = name = None
        for kind in self.kinds:
            if not self.needs(item, kind, force):
                outcome[kind] = "cached"
                continue
            try:
                if system is None:
                    system, name, code = art.rom_identity(rom)
                if kind == "info":
                    fields, synopses, provider = self.information(rom, system, code)
                    self.save_information(item, fields, synopses, provider)
                else:
                    self.picture(item, kind, rom, system, name, code)
                outcome[kind] = status[kind] = "ok"
            except NotFound:
                outcome[kind] = status[kind] = "missing"
            except ScraperError as error:
                outcome[kind] = "error: " + str(error)
                status.pop(kind, None)
            except (OSError, ValueError, zipfile.BadZipFile):
                outcome[kind] = "error: Could not read this game."
                status.pop(kind, None)
        status["version"] = VERSION
        write_fields(item / "info-status.ini", status, durable=False)
        return outcome


def write_fields_text(path, text):
    """Atomically write a text file (a synopsis); identical content is left alone."""
    content = text.replace("\r\n", "\n").replace("\r", "\n").strip() + "\n"
    try:
        if path.read_text(encoding="utf-8") == content: return
    except (OSError, UnicodeError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, pending = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
        os.replace(pending, path)
    finally:
        Path(pending).unlink(missing_ok=True)
