#!/usr/bin/env python3
"""Build exact shader previews from original pixel scenes and the owned GLES pass.

Requires host SDL/C++ headers and a real EGL/GLES2 implementation (Mesa or
SwiftShader). This never starts an emulator or contacts a device. Lossless tile
packing keeps artwork compact without palette quantization or filter imitation.
"""
import argparse
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile

from PIL import Image, ImageDraw
from validate_shader_previews import PRESET_COUNT, SHADER_FILES

ROOT=Path(__file__).resolve().parents[1]
NATIVE=(256,192)
OUTPUT=(640,480)
SCENES=('grove','circuit','starfall')


def sprite(draw,x,y,rows,palette,scale=1):
    for row,line in enumerate(rows):
        for column,key in enumerate(line):
            if key in palette:draw.rectangle((x+column*scale,y+row*scale,x+(column+1)*scale-1,y+(row+1)*scale-1),fill=palette[key])


def grove():
    image=Image.new('RGBA',NATIVE,'#f8c69e');d=ImageDraw.Draw(image);rng=random.Random(13)
    for y,color in [(0,'#edaa92'),(26,'#f3b994'),(50,'#f8c69e'),(80,'#ffdab0')]:d.rectangle((0,y,255,y+27),fill=color)
    d.ellipse((184,14,233,63),fill='#ffe7b2');d.rectangle((181,37,235,43),fill='#ffd6a0')
    d.polygon([(0,81),(27,57),(57,76),(94,54),(125,85),(159,58),(190,82),(220,63),(255,90),(255,125),(0,125)],fill='#83b7a2')
    d.polygon([(0,106),(39,75),(77,103),(116,80),(151,107),(192,85),(229,103),(255,89),(255,142),(0,142)],fill='#478f87')
    for x in range(-8,268,23):
        y=rng.randint(81,104);d.rectangle((x+8,y+4,x+11,142),fill='#326c6d');d.polygon([(x,y+30),(x+21,y+30),(x+10,y),(x+4,y+12),(x+17,y+18)],fill='#347d73')
    # A stone shrine and warm stained glass give colour treatments a reference.
    d.rectangle((153,83,182,132),fill='#597b80');d.rectangle((149,128,186,134),fill='#3e6468')
    d.polygon([(147,83),(168,65),(188,83)],fill='#3d676b');d.rectangle((155,83,180,88),fill='#81aa9a')
    d.rectangle((160,92,175,119),fill='#264e67');d.rectangle((162,94,173,117),fill='#f5ce85');d.rectangle((167,94,168,117),fill='#bb8167');d.rectangle((162,106,173,107),fill='#bb8167')
    # Platforms, moss and a bright stream.
    d.polygon([(0,143),(88,143),(103,156),(131,157),(141,145),(255,145),(255,191),(0,191)],fill='#5a6653')
    d.polygon([(0,147),(85,147),(103,160),(133,161),(144,150),(255,150),(255,191),(0,191)],fill='#485347')
    d.polygon([(101,158),(129,158),(139,191),(88,191)],fill='#80c2b2');d.polygon([(108,159),(119,159),(120,191),(104,191)],fill='#cee4bf')
    for x in range(0,256,4):
        if 91<x<143:continue
        y=142 if x<91 else 144;d.rectangle((x,y,x+3,y+3),fill='#b2c780');d.line((x,y+4,x+3,y+4),fill='#6c9774')
    for _ in range(110):
        x=rng.randrange(256);y=rng.randrange(151,191)
        if 88<x<143:continue
        d.rectangle((x,y,x+2,y+1),fill=rng.choice(('#62664e','#394c43','#817858')))
    d.rectangle((197,114,244,119),fill='#53675b');d.rectangle((195,111,247,114),fill='#b2c780');d.rectangle((209,121,232,127),fill='#394f4a')
    # Original traveller sprite, flowers, bright single-pixel details.
    sprite(d,52,123,["...hh....","..hhhh...","..hffh...","..hffh...","...nn....","..cycc...",".ccyycc..",".ccyyc...","..cyyc...","..bbbb...","..b..b...","..b..b...",".kk..kk.."],{'h':'#433d51','f':'#f8d4ae','n':'#c98770','c':'#f8cd76','y':'#ffe9a6','b':'#344f68','k':'#273e50'})
    for x,y in [(25,137),(42,139),(82,136),(194,139),(230,138)]:d.line((x,y,x,y+5),fill='#86ac78');d.rectangle((x-1,y-1,x+1,y+1),fill='#f5b99f')
    for x,y in [(123,109),(132,125),(91,108),(206,95),(38,114)]:d.point((x,y),fill='#ffeab5');d.point((x+1,y+1),fill='#b5d7a0')
    # Foreground canopy framing the game world.
    d.rectangle((8,36,16,145),fill='#355957');d.rectangle((14,40,19,87),fill='#436c60');d.line((15,73,42,54),fill='#355957',width=5)
    for box in [(-12,17,28,45),(4,4,48,33),(24,18,62,44),(-7,36,35,63),(35,36,58,54)]:d.ellipse(box,fill='#366e62')
    for box in [(4,9,39,29),(26,20,50,38),(-7,32,19,50)]:d.ellipse(box,fill='#679a70')
    return image


def circuit():
    image=Image.new('RGBA',NATIVE,'#191c38');d=ImageDraw.Draw(image);rng=random.Random(27)
    for y,c in [(0,'#18213d'),(28,'#27234e'),(54,'#432d60'),(78,'#7b3d68')]:d.rectangle((0,y,255,y+28),fill=c)
    d.ellipse((194,15,226,47),fill='#f3a59a');d.rectangle((191,33,228,37),fill='#7b3d68')
    for x in range(-8,260,17):
        y=rng.randrange(26,66);w=rng.randrange(10,18);d.rectangle((x,y,x+w,99),fill=rng.choice(('#272544','#242d4c','#36315c')))
        d.rectangle((x+2,y+3,x+w-2,y+5),fill='#8a7199')
        for yy in range(y+10,98,7):
            for xx in range(x+3,x+w-1,5):
                if rng.randrange(3):d.rectangle((xx,yy,xx+1,yy+2),fill=rng.choice(('#8dc8b9','#eab4a7','#986a9f')))
    d.rectangle((0,83,255,92),fill='#30445f');d.line((0,84,255,84),fill='#66cccb');d.rectangle((0,93,255,98),fill='#1e314b')
    d.polygon([(108,92),(148,92),(253,191),(0,191)],fill='#4c4772')
    d.polygon([(111,93),(144,93),(213,191),(33,191)],fill='#36365c')
    d.line((108,92,0,191),fill='#8cded0',width=2);d.line((148,92,253,191),fill='#d782b7',width=2)
    d.line((94,106,37,179),fill='#293351',width=2);d.line((163,109,220,179),fill='#293351',width=2)
    for y in (105,124,151,187):
        half=int((y-92)*.45+2);d.rectangle((127-half,y,130+half,y+2),fill='#a3adce')
    for y in range(104,183,13):
        x=109-int((y-92)*1.09);d.line((x,y,x+12,y),fill='#66959f');x=147+int((y-92)*1.02);d.line((x-12,y,x,y),fill='#8c6596')
    # A hand-built hover racer, with a readable bright body and dark outline.
    d.ellipse((92,142,162,150),fill='#262a49');d.polygon([(104,129),(117,115),(141,115),(155,129),(157,140),(101,140)],fill='#1c2849')
    d.polygon([(108,129),(120,116),(138,116),(152,129),(151,136),(105,136)],fill='#e5bb91')
    d.polygon([(117,121),(122,116),(136,116),(143,125),(115,125)],fill='#375777');d.line((122,118,135,118),fill='#73c8cb')
    d.rectangle((104,131,115,134),fill='#a1eee0');d.rectangle((143,131,153,134),fill='#e39dac');d.rectangle((123,128,136,136),fill='#f8d5a7')
    d.rectangle((101,139,113,142),fill='#db85b1');d.rectangle((146,139,158,142),fill='#db85b1')
    d.rectangle((104,143,114,146),fill='#79d7d0');d.rectangle((145,143,156,146),fill='#79d7d0')
    # Roadside signs, city banners and HUD blocks at native resolution.
    d.rectangle((24,68,27,114),fill='#202b4a');d.rectangle((10,60,45,80),fill='#252743');d.rectangle((12,62,43,78),outline='#81c2c3')
    d.line((19,69,34,69),fill='#eeb897',width=2);d.line((28,65,34,69),fill='#eeb897',width=2);d.line((28,73,34,69),fill='#eeb897',width=2)
    d.rectangle((10,9,46,15),fill='#87d2ca');d.rectangle((10,17,31,19),fill='#c989b2');d.rectangle((203,167,245,185),fill='#242843');d.line((210,181,237,172),fill='#87d2ca');d.line((217,181,238,181),fill='#a08aba')
    return image


def starfall():
    image=Image.new('RGBA',NATIVE,'#19273c');d=ImageDraw.Draw(image);rng=random.Random(43)
    for y,c in [(0,'#14253a'),(36,'#1c3147'),(73,'#2d3b53'),(116,'#3d3d57'),(158,'#473c54')]:d.rectangle((0,y,255,y+38),fill=c)
    d.polygon([(0,93),(20,77),(57,79),(89,57),(114,50),(142,50),(170,26),(218,8),(254,16),(255,50),(220,44),(190,53),(168,75),(127,78),(106,91),(67,100),(37,106),(0,120)],fill='#3e5064')
    d.polygon([(0,103),(41,86),(79,88),(105,69),(151,66),(188,45),(218,32),(255,37),(255,52),(213,43),(177,61),(152,80),(115,82),(95,98),(51,104),(0,126)],fill='#52657a')
    for _ in range(90):
        x=rng.randrange(256);y=rng.randrange(180);d.point((x,y),fill=rng.choice(('#c7d7cf','#e6c8b0','#829bba')))
    for x,y in [(18,35),(52,60),(85,19),(226,70),(198,120),(47,153)]:d.line((x-2,y,x+2,y),fill='#cde4d6');d.line((x,y-2,x,y+2),fill='#cde4d6');d.point((x,y),fill='#f4e5b8')
    d.ellipse((157,17,214,74),fill='#78b9ae');d.ellipse((169,17,214,74),fill='#547d91');d.ellipse((183,24,207,52),fill='#45667e')
    d.arc((139,25,229,68),12,189,fill='#d8cfab',width=3);d.arc((139,25,229,68),191,346,fill='#90a4aa',width=2)
    for x,y in [(44,105),(198,91),(75,128)]:
        d.line((x-17,y-20,x,y),fill='#db9f8e',width=2);d.ellipse((x-7,y-5,x+7,y+7),fill='#a99a94');d.rectangle((x-4,y-2,x,y+2),fill='#6d7485');d.point((x+3,y+3),fill='#d7b89e')
    # Original central spacecraft, with a tiny bright canopy and thrusters.
    sprite(d,113,110,["..........a..........",".........aaa.........",".........aba.........","........abbba........",".......aabbbaa.......","......aaabbbaaa......",".....aaaacccaaaa.....","...aaaacccccccaaaa...","..aaaccccaaccccaaaa..",".aaaacccaaaccccaaaaa.","aaaaacccaaaccccaaaaaa","...ddaaaaaaaaddd.....","...ee...fff...ee.....","...gg...fgf...gg.....",".........gfg.........","..........g.........."],{'a':'#d4d6bf','b':'#f4e5b8','c':'#628ba1','d':'#263e5b','e':'#e5a187','f':'#edc794','g':'#be788e'})
    for x,y in [(29,62),(216,148)]:sprite(d,x,y,["..r..",".rrr.","rrarr","..b.."],{'r':'#ce8e9e','a':'#f0c798','b':'#516c86'},2)
    d.rectangle((10,175,86,184),fill='#1c2b40');d.rectangle((13,178,61,180),fill='#85beaf');d.rectangle((65,178,78,180),fill='#cf8c9e')
    for x in (218,229,240):sprite(d,x,175,[".x.x.","xxxxx","xxxxx",".xxx.","..x.."],{'x':'#d7a3a1'})
    return image


def pack_rgsp(image):
    """Exact fixed-tile deduplication; preserves every RGBA byte."""
    image=image.convert('RGBA');w,h=image.size;side=5
    if w%side or h%side:raise ValueError('Preview dimensions must be multiples of 5')
    raw=image.tobytes();palette={};tiles=[];indices=[]
    for y in range(0,h,side):
        for x in range(0,w,side):
            tile=b''.join(raw[((y+row)*w+x)*4:((y+row)*w+x+side)*4] for row in range(side))
            index=palette.get(tile)
            if index is None:index=len(tiles);palette[tile]=index;tiles.append(tile)
            indices.append(index)
    if len(tiles)>65535:raise ValueError('Too many exact tiles')
    return b'RGSPV001'+struct.pack('<HHHH',w,h,side,len(tiles))+b''.join(tiles)+struct.pack('<'+'H'*len(indices),*indices)


def graphics_libraries():
    egl=os.environ.get('RGDS_TEST_EGL_LIBRARY');gles=os.environ.get('RGDS_TEST_GLES_LIBRARY')
    if egl and gles:return egl,gles
    swift=Path.home()/'Library/Android/sdk/emulator/lib64/gles_angle'
    if (swift/'libswiftshader_libEGL.dylib').is_file():return str(swift/'libswiftshader_libEGL.dylib'),str(swift/'libswiftshader_libGLESv2.dylib')
    return ctypes.util.find_library('EGL'),ctypes.util.find_library('GLESv2')


CPP=r'''
#include "platform/game_shader.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using U=unsigned;using I=int;using B=unsigned char;
int main(int argc,char** argv){
 if(argc!=3)return 2;auto proc=rgds::overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
#define GL(name,ret,args) auto name=reinterpret_cast<ret(*)args>(proc("gl" #name));if(!name)return 3
 GL(GetError,U,());GL(GetString,const B*,(U));GL(CreateShader,U,(U));GL(ShaderSource,void,(U,I,const char* const*,const I*));GL(CompileShader,void,(U));GL(GetShaderiv,void,(U,U,I*));
 GL(CreateProgram,U,());GL(AttachShader,void,(U,U));GL(BindAttribLocation,void,(U,U,const char*));GL(LinkProgram,void,(U));GL(GetProgramiv,void,(U,U,I*));GL(UseProgram,void,(U));
 GL(GenTextures,void,(I,U*));GL(ActiveTexture,void,(U));GL(BindTexture,void,(U,U));GL(TexParameteri,void,(U,U,I));GL(TexImage2D,void,(U,I,I,I,I,I,U,U,const void*));
 GL(GenBuffers,void,(I,U*));GL(BindBuffer,void,(U,U));GL(BufferData,void,(U,std::ptrdiff_t,const void*,U));GL(VertexAttribPointer,void,(U,I,U,B,I,const void*));GL(EnableVertexAttribArray,void,(U));
 GL(Viewport,void,(I,I,I,I));GL(ReadPixels,void,(I,I,I,I,U,U,void*));
#undef GL
 const char* vertex="attribute vec2 position;attribute vec2 uv;varying vec2 tc;void main(){tc=uv;gl_Position=vec4(position,0.,1.);}";
 const char* fragment="precision mediump float;varying vec2 tc;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,tc);}";
 auto compile=[&](U type,const char* source){auto id=CreateShader(type);ShaderSource(id,1,&source,nullptr);CompileShader(id);I ok=0;GetShaderiv(id,0x8b81,&ok);return ok?id:U(0);};
 U original=CreateProgram();U vs=compile(0x8b31,vertex),fs=compile(0x8b30,fragment);if(!vs || !fs)return 4;AttachShader(original,vs);AttachShader(original,fs);BindAttribLocation(original,0,"position");BindAttribLocation(original,1,"uv");LinkProgram(original);I linked=0;GetProgramiv(original,0x8b82,&linked);if(!linked)return 5;UseProgram(original);
 std::vector<B> input(256*192*4);std::ifstream source(argv[1],std::ios::binary);if(!source.read(reinterpret_cast<char*>(input.data()),input.size()))return 6;
 U image=0;GenTextures(1,&image);ActiveTexture(0x84c0);BindTexture(0x0de1,image);TexParameteri(0x0de1,0x2801,0x2600);TexParameteri(0x0de1,0x2800,0x2600);TexParameteri(0x0de1,0x2802,0x812f);TexParameteri(0x0de1,0x2803,0x812f);TexImage2D(0x0de1,0,0x1908,256,192,0,0x1908,0x1401,input.data());
 const float vertices[]={-1,1,0,0,1,1,1,0,-1,-1,0,1,-1,-1,0,1,1,1,1,0,1,-1,1,1};U buffer=0;GenBuffers(1,&buffer);BindBuffer(0x8892,buffer);BufferData(0x8892,sizeof(vertices),vertices,0x88e4);VertexAttribPointer(0,2,0x1406,0,16,nullptr);VertexAttribPointer(1,2,0x1406,0,16,reinterpret_cast<void*>(8));EnableVertexAttribArray(0);EnableVertexAttribArray(1);Viewport(0,0,640,480);
 rgds::GameShader shader;auto draw=reinterpret_cast<void(*)(U,I,I)>(proc("glDrawArrays"));std::vector<B> output(640*480*4);
 for(int selected=0;selected<rgds::shader_count;selected++){
   if(selected==0)draw(4,0,6);else if(!shader.native(selected,256,192,0,1,4,0,6,640,480))return 10+selected;
   ReadPixels(0,0,640,480,0x1908,0x1401,output.data());if(GetError())return 40+selected;
   std::ofstream file(std::string(argv[2])+"/shader-"+std::to_string(selected)+".rgba",std::ios::binary);file.write(reinterpret_cast<const char*>(output.data()),output.size());if(!file)return 60;
 }
 std::cout<<"{\"vendor\":\""<<GetString(0x1f00)<<"\",\"renderer\":\""<<GetString(0x1f01)<<"\",\"version\":\""<<GetString(0x1f02)<<"\"}\n";
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shader-dir',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'assets/shader-previews')
    parser.add_argument('--contact-sheet',type=Path,default=ROOT/'build/shader-preview-scenes.png')
    args=parser.parse_args()
    pack=args.shader_dir or ROOT/'vendor/engines/shaders'
    if not pack.is_dir():parser.error('Provision vendor/engines/shaders or pass --shader-dir with the community .dsd preset pack')
    egl,gles=graphics_libraries()
    if not egl or not gles or not shutil.which('clang++') or not shutil.which('sdl2-config'):parser.error('Host EGL/GLES2 and C++/SDL headers are required')
    flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
    args.output.mkdir(parents=True,exist_ok=True)
    manifest={'schema':1,'native_size':NATIVE,'preview_size':OUTPUT,'scenes':[],'encoding':'RGBA lossless 5x5 tile deduplication, RGSPV001','shaders':{},'renderer':None,'pipeline_sha256':hashlib.sha256((ROOT/'src/platform/game_shader.hpp').read_bytes()).hexdigest()}
    sources={path.name:path for directory in (pack,ROOT/'assets/shaders') for path in directory.glob('*.dsd')}
    sources={name:sources[name] for name in SHADER_FILES}
    for name,path in sorted(sources.items()):manifest['shaders'][name]=hashlib.sha256(path.read_bytes()).hexdigest()
    contact=Image.new('RGB',(640*3,480*2),'#171d29');total=0
    with tempfile.TemporaryDirectory(prefix='rgds-shader-previews-') as folder:
        temporary=Path(folder);driver=temporary/('driver.dylib' if os.uname().sysname=='Darwin' else 'driver.so');source=temporary/'render.cpp';binary=temporary/'render'
        subprocess.run(['clang++','-std=c++17','-O2',*flags,'-shared','-fPIC',str(ROOT/'tests/fixtures/shader_gles_driver.cpp'),'-o',str(driver)],check=True,capture_output=True,text=True)
        source.write_text(CPP);subprocess.run(['clang++','-std=c++17','-O2',*flags,'-I'+str(ROOT/'src'),str(source),str(driver),'-o',str(binary)],check=True,capture_output=True,text=True)
        merged=temporary/'shaders';merged.mkdir()
        for name,path in sources.items():shutil.copyfile(path,merged/name)
        environment=dict(os.environ,RGDS_SDL_LIBRARY=str(driver),RGDS_SHADER_DIR=str(merged),RGDS_TEST_EGL_LIBRARY=egl,RGDS_TEST_GLES_LIBRARY=gles,EGL_PLATFORM='surfaceless')
        for scene_index,(name,render) in enumerate(zip(SCENES,(grove,circuit,starfall))):
            native=render();destination=args.output/name;destination.mkdir(exist_ok=True);native.save(destination/'source.png')
            input_file=temporary/'source.rgba';input_file.write_bytes(native.tobytes());result=subprocess.run([str(binary),str(input_file),str(temporary)],env=environment,capture_output=True,text=True,timeout=60)
            if result.returncode:raise RuntimeError('Real GLES preview render failed: '+result.stdout+result.stderr)
            manifest['renderer']=json.loads(result.stdout)
            entry={'id':name,'native_sha256':hashlib.sha256(native.tobytes()).hexdigest(),'presets':[]}
            for selected in range(PRESET_COUNT):
                rgba=(temporary/f'shader-{selected}.rgba').read_bytes();image=Image.frombytes('RGBA',OUTPUT,rgba).transpose(Image.Transpose.FLIP_TOP_BOTTOM);packed=pack_rgsp(image);output=destination/f'shader-{selected}.rgsp';output.write_bytes(packed);total+=len(packed)
                entry['presets'].append({'id':selected,'rgba_sha256':hashlib.sha256(image.tobytes()).hexdigest(),'sha256':hashlib.sha256(packed).hexdigest(),'bytes':len(packed)})
                if selected in (0,8):contact.paste(image.convert('RGB'),(scene_index*640,(0 if selected==0 else 1)*480))
            manifest['scenes'].append(entry)
    manifest['packed_bytes']=total;manifest['uncompressed_rgba_bytes']=3*PRESET_COUNT*640*480*4
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    args.contact_sheet.parent.mkdir(parents=True,exist_ok=True);contact.save(args.contact_sheet)
    print(json.dumps({'presets':3*PRESET_COUNT,'packed_bytes':total,'uncompressed_rgba_bytes':manifest['uncompressed_rgba_bytes'],'renderer':manifest['renderer'],'output':str(args.output),'contact_sheet':str(args.contact_sheet)},indent=2))


if __name__=='__main__':main()
