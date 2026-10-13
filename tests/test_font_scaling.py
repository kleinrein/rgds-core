"""The real host font renderer scales text without multiplying atlas textures."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++") and shutil.which("sdl2-config"), "Host SDL compiler required")
class FontScalingTests(unittest.TestCase):
    def test_scaled_metrics_and_texture_cache_stay_bounded_across_point_sizes(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source, binary = folder / "font.cpp", folder / "font"
            source.write_text(r'''
#include "launcher/font.hpp"
#include <cassert>
#include <cmath>
static decltype(&SDL_CreateTextureFromSurface) original_create;
static int textures=0;
static SDL_Texture* counted_create(SDL_Renderer* renderer,SDL_Surface* surface) {
    ++textures;return original_create(renderer,surface);
}
int main(int argc,char** argv) {
    assert(argc==2);Sdl s;s.SetMainReady();assert(s.Init(SDL_INIT_VIDEO)==0);
    auto* surface=s.CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_RGBA32);
    assert(surface);auto* renderer=s.CreateSoftwareRenderer(surface);assert(renderer);
    original_create=s.CreateTextureFromSurface;s.CreateTextureFromSurface=counted_create;
    {
        rgds::RoundedFont font(s,argv[1]);
        assert(std::abs(font.width("Shared volume",13)-font.width("Shared volume",16)*13/16)<.001f);
        assert(std::abs(font.width("Game settings",34)-font.width("Game settings",36)*34/36)<.001f);
        for(int repetition=0;repetition<3;++repetition)for(int size=10;size<=84;++size) {
            font.draw(renderer,"Aa Bb 0123",24,24,size,{30,60,90},100,560);
        }
        assert(font.loaded_atlases()==5);assert(textures==5);
        assert(font.width("Aa Bb 0123",84)>font.width("Aa Bb 0123",16));
    }
    s.DestroyRenderer(renderer);s.FreeSurface(surface);s.Quit();return 0;
}
''')
            includes = [flag for flag in shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True)) if flag.startswith("-I")]
            subprocess.run(["clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *includes,
                            "-I" + str(ROOT / "src"), str(source), *( [] if sys.platform == "darwin" else ["-ldl"]), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(ROOT / "assets/fonts")],
                           env={**os.environ, "SDL_VIDEODRIVER": "dummy"}, check=True)


if __name__ == "__main__":
    unittest.main()
