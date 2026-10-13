#!/usr/bin/env python3
"""Render a saved diagnostic thumbnail without modifying an emulator checkpoint.

DraStic and NNDDSS formats carry two 16-bit screens; the reviewed version 15
NNDDSS thumbnail uses BGR565, while version 14 DraStic uses RGB565. DSperate
carries a smaller RGB565 top-screen thumbnail. This is not a converter or
evidence of save compatibility.
Layouts were checked against pinned SuperDrastic src/menu.c and DSperate
src/core/nds.cpp; melonDS deliberately requires a running-frontend capture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

try:
    from .probe_checkpoint import MAX_BYTES, decode_checkpoint
except ImportError:
    from probe_checkpoint import MAX_BYTES, decode_checkpoint


def thumbnail(data):
    encoding,decoded=decode_checkpoint(data)
    bgr=False
    if encoding=='drastic-zlib':
        version=struct.unpack_from('<I',data,0x20)[0]
        if version not in (14,15):raise ValueError('Unreviewed DraStic thumbnail version')
        bgr=version==15
        width,height=256,384;pixels=decoded[:width*height*2]
        description='Two DS screens from DraStic/NNDDSS checkpoint, top above bottom'
    elif data.startswith(b'DSST'):
        if len(data)<64 or struct.unpack_from('<I',data,4)[0]!=4 or data[8:12]!=b'HEAD':
            raise ValueError('Unreviewed DSperate checkpoint header')
        length=struct.unpack_from('<I',data,12)[0]
        if length<48 or 16+length>len(data):raise ValueError('Truncated DSperate header chunk')
        width,height=struct.unpack_from('<II',data,56)
        if (width,height)!=(128,96) or length<48+width*height*2:
            raise ValueError('Invalid DSperate thumbnail dimensions')
        pixels=data[64:64+width*height*2]
        description='DSperate top-screen half-resolution checkpoint thumbnail'
    else:
        raise ValueError('This checkpoint has no supported standalone picture; capture the running frontend')
    if len(pixels)!=width*height*2:raise ValueError('Truncated checkpoint picture')
    rgb=bytearray(width*height*3)
    for index,(pixel,) in enumerate(struct.iter_unpack('<H',pixels)):
        high,green,low=((pixel>>11)&31)*255//31,((pixel>>5)&63)*255//63,(pixel&31)*255//31
        rgb[index*3:index*3+3]=bytes((low,green,high) if bgr else (high,green,low))
    return width,height,bytes(rgb),description


def png(width,height,rgb):
    if width<=0 or height<=0 or len(rgb)!=width*height*3:raise ValueError('Invalid RGB image')
    def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xffffffff)
    scanlines=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scanlines))+chunk(b'IEND',b'')


def extract(path,output):
    path,output=Path(path),Path(output)
    if not path.is_file() or not 0<path.stat().st_size<=MAX_BYTES:raise ValueError('Missing or oversized checkpoint')
    data=path.read_bytes();width,height,rgb,description=thumbnail(data)
    with output.open('xb') as stream:stream.write(png(width,height,rgb))
    return dict(checkpoint=str(path),sha256=hashlib.sha256(data).hexdigest(),output=str(output),width=width,height=height,description=description)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args()
    try:print(json.dumps(extract(args.checkpoint,args.output),sort_keys=True))
    except (OSError,ValueError) as error:parser.exit(1,str(error)+'\n')
