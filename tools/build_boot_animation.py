#!/usr/bin/env python3
"""Pre-render a lightweight, looping early-boot animation; no device image libraries."""
import math
from pathlib import Path
import struct
import zlib
from build_boot_assets import boot_panel, BACKGROUND, ACCENT, FOCUS
ROOT=Path(__file__).resolve().parents[1]


def frames():
    base=bytearray(boot_panel())
    def box(p,x,y,w,h,colour):
        for row in range(y,y+h):
            start=(row*640+x)*3;p[start:start+w*3]=bytes(colour)*w
    # The early static composition supplies the geometry and typeset identity.
    for x,y in ((390,185),(320,247),(250,185),(320,123)):
        box(base,x-3,y-3,6,6,BACKGROUND)
    for index in range(36):
        p=bytearray(base);phase=index/36
        for i in range(4):
            angle=phase*2*math.pi+i*math.pi/2
            x=320+round(math.cos(angle)*70);y=185+round(math.sin(angle)*62)
            strength=.8 if i==0 else .35
            colour=tuple(round(a*(1-strength)+b*strength) for a,b in zip(BACKGROUND,ACCENT))
            box(p,x-2,y-2,4,4,colour)
        for screen in range(2):
            head=int(((phase-screen*.16)%1)*84)-8
            for col in range(68):
                light=max(0,1-abs(col-head)/10)*.35
                colour=tuple(round(a*(1-light)+b*light) for a,b in zip(FOCUS,ACCENT))
                box(p,286+col,151+screen*48,1,20,colour)
        # A restrained traveling highlight makes the wordmark itself animate.
        head=int(phase*440)-42
        for x in range(142,498):
            strength=max(0,1-abs(x-142-head)/32)*.3
            if strength:
                for y in range(264,316):
                    pos=(y*640+x)*3
                    if tuple(p[pos:pos+3]) != BACKGROUND:
                        p[pos:pos+3]=bytes(round(c*(1-strength)+255*strength) for c in p[pos:pos+3])
        for i in range(5):
            strength=.25+.65*(1+math.cos(phase*2*math.pi-i*.65))*.5
            colour=tuple(round(a*(1-strength)+b*strength) for a,b in zip(BACKGROUND,ACCENT))
            box(p,290+i*14,364,4,4,colour)
        bgra=bytearray(640*480*4)
        bgra[0::4]=p[2::3];bgra[1::4]=p[1::3];bgra[2::4]=p[0::3];bgra[3::4]=b'\xff'*(640*480)
        yield bytes(bgra)


def build(path):
    compressor=zlib.compressobj(6)
    data=b''.join(compressor.compress(frame) for frame in frames())+compressor.flush()
    Path(path).write_bytes(struct.pack('<4s4I',b'RGA1',640,480,36,18)+data)

if __name__=='__main__':build(ROOT/'assets/boot/animation.rga')
