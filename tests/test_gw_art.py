#!/usr/bin/env python3
"""A Game & Watch's cover comes from the scan inside its simulator."""
import bz2
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "runtime"))
import gw_art


def scan(width, height, colour):
    """Every row one column of one opaque run, then a transparent one, as rlrle writes it."""
    row = struct.pack(">HHH", 1, 2, (4 << 13) | (width - 1)) + struct.pack(">H", colour) * (width - 1)
    row += struct.pack(">H", 1)
    offsets = [i * len(row) for i in range(height)]
    return struct.pack(">HHI", width, height, 0) + struct.pack(">%dI" % height, *offsets) + row * height


def member(name, body):
    head = bytearray(512)
    head[:len(name)] = name.encode()
    head[124:136] = b"%011o\0" % len(body)
    return bytes(head) + body + bytes(-len(body) % 512)


class Cover(unittest.TestCase):
    def mgw(self, folder, *parts):
        path = Path(folder) / "game.mgw"
        path.write_bytes(bz2.compress(b"".join(member(n, b) for n, b in parts) + bytes(1024)))
        return path

    def test_closed_scan_first(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.mgw(folder, ("im_background_open.rle", scan(8, 4, 0x07E0)), ("im_background_closed.rle", scan(8, 4, 0xF800)))
            found = gw_art.members(path)
            self.assertEqual(len(found), 2)
            width, height, rows = gw_art.pixels(found["im_background_closed.rle"])
            self.assertEqual((width, height), (8, 4))
            self.assertEqual(rows[0][:4], bytes((255, 0, 0, 255)))
            self.assertEqual(rows[0][-4:], bytes(4)) # the transparent run
            self.assertTrue(gw_art.cover(path).startswith(b"\x89PNG"))

    def test_large_scan_is_sampled(self):
        width, height, rows = gw_art.pixels(scan(1000, 800, 0xFFFF))
        self.assertLessEqual(max(width, height), gw_art.LARGEST)
        self.assertEqual(len(rows), height)

    def test_no_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(gw_art.cover(self.mgw(folder, ("main.bs", b"x"))))


if __name__ == "__main__":
    unittest.main()
