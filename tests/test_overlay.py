"""Actual shared-frame publication and SDL composition with a game beneath it."""
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('clang++') and shutil.which('sdl2-config'),'SDL headers required')
class OverlayTests(unittest.TestCase):
    def test_gl_symbols_and_static_geometry_are_cached_per_context(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);stub=root/'gl.cpp';library=root/('gl.dylib' if os.uname().sysname=='Darwin' else 'gl.so')
            # Supply a typed fake GL driver; no video window or GPU is opened.
            signatures=re.findall(r'\bGL\((\w+),(\w+),(\([^;\n]*?\))\)',(ROOT/'src/platform/emulator_overlay.hpp').read_text())
            overrides={
                'CreateShader':'U impl_CreateShader(U){return 1;}', 'CreateProgram':'U impl_CreateProgram(){return 1;}',
                'GetShaderiv':'void impl_GetShaderiv(U,U,I* value){*value=1;}', 'GetProgramiv':'void impl_GetProgramiv(U,U,I* value){*value=1;}',
                'GenTextures':'void impl_GenTextures(I n,U* value){for(I i=0;i<n;i++)value[i]=i+1;}',
                'GenBuffers':'void impl_GenBuffers(I,U* value){*value=1;}',
                'BufferData':'void impl_BufferData(U,P,const void*,U){++geometry_uploads;}',
                'TexImage2D':'void impl_TexImage2D(U,I,I,I,I,I,U,U,const void*){++storage_allocations;}',
                'TexSubImage2D':'void impl_TexSubImage2D(U,I,I,I,I,I,U,U,const void*){++texture_updates;}',
            }
            declarations='\n'.join(overrides.get(name,f'{ret} impl_{name}{args}{{return {ret}();}}') for name,ret,args in signatures)
            lookup='\n'.join(f'if(!std::strcmp(name,"gl{name}"))return reinterpret_cast<void*>(&impl_{name});' for name,_,_ in signatures)
            stub.write_text('#include <SDL.h>\n#include <cstring>\n#include <cstddef>\nusing U=unsigned int;using I=int;using B=unsigned char;using P=std::ptrdiff_t;int geometry_uploads=0,storage_allocations=0,texture_updates=0;\n'+declarations+'''
extern "C" SDL_GLContext SDL_GL_GetCurrentContext(){return reinterpret_cast<void*>(1);}
extern "C" void SDL_GL_GetDrawableSize(SDL_Window*,int* w,int* h){*w=1280;*h=480;}
extern "C" int TestGeometryUploads(){return geometry_uploads;}
extern "C" int TestStorageAllocations(){return storage_allocations;}
extern "C" int TestTextureUpdates(){return texture_updates;}
extern "C" void* SDL_GL_GetProcAddress(const char* name){'''+lookup+'return nullptr;}\n')
            flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
            subprocess.run(['clang++','-std=c++17','-shared','-fPIC',*flags,str(stub),'-o',str(library)],check=True,capture_output=True,text=True)
            source=root/'check.cpp';binary=root/'check'
            source.write_text(r'''
#define RGDS_BRIDGE_PROFILE
#include "platform/emulator_overlay.hpp"
#include <cassert>
int main(){
 rgds::EmulatorOverlay overlay;overlay.volume(65);overlay.gl(nullptr,false);
 auto first=rgds::bridge_metrics();assert(first.gl_lookups==39 && first.texture_uploads==1 && first.draws==1);
 auto geometry=rgds::overlay_sdl<int(*)()>("TestGeometryUploads");assert(geometry && geometry()==1);
 auto storage=rgds::overlay_sdl<int(*)()>("TestStorageAllocations");auto updates=rgds::overlay_sdl<int(*)()>("TestTextureUpdates");assert(storage()==1 && updates()==0);
 rgds::bridge_metrics()={};overlay.gl(nullptr,false);auto steady=rgds::bridge_metrics();assert(steady.gl_lookups==0 && steady.texture_uploads==0 && steady.draws==1 && geometry()==1);
 overlay.volume(70);rgds::bridge_metrics()={};overlay.gl(nullptr,false);assert(rgds::bridge_metrics().gl_lookups==0 && rgds::bridge_metrics().texture_uploads==1 && geometry()==1 && storage()==1 && updates()==1);
 overlay.destroy_gl(reinterpret_cast<void*>(1));rgds::bridge_metrics()={};overlay.gl(nullptr,false);
 assert(rgds::bridge_metrics().gl_lookups==39 && geometry()==2);
}
''')
            subprocess.run(['clang++','-std=c++17',*flags,'-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True,capture_output=True,text=True)
            result=subprocess.run([str(binary)],env=dict(os.environ,RGDS_SDL_LIBRARY=str(library)),capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_frames_blend_over_both_panels_and_restore_renderer_state(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'overlay.cpp';binary=root/'overlay'
            source.write_text(r'''
#define RGDS_BRIDGE_PROFILE
#include "platform/emulator_overlay.hpp"
#include <cassert>
int main(int argc,char** argv){
 assert(argc==2);setenv("RGDS_OVERLAY_FILE",argv[1],1);
 rgds::OverlayFrame writer;assert(writer.open(argv[1],true));auto* pixels=writer.begin();
 std::memset(pixels,0,rgds::overlay_panel_bytes*2);
 for(int y=10;y<50;y++)for(int x=10;x<50;x++){
   auto* a=pixels+(y*640+x)*4;a[0]=128;a[3]=128;
   auto* b=a+rgds::overlay_panel_bytes;b[2]=255;b[3]=255;
 }
 rgds::OverlayFrame reader;assert(reader.open(argv[1],false));assert(!reader.refresh());writer.publish();assert(reader.refresh());
 auto* pending=writer.begin();std::memset(pending,0,rgds::overlay_panel_bytes*2);
 assert(reader.refresh());assert(reader.version()==2); // The last menu stays visible during publication.
 assert(reader.panel(0)[(20*640+20)*4]==128);
 std::memcpy(pending,pixels,rgds::overlay_panel_bytes*2);writer.publish();assert(reader.refresh());assert(reader.version()==4);
 __atomic_store_n(&writer.header()->visible,0u,__ATOMIC_RELEASE);assert(!reader.refresh());
 __atomic_store_n(&writer.header()->visible,1u,__ATOMIC_RELEASE);assert(reader.refresh());
 reader.close();assert(reader.open(argv[1],false));assert(reader.refresh());
 auto init=rgds::overlay_sdl<decltype(&SDL_Init)>("SDL_Init");assert(init && init(0)==0);
 auto create=rgds::overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat");
 auto software=rgds::overlay_sdl<decltype(&SDL_CreateSoftwareRenderer)>("SDL_CreateSoftwareRenderer");
 auto* surface=create(0,1280,480,32,SDL_PIXELFORMAT_RGBA32);assert(surface);auto* renderer=software(surface);assert(renderer);
 auto color=rgds::overlay_sdl<decltype(&SDL_SetRenderDrawColor)>("SDL_SetRenderDrawColor");
 auto clear=rgds::overlay_sdl<decltype(&SDL_RenderClear)>("SDL_RenderClear");color(renderer,0,200,0,255);clear(renderer);
 auto texture=rgds::overlay_sdl<decltype(&SDL_CreateTexture)>("SDL_CreateTexture");
 auto settarget=rgds::overlay_sdl<decltype(&SDL_SetRenderTarget)>("SDL_SetRenderTarget");
 auto gettarget=rgds::overlay_sdl<decltype(&SDL_GetRenderTarget)>("SDL_GetRenderTarget");
 auto* target=texture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,256,192);assert(target);assert(settarget(renderer,target)==0);
 color(renderer,0,0,200,255);clear(renderer);
 auto logical=rgds::overlay_sdl<decltype(&SDL_RenderSetLogicalSize)>("SDL_RenderSetLogicalSize");logical(renderer,640,240);
 auto setclip=rgds::overlay_sdl<decltype(&SDL_RenderSetClipRect)>("SDL_RenderSetClipRect");SDL_Rect clip{0,0,1,1};setclip(renderer,&clip);
 rgds::EmulatorOverlay overlay;overlay.renderer(renderer,true);assert(writer.header()->reserved[0]>0);
 assert(gettarget(renderer)==target); // The menu must draw on the output, never the emulator's own target.
 auto getlogical=rgds::overlay_sdl<decltype(&SDL_RenderGetLogicalSize)>("SDL_RenderGetLogicalSize");int w=0,h=0;getlogical(renderer,&w,&h);assert(w==640 && h==240);
 auto getclip=rgds::overlay_sdl<decltype(&SDL_RenderGetClipRect)>("SDL_RenderGetClipRect");SDL_Rect restored{};getclip(renderer,&restored);assert(restored.w==1 && restored.h==1);
 std::vector<unsigned char> target_pixels(256*192*4);auto read=rgds::overlay_sdl<decltype(&SDL_RenderReadPixels)>("SDL_RenderReadPixels");
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,target_pixels.data(),256*4)==0);assert(target_pixels[2]==200 && target_pixels[0]==0);
 assert(settarget(renderer,nullptr)==0);
 std::vector<unsigned char> output(1280*480*4);assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,output.data(),1280*4)==0);
 auto* outside=output.data()+(100*1280+100)*4;assert(outside[0]==0 && outside[1]==200 && outside[2]==0);
 auto* top=output.data()+(20*1280+20)*4;assert(top[0]>=127 && top[0]<=129 && top[1]>=98 && top[1]<=101);
 auto* bottom=output.data()+(20*1280+660)*4;assert(bottom[0]==0 && bottom[1]==0 && bottom[2]==255);
 rgds::bridge_metrics()={};overlay.renderer(renderer,true);auto steady=rgds::bridge_metrics();
 assert(steady.sdl_lookups==0 && steady.frame_copies==0 && steady.composites==0 && steady.texture_uploads==0 && steady.draws==2);
 color(renderer,0,200,0,255);clear(renderer);rgds::bridge_metrics()={};overlay.volume(65);overlay.renderer(renderer,false);
 auto hud_work=rgds::bridge_metrics();assert(hud_work.texture_uploads==1 && hud_work.draws==1 && hud_work.composites==1);
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,output.data(),1280*4)==0);
 auto* hud=output.data()+(116*1280+200)*4;assert(hud[0]<50 && hud[2]<80 && hud[3]>200);
 auto* hud_bottom=output.data()+(116*1280+840)*4;assert(hud_bottom[0]==0 && hud_bottom[1]==200 && hud_bottom[2]==0);
 assert(output[(20*1280+20)*4]==0); // Closing the menu leaves only the small top-screen HUD.
 overlay.fast_forward(true);color(renderer,0,200,0,255);clear(renderer);overlay.renderer(renderer,false);
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,output.data(),1280*4)==0);
 auto* ff=output.data()+(439*1280+44)*4;assert(ff[0]==78 && ff[1]==187 && ff[2]==255);
 std::string control=std::string(argv[1])+".control";setenv("RGDS_CONTROL_FILE",control.c_str(),1);
 {std::ofstream request(control+".capture");request<<"hud-test";}
 overlay.capture(renderer);assert(std::ifstream(control+".capture").good());
 assert(!std::ifstream(control+".capture-combined.bmp").good());
 overlay.capture(renderer,true);assert(!std::ifstream(control+".capture").good());
 assert(std::ifstream(control+".capture-combined.bmp").good());
 rgds::bridge_metrics()={};overlay.renderer(renderer,false);assert(rgds::bridge_metrics().texture_uploads==0);
 overlay.fast_forward(false);color(renderer,0,200,0,255);clear(renderer);overlay.renderer(renderer,false);
 assert(read(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,output.data(),1280*4)==0);
 assert(ff[0]==0 && ff[1]==200 && ff[2]==0); // FF switch must erase its badge.

 rgds::overlay_sdl<decltype(&SDL_DestroyTexture)>("SDL_DestroyTexture")(target);
 overlay.destroy(renderer);rgds::overlay_sdl<decltype(&SDL_DestroyRenderer)>("SDL_DestroyRenderer")(renderer);
 rgds::overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface")(surface);rgds::overlay_sdl<decltype(&SDL_Quit)>("SDL_Quit")();
}
''')
            flags=subprocess.check_output(['sdl2-config','--cflags'],text=True).split()
            build=subprocess.run(['clang++','-std=c++17','-O2',*flags,'-I'+str(ROOT/'src'),str(source),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            prefix=subprocess.check_output(['sdl2-config','--prefix'],text=True).strip()
            library=Path(prefix)/'lib/libSDL2.dylib' if os.uname().sysname=='Darwin' else Path(prefix)/'lib/libSDL2-2.0.so.0'
            env=dict(os.environ,RGDS_SDL_LIBRARY=str(library))
            result=subprocess.run([str(binary),str(root/'frame')],env=env,capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_menu_renders_without_a_video_window(self):
        binary=ROOT/'build/host/rgds-launcher'
        if not binary.exists():self.skipTest('Host build required')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);frame=root/'menu.frame'
            env=dict(os.environ,RGDS_OVERLAY_FILE=str(frame),RGDS_SYSTEM_MANAGED='1',RGDS_VOLUME_EXTERNAL='1',SDL_VIDEODRIVER='dummy')
            result=subprocess.run([str(binary),'--game-menu','--app',str(ROOT),'--data',str(root/'data'),'--seconds','1'],env=env,cwd=ROOT,capture_output=True,text=True,timeout=4)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            header=struct.unpack('16I',frame.read_bytes()[:64])
            self.assertEqual(header[:3],(0x52474453,640,480));self.assertGreater(header[3],0)
            self.assertEqual(header[4],0,'Closing the helper must hide its frame')
            self.assertIn('driver=unknown',result.stdout)
