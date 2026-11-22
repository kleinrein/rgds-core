"""ScreenScraper as an artwork source, for every system: covers, game information and extra pictures.

Optional and signed in: the player's own screenscraper.fr account (scraper.py login) plus this build's developer
credentials (scraper.credentials). One jeuInfos request per game answers the cover, the details and the pictures
together, which is why it is quicker than the public catalogs. A game ScreenScraper does not know, or a run that
reached the account's daily quota, falls back to the public catalogs (public_artwork.PublicClient), so turning it on
never loses a cover the public sources would have found.

Only the ROM's CRC32, size and file name are sent; ROM contents are never uploaded. The CRC of a zipped game comes
from the ZIP directory and big discs (PSP, 3DS, PlayStation) are matched by name and size, so nothing is unpacked or
read whole.
"""
import re
import time
import zipfile
from pathlib import Path

import public_artwork as art
from game_info import GameInfo, clean_genre, release_text, rom_crc
from age_rating import minimum_age
from scraper import Client, ScraperError, NotFound, image_to_bmp

# ScreenScraper's systemesListe ids for the launcher's systems.
SYSTEMS = {
    "md": 1,
    "sms": 2,
    "nes": 3,
    "snes": 4,
    "gb": 9,
    "gbc": 10,
    "gba": 12,
    "n64": 14,
    "nds": 15,
    "3ds": 17,
    "gg": 21,
    "segacd": 20,
    "sg": 109,
    "pico": 250,
    "pico8": 234,
    "coleco": 48,
    "intellivision": 115,
    "msx": 113,
    "cpc": 65,
    "pce": 31,
    "pcecd": 114,
    "atari": 26,
    "atari7800": 41,
    "atari5200": 40,
    "lynx": 28,
    # One shelf each, two lists: public_artwork.rom_identity names a Color game's
    "wonderswan": 45,
    "wsc": 46,
    "ngp": 25,
    "ngpc": 82,
    "vb": 11,
    "amiga": 64,
    "dos": 135,
    "gw": 52,
    "arcade": 75,
    "psx": 57,
    "psp": 61,
    "dc": 23,
    "c64": 66,
    "zx": 76,
    "scummvm": 123
}
# Bigger than this is matched by name and size: a disc's CRC means reading all of it.
HASH_LIMIT = 64 * 1024**2
INDEX_FILES = (".cue", ".m3u", ".m3u8", ".ccd", ".toc", ".gdi", ".chd")
# Release words in No-Intro style names -> ScreenScraper regions.
NAME_REGIONS = (
    ("USA", "us"), ("Europe", "eu"), ("Japan", "jp"), ("World", "wor"), ("Germany", "de"), ("France", "fr"),
    ("Spain", "sp"), ("Italy", "it"), ("Korea", "kr"), ("Australia", "au"), ("Brazil", "br"), ("Netherlands", "nl"),
    ("Sweden", "se"), ("Norway", "no"), ("Denmark", "dk"), ("Finland", "fi"), ("China", "cn")
)
# The last letter of a DS/GBA/3DS game code -> region.
CODE_REGIONS = {
    "E": "us",
    "J": "jp",
    "P": "eu",
    "D": "de",
    "F": "fr",
    "I": "it",
    "S": "sp",
    "K": "kr",
    "H": "nl",
    "N": "no",
    "V": "eu",
    "U": "au"
}
FALLBACK_REGIONS = ("wor", "eu", "us", "ss", "jp")
# The age boards the launcher knows (age_rating.py), in the order one is preferred.
BOARDS = ("PEGI", "ESRB", "USK", "CERO")
# Errors after which this run stops asking ScreenScraper and uses the public catalogs for the rest.
STOPPING = (401, 403, 430, 431)
FAILURE_LIMIT = 5
PICTURE_MEDIA = {"snap": ("ss", "snap.bmp"), "title": ("sstitle", "title.bmp")}


def game_entry(path, system):
    """(file name, size, CRC32 or '') of the game itself: the one game file in a ZIP, a folder for ScummVM."""
    path = Path(path)
    if system == "scummvm": return path.parent.name, 0, ""
    if path.suffix.lower() == ".7z": return path.name, 0, ""  # its contents unreadable here: by name only
    if system == "dos" and path.suffix.lower() == ".zip": return path.name, path.stat().st_size, ""  # a zipped folder
    if path.suffix.lower() == ".zip":
        from play_options import SYSTEMS as SUFFIXES
        suffixes = {s for values in SUFFIXES.values() for s in values} | {".nds", ".dsi"}
        with zipfile.ZipFile(path) as archive:
            entries = [i for i in archive.infolist() if Path(i.filename).suffix.lower() in suffixes]
        if len(entries) != 1: raise NotFound("ZIP must contain one game.")
        entry = entries[0]
        return Path(entry.filename).name, entry.file_size, "%08X" % (entry.CRC & 0xffffffff)
    size = path.stat().st_size
    # A disc's index file (or its list) is a few hundred bytes naming the tracks: its CRC matches no game.
    if size > HASH_LIMIT or path.suffix.lower() in INDEX_FILES: return path.name, size, ""
    return path.name, size, rom_crc(path)


def regions(name, code=""):
    """ScreenScraper regions to try, the ROM's own first."""
    found = [region for word, region in NAME_REGIONS if re.search(r"\(([^)]*\b)?" + word + r"\b", name)]
    if code[-1:] in CODE_REGIONS: found.append(CODE_REGIONS[code[-1]])
    return tuple(dict.fromkeys(found + list(FALLBACK_REGIONS)))


def pick(entries, order, key="text"):
    """The value of the first entry in the preferred region, else the first one with a value."""
    if not isinstance(entries, list): return ""
    entries = [e for e in entries if isinstance(e, dict) and e.get(key)]
    for region in order:
        for entry in entries:
            if entry.get("region") == region: return str(entry[key]).strip()
    return str(entries[0][key]).strip() if entries else ""


def text_of(value):
    return str(value.get("text", "")).strip() if isinstance(value, dict) else ""


def information(jeu, order):
    """({info.ini fields}, {LANGUAGE: synopsis}) from one jeuInfos game."""
    genres = []
    for genre in sorted([g for g in jeu.get("genres") or [] if isinstance(g, dict)], key=lambda g: str(g.get("principale", "0")) != "1"):
        names = [n for n in genre.get("noms") or [] if isinstance(n, dict)]
        english = next((n.get("text") for n in names if n.get("langue") == "en"), "")
        if english and english not in genres: genres.append(english)
    date = pick(jeu.get("dates"), order)
    year, month, day = (re.findall(r"\d+", date) + ["", "", ""])[:3]
    rating = ""
    ratings = {str(c.get("type", "")).upper(): str(c.get("text", "")).strip() for c in jeu.get("classifications") or [] if isinstance(c, dict)}
    for board in BOARDS:
        if minimum_age(board + " " + ratings.get(board, "")) is not None:
            rating = board + " " + ratings[board]
            break
    players = [int(n) for n in re.findall(r"\d+", text_of(jeu.get("joueurs")))]
    synopses = {}
    for entry in jeu.get("synopsis") or []:
        if not isinstance(entry, dict): continue
        language = str(entry.get("langue", "")).upper()
        if re.fullmatch(r"[A-Z]{2,3}", language) and str(entry.get("text", "")).strip():
            synopses.setdefault(language, str(entry["text"]).strip())
    fields = {
        "info_title": pick(jeu.get("noms"), order),
        "developer": text_of(jeu.get("developpeur")),
        "publisher": text_of(jeu.get("editeur")),
        # "Action" and "Action / Adventure" are two genres there: each word once.
        "genre": ", ".join(dict.fromkeys(clean_genre(", ".join(genres[:2])).split(", "))),
        "release": release_text(year, month, day),
        "rating": rating,
        "players": str(max(players)) if players else ""
    }
    return fields, synopses


class ScreenScraperClient:
    """Covers from ScreenScraper with a public-catalog fallback; the same shape as PublicClient.artwork."""
    provider = "screenscraper"

    def __init__(self, client, public):
        self.client, self.public = client, public
        self.cache = public.cache  # GameInfo keeps its catalogs here
        self.stopped = ""  # why this run stopped asking ScreenScraper, if it did
        self.failures = 0  # games in a row it could not answer (busy, closed, unreadable)
        self.games = {}  # (rom, matching title) -> jeu or the NotFound it gave: the cover and the details share it

    def login(self):
        return self.client.login()

    def request(self, endpoint, params=None, media=False):
        for attempt in range(2):
            try:
                return self.client.request(endpoint, params, media)
            except NotFound:
                raise
            except ScraperError as error:
                code = getattr(error, "status", 0)
                if code in STOPPING:
                    self.stopped = str(error)
                    raise
                if code != 429 or attempt: raise
                time.sleep(3)  # too many requests at once for this account: once more, slower

    def game(self, rom, name_override=""):
        key = (str(rom), name_override.strip())
        if key not in self.games:
            if len(self.games) > 4: self.games.clear()  # one game at a time; a few kept for its pictures
            try:
                self.games[key] = self.lookup(rom, name_override.strip())
            except NotFound as missing:
                self.games[key] = missing
        found = self.games[key]
        if isinstance(found, NotFound): raise found
        return found

    def lookup(self, rom, override):
        system, name, code = art.rom_identity(rom)
        if system not in SYSTEMS: raise NotFound("ScreenScraper has no list for this system.")
        if override:
            # A matching title the player typed: ScreenScraper's search, best match first.
            found = self.request("jeuRecherche.php", {"systemeid": SYSTEMS[system], "recherche": override})
            games = found.get("jeux") if isinstance(found, dict) else None
            games = [g for g in games or [] if isinstance(g, dict) and str(g.get("id", "")).isdigit()]
            if not games: raise NotFound("ScreenScraper has no game by that title.")
            jeu = games[0]
            if not jeu.get("medias"):
                jeu = self.request("jeuInfos.php", {"systemeid": SYSTEMS[system], "gameid": jeu["id"]}).get("jeu")
        else:
            filename, size, crc = game_entry(rom, system)
            params = {"systemeid": SYSTEMS[system], "romtype": "dossier" if system == "scummvm" else "rom"}
            params["romnom"] = filename
            if size: params["romtaille"] = size
            if crc: params["crc"] = crc
            jeu = self.request("jeuInfos.php", params).get("jeu")
        if not isinstance(jeu, dict) or not str(jeu.get("id", "")).isdigit():
            raise NotFound("ScreenScraper has no match for this game.")
        jeu["_system"], jeu["_order"] = system, regions(name, code)
        return jeu

    def media(self, jeu, kind):
        """A picture of `kind` (box-2D, ss, sstitle) in the best region, at most 384 px."""
        entries = [m for m in jeu.get("medias") or [] if isinstance(m, dict) and m.get("type") == kind]
        if not entries: raise NotFound("ScreenScraper has no picture of this kind for this game.")
        region = next((r for r in jeu["_order"] for m in entries if m.get("region") == r), entries[0].get("region"))
        return self.request(
            "mediaJeu.php", {
                "systemeid": SYSTEMS[jeu["_system"]],
                "jeuid": jeu["id"],
                "media": "%s(%s)" % (kind, region) if region else kind,
                "maxwidth": 384,
                "maxheight": 384,
                "outputformat": "png"
            },
            media=True
        )

    def artwork(self, rom, name_override=""):
        if not self.stopped:
            try:
                jeu = self.game(rom, name_override)
                raw = self.media(jeu, "box-2D")
                self.failures = 0
                return raw, {
                    "provider": "ScreenScraper",
                    "provider_id": str(jeu["id"]),
                    "artwork_title": pick(jeu.get("noms"), jeu["_order"]),
                    "artwork_match": "title" if name_override.strip() else "rom"
                }
            except NotFound:
                self.failures = 0
            except ScraperError as error:
                # Busy or closed for this game: the public catalogs answer it. Several in a row mean the service is
                # down, and the rest of the run does not wait for it.
                self.failures += 1
                if self.failures >= FAILURE_LIMIT and not self.stopped: self.stopped = str(error)
        return self.public.artwork(rom, name_override)


class ScreenScraperInfo(GameInfo):
    """Game information and extra pictures from the game ScreenScraper already found for the cover."""
    def __init__(self, screenscraper, kinds):
        super().__init__(screenscraper.public, kinds)
        self.ss = screenscraper

    def prepare(self, roms, data_item, force=False):
        pass  # no GameTDB download: ScreenScraper has the DS and 3DS details too

    def supports(self, system):
        return system in SYSTEMS or super().supports(system)

    def process(self, rom, item, force=False):
        self.item = item  # its matching title picks the same game the cover used
        return super().process(rom, item, force)

    def jeu(self, rom, item):
        if self.ss.stopped: raise NotFound(self.ss.stopped)
        from library import read_fields
        return self.ss.game(rom, read_fields(item / "artwork.ini").get("match_title", ""))

    def information(self, rom, system, code):
        try:
            jeu = self.jeu(rom, self.item)
        except (NotFound, ScraperError):
            if system in ("nds", "3ds") or not super().supports(system): raise
            return super().information(rom, system, code)
        fields, synopses = information(jeu, jeu["_order"])
        if not any(fields.values()) and not synopses: raise NotFound("ScreenScraper has no details for this game.")
        return fields, synopses, "ScreenScraper"

    def picture(self, item, kind, rom, system, name, code):
        try:
            raw = self.ss.media(self.jeu(rom, item), PICTURE_MEDIA[kind][0])
        except NotFound:
            if not super().supports(system): raise
            return super().picture(item, kind, rom, system, name, code)
        except ScraperError:
            if not self.ss.stopped or not super().supports(system): raise
            return super().picture(item, kind, rom, system, name, code)
        image_to_bmp(raw, item / PICTURE_MEDIA[kind][1])


def open_client(auth, public):
    """Signed in and paced to the account's own request rate."""
    client = ScreenScraperClient(Client(auth), public)
    client.login()
    return client
