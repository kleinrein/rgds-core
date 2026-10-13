"""Persistent artwork metadata and session playtime, separate from immutable releases."""
import os
from pathlib import Path
import tempfile
import struct
import zipfile
from fields import read_fields, write_fields


def game_key(rom):
    value = 14695981039346656037
    for byte in str(Path(rom).resolve()).encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return "%016x" % value


def playtime(data, rom):
    fields = read_fields(Path(data) / "library" / game_key(rom) / "stats.ini")
    try:
        return max(0, int(fields.get("seconds", 0))), max(0, int(fields.get("sessions", 0)))
    except ValueError:
        return 0, 0


def record_playtime(data, rom, seconds, sessions, started):
    write_fields(Path(data) / "library" / game_key(rom) / "stats.ini",
                 {"seconds": max(0, int(seconds)), "sessions": max(0, int(sessions)), "last_played": started})


def banner_data(path):
    archive = None
    try:
        if path.suffix.lower() == ".zip":
            archive = zipfile.ZipFile(path)
            games = [i for i in archive.infolist() if Path(i.filename).suffix.lower() in (".nds", ".dsi")]
            if len(games) != 1: return None
            stream = archive.open(games[0])
        else: stream = path.open("rb")
        with stream:
            header = stream.read(0x6c)
            if len(header) != 0x6c: return None
            offset = struct.unpack_from("<I", header, 0x68)[0]
            if offset < 0x200 or offset > 16*1024**2: return None
            stream.seek(offset)
            banner = stream.read(0x440)
            return banner if len(banner) == 0x440 else None
    except (OSError, ValueError, zipfile.BadZipFile): return None
    finally:
        if archive: archive.close()


def index_banner(data, rom):
    item = Path(data) / "library" / game_key(rom)
    marker = item / "banner.ini"
    stamp = str(rom.stat().st_mtime_ns) + ":" + str(rom.stat().st_size)
    if read_fields(marker).get("stamp") == stamp: return
    banner = banner_data(rom)
    if not banner: return
    palette = struct.unpack_from("<16H", banner, 0x220)
    pixels = bytearray()
    for y in range(32):
        for x in range(32):
            tile = (y//8)*4+x//8
            byte = banner[0x20+tile*32+(y%8)*4+(x%8)//2]
            index = (byte >> 4) if x%2 else byte & 15
            rgb = palette[index]
            pixels.extend((((rgb>>10)&31)*255//31, ((rgb>>5)&31)*255//31, (rgb&31)*255//31, 255 if index else 0))
    info = struct.pack("<IiiHHIIiiII", 108, 32, -32, 1, 32, 3, len(pixels), 2835, 2835, 0, 0)
    info += struct.pack("<5I", 0xff0000, 0xff00, 0xff, 0xff000000, 0x73524742) + bytes(48)
    item.mkdir(parents=True, exist_ok=True)
    pending = item / "icon.pending.bmp"
    pending.write_bytes(struct.pack("<2sIHHI", b"BM", 14+108+len(pixels), 0, 0, 14+108) + info + pixels)
    pending.replace(item / "icon.bmp")
    title = banner[0x340:0x440].decode("utf-16le", errors="replace").split("\x00", 1)[0].replace("\n", " ")
    write_fields(marker, {"title": title, "stamp": stamp})
