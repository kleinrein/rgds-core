"""Exercise the actual SDL resize/save path with SDL's built-in BMP decoder."""
import ctypes
import ctypes.util
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
import scraper

@unittest.skipUnless(ctypes.util.find_library('SDL2'),'Requires SDL2')
class ArtworkDecoderTests(unittest.TestCase):
    def test_large_art_is_bounded_and_keeps_aspect_ratio(self):
        sdl=ctypes.CDLL(ctypes.util.find_library('SDL2'))
        image=SimpleNamespace(IMG_Load_RW=sdl.SDL_LoadBMP_RW)
        width,height=800,400
        pixels=b'\x00\x00\xff'*(width*height)
        bmp=struct.pack('<2sIHHI',b'BM',54+len(pixels),0,0,54)+struct.pack('<IiiHHIIiiII',40,width,height,1,24,0,len(pixels),0,0,0,0)+pixels
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'box.bmp'
            with patch.object(ctypes.util,'find_library',return_value='test-library'),patch.object(ctypes,'CDLL',side_effect=[sdl,image]):
                scraper.image_to_bmp(bmp,path)
            raw=path.read_bytes();w,h=struct.unpack_from('<ii',raw,18)
            self.assertEqual((w,abs(h)),(384,192))
            offset=struct.unpack_from('<I',raw,10)[0]
            self.assertEqual(raw[offset:offset+3],b'\x00\x00\xff')
            self.assertLess(len(raw),384*192*4+1024)
            self.assertFalse(path.with_suffix('.pending.bmp').exists())
