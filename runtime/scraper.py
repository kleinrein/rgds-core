#!/usr/bin/env python3
"""Background artwork service using public, account-free providers."""
import argparse
import ctypes
import ctypes.util
import errno
from datetime import datetime, timezone
import getpass
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib

sys.dont_write_bytecode = True
from library import game_key, read_fields, write_fields, index_banner

API = "https://api.screenscraper.fr/api2/"
SYSTEM = 15  # Nintendo DS, ScreenScraper systemesListe.
REGIONS = ("eu", "us", "wor", "jp")


class ScraperError(Exception):
    pass


class NotFound(ScraperError):
    pass


class ServiceRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != "https" or target.hostname != "api.screenscraper.fr":
            raise ScraperError("ScreenScraper returned an unexpected redirect.")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def private_root(data):
    # The stock card's /userdata is ext4; SD /mnt/mmc is FAT and cannot enforce mode 0600.
    if sys.platform == "linux" and Path("/userdata").is_dir():
        return Path("/userdata/rgds/private")
    return Path(data) / "private"


def save_private(path, value):
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    path.parent.chmod(0o700)
    fd, pending = tempfile.mkstemp(prefix=".credentials-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        path.chmod(0o600)
    finally:
        Path(pending).unlink(missing_ok=True)


def read_private(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def credentials(folder, account=None):
    app = read_private(folder / "screenscraper-app.json")
    app = {
        "devid": os.environ.get("SCREENSCRAPER_DEVID", app.get("devid", "")),
        "devpassword": os.environ.get("SCREENSCRAPER_DEVPASSWORD", app.get("devpassword", ""))
    }
    if not all(isinstance(v, str) and v for v in app.values()):
        raise ScraperError("ScreenScraper is not configured for this build yet.")
    user = account if account is not None else read_private(folder / "screenscraper-account.json")
    if not all(isinstance(user.get(k), str) and user[k] for k in ("ssid", "sspassword")):
        raise ScraperError("Sign in to ScreenScraper first.")
    return dict(app, ssid=user["ssid"], sspassword=user["sspassword"], softname="RGDS", output="json")


class Client:
    def __init__(self, auth, opener=None):
        self.auth = auth
        self.opener = opener or urllib.request.build_opener(ServiceRedirect()).open
        self.interval = 2.0
        self.next_request = 0.0

    def request(self, endpoint, params=None, media=False):
        delay = self.next_request - time.monotonic()
        if delay > 0: time.sleep(delay)
        url = API + endpoint + "?" + urllib.parse.urlencode(dict(self.auth, **(params or {})))
        request = urllib.request.Request(
            url, headers={
                "User-Agent": "RGDS/0.0.1",
                "Accept": "image/*" if media else "application/json"
            }
        )
        try:
            with self.opener(request, timeout=20) as response:
                final = urllib.parse.urlsplit(response.geturl())
                if final.scheme != "https" or final.hostname != "api.screenscraper.fr":
                    raise ScraperError("ScreenScraper returned an unexpected redirect.")
                raw = response.read(8 * 1024 * 1024 + 1)
            if len(raw) > 8 * 1024 * 1024:
                raise ScraperError("ScreenScraper response exceeds the size limit.")
        except urllib.error.HTTPError as error:
            if error.code == 404: raise NotFound("No matching game found.") from None
            message = {
                401: "Check your ScreenScraper login.",
                403: "ScreenScraper refused access; check account and app configuration.",
                429: "ScreenScraper is busy. Try again later.",
                430: "Your daily ScreenScraper quota has been reached.",
                431: "ScreenScraper's daily unknown-game quota has been reached.",
                503: "ScreenScraper is temporarily unavailable."
            }
            raise ScraperError(
                message.get(error.code, "ScreenScraper request failed (HTTP %d)." % error.code)
            ) from None
        except (urllib.error.URLError, TimeoutError, OSError):
            raise ScraperError("Could not reach ScreenScraper. Check Wi-Fi and try again.") from None
        finally:
            self.next_request = time.monotonic() + self.interval
        if media:
            if raw in (b"NOMEDIA", b"CRCOK", b"MD5OK", b"SHA1OK"):
                raise NotFound("No box art available for this game.")
            return raw
        try:
            result = json.loads(raw)
            response = result.get("response", {})
            if not isinstance(response, dict): raise ValueError()
            return response
        except (ValueError, AttributeError):
            raise ScraperError("ScreenScraper returned an unreadable response.") from None

    def login(self):
        user = self.request("ssuserInfos.php").get("ssuser", {})
        if not isinstance(user, dict) or not user.get("id"):
            raise ScraperError("ScreenScraper did not accept this login.")
        try:
            quota = int(user.get("maxrequestspermin", 30))
            self.interval = max(2.0, 60.0 / max(1, quota))
        except (ValueError, TypeError):
            pass
        return user

    def game(self, rom):
        identity = rom_identity(rom)
        game = self.request("jeuInfos.php", dict(identity, systemeid=SYSTEM, romtype="rom")).get("jeu")
        if not isinstance(game, dict) or not str(game.get("id", "")).isdigit():
            raise NotFound("No matching game found.")
        return game


def rom_identity(path):
    path = Path(path)
    crc, md5, sha1, size = 0, hashlib.md5(), hashlib.sha1(), 0
    archive = None
    if path.suffix.lower() == ".zip":
        archive = zipfile.ZipFile(path)
        games = [i for i in archive.infolist() if Path(i.filename).suffix.lower() in (".nds", ".dsi")]
        if len(games) != 1 or games[0].file_size > 1024**3:
            archive.close()
            raise NotFound("ZIP must contain one DS game.")
        stream, name = archive.open(games[0]), Path(games[0].filename).name
    else:
        stream, name = path.open("rb"), path.name
    try:
        with stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk: break
                size += len(chunk)
                if size > 1024**3: raise ScraperError("ROM exceeds the supported size limit.")
                crc = zlib.crc32(chunk, crc)
                md5.update(chunk)
                sha1.update(chunk)
    finally:
        if archive: archive.close()
    return {
        "crc": "%08X" % (crc & 0xffffffff),
        "md5": md5.hexdigest(),
        "sha1": sha1.hexdigest(),
        "romtaille": size,
        "romnom": name
    }


def localized(entries, key):
    if not isinstance(entries, list): return ""
    for region in REGIONS:
        for entry in entries:
            if entry.get("region") == region and entry.get(key): return str(entry[key])
    return str(entries[0].get(key, "")) if entries else ""


def metadata(game):
    developer = game.get("developpeur", {})
    return {
        "title": localized(game.get("noms", []), "text"),
        "year": localized(game.get("dates", []), "text")[:4],
        "developer": str(developer.get("text", "")) if isinstance(developer, dict) else "",
        "provider": "ScreenScraper",
        "provider_id": str(game["id"])
    }


def image_to_bmp(content, path):
    """Use the installed SDL_image decoder; never ship another device display library."""
    sdl_name = os.environ.get("RGDS_SDL_LIBRARY") or ctypes.util.find_library("SDL2")
    image_name = ctypes.util.find_library("SDL2_image")
    if not sdl_name:  # find_library needs ldconfig or gcc, which the device images do not ship
        for candidate in (
            "/usr/lib/libSDL2-2.0.so.0", "/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0",
            "/opt/homebrew/lib/libSDL2.dylib"
        ):
            if Path(candidate).exists():
                sdl_name = candidate
                break
    if not image_name:
        for candidate in ("/usr/lib/libSDL2_image-2.0.so.0", "/opt/homebrew/lib/libSDL2_image.dylib"):
            if Path(candidate).exists():
                image_name = candidate
                break
    if not sdl_name or not image_name:
        raise ScraperError("Image decoder unavailable. Install SDL2_image for desktop scraping.")
    sdl, image = ctypes.CDLL(sdl_name), ctypes.CDLL(image_name)
    sdl.SDL_RWFromConstMem.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sdl.SDL_RWFromConstMem.restype = ctypes.c_void_p
    image.IMG_Load_RW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    image.IMG_Load_RW.restype = ctypes.c_void_p
    sdl.SDL_RWFromFile.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    sdl.SDL_RWFromFile.restype = ctypes.c_void_p
    sdl.SDL_SaveBMP_RW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    sdl.SDL_SaveBMP_RW.restype = ctypes.c_int
    sdl.SDL_FreeSurface.argtypes = [ctypes.c_void_p]

    class Surface(ctypes.Structure):
        _fields_ = [("flags", ctypes.c_uint32), ("format", ctypes.c_void_p), ("w", ctypes.c_int), ("h", ctypes.c_int)]

    buffer = ctypes.create_string_buffer(content)
    surface = image.IMG_Load_RW(sdl.SDL_RWFromConstMem(buffer, len(content)), 1)
    if not surface: raise ScraperError("The downloaded artwork could not be decoded.")
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix(".pending.bmp")
    sdl.SDL_ConvertSurfaceFormat.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    sdl.SDL_ConvertSurfaceFormat.restype = ctypes.c_void_p
    sdl.SDL_CreateRGBSurfaceWithFormat.argtypes = [
        ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint32
    ]
    sdl.SDL_CreateRGBSurfaceWithFormat.restype = ctypes.c_void_p
    # SDL_SoftStretchLinear (2.0.16+) filters; older SDL falls back to nearest.
    stretch = getattr(sdl, "SDL_SoftStretchLinear", None) or sdl.SDL_SoftStretch
    stretch.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    stretch.restype = ctypes.c_int
    surfaces = [surface]
    try:
        size = ctypes.cast(surface, ctypes.POINTER(Surface)).contents
        width, height = size.w, size.h
        if width < 1 or height < 1 or width * height > 4 * 1024**2:
            raise ScraperError("Artwork dimensions exceed the limit.")
        result = surface
        if max(width, height) > 384:
            # Paletted and 24-bit covers (common on thumbnails.libretro.com) can
            # not be scaled by a blit: stretch one 32-bit copy instead, halving
            # first so a large cover is filtered rather than decimated.
            current = sdl.SDL_ConvertSurfaceFormat(surface, 0x16362004, 0)  # ARGB8888
            if not current: raise ScraperError("Could not prepare the downloaded artwork.")
            surfaces.append(current)
            ratio = 384 / max(width, height)
            final = max(1, round(width * ratio)), max(1, round(height * ratio))
            while True:
                step = (max(final[0], width // 2), max(final[1], height // 2)) if width >= final[0] * 2 else final
                smaller = sdl.SDL_CreateRGBSurfaceWithFormat(0, step[0], step[1], 32, 0x16362004)
                if not smaller: raise ScraperError("Could not prepare the downloaded artwork.")
                surfaces.append(smaller)
                if stretch(current, None, smaller, None) != 0:
                    raise ScraperError("Could not resize the downloaded artwork.")
                current, (width, height) = smaller, step
                if step == final: break
            result = current
        destination = sdl.SDL_RWFromFile(os.fsencode(pending), b"wb")
        if not destination or sdl.SDL_SaveBMP_RW(result, destination, 1) != 0:
            raise ScraperError("Could not save the downloaded artwork.")
        pending.replace(path)
    finally:
        for item in reversed(surfaces):
            sdl.SDL_FreeSurface(item)
        pending.unlink(missing_ok=True)


# Covers narrower than this predate the large cover screen (GameTDB coverS is
# 128x115). "Fetch missing artwork" tries once per game for a sharper one.
SHARP_WIDTH = 200


def low_resolution(path):
    try:
        with open(path, "rb") as stream:
            header = stream.read(26)
    except OSError:
        return False
    return len(header) == 26 and header[:2] == b"BM" and abs(struct.unpack_from("<i", header, 18)[0]) < SHARP_WIDTH


def system_games(entries):
    """Other systems' games (RG DS Play) from --system-dir system=path entries."""
    from play_options import SYSTEMS, FOLDER_EXTENSIONS
    result = set()
    for entry in entries:
        system, _, folder = entry.partition("=")
        root = Path(folder)
        if system not in SYSTEMS or not root.is_dir(): continue
        suffixes = SYSTEMS[system] + FOLDER_EXTENSIONS.get(system, ()) + (".zip",)
        result.update(
            p.resolve() for p in root.rglob("*") if p.is_file() and not p.is_symlink() and p.suffix.lower() in suffixes
        )
    return sorted(result)


def games(roots):
    return sorted(
        {
            p.resolve()
            for root in roots if root.is_dir() for p in root.rglob("*")
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in (".nds", ".dsi", ".zip")
        }
    )


def scrape(data, client, roms, force=False, extras=None):
    folder = data / "scraper"
    total, completed, missing, failed, cached, downloaded = len(roms), 0, 0, 0, 0, 0
    report = []

    def publish(state, message, game=""):
        write_fields(
            folder / "status.ini", {
                "state": state,
                "message": message,
                "completed": completed,
                "total": total,
                "missing": missing,
                "failed": failed,
                "cached": cached,
                "downloaded": downloaded,
                "game": game
            },
            durable=False
        )

    try:
        if extras:
            publish("scraping", "Preparing game information")
            extras.prepare(roms, lambda rom: data / "library" / game_key(rom), force)
        for rom in roms:
            if (folder / "cancel").exists(): raise KeyboardInterrupt()
            item = data / "library" / game_key(rom)
            publish("scraping", "Fetching artwork", rom.stem)
            outcome = {
                "game": rom.stem,
                "key": game_key(rom),
                "state": "cached",
                "message": "Artwork already available."
            }
            upgrade = (
                not force and hasattr(client, "artwork") and getattr(client, "provider", "auto") != "gametdb"
                and low_resolution(item / "box.bmp") and read_fields(item / "info.ini").get("sharp_checked") != "1"
            )
            if not force and not upgrade and (item / "box.bmp").exists():
                cached += 1
            else:
                try:
                    if hasattr(client, "artwork"):
                        override = read_fields(item / "artwork.ini").get("match_title", "")
                        raw, fields = client.artwork(rom, name_override=override) if override else client.artwork(rom)
                    else:
                        game = client.game(rom)
                        fields = metadata(game)
                        media = game.get("medias", [])
                        region = next(
                            (
                                region for region in REGIONS
                                if any(m.get("type") == "box-2D" and m.get("region") == region for m in media)
                            ), "wor"
                        )
                        raw = client.request(
                            "mediaJeu.php", {
                                "systemeid": SYSTEM,
                                "jeuid": game["id"],
                                "media": "box-2D(" + region + ")",
                                "maxwidth": 384,
                                "maxheight": 384,
                                "outputformat": "png"
                            },
                            media=True
                        )
                    image_to_bmp(raw, item / "box.bmp")
                    existing = read_fields(item / "info.ini")
                    existing.update(fields)
                    if upgrade: existing["sharp_checked"] = "1"
                    write_fields(item / "info.ini", existing)
                    downloaded += 1
                    outcome.update(state="downloaded", message="Artwork saved.", provider=fields.get("provider", ""))
                except NotFound as error:
                    if upgrade:
                        # No sharper cover exists: keep the small one, ask once.
                        existing = read_fields(item / "info.ini")
                        existing["sharp_checked"] = "1"
                        write_fields(item / "info.ini", existing)
                        cached += 1
                    else:
                        missing += 1
                        outcome.update(state="missing", message=str(error) or "No matching cover found.")
                except ScraperError as error:
                    failed += 1
                    outcome.update(state="error", message=str(error))
                except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
                    if isinstance(error, OSError) and error.errno in (errno.ENOSPC, errno.EROFS): raise
                    failed += 1
                    outcome.update(
                        state="error",
                        message="Could not read this game or save its artwork. Existing artwork is retained."
                    )
            if extras:
                publish("scraping", "Fetching game information", rom.stem)
                outcome["extras"] = extras.process(rom, item, force)
            # Per-game results power Review unmatched and survive cancellation.
            write_fields(item / "artwork-status.ini", outcome, durable=False)
            report.append(outcome)
            completed += 1
        message = "%d / %d covers available." % (cached + downloaded, total)
        if missing or failed: message += " %d unmatched, %d to retry." % (missing, failed)
        publish("done", message)
    except KeyboardInterrupt:
        publish("cancelled", "Stopped. %d / %d games checked; fetch missing artwork to continue." % (completed, total))
    finally:
        folder.mkdir(parents=True, exist_ok=True)
        pending = folder / "report.pending.json"
        pending.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "total": total,
                    "completed": completed,
                    "results": report
                }, ensure_ascii=False, indent=2
            ) + "\n"
        )
        pending.replace(folder / "report.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "scrape", "index"))
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--private-dir", type=Path)
    parser.add_argument("--rom-dir", type=Path, action="append", default=[])
    parser.add_argument("--system-dir", action="append", default=[], help="system=folder (Show all systems)")
    parser.add_argument("--stdin", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--rom", type=Path, help="Fetch one game without scanning the library")
    parser.add_argument("--provider", choices=("auto", "gametdb", "libretro"), default="auto")
    parser.add_argument("--extras", default="", help="Comma list of info, snap, title: game information and pictures")
    args = parser.parse_args()
    folder = args.private_dir or private_root(args.data)
    status = args.data / "scraper/status.ini"
    try:
        if args.action == "index":
            for rom in games(args.rom_dir):
                index_banner(args.data, rom)
        elif args.action == "status":
            previous = read_fields(status)
            if previous.get("state") not in ("done", "cancelled"
                                             ) and not (previous.get("state") == "error" and "completed" in previous):
                write_fields(
                    status, {
                        "state": "ready",
                        "message": "Ready to fetch artwork.",
                        "provider": args.provider
                    }
                )
        else:
            (args.data / "scraper/cancel").unlink(missing_ok=True)
            from public_artwork import PublicClient
            client = PublicClient(
                args.provider,
                cache=args.data / "scraper/catalogs",
                cancelled=lambda: (args.data / "scraper/cancel").exists()
            )
            roms = [args.rom.resolve()] if args.rom else games(args.rom_dir) + system_games(args.system_dir)
            extras = None
            if args.extras:
                from game_info import GameInfo, parse_kinds
                kinds = parse_kinds(args.extras)
                extras = GameInfo(client, kinds) if kinds else None
            scrape(args.data, client, roms, args.force, extras)
        return 0
    except KeyboardInterrupt:
        if args.action != "index": write_fields(status, {"state": "cancelled", "message": "Scraping stopped."})
        return 130
    except ScraperError as error:
        write_fields(status, {"state": "error", "message": str(error)})
        return 1
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile):
        if args.action != "index":
            write_fields(
                status, {
                    "state": "error",
                    "message": "Could not complete scraping. Cached artwork is still available."
                }
            )
        return 1


if __name__ == "__main__":
    sys.modules["scraper"] = sys.modules[__name__]
    signal.signal(signal.SIGTERM, lambda _s, _f: (_ for _ in ()).throw(KeyboardInterrupt()))
    sys.exit(main())
