"""Credential-free cover providers for every system. No ROM bytes are uploaded."""
import re
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from html.parser import HTMLParser
from pathlib import Path

# Imported lazily by scraper so startup/indexing never loads network catalogs.
from scraper import ScraperError, NotFound

PROVIDERS = ("auto", "gametdb", "libretro")
# Libretro's box-art catalogue and No-Intro serials, per system (GameTDB is DS only).
CATALOGS = {
    "nds": "Nintendo - Nintendo DS",
    "gba": "Nintendo - Game Boy Advance",
    "gbc": "Nintendo - Game Boy Color",
    "gb": "Nintendo - Game Boy"
}


def libretro_base(system):
    return "https://thumbnails.libretro.com/" + urllib.parse.quote(CATALOGS[system]) + "/Named_Boxarts/"


def serials_url(system):
    return "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/" + urllib.parse.quote(
        CATALOGS[system]
    ) + ".dat"


LIBRETRO = libretro_base("nds")
SERIALS = serials_url("nds")
HOSTS = {"thumbnails.libretro.com", "art.gametdb.com", "www.gametdb.com", "raw.githubusercontent.com"}
# Only release tags are discarded. Parenthetical words can be part of a title.
TAGS = re.compile(
    r"\((?:USA|Europe|Japan|World|Australia|Canada|France|Germany|Italy|Spain|Korea|China|Taiwan|Norway|Denmark|Sweden|Netherlands|United Kingdom|Scandinavia|Russia|Rev \d+|Beta[^)]*|Demo[^)]*|Proto[^)]*|NDSi Enhanced|[A-Z][a-z](?:,[A-Z][a-z])*)\)",
    re.I
)


def display_title(name):
    name = TAGS.sub("", re.sub(r"\[[^]]*\]", "", name))
    name = re.sub(r"^\d{4}\s*[-.]\s*", "", name).strip()
    if name.endswith(", The"): name = "The " + name[:-5]
    return " ".join(name.split())


def serial_index(raw):
    result = {}
    for block in re.findall(r'^game \(\s*$(.*?)^\)', raw, re.M | re.S):
        name = re.search(r'^\s*name "([^"\n]+)"', block, re.M)
        code = re.search(r'^\s*serial "([A-Z0-9]{4})"', block, re.M)
        if name and code:
            result.setdefault(code[1], []).append(name[1])
    if not result: raise ScraperError("The artwork title catalog was unreadable.")
    return result


def allowed(url):
    parts = urllib.parse.urlsplit(url)
    return parts.scheme == "https" and parts.hostname in HOSTS and parts.port in (None, 443) and not parts.username


class PublicRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if not allowed(newurl) or urllib.parse.urlsplit(request.full_url
                                                        ).hostname != urllib.parse.urlsplit(newurl).hostname:
            raise ScraperError("Artwork service returned an unexpected redirect.")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def rom_details(path):
    path = Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            entries = [i for i in archive.infolist() if Path(i.filename).suffix.lower() in (".nds", ".dsi")]
            if len(entries) != 1 or entries[0].file_size > 1024**3:
                raise NotFound("ZIP must contain one DS game.")
            with archive.open(entries[0]) as stream:
                header = stream.read(16)
            name = Path(entries[0].filename).stem
    else:
        with path.open("rb") as stream:
            header = stream.read(16)
        name = path.stem
    code = header[12:16].decode("ascii", errors="replace")
    return name, code if re.fullmatch(r"[A-Z0-9]{4}", code) and code not in ("####", "0000") else ""


def rom_identity(path):
    """(system, name, game code). Other systems' codes: the GBA header's at 0xAC; GB has none."""
    from play_options import SYSTEMS, system_of
    system = system_of(path)
    if system == "nds":
        return (system,) + rom_details(path)
    path = Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            entries = [
                i for i in archive.infolist()
                if any(Path(i.filename).suffix.lower() in suffixes for suffixes in SYSTEMS.values())
            ]
            if len(entries) != 1 or entries[0].file_size > 64 * 1024**2: raise NotFound("ZIP must contain one game.")
            with archive.open(entries[0]) as stream:
                header = stream.read(0xB0)
            name = Path(entries[0].filename).stem
    else:
        with path.open("rb") as stream:
            header = stream.read(0xB0)
        name = path.stem
    code = header[0xAC:0xB0].decode("ascii", errors="replace") if system == "gba" else ""
    return system, name, code if re.fullmatch(r"[A-Z0-9]{4}", code) else ""


def title_key(name):
    name = display_title(name).translate(str.maketrans({"ø": "o", "Ø": "O", "æ": "ae", "Æ": "Ae", "œ": "oe"}))
    return "".join(c for c in unicodedata.normalize("NFKD", name).casefold() if c.isalnum())


class CoverIndex(HTMLParser):
    def __init__(self):
        super().__init__()
        self.names = []

    def handle_starttag(self, tag, attrs):
        href = dict(attrs).get("href", "")
        name = urllib.parse.unquote(href)
        if tag == "a" and name.endswith(".png") and "/" not in name and "\\" not in name:
            self.names.append(name[:-4])


class PublicClient:
    def __init__(self, provider="auto", opener=None, cache=None, cancelled=None):
        if provider not in PROVIDERS[:3]: raise ScraperError("Unknown public artwork provider.")
        self.provider = provider
        self.opener = opener or urllib.request.build_opener(PublicRedirect()).open
        self.next_request = 0.0
        self.index = None  # the DS catalogue
        self.serials = None
        self.indexes, self.serial_sets = {}, {}  # other systems
        self.cache = Path(cache) if cache else None
        self.cancelled = cancelled or (lambda: False)
        self.unavailable = {}

    def fetch(self, url, limit=8 * 1024 * 1024):
        if not allowed(url): raise ScraperError("Unrecognized artwork address.")
        host = urllib.parse.urlsplit(url).hostname
        if self.unavailable.get(host, 0) > time.monotonic():
            raise ScraperError("Artwork service temporarily unavailable. Retry missing artwork later.")
        for attempt in range(2):
            if self.cancelled(): raise KeyboardInterrupt()
            delay = self.next_request - time.monotonic()
            if delay > 0: time.sleep(delay)
            if self.cancelled(): raise KeyboardInterrupt()
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "RGDS/0.0.1 (personal artwork cache)"})
                with self.opener(request, timeout=12) as response:
                    if not allowed(response.geturl()) or host != urllib.parse.urlsplit(response.geturl()).hostname:
                        raise ScraperError("Artwork service returned an unexpected redirect.")
                    content = response.read(limit + 1)
                if len(content) > limit: raise ScraperError("Artwork response exceeds the size limit.")
                return content
            except urllib.error.HTTPError as error:
                try:
                    error.close()
                except (AttributeError, KeyError, OSError):
                    pass  # an error without a body (fp=None)
                if error.code == 404: raise NotFound("No matching cover found.") from None
                retryable = error.code in (408, 429, 500, 502, 503, 504)
                if not retryable or attempt:
                    self.unavailable[host] = time.monotonic() + 60
                    raise ScraperError(
                        "Artwork service unavailable (HTTP %d). Retry missing artwork later." % error.code
                    ) from None
            except (urllib.error.URLError, TimeoutError, OSError):
                if attempt:
                    self.unavailable[host] = time.monotonic() + 60
                    raise ScraperError("Could not reach artwork service. Check Wi-Fi.") from None
            finally:
                self.next_request = time.monotonic() + (1.0 if attempt == 0 else 0.5)
        raise ScraperError("Artwork service unavailable.")

    def catalog(self, filename, url, parse):
        path = self.cache / filename if self.cache else None
        saved = None
        if path:
            try:
                saved = path.read_bytes()
                if len(saved) > 8 * 1024 * 1024: saved = None
                elif time.time() - path.stat().st_mtime < 7 * 86400: return parse(saved.decode("utf-8-sig"))
            except (OSError, UnicodeError, ScraperError):
                saved = None
        try:
            raw = self.fetch(url)
            value = parse(raw.decode("utf-8-sig"))
        except (ScraperError, UnicodeError):
            if saved:
                try:
                    return parse(saved.decode("utf-8-sig"))
                except (UnicodeError, ScraperError):
                    pass
            raise ScraperError("The artwork catalog is unavailable. Retry missing artwork later.") from None
        if path:
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                pending = path.with_suffix(".pending")
                pending.write_bytes(raw)
                pending.replace(path)
            except OSError:
                pass  # A catalog cache is optional; the cover still works.
        return value

    def names_for_code(self, code, system="nds"):
        if not code: return []
        if system != "nds":
            if system not in self.serial_sets:
                self.serial_sets[system] = self.catalog(system + "-serials.dat", serials_url(system), serial_index)
            return self.serial_sets[system].get(code, [])
        if self.serials is None: self.serials = self.catalog("ds-serials.dat", SERIALS, serial_index)
        return self.serials.get(code, [])

    def gametdb(self, name, code):
        if not code: raise NotFound("This ROM has no retail game code.")
        region = {
            "E": "US",
            "J": "JA",
            "D": "DE",
            "F": "FR",
            "I": "IT",
            "S": "ES",
            "K": "KO",
            "N": "NO",
            "H": "NL",
            "V": "SE"
        }.get(code[-1], "EN")
        for language in dict.fromkeys((region, "EN", "US", "JA", *(("DK", "SE") if code[-1] == "N" else ()))):
            url = "https://art.gametdb.com/ds/coverS/%s/%s.png" % (language, code)
            try:
                raw = self.fetch(url)
                return raw, {"provider": "GameTDB", "provider_id": code, "artwork_url": url}
            except NotFound:
                pass
        raise NotFound("No matching GameTDB cover found.")

    def libretro(self, name, code, system="nds"):
        base = LIBRETRO if system == "nds" else libretro_base(system)
        exact = re.sub(r'[&*/:`<>?\\|]', '_', name)
        url = base + urllib.parse.quote(exact, safe="") + ".png"
        matched_by = "filename"
        try:
            raw = self.fetch(url)
        except NotFound:

            def parse(raw):
                parser = CoverIndex()
                parser.feed(raw)
                if not parser.names: raise ScraperError("The artwork catalog was unreadable.")
                return parser.names

            if system == "nds":
                if self.index is None: self.index = self.catalog("ds-covers.html", LIBRETRO, parse)
                index = self.index
            else:
                if system not in self.indexes:
                    self.indexes[system] = self.catalog(system + "-covers.html", base, parse)
                index = self.indexes[system]
            matches = [candidate for candidate in index if title_key(candidate) == title_key(name)]
            if not matches:
                # An exact header serial recovers renamed/localized dumps. No
                # approximate title/substring guesses that can cross sequels.
                names = self.names_for_code(code, system)
                keys = {title_key(candidate) for candidate in names}
                matches = [candidate for candidate in index if title_key(candidate) in keys]
                matched_by = "game_code"
            if not matches:
                raise NotFound("No cover in the public catalogs. Try a matching title in Game options > Artwork.")
            preferred = {
                "E": "USA",
                "J": "Japan",
                "K": "Korea",
                "N": "Norway",
                "D": "Germany",
                "F": "France"
            }.get(code[-1:] or "", "Europe")
            regional = [candidate for candidate in matches if "(" + preferred in candidate]
            choices = regional or matches
            exact_region = [candidate for candidate in choices if candidate == name]
            selected = exact_region[0] if exact_region else sorted(choices, key=lambda x: ("(Rev " in x, len(x), x))[0]
            # Different localized titles sharing a serial need the filename to
            # disambiguate. A provider must not silently choose another game.
            if len({title_key(candidate) for candidate in choices}) > 1:
                raise NotFound(
                    "More than one title matches this game code. Set the matching title in Game options > Artwork."
                )
            url = base + urllib.parse.quote(selected, safe="") + ".png"
            raw = self.fetch(url)
            exact = selected
        return raw, {
            "provider": "Libretro",
            "provider_id": exact,
            "artwork_url": url,
            "artwork_match": matched_by,
            "artwork_title": display_title(exact)
        }

    def artwork(self, rom, name_override=""):
        system, name, code = rom_identity(rom)
        if system != "nds":
            # Other systems: Libretro's catalogue for that system (GameTDB has DS covers only).
            return self.libretro(name_override.strip() or name, "" if name_override.strip() else code, system)
        name, code = rom_details(rom)
        # An explicit matching title intentionally overrides header identity for
        # hacks/homebrew or a correction. It never renames the ROM or save path.
        if name_override.strip(): return self.libretro(name_override.strip(), "")
        if self.provider == "gametdb": return self.gametdb(name, code)
        if self.provider == "libretro": return self.libretro(name, code)
        # Automatic prefers Libretro's box art (512 px, stored at 384) for the
        # large cover screen; GameTDB's coverS is 128x115. GameTDB's exact
        # game-code match remains the fallback for titles Libretro lacks.
        error = None
        try:
            return self.libretro(name, code)
        except ScraperError as failure:
            error = failure
        try:
            return self.gametdb(name, code)
        except NotFound:
            if error and not isinstance(error, NotFound): raise error
            raise
