"""Unavailable shader failure preserves the game frame on host software SDL."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'),'Host C++/SDL headers required')
class LiveShaderTests(unittest.TestCase):
    def compile(self, source, output, shared=False, link_sdl=False):
        flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
        libs=subprocess.check_output(['sdl2-config','--libs'],text=True).split() if link_sdl else []
        command=['clang++','-std=c++17','-O1',*flags,'-I'+str(ROOT/'src')]
        if shared:command+=['-shared','-fPIC']
        result=subprocess.run([*command,str(source),*libs,'-o',str(output)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_unsupported_sdl_shader_keeps_original_pixels_and_reports_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'software.cpp';binary=root/'software'
            source.write_text(r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::string result(){std::ifstream f(control+".result");return std::string(std::istreambuf_iterator<char>(f),{});}
void choose(int selected){std::ofstream f(control);f<<selected<<" shader:"<<selected<<'\n';f.close();test_time+=30;tick();}
int main(int argc,char** argv){
 assert(argc==2);setenv("RGDS_CORE","drastic",1);setenv("RGDS_SHARED_MENU","1",1);setenv("RGDS_CONTROL_FILE",(std::string(argv[1])+"/control").c_str(),1);initialize();
 auto init=rgds::overlay_sdl<decltype(&SDL_Init)>("SDL_Init");assert(init(0)==0);
 auto bitmap=rgds::overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat")(0,640,480,32,SDL_PIXELFORMAT_RGBA32);
 auto renderer=rgds::overlay_sdl<decltype(&SDL_CreateSoftwareRenderer)>("SDL_CreateSoftwareRenderer")(bitmap);assert(renderer);
 auto texture=rgds::overlay_sdl<decltype(&SDL_CreateTexture)>("SDL_CreateTexture")(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,256,192);
 std::vector<unsigned char> pixels(256*192*4);for(size_t i=0;i<pixels.size();i+=4){pixels[i+1]=200;pixels[i+3]=255;}
 assert(rgds::overlay_sdl<decltype(&SDL_UpdateTexture)>("SDL_UpdateTexture")(texture,nullptr,pixels.data(),256*4)==0);
 auto read=rgds::overlay_sdl<decltype(&SDL_RenderReadPixels)>("SDL_RenderReadPixels");std::vector<unsigned char> image(640*480*4);SDL_Rect target{0,0,640,480};
 choose(2);assert(result().find("shader-applying")!=std::string::npos);
 assert(SDL_RenderCopy(renderer,texture,nullptr,&target)==0);SDL_RenderPresent(renderer);
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,image.data(),640*4)==0);
 assert(image[0]==0 && image[1]==200 && live_shader==0 && !shader_waiting);assert(result().find("shader-failed")!=std::string::npos);
 choose(0);SDL_RenderPresent(renderer);assert(result().find("shader-applied")!=std::string::npos);
 choose(14);assert(SDL_RenderCopyEx(renderer,texture,nullptr,&target,0,nullptr,SDL_FLIP_NONE)==0);SDL_RenderPresent(renderer);
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,image.data(),640*4)==0);assert(image[0]==0 && image[1]==200 && live_shader==0);
 SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);rgds::overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface")(bitmap);
}
''')
            self.compile(source,binary)
            prefix=Path(subprocess.check_output(['sdl2-config','--prefix'],text=True).strip())
            sdl=prefix/('lib/libSDL2.dylib' if os.uname().sysname=='Darwin' else 'lib/libSDL2-2.0.so.0')
            result=subprocess.run([str(binary),str(root)],env=dict(os.environ,RGDS_SDL_LIBRARY=str(sdl)),capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)



if __name__=='__main__':unittest.main()
