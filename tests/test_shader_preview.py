"""Exact GLES preview provenance, lossless packing and cache-only UI drawing."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
ASSETS=ROOT/'assets/shader-previews'


def unpack(path):
    data=path.read_bytes()
    if data[:8]!=b'RGSPV001':raise ValueError('Invalid preview')
    w,h,side,count=struct.unpack_from('<HHHH',data,8)
    size=side*side*4;offset=16+count*size;pixels=bytearray(w*h*4)
    indices=struct.unpack_from('<'+'H'*((w//side)*(h//side)),data,offset)
    for i,index in enumerate(indices):
        tile=data[16+index*size:16+(index+1)*size];x=(i%(w//side))*side;y=(i//(w//side))*side
        for row in range(side):pixels[((y+row)*w+x)*4:((y+row)*w+x+side)*4]=tile[row*side*4:(row+1)*side*4]
    return w,h,bytes(pixels)


class ShaderPreviewAssetsTests(unittest.TestCase):
    def test_lcd_grids_retain_scene_brightness_without_clipping_highlights(self):
        for scene in ('grove','circuit','starfall'):
            original=unpack(ASSETS/scene/'shader-0.rgsp')[2]
            source=sum(value for i,value in enumerate(original) if i%4<3)
            for preset,minimum in ((2,.89),(3,.93)):
                filtered=unpack(ASSETS/scene/f'shader-{preset}.rgsp')[2]
                level=sum(value for i,value in enumerate(filtered) if i%4<3)/source
                self.assertGreater(level,minimum,(scene,preset))
                self.assertLess(level,.99,(scene,preset))
                self.assertLessEqual(sum(value==255 for i,value in enumerate(filtered) if i%4<3),
                                     sum(value==255 for i,value in enumerate(original) if i%4<3))

    def test_every_original_scene_and_preset_is_exact_and_records_real_gles_provenance(self):
        manifest=json.loads((ASSETS/'manifest.json').read_text())
        self.assertEqual(manifest['native_size'],[256,192]);self.assertEqual(manifest['preview_size'],[640,480])
        self.assertIn('OpenGL ES',manifest['renderer']['version']);self.assertEqual(len(manifest['shaders']),20)
        self.assertEqual(len(manifest['scenes']),3)
        total=0
        for scene in manifest['scenes']:
            self.assertEqual([p['id'] for p in scene['presets']],list(range(21)))
            for preset in scene['presets']:
                path=ASSETS/scene['id']/('shader-'+str(preset['id'])+'.rgsp');packed=path.read_bytes();w,h,pixels=unpack(path)
                self.assertEqual((w,h),(640,480));self.assertEqual(hashlib.sha256(packed).hexdigest(),preset['sha256']);self.assertEqual(hashlib.sha256(pixels).hexdigest(),preset['rgba_sha256']);total+=len(packed)
            hashes=[p['rgba_sha256'] for p in scene['presets']]
            self.assertEqual(len(set(hashes)),21,'Every effect must be visible in the 2.5x scene')
        self.assertEqual(total,manifest['packed_bytes']);self.assertLess(total,manifest['uncompressed_rgba_bytes']//3)


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'),'Host C++ and SDL headers required')
class ShaderPreviewCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name);cls.binary=cls.root/'preview';source=cls.root/'preview.cpp';source.write_text(CPP)
        flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
        result=subprocess.run(['clang++','-std=c++17','-O1',*flags,'-I'+str(ROOT/'src'),str(source),'-o',str(cls.binary)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def test_runtime_decodes_exact_pixels_split_and_detail_without_drawing_file_io(self):
        with tempfile.TemporaryDirectory() as folder:
            result=subprocess.run([str(self.binary),str(ASSETS),folder],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            root=Path(folder);_,_,original=unpack(ASSETS/'grove/shader-0.rgsp');_,_,filtered=unpack(ASSETS/'grove/shader-2.rgsp')
            self.assertEqual((root/'original.rgba').read_bytes(),original)
            self.assertEqual((root/'filtered.rgba').read_bytes(),filtered)
            expected=bytearray()
            for y in range(480):expected.extend(original[y*640*4:(y*640+320)*4]);expected.extend(filtered[(y*640+320)*4:(y+1)*640*4])
            self.assertEqual((root/'split.rgba').read_bytes(),bytes(expected))
            # detail() samples a native64x48 patch and magnifies it with nearest.
            detail=(root/'detail.rgba').read_bytes();self.assertEqual(detail[:4],filtered[(255*640+90)*4:(255*640+90)*4+4])

    def test_malformed_and_missing_assets_are_cached_and_replacements_refresh(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'grove').mkdir();(root/'grove/shader-0.rgsp').write_bytes(b'RGSPV001'+struct.pack('<HHHH',640,480,5,1)+bytes(100)+struct.pack('<H',1)*12288)
            result=subprocess.run([str(self.binary),folder,folder,'invalid'],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


CPP=r'''
#include "launcher/shader_preview.hpp"
#include <cassert>
#include <fstream>
#include <vector>
using namespace rgds;
int main(int argc,char** argv){
 assert(argc==3 || argc==4);Sdl s;assert(s.Init(0)==0);auto* surface=s.CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_RGBA32);assert(surface);auto* renderer=s.CreateSoftwareRenderer(surface);assert(renderer);
 {ShaderPreview preview(s);fs::path folder=argv[1];SDL_Rect box{0,0,640,480};
 if(argc==4){
   assert(!preview.warm(renderer,folder,0,0));auto reads=preview.counters.file_reads,stamps=preview.counters.stamp_checks;
   for(int i=0;i<100;i++){assert(!preview.warm(renderer,folder,0,0));assert(!preview.draw(renderer,folder,0,0,box));}
   assert(preview.counters.file_reads==reads && preview.counters.stamp_checks==stamps && preview.bytes()==0);
   std::ofstream(folder/"grove/shader-0.rgsp",std::ios::trunc)<<"replaced";assert(preview.refresh());assert(!preview.warm(renderer,folder,0,0));assert(preview.counters.file_reads==reads+1);return 0;
 }
 assert(preview.warm(renderer,folder,0,2));auto reads=preview.counters.file_reads,stamps=preview.counters.stamp_checks;assert(reads==2);
 auto save=[&](const char* name){std::vector<unsigned char> pixels(640*480*4);assert(s.RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),640*4)==0);std::ofstream file(fs::path(argv[2])/name,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());};
 assert(preview.draw(renderer,folder,0,2,box,ShaderPreviewMode::Original));save("original.rgba");
 assert(preview.draw(renderer,folder,0,2,box,ShaderPreviewMode::Filtered));save("filtered.rgba");
 assert(preview.draw(renderer,folder,0,2,box,ShaderPreviewMode::Split,.5));save("split.rgba");
 assert(preview.detail(renderer,folder,0,2,box,ShaderPreviewMode::Filtered));save("detail.rgba");
 for(int i=0;i<100;i++){assert(preview.draw(renderer,folder,0,2,box,ShaderPreviewMode::Split));assert(preview.detail(renderer,folder,0,2,box));}
 assert(preview.counters.file_reads==reads && preview.counters.stamp_checks==stamps);assert(!preview.draw(renderer,folder,0,rgds::shader_count,box));assert(!preview.draw(renderer,folder,0,7,box));
 for(int scene=0;scene<3;scene++)for(int preset=0;preset<rgds::shader_count;preset++){assert(preview.warm(renderer,folder,scene,preset,true));assert(preview.draw(renderer,folder,scene,preset,box,ShaderPreviewMode::Split));assert(preview.count()<=8 && preview.bytes()<=10*1024*1024);}
 assert(preview.counters.evictions>0);preview.clear();assert(!preview.count() && !preview.bytes());
 }
 s.DestroyRenderer(renderer);s.FreeSurface(surface);s.Quit();
}
'''


if __name__=='__main__':unittest.main()
