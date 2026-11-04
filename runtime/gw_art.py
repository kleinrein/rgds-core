"""A Game & Watch's cover: the machine itself, from the scan inside its simulator (closed where it has both)."""
import bz2
import struct
import zlib
from pathlib import Path

SCANS = ("im_background_closed.rle", "im_background.rle", "im_background_open.rle")
LARGEST = 384


def members(mgw):
    """{name: bytes} of an .mgw: bzip2 over tar headers Python's tarfile refuses."""
    raw = bz2.decompress(Path(mgw).read_bytes())
    found, at = {}, 0
    while at + 512 <= len(raw) and raw[at]:
        name = raw[at:at + 100].split(b"\0")[0].decode("latin-1")
        size = int(raw[at + 124:at + 136].split(b"\0")[0].strip() or b"0", 8)
        if size: found[name] = raw[at + 512:at + 512 + size]
        at += 512 + ((size + 511) & ~511)
    return found


def pixels(scan):
    """(width, height, RGBA rows) at most LARGEST on a side, reading only the rows it keeps: quick on the handheld."""
    width, height = struct.unpack(">HH", scan[:4])
    start = 8 + 4 * height
    offsets = struct.unpack(">%dI" % height, scan[8:start])
    step = max(1, (max(width, height) + LARGEST - 1) // LARGEST)
    rows = []
    for y in range(0, height, step):
        at = start + offsets[y]
        columns = struct.unpack(">H", scan[at:at + 2])[0]
        at += 2 * columns
        row, x = bytearray(), 0
        for _ in range(columns):
            runs = struct.unpack(">H", scan[at:at + 2])[0]
            at += 2
            for _ in range(runs):
                head = struct.unpack(">H", scan[at:at + 2])[0]
                at += 2
                alpha, count = head >> 13, head & 0x1FFF
                for _ in range(count):
                    if x % step == 0:
                        if alpha:
                            value = (scan[at] << 8) | scan[at + 1]
                            row += bytes(((value >> 11) * 255 // 31, ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31, alpha * 255 // 4))
                        else:
                            row += b"\0\0\0\0"
                    if alpha: at += 2
                    x += 1
        rows.append(bytes(row))
    return len(rows[0]) // 4, len(rows), rows


def png(width, height, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    body = zlib.compress(b"".join(b"\0" + row for row in rows))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", body) + chunk(b"IEND", b"")


def cover(mgw):
    """A PNG of the machine, or None when the simulator holds no scan."""
    found = members(mgw)
    scan = next((found[name] for name in SCANS if name in found), None)
    return png(*pixels(scan)) if scan else None
