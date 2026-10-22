#!/usr/bin/env python3
"""Host-only bridge/overlay microbenchmarks; opens no devices or emulator cores."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

BRIDGE = r'''
#define RGDS_BRIDGE_TESTS
#define RGDS_BRIDGE_PROFILE
#include "platform/emulator_bridge.cpp"
#include <chrono>
#include <iostream>
const Uint8* fake_keys(int* count){static Uint8 keys[512]{};*count=512;return keys;}
extern "C" void* dlsym(void*,const char* name){return !std::strcmp(name,"SDL_GetKeyboardState")?reinterpret_cast<void*>(&fake_keys):nullptr;}
int main(int argc,char** argv){
 if(argc!=2)return 2;fs::path root=argv[1];fs::create_directories(root/"session");fs::create_directories(root/"states");
 setenv("RGDS_CORE","drastic",1);setenv("RGDS_SHARED_MENU","1",1);setenv("RGDS_VOLUME_DATA",argv[1],1);
 setenv("RGDS_CONTROL_FILE",(root/"session/control").c_str(),1);setenv("RGDS_STATE_DIR",(root/"states").c_str(),1);setenv("RGDS_STATE_SLOT",(root/"states/slot.dss").c_str(),1);
 std::ofstream(root/"session/control")<<"old fast-forward\n";std::ofstream(root/"volume.ini")<<"schema=1\nvolume=25\n";
 initialize();tick();input_blocked();int count=0;SDL_GetKeyboardState(&count);
#if __has_include("platform/bridge_metrics.hpp")
 rgds::bridge_metrics()={};
#endif
 auto began=std::chrono::steady_clock::now();
 for(unsigned frame=0;frame<1800;frame++){test_time=1+frame*1000/60;for(int query=0;query<30;query++){tick();input_blocked();SDL_GetKeyboardState(&count);}}
 auto idle=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
 for(int i=0;i<1000;i++)std::ofstream(root/"states"/(std::to_string(i)+".dss"))<<"unrelated";
 std::ofstream(root/"states/slot.dss")<<"expected checkpoint";began=std::chrono::steady_clock::now();
 for(int i=0;i<200;i++)stamps();
 auto save=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
 std::cout<<"{\"idle_30_seconds_ms\":"<<idle<<",\"known_slot_200_checks_ms\":"<<save;
#if __has_include("platform/bridge_metrics.hpp")
 auto c=rgds::bridge_metrics();std::cout<<",\"file_stats\":"<<c.file_stats<<",\"file_reads\":"<<c.file_reads<<",\"block_checks\":"<<c.block_checks<<",\"keyboard_copies\":"<<c.keyboard_copies<<",\"state_stats\":"<<c.state_stats;
#endif
 std::cout<<"}\n";
}
'''

OVERLAY = r'''
#define RGDS_BRIDGE_PROFILE
#include "platform/emulator_overlay.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
int main(int argc,char** argv){
 if(argc!=2)return 2;setenv("RGDS_OVERLAY_FILE",argv[1],1);rgds::OverlayFrame writer;if(!writer.open(argv[1],true))return 3;
 auto init=rgds::overlay_sdl<decltype(&SDL_Init)>("SDL_Init");auto surface=rgds::overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat");auto software=rgds::overlay_sdl<decltype(&SDL_CreateSoftwareRenderer)>("SDL_CreateSoftwareRenderer");
 if(!init || init(0)!=0 || !surface || !software)return 4;auto* bitmap=surface(0,1280,480,32,SDL_PIXELFORMAT_RGBA32);auto* renderer=bitmap?software(bitmap):nullptr;if(!renderer)return 5;
 rgds::EmulatorOverlay overlay;auto* first=writer.begin();std::memset(first,255,rgds::overlay_panel_bytes*2);writer.publish();overlay.renderer(renderer,true);
#if __has_include("platform/bridge_metrics.hpp")
 rgds::bridge_metrics()={};
#endif
 auto began=std::chrono::steady_clock::now();
 for(int i=0;i<60;i++){auto* p=writer.begin();std::memset(p,255,rgds::overlay_panel_bytes*2);p[0]=i;writer.publish();for(int j=0;j<3;j++)overlay.renderer(renderer,true);}
 auto menu=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
#if __has_include("platform/bridge_metrics.hpp")
 auto m=rgds::bridge_metrics();rgds::bridge_metrics()={};
#endif
 overlay.volume(65);began=std::chrono::steady_clock::now();for(int i=0;i<180;i++)overlay.renderer(renderer,false);
 auto hud=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
 std::cout<<"{\"menu_180_presentations_ms\":"<<menu<<",\"hud_180_presentations_ms\":"<<hud;
#if __has_include("platform/bridge_metrics.hpp")
 auto h=rgds::bridge_metrics();std::cout<<",\"menu_sdl_lookups\":"<<m.sdl_lookups<<",\"menu_frame_allocations\":"<<m.frame_allocations<<",\"menu_frame_copies\":"<<m.frame_copies<<",\"menu_composites\":"<<m.composites<<",\"menu_texture_uploads\":"<<m.texture_uploads<<",\"menu_draws\":"<<m.draws<<",\"hud_texture_uploads\":"<<h.texture_uploads<<",\"hud_draws\":"<<h.draws;
#endif
 std::cout<<"}\n";overlay.destroy(renderer);rgds::overlay_sdl<decltype(&SDL_DestroyRenderer)>("SDL_DestroyRenderer")(renderer);rgds::overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface")(bitmap);rgds::overlay_sdl<decltype(&SDL_Quit)>("SDL_Quit")();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root", type=Path, default=ROOT, help="Repository or before snapshot with src/platform"
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not shutil.which("clang++") or not shutil.which("sdl2-config"):
        parser.error("clang++ and host SDL headers are required")
    flags = subprocess.check_output(["sdl2-config", "--cflags"], text=True).split()
    prefix = Path(subprocess.check_output(["sdl2-config", "--prefix"], text=True).strip())
    library = prefix / ("lib/libSDL2.dylib" if os.uname().sysname == "Darwin" else "lib/libSDL2-2.0.so.0")
    result = {"scope": "host CPU/software SDL, no device or emulator process", "source_root": str(args.source_root)}
    with tempfile.TemporaryDirectory(prefix="rgds-bridge-benchmark-") as folder:
        temp = Path(folder)
        for name, source in (("bridge", BRIDGE), ("overlay", OVERLAY)):
            path = temp / (name + ".cpp")
            path.write_text(source)
            binary = temp / name
            subprocess.run(
                [
                    "clang++", "-std=c++17", "-O2", *flags, "-I" + str(args.source_root / "src"),
                    str(path), "-o",
                    str(binary)
                ],
                check=True,
                capture_output=True,
                text=True
            )
            run = subprocess.run(
                [str(binary), str(temp / ("data" if name == "bridge" else "frame"))],
                env=dict(os.environ, RGDS_SDL_LIBRARY=str(library)),
                check=True,
                capture_output=True,
                text=True,
                timeout=30
            )
            result[name] = json.loads(run.stdout)
    serialized = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(serialized)
    print(serialized, end="")


if __name__ == "__main__":
    main()
