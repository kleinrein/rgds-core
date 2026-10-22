"""Actual GLES2 compilation, pixels and native draw interception on the host.

Linux CI can provide Mesa EGL/GLES; macOS uses installed SwiftShader libraries.
Set RGDS_TEST_EGL_LIBRARY/RGDS_TEST_GLES_LIBRARY to use another host driver.
No device, emulator, ROM or remote connection is involved.
"""
import ctypes.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def graphics_libraries():
    egl = os.environ.get('RGDS_TEST_EGL_LIBRARY')
    gles = os.environ.get('RGDS_TEST_GLES_LIBRARY')
    if egl and gles: return egl, gles
    swift = Path.home() / 'Library/Android/sdk/emulator/lib64/gles_angle'
    if (swift / 'libswiftshader_libEGL.dylib').is_file():
        return str(swift / 'libswiftshader_libEGL.dylib'), str(swift / 'libswiftshader_libGLESv2.dylib')
    return ctypes.util.find_library('EGL'), ctypes.util.find_library('GLESv2')


@unittest.skipUnless(
    shutil.which('clang++') and shutil.which('sdl2-config') and all(graphics_libraries()),
    'A host EGL/GLES2 driver and C++/SDL headers are required'
)
class ShaderPixelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        cls.library = cls.folder / ('driver.dylib' if os.uname().sysname == 'Darwin' else 'driver.so')
        flags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
        cls.flags = ['clang++', '-std=c++17', '-O1', *flags, '-I' + str(ROOT / 'src')]
        result = subprocess.run(
            [
                *cls.flags, '-shared', '-fPIC',
                str(ROOT / 'tests/fixtures/shader_gles_driver.cpp'), '-o',
                str(cls.library)
            ],
            capture_output=True,
            text=True
        )
        if result.returncode: raise AssertionError(result.stderr)
        cls.binary = cls.folder / 'shader-pixels'
        source = cls.folder / 'pixels.cpp'
        source.write_text(CPP)
        result = subprocess.run(
            [*cls.flags, str(source), str(cls.library), '-o',
             str(cls.binary)], capture_output=True, text=True
        )
        if result.returncode: raise AssertionError(result.stderr)
        egl, gles = graphics_libraries()
        cls.environment = dict(
            os.environ,
            RGDS_TEST_EGL_LIBRARY=egl,
            RGDS_TEST_GLES_LIBRARY=gles,
            RGDS_SDL_LIBRARY=str(cls.library),
            RGDS_SHADER_DIR=str(ROOT / 'vendor/engines/shaders'),
            EGL_PLATFORM='surfaceless'
        )
        merged = cls.folder / 'shaders'
        shutil.copytree(cls.environment['RGDS_SHADER_DIR'], merged)
        for shader in (ROOT / 'assets/shaders').glob('*.dsd'):
            shutil.copyfile(shader, merged / shader.name)
        cls.environment['RGDS_SHADER_DIR'] = str(merged)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_all_presets_compile_link_and_change_real_pixels_on_every_core(self):
        for core in ('nnddss', 'drastic', 'dsperate', 'melonds'):
            with self.subTest(core=core), tempfile.TemporaryDirectory() as folder:
                result = subprocess.run(
                    [str(self.binary), folder, core], env=self.environment, capture_output=True, text=True, timeout=30
                )
                if result.returncode == 77: self.skipTest('Host EGL driver cannot create a pbuffer')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('presets=20 pixels=verified', result.stdout)

    def test_enhanced_rgds_panel_filters_actual_640_by_480_pixels(self):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run(
                [str(self.binary), folder, 'dsperate', 'enhanced'],
                env=dict(self.environment, RGDS_OWN_FF_HUD='1', RGDS_ACTIVE_SCALE='5'),
                capture_output=True,
                text=True,
                timeout=30
            )
            if result.returncode == 77: self.skipTest('Host EGL driver cannot create a pbuffer')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('enhanced panel pixels=verified', result.stdout)

    def test_classic_lcd_grids_clearly_modulate_a_flat_grey_640_by_480_panel(self):
        # The community grids were tuned for 1080p. On the handheld's 2.5x panel
        # they must still read as a grid: at least about 20 % column modulation
        # of a flat mid grey, while the mean brightness stays above three quarters.
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run(
                [str(self.binary), folder, 'drastic', 'grid'],
                env=self.environment,
                capture_output=True,
                text=True,
                timeout=60
            )
            if result.returncode == 77: self.skipTest('Host EGL driver cannot create a pbuffer')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('grid amplitudes=measured', result.stdout)
            measured = {
                int(m.group(1)): tuple(float(m.group(i)) for i in (2, 3, 4))
                for m in re.finditer(r'^grid (\d+) column=([\d.]+) row=([\d.]+) mean=([\d.]+)$', result.stdout, re.M)
            }
            for selected in (2, 6, 7, 8):  # lcd1x: a vertical LCD grid with faint scanlines.
                with self.subTest(preset=selected):
                    column, row, mean = measured[selected]
                    self.assertGreaterEqual(column, 20)
                    self.assertGreaterEqual(row, 5)
                    self.assertGreaterEqual(mean, 80 if selected == 2 else 65)
            for selected in (3, 12, 13, 14):  # zfast: a square grid.
                with self.subTest(preset=selected):
                    column, row, mean = measured[selected]
                    self.assertGreaterEqual(column, 20)
                    self.assertGreaterEqual(row, 20)
                    self.assertGreaterEqual(mean, 80 if selected == 3 else 65)
            for selected in (4, 5):  # Colour-only presets leave a flat image flat.
                with self.subTest(preset=selected):
                    self.assertLess(measured[selected][0], 1)

    def test_compile_failure_restores_previous_selection_and_is_cached(self):
        with tempfile.TemporaryDirectory() as folder:
            pack = Path(folder) / 'shaders'
            shutil.copytree(self.environment['RGDS_SHADER_DIR'], pack)
            bad = pack / 'lcd1x.dsd'
            bad.write_text(bad.read_text().replace('void main()', 'void broken_main(', 1))
            for core in ('nnddss', 'drastic', 'dsperate', 'melonds'):
                with self.subTest(core=core), tempfile.TemporaryDirectory() as control:
                    result = subprocess.run(
                        [str(self.binary), control, core, 'failure'],
                        env=dict(self.environment, RGDS_SHADER_DIR=str(pack)),
                        capture_output=True,
                        text=True,
                        timeout=30
                    )
                    if result.returncode == 77: self.skipTest('Host EGL driver cannot create a pbuffer')
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('compile failure preserved previous pixels', result.stdout)


CPP = r'''
#define RGDS_BRIDGE_TESTS
#include "platform/emulator_bridge.cpp"
#include <algorithm>
#include <cassert>
#include <cmath>
using U=unsigned;using I=int;using B=unsigned char;
extern "C" SDL_Texture* TestTexture(U,int,int);
extern "C" void TestGeometry(float,float,int,int,int,int,int,int,int,int);
extern "C" int TestCopies();
extern "C" int TestSwaps();
extern "C" int TestCompiles();
extern "C" U TestProgram(int);
extern "C" void TestRenderTarget(SDL_Texture*);
extern "C" SDL_Texture* TestTextureFormat(U,int,int,Uint32);
extern "C" void TestAlpha(SDL_Texture*,Uint8);
int main(int argc,char** argv){
 assert(argc==3 || argc==4);setenv("RGDS_CORE",argv[2],1);setenv("RGDS_SHARED_MENU","1",1);setenv("RGDS_CONTROL_FILE",(std::string(argv[1])+"/control").c_str(),1);initialize();
 auto proc=rgds::overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
#define GL(name,ret,args) auto name=reinterpret_cast<ret(*)args>(proc("gl" #name));assert(name)
 GL(IsProgram,B,(U));GL(GetError,U,());GL(GetIntegerv,void,(U,I*));GL(GetTexParameteriv,void,(U,U,I*));GL(IsEnabled,B,(U));GL(Enable,void,(U));GL(Disable,void,(U));
 GL(CreateShader,U,(U));GL(ShaderSource,void,(U,I,const char* const*,const I*));GL(CompileShader,void,(U));GL(GetShaderiv,void,(U,U,I*));GL(GetShaderInfoLog,void,(U,I,I*,char*));
 GL(CreateProgram,U,());GL(AttachShader,void,(U,U));GL(BindAttribLocation,void,(U,U,const char*));GL(LinkProgram,void,(U));GL(GetProgramiv,void,(U,U,I*));GL(UseProgram,void,(U));
 GL(GenTextures,void,(I,U*));GL(ActiveTexture,void,(U));GL(BindTexture,void,(U,U));GL(TexParameteri,void,(U,U,I));GL(TexImage2D,void,(U,I,I,I,I,I,U,U,const void*));
 GL(GenBuffers,void,(I,U*));GL(BindBuffer,void,(U,U));GL(BufferData,void,(U,std::ptrdiff_t,const void*,U));GL(VertexAttribPointer,void,(U,I,U,B,I,const void*));GL(EnableVertexAttribArray,void,(U));GL(DisableVertexAttribArray,void,(U));
 GL(Viewport,void,(I,I,I,I));GL(Scissor,void,(I,I,I,I));GL(ColorMask,void,(B,B,B,B));GL(ClearColor,void,(float,float,float,float));GL(Clear,void,(U));GL(ReadPixels,void,(I,I,I,I,U,U,void*));
#undef GL
 const char* vs="attribute vec2 aPosition;attribute vec2 aTextureCoord;varying vec2 v;void main(){v=aTextureCoord;gl_Position=vec4(aPosition,0.,1.);}";
 const char* fs="precision mediump float;varying vec2 v;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,v);}";
 auto compile=[&](U type,const char* text){auto id=CreateShader(type);ShaderSource(id,1,&text,nullptr);CompileShader(id);I ok=0;GetShaderiv(id,0x8b81,&ok);assert(ok);return id;};
 U original=CreateProgram();AttachShader(original,compile(0x8b31,vs));AttachShader(original,compile(0x8b30,fs));BindAttribLocation(original,3,"aPosition");BindAttribLocation(original,5,"aTextureCoord");LinkProgram(original);I linked=0;GetProgramiv(original,0x8b82,&linked);assert(linked);UseProgram(original);
 U image=0,other=0;GenTextures(1,&image);GenTextures(1,&other);ActiveTexture(0x84c0);BindTexture(0x0de1,image);
 TexParameteri(0x0de1,0x2801,0x2600);TexParameteri(0x0de1,0x2800,0x2600);TexParameteri(0x0de1,0x2802,0x812f);TexParameteri(0x0de1,0x2803,0x812f);
 std::vector<B> pixels(256*192*4);
 for(int y=0;y<192;y++)for(int x=0;x<256;x++){auto p=&pixels[(y*256+x)*4];p[0]=x<128?180:30;p[1]=y<96?90:160;p[2]=((x+y)%2)?140:70;p[3]=255;}
 glTexImage2D(0x0de1,0,0x1908,256,192,0,0x1908,0x1401,pixels.data());
 auto texture=TestTexture(image,256,192);auto renderer=reinterpret_cast<SDL_Renderer*>(1);
 const float vertices[]={-1,1,0,0,1,1,1,0,-1,-1,0,1,-1,-1,0,1,1,1,1,0,1,-1,1,1};U buffer=0;GenBuffers(1,&buffer);BindBuffer(0x8892,buffer);glBufferData(0x8892,4416,nullptr,0x88e8);glBufferSubData(0x8892,2208,sizeof(vertices),vertices);
 VertexAttribPointer(3,2,0x1406,0,16,reinterpret_cast<void*>(2208));VertexAttribPointer(5,2,0x1406,0,16,reinterpret_cast<void*>(2216));EnableVertexAttribArray(3);EnableVertexAttribArray(5);
 auto clear=[&](){Disable(0x0c11);Disable(0x0be2);ColorMask(1,1,1,1);Viewport(0,0,1280,960);ClearColor(0,0,0,1);Clear(0x4000);};
 auto read=[&](){std::vector<B> values(1280*960*4);ReadPixels(0,0,1280,960,0x1908,0x1401,values.data());assert(GetError()==0);return values;};
 int serial=0;auto command=[&](int id){std::ofstream file(control);file<<++serial<<" shader:"<<id<<'\n';file.close();test_time+=30;tick();};
 auto result=[&](){std::ifstream file(control+".result");return std::string(std::istreambuf_iterator<char>(file),{});};
 SDL_Rect rect{0,0,1280,960};TestGeometry(1,1,0,0,1280,960,0,0,-1,-1);
 clear();glDrawArrays(4,0,6);auto original_pixels=read();
 if(argc==4 && std::string(argv[3])=="enhanced"){
   assert(enhanced_game_texture);
   std::vector<B> panel(640*480*4);for(int y=0;y<480;y++)for(int x=0;x<640;x++){
     int p=(y*640+x)*4;panel[p]=x%256;panel[p+1]=y%256;panel[p+2]=180;panel[p+3]=255;
   }
   BindTexture(0x0de1,image);glTexImage2D(0x0de1,0,0x1908,640,480,0,0x1908,0x1401,panel.data());
   auto big=TestTexture(image,640,480);clear();glDrawArrays(4,0,6);auto unfiltered=read();
   for(int selected : {2,4}){
     command(selected);clear();assert(SDL_RenderCopy(renderer,big,nullptr,&rect)==0);
     assert(!shader_waiting && live_shader==selected && TestCopies()==0);
     auto filtered=read();size_t changed=0;for(size_t i=0;i<filtered.size();i+=4)if(filtered[i]!=unfiltered[i] || filtered[i+1]!=unfiltered[i+1] || filtered[i+2]!=unfiltered[i+2])++changed;
     assert(changed>1000);
   }
   enhanced_game_texture=false;native_textures.clear();assert(!sdl_shader(renderer,big,nullptr,&rect));
   SDL_DestroyTexture(big);std::puts("enhanced panel pixels=verified");return 0;
 }
 if(argc==4 && std::string(argv[3])=="grid"){
   // A flat mid grey at the panel's 2.5x scale isolates each preset's grid:
   // column/row amplitude in percent of the brightest line, mean in percent
   // of the input grey.
   for(size_t i=0;i<pixels.size();i+=4){pixels[i]=pixels[i+1]=pixels[i+2]=128;pixels[i+3]=255;}
   BindTexture(0x0de1,image);glTexImage2D(0x0de1,0,0x1908,256,192,0,0x1908,0x1401,pixels.data());SDL_Rect panel{0,0,640,480};
   for(int selected=1;selected<rgds::shader_count;selected++){
     command(selected);clear();assert(SDL_RenderCopy(renderer,texture,nullptr,&panel)==0);assert(!shader_waiting && live_shader==selected);
     auto frame=read();std::vector<double> columns(640,0),rows(480,0);double total=0;
     for(int y=0;y<480;y++)for(int x=0;x<640;x++){auto p=&frame[((959-y)*1280+x)*4];double v=(p[0]+p[1]+p[2])/3.0;columns[x]+=v/480;rows[y]+=v/640;total+=v/(640.0*480);}
     auto amplitude=[](const std::vector<double>& lines){double lo=*std::min_element(lines.begin(),lines.end()),hi=*std::max_element(lines.begin(),lines.end());return hi>0?100.0*(hi-lo)/hi:0.0;};
     std::printf("grid %d column=%.1f row=%.1f mean=%.1f\n",selected,amplitude(columns),amplitude(rows),100.0*total/128.0);
   }
   std::puts("grid amplitudes=measured");return 0;
 }
 if(argc==4){
   auto draw=[&](){clear();if(core=="nnddss")glDrawArrays(4,0,6);else assert(SDL_RenderCopy(renderer,texture,nullptr,&rect)==0);};
   command(4);draw();auto previous=read();assert(live_shader==4 && !shader_waiting);
   command(2);draw();assert(live_shader==4 && !shader_waiting && result().find("shader-failed")!=std::string::npos);draw();assert(read()==previous);
   int compiled=TestCompiles();command(2);draw();assert(live_shader==4 && !shader_waiting && TestCompiles()==compiled);draw();assert(read()==previous);
   game_shader.destroy(rgds::overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext")());assert(!IsProgram(TestProgram(1)) && IsProgram(original));
   std::puts("compile failure preserved previous pixels");return 0;
 }
 for(int selected=1;selected<rgds::shader_count;selected++){
   command(selected);assert(shader_waiting && result().find("shader-applying")!=std::string::npos);clear();
   if(core=="nnddss")glDrawArrays(4,0,6);else{assert(SDL_RenderClear(renderer)==0);assert(SDL_RenderCopy(renderer,texture,nullptr,&rect)==0);SDL_RenderPresent(renderer);}
   assert(!shader_waiting && live_shader==selected && result().find("shader-applied")!=std::string::npos);
   auto filtered=read();size_t changed=0;for(size_t i=0;i<filtered.size();i+=4)if(filtered[i]!=original_pixels[i] || filtered[i+1]!=original_pixels[i+1] || filtered[i+2]!=original_pixels[i+2])++changed;
   // Sharp alone must visibly interpolate a noninteger-scaled source. The
   // full-frame 5x case can have identical sampled pixels, so inspect another
   // 640x480 (2.5x) draw as well instead of accepting an identity effect.
   if(selected!=1)assert(changed>1000);
   I restored=0;GetIntegerv(0x8b8d,&restored);assert(U(restored)==original);GetIntegerv(0x8894,&restored);assert(U(restored)==buffer);GetTexParameteriv(0x0de1,0x2801,&restored);assert(restored==0x2600);GetTexParameteriv(0x0de1,0x2800,&restored);assert(restored==0x2600);
 }
 assert(TestCompiles()==2*rgds::shader_count); // Original program + every preset pair.
 // Sharp scaling at a noninteger 2.5x scale must change real edge pixels.
 clear();Viewport(0,480,640,480);live_shader=0;glDrawArrays(4,0,6);auto scaled=read();
 live_shader=1;clear();Viewport(0,480,640,480);
 if(core=="nnddss")glDrawArrays(4,0,6);else{SDL_Rect noninteger{0,0,640,480};assert(SDL_RenderCopy(renderer,texture,nullptr,&noninteger)==0);}
 auto sharp=read();assert(sharp!=scaled);assert(TestCompiles()==2*rgds::shader_count);
 if(core=="nnddss"){
   const float half[]={-1,1,0,0,0,1,1,0,-1,-1,0,1,-1,-1,0,1,0,1,1,0,0,-1,1,1};
   glBufferSubData(0x8892,2208,sizeof(half),half);clear();Viewport(0,480,1280,480);glDrawArrays(4,0,6);
   assert(read()==sharp); // Half-span geometry still means a 640x480 target.
   glBufferSubData(0x8892,2208,sizeof(vertices),vertices);
   glBufferSubData(0x8892,2208+sizeof(vertices),sizeof(vertices),vertices);clear();Viewport(0,480,640,480);glDrawArrays(4,6,6);assert(read()==sharp);
   // Exact default loader topology: static interleaved CPU arrays, one
   // 1280x480 viewport, triangles6 at first0 and first6.
   const float lr[]={-1,1,0,0,0,1,1,0,-1,-1,0,1,-1,-1,0,1,0,1,1,0,0,-1,1,1,0,1,0,0,1,1,1,0,0,-1,0,1,0,-1,0,1,1,1,1,0,1,-1,1,1};
   BindBuffer(0x8892,0);VertexAttribPointer(3,2,0x1406,0,16,lr);VertexAttribPointer(5,2,0x1406,0,16,lr+2);clear();Viewport(0,480,1280,480);glDrawArrays(4,0,6);assert(read()==sharp);glDrawArrays(4,6,6);auto side_by_side=read();assert(side_by_side[((959-200)*1280+700)*4]>0);
   BindBuffer(0x8892,buffer);VertexAttribPointer(3,2,0x1406,0,16,reinterpret_cast<void*>(2208));VertexAttribPointer(5,2,0x1406,0,16,reinterpret_cast<void*>(2216));
 }
 // Real precision regression: unprepared original fragments MUST fail on
 // this GLES compiler, proving the harness catches the original defect.
 std::ifstream raw(std::string(std::getenv("RGDS_SHADER_DIR"))+"/lcd1x.dsd");std::string text(std::istreambuf_iterator<char>(raw),{});auto start=text.find("<fragment>")+10,end=text.find("</fragment>");std::string body=text.substr(start,end-start);const char* unprepared=body.c_str();U bad=CreateShader(0x8b30);ShaderSource(bad,1,&unprepared,nullptr);CompileShader(bad);I accepted=1;GetShaderiv(bad,0x8b81,&accepted);assert(!accepted);
 // Per-screen rect, letterbox, logical scale, clipping and UV orientation.
 if(core!="nnddss"){
   command(4);clear();TestGeometry(2,2,10,20,600,440,30,40,120,80);SDL_Rect box{20,20,256,192};assert(SDL_RenderCopy(renderer,texture,nullptr,&box)==0);auto frame=read();
   auto pixel=[&](int x,int y){return &frame[((959-y)*1280+x)*4];};assert(pixel(10,10)[0]==0);assert(pixel(90,130)[0]>100);assert(pixel(350,200)[0]==0);assert(pixel(90,110)[0]==0);
   clear();TestGeometry(1,1,0,0,1280,960,0,0,-1,-1);SDL_Rect rotated{200,200,256,192};assert(SDL_RenderCopyEx(renderer,texture,nullptr,&rotated,90,nullptr,SDL_FLIP_VERTICAL)==0);frame=read();assert(pixel(230,160)[0]==0);assert(pixel(300,250)[0]>0);
   assert(TestCopies()==0); // Presets replace SDL's game blit; no second copy.
   TestRenderTarget(texture);assert(SDL_RenderCopy(renderer,texture,nullptr,&rect)==0);TestRenderTarget(nullptr);assert(live_shader==4 && TestCopies()==1);
   // DraStic can upload both screens into one native texture and draw source
   // rectangles. Their grids/scaling must remain independent, without seams.
   U combined=0;GenTextures(1,&combined);BindTexture(0x0de1,combined);TexParameteri(0x0de1,0x2801,0x2600);TexParameteri(0x0de1,0x2800,0x2600);TexParameteri(0x0de1,0x2802,0x812f);TexParameteri(0x0de1,0x2803,0x812f);
   std::vector<B> dual(256*384*4);std::copy(pixels.begin(),pixels.end(),dual.begin());std::copy(pixels.begin(),pixels.end(),dual.begin()+pixels.size());glTexImage2D(0x0de1,0,0x1908,256,384,0,0x1908,0x1401,dual.data());auto stacked=TestTexture(combined,256,384);
   live_shader=2;clear();SDL_Rect full{0,0,640,960};assert(SDL_RenderCopy(renderer,stacked,nullptr,&full)==0);auto combined_frame=read();
   clear();SDL_Rect upper{0,0,640,480},lower{0,480,640,480};assert(SDL_RenderCopy(renderer,texture,nullptr,&upper)==0);assert(SDL_RenderCopy(renderer,texture,nullptr,&lower)==0);assert(read()==combined_frame);
   clear();SDL_Rect bottom_source{0,192,256,192};assert(SDL_RenderCopy(renderer,stacked,&bottom_source,&upper)==0);auto source_frame=read();clear();assert(SDL_RenderCopy(renderer,texture,nullptr,&upper)==0);assert(read()==source_frame);
   // Distinct adjacent screens catch linear sampling into another subrect.
   std::vector<B> other_pixels=pixels;for(size_t i=0;i<other_pixels.size();i+=4){other_pixels[i]=20;other_pixels[i+1]=40;other_pixels[i+2]=230;}
   std::copy(other_pixels.begin(),other_pixels.end(),dual.begin()+pixels.size());BindTexture(0x0de1,combined);glTexImage2D(0x0de1,0,0x1908,256,384,0,0x1908,0x1401,dual.data());
   U bottom=0;GenTextures(1,&bottom);BindTexture(0x0de1,bottom);TexParameteri(0x0de1,0x2801,0x2600);TexParameteri(0x0de1,0x2800,0x2600);TexParameteri(0x0de1,0x2802,0x812f);TexParameteri(0x0de1,0x2803,0x812f);glTexImage2D(0x0de1,0,0x1908,256,192,0,0x1908,0x1401,other_pixels.data());auto bottom_texture=TestTexture(bottom,256,192);
   live_shader=1;clear();assert(SDL_RenderCopy(renderer,stacked,nullptr,&full)==0);auto distinct=read();clear();assert(SDL_RenderCopy(renderer,texture,nullptr,&upper)==0);assert(SDL_RenderCopy(renderer,bottom_texture,nullptr,&lower)==0);assert(read()==distinct);
   clear();assert(SDL_RenderCopy(renderer,stacked,&bottom_source,&upper)==0);auto distinct_crop=read();clear();assert(SDL_RenderCopy(renderer,bottom_texture,nullptr,&upper)==0);assert(read()==distinct_crop);
   SDL_DestroyTexture(bottom_texture);SDL_DestroyTexture(stacked);BindTexture(0x0de1,image);
   live_shader=4;clear();assert(SDL_RenderCopy(renderer,texture,nullptr,&upper)==0);auto rgba_frame=read();
   U bgra=0;GenTextures(1,&bgra);BindTexture(0x0de1,bgra);TexParameteri(0x0de1,0x2801,0x2600);TexParameteri(0x0de1,0x2800,0x2600);std::vector<B> bgra_pixels=pixels;for(size_t i=0;i<bgra_pixels.size();i+=4)std::swap(bgra_pixels[i],bgra_pixels[i+2]);glTexImage2D(0x0de1,0,0x1908,256,192,0,0x1908,0x1401,bgra_pixels.data());auto bgra_texture=TestTextureFormat(bgra,256,192,SDL_PIXELFORMAT_ARGB8888);
   clear();assert(SDL_RenderCopy(renderer,bgra_texture,nullptr,&upper)==0);assert(read()==rgba_frame); // SDL's BGR native upload must be converted before colour treatment.
   TestAlpha(bgra_texture,128);clear();assert(SDL_RenderCopy(renderer,bgra_texture,nullptr,&upper)==0);auto translucent=read();int p=((959-200)*1280+200)*4;assert(std::abs(int(translucent[p])*2-int(rgba_frame[p]))<=2);
   SDL_DestroyTexture(bgra_texture);BindTexture(0x0de1,image);


 }
 command(0);assert(live_shader==0 && !shader_waiting);clear();glDrawArrays(4,0,6);assert(read()==original_pixels);
 if(core=="nnddss"){
   // Render-target/core/UI draws keep their original program and pixels.
   live_shader=4;native_blits.clear();UseProgram(0);glDrawArrays(4,0,6);assert(native_blits.size()==1);assert(native_blits.begin()->second.position==-1);GetError();UseProgram(original);live_shader=0;
 }
 SDL_DestroyTexture(texture);game_shader.destroy(rgds::overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext")());assert(GetError()==0);
 for(int i=1;i<rgds::shader_count;i++)assert(!IsProgram(TestProgram(i)));assert(IsProgram(original));
 std::printf("%s presets=20 pixels=verified\n",core.c_str());
}
'''

if __name__ == '__main__': unittest.main()
