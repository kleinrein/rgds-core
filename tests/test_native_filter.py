"""Native DSperate texture capability/failure handling with host software SDL."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'), 'Host C++/SDL headers required')
class NativeFilterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        source = cls.root / 'filter.cpp'
        cls.binary = cls.root / 'filter'
        source.write_text(
            r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <cassert>
std::string result(){std::ifstream f(control+".result");return std::string(std::istreambuf_iterator<char>(f),{});}
void choice(const char* id,const char* selection){std::ofstream f(control);f<<id<<" shader:"<<selection<<'\n';f.close();test_time+=30;tick();}
int main(int argc,char** argv){
 assert(argc==3);setenv("RGDS_CORE","dsperate",1);setenv("RGDS_SHARED_MENU","1",1);setenv("RGDS_CONTROL_FILE",(std::string(argv[1])+"/control").c_str(),1);initialize();
 if(!std::strcmp(argv[2],"missing")){
   choice("missing","1");assert(shader_waiting && result().find("shader-applying")!=std::string::npos);
   test_time+=1600;tick();assert(!shader_waiting && live_shader==0 && result().find("shader-failed")!=std::string::npos);return 0;
 }
 auto init=rgds::overlay_sdl<decltype(&SDL_Init)>("SDL_Init");assert(init && init(0)==0);
 auto surface=rgds::overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat");auto software=rgds::overlay_sdl<decltype(&SDL_CreateSoftwareRenderer)>("SDL_CreateSoftwareRenderer");
 auto* bitmap=surface(0,640,480,32,SDL_PIXELFORMAT_RGBA32);auto* renderer=software(bitmap);assert(renderer);
 auto create=rgds::overlay_sdl<decltype(&SDL_CreateTexture)>("SDL_CreateTexture");auto update=rgds::overlay_sdl<decltype(&SDL_UpdateTexture)>("SDL_UpdateTexture");
 auto mode=rgds::overlay_sdl<int(*)(SDL_Texture*,int*)>("SDL_GetTextureScaleMode");assert(mode);
 auto* game=create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,256,192);auto* icon=create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,16,16);assert(game && icon);
 std::vector<unsigned char> pixels(256*192*4);for(int y=0;y<192;y++)for(int x=0;x<256;x++){auto* p=pixels.data()+(y*256+x)*4;p[(x+y)%2?0:2]=255;p[3]=255;}assert(update(game,nullptr,pixels.data(),256*4)==0);
 SDL_Rect target{0,0,512,384};
 auto read=rgds::overlay_sdl<decltype(&SDL_RenderReadPixels)>("SDL_RenderReadPixels");std::vector<unsigned char> original(640*480*4),filtered(original.size());
 assert(SDL_RenderCopy(renderer,game,nullptr,&target)==0);assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,original.data(),640*4)==0);
 for(int selected:{1,2,14}){
   choice(std::to_string(selected).c_str(),std::to_string(selected).c_str());assert(shader_waiting && result().find("shader-applying")!=std::string::npos);
   // A software renderer must keep original pixels and report an unavailable
   // GLSL effect. A native bilinear toggle does not prove Sharp was applied.
   assert(SDL_RenderCopy(renderer,game,nullptr,&target)==0);assert(!shader_waiting && live_shader==0 && result().find("shader-failed")!=std::string::npos);
   assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,filtered.data(),640*4)==0);assert(original==filtered);
 }
 choice("ui","4");assert(SDL_RenderCopy(renderer,icon,nullptr,&target)==0);assert(shader_waiting);
 assert(native_textures.size()==2 && native_textures.at(game).game && !native_textures.at(icon).game);
 int game_mode=-1,icon_mode=-1;assert(mode(game,&game_mode)==0 && game_mode==0);assert(mode(icon,&icon_mode)==0 && icon_mode==0);
 choice("off","0");assert(!shader_waiting && result().find("shader-applied")!=std::string::npos);
 SDL_DestroyTexture(game);assert(!native_textures.count(game));SDL_DestroyTexture(icon);assert(native_textures.empty());
 rgds::overlay_sdl<decltype(&SDL_DestroyRenderer)>("SDL_DestroyRenderer")(renderer);rgds::overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface")(bitmap);rgds::overlay_sdl<decltype(&SDL_Quit)>("SDL_Quit")();
}
'''
        )
        flags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
        build = subprocess.run(
            ['clang++', '-std=c++17', '-O2', *flags, '-I' + str(ROOT / 'src'),
             str(source), '-o',
             str(cls.binary)],
            capture_output=True,
            text=True
        )
        if build.returncode: raise AssertionError(build.stderr)
        prefix = Path(subprocess.check_output(['sdl2-config', '--prefix'], text=True).strip())
        cls.library = prefix / ('lib/libSDL2.dylib' if os.uname().sysname == 'Darwin' else 'lib/libSDL2-2.0.so.0')
        stub = cls.root / 'stub.cpp'
        stub.write_text('extern "C" int SDL_QueryTexture(void*,void*,void*,int*,int*){return 0;}\n')
        cls.missing = cls.root / ('stub.dylib' if os.uname().sysname == 'Darwin' else 'stub.so')
        subprocess.run(
            ['clang++', '-shared', '-fPIC', str(stub), '-o',
             str(cls.missing)],
            check=True,
            capture_output=True,
            text=True
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixture(self, mode, library):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run(
                [str(self.binary), folder, mode],
                env=dict(os.environ, RGDS_SDL_LIBRARY=str(library)),
                capture_output=True,
                text=True,
                timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_software_renderer_preserves_pixels_and_leaves_ui_textures_alone(self):
        self.run_fixture('native', self.library)

    def test_missing_renderer_api_reports_failure_without_requesting_a_restart(self):
        self.run_fixture('missing', self.missing)


if __name__ == '__main__': unittest.main()
