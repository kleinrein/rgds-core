"""Native checkpoint thumbnails remain read-only, bounded and format-specific."""
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from tools import checkpoint_picture as picture


def drastic(version=14,short=False,top=None):
    magic=b'DraStic-SaveState---------------';header=bytearray(0x44);header[:len(magic)]=magic
    struct.pack_into('<I',header,0x20,version)
    if top is None:top=0x001f if version==15 else 0xf800
    pixels=struct.pack('<H',top)*(256*192)+struct.pack('<H',0x07e0)*(256*192)
    compressed=zlib.compress(pixels[:12] if short else pixels+b'other machine state')
    struct.pack_into('<I',header,0x40,len(compressed))
    return bytes(header)+compressed


def dsperate():
    pixels=struct.pack('<H',0x001f)*(128*96)
    payload=struct.pack('<IQQQQIII',0,0,0,0,42,1,128,96)+pixels
    return b'DSST'+struct.pack('<I',4)+b'HEAD'+struct.pack('<I',len(payload))+payload


class CheckpointPictureTests(unittest.TestCase):
    def test_reviewed_drastic_and_nnddss_pictures_keep_screen_order_and_rgb(self):
        for version in (14,15):
            width,height,rgb,_=picture.thumbnail(drastic(version))
            self.assertEqual((width,height),(256,384))
            self.assertEqual(rgb[:3],b'\xff\0\0')
            self.assertEqual(rgb[256*192*3:256*192*3+3],b'\0\xff\0')
            self.assertEqual(rgb[-3:],b'\0\xff\0')

    def test_dsperate_header_has_half_resolution_top_screen(self):
        width,height,rgb,description=picture.thumbnail(dsperate())
        self.assertEqual((width,height),(128,96))
        self.assertEqual(rgb,b'\0\0\xff'*(width*height))
        self.assertIn('top-screen',description)

    def test_nnddss_original_probe_color_is_not_reversed(self):
        # Original ARM9 probe wrote DS VRAM0x840a (R10 G0 B1). Its reviewed
        # v15 checkpoint contains thumbnail0x0809 after native quantization.
        # Treating it as RGB565 incorrectly makes blue dominant.
        _,_,rgb,_=picture.thumbnail(drastic(15,top=0x0809))
        self.assertEqual(rgb[:3],bytes((9*255//31,0,255//31)))
        _,_,standard,_=picture.thumbnail(drastic(14,top=0x0809))
        self.assertEqual(standard[:3],bytes((255//31,0,9*255//31)))

    def test_unknown_versions_bad_lengths_and_missing_pixels_are_rejected(self):
        changed=bytearray(dsperate());struct.pack_into('<I',changed,56,65536)
        for data in (b'MELN'+bytes(100),drastic(16),drastic(short=True),dsperate()[:-1],bytes(changed)):
            with self.assertRaises(ValueError):picture.thumbnail(data)

    def test_png_payload_and_existing_evidence_are_preserved(self):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);source=directory/'native.dss';output=directory/'picture.png'
            original=drastic();source.write_bytes(original)
            record=picture.extract(source,output);self.assertEqual(record['height'],384)
            result=output.read_bytes();self.assertEqual(result[:8],b'\x89PNG\r\n\x1a\n')
            length=struct.unpack_from('>I',result,33)[0];self.assertEqual(result[37:41],b'IDAT')
            rows=zlib.decompress(result[41:41+length]);self.assertEqual(rows[:4],b'\0\xff\0\0')
            with self.assertRaises(FileExistsError):picture.extract(source,output)
            self.assertEqual(source.read_bytes(),original)
            self.assertEqual(output.read_bytes(),result)


if __name__=='__main__':unittest.main()
