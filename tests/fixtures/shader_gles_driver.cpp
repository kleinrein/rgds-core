// A host-only SDL adapter around a genuine EGL pbuffer and GLES2 compiler.
// It does not start an emulator or connect to a console.
#include <SDL.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
using U=unsigned;using I=int;
namespace {
void* egl=nullptr;void* gles=nullptr;void* display=nullptr;void* surface=nullptr;void* context=nullptr;
using Proc=void*(*)(const char*);Proc lookup=nullptr;
int width=1280,height=960;float scale_x=1,scale_y=1;SDL_Rect viewport{0,0,1280,960},clip{};bool clipped=false;
struct Texture {U name=0;int width=0,height=0;Uint32 format=SDL_PIXELFORMAT_RGBA32;Uint8 alpha=255;SDL_BlendMode blend=SDL_BLENDMODE_NONE;};
int swaps=0,copies=0,compiles=0;SDL_Texture* render_target=nullptr;bool queued_clear=false;
std::vector<U> programs;
void* gl(const char* name);
U compile_shader(U type){++compiles;return reinterpret_cast<U(*)(U)>(gl("glCreateShader"))(type);}
U create_program(){U id=reinterpret_cast<U(*)()>(gl("glCreateProgram"))();programs.push_back(id);return id;}
void* gl(const char* name){auto p=gles?dlsym(gles,name):nullptr;return p?p:lookup?lookup(name):nullptr;}
void setup(){
    if(context)return;
    const char* e=std::getenv("RGDS_TEST_EGL_LIBRARY");const char* g=std::getenv("RGDS_TEST_GLES_LIBRARY");
    egl=e?dlopen(e,RTLD_NOW|RTLD_LOCAL):nullptr;gles=g?dlopen(g,RTLD_NOW|RTLD_LOCAL):nullptr;
    if(!egl){std::fprintf(stderr,"EGL library unavailable: %s\n",dlerror());std::exit(77);}
#define EGL(name,ret,args) auto name=reinterpret_cast<ret(*)args>(dlsym(egl,"egl" #name));if(!name)std::exit(77)
    EGL(GetDisplay,void*,(void*));EGL(Initialize,U,(void*,I*,I*));EGL(BindAPI,U,(U));
    EGL(ChooseConfig,U,(void*,const I*,void**,I,I*));EGL(CreatePbufferSurface,void*,(void*,void*,const I*));
    EGL(CreateContext,void*,(void*,void*,void*,const I*));EGL(MakeCurrent,U,(void*,void*,void*,void*));
#undef EGL
    lookup=reinterpret_cast<Proc>(dlsym(egl,"eglGetProcAddress"));
    display=GetDisplay(nullptr);I major=0,minor=0;if(!display || !Initialize(display,&major,&minor) || !BindAPI(0x30a0))std::exit(77);
    const I config[]={0x3033,1,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3038};void* chosen=nullptr;I count=0;
    if(!ChooseConfig(display,config,&chosen,1,&count) || count!=1)std::exit(77);
    const I pbuffer[]={0x3057,width,0x3056,height,0x3038};surface=CreatePbufferSurface(display,chosen,pbuffer);
    const I attributes[]={0x3098,2,0x3038};context=CreateContext(display,chosen,nullptr,attributes);
    if(!surface || !context || !MakeCurrent(display,surface,surface,context))std::exit(77);
}
}
extern "C" SDL_GLContext SDL_GL_GetCurrentContext(){setup();return context;}
extern "C" SDL_Window* SDL_GL_GetCurrentWindow(){setup();return reinterpret_cast<SDL_Window*>(1);}
extern "C" void SDL_GL_GetDrawableSize(SDL_Window*,int* w,int* h){*w=width;*h=height;}
extern "C" void* SDL_GL_GetProcAddress(const char* name){setup();if(!std::strcmp(name,"glCreateShader"))return reinterpret_cast<void*>(&compile_shader);if(!std::strcmp(name,"glCreateProgram"))return reinterpret_cast<void*>(&create_program);return gl(name);}
extern "C" SDL_Window* SDL_RenderGetWindow(SDL_Renderer*){return reinterpret_cast<SDL_Window*>(1);}
extern "C" SDL_Texture* SDL_GetRenderTarget(SDL_Renderer*){return render_target;}
extern "C" int SDL_GetRendererInfo(SDL_Renderer*,SDL_RendererInfo* info){info->name="opengles2";return 0;}
extern "C" int SDL_QueryTexture(SDL_Texture* texture,Uint32* format,int* access,int* w,int* h){auto t=reinterpret_cast<Texture*>(texture);if(format)*format=t->format;if(access)*access=SDL_TEXTUREACCESS_STREAMING;if(w)*w=t->width;if(h)*h=t->height;return 0;}
extern "C" int SDL_GL_BindTexture(SDL_Texture* texture,float* tw,float* th){auto t=reinterpret_cast<Texture*>(texture);reinterpret_cast<void(*)(U,U)>(gl("glBindTexture"))(0x0de1,t->name);if(tw)*tw=1;if(th)*th=1;return 0;}
extern "C" void SDL_RenderGetScale(SDL_Renderer*,float* sx,float* sy){*sx=scale_x;*sy=scale_y;}
extern "C" void SDL_RenderGetViewport(SDL_Renderer*,SDL_Rect* result){*result=viewport;}
extern "C" void SDL_RenderGetClipRect(SDL_Renderer*,SDL_Rect* result){*result=clip;}
extern "C" SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer*){return clipped?SDL_TRUE:SDL_FALSE;}
extern "C" int SDL_RenderClear(SDL_Renderer*){queued_clear=true;return 0;}
extern "C" int SDL_RenderFlush(SDL_Renderer*){if(queued_clear){reinterpret_cast<void(*)(U)>(gl("glClear"))(0x4000);queued_clear=false;}return 0;}
extern "C" void SDL_RenderPresent(SDL_Renderer* renderer){SDL_RenderFlush(renderer);++swaps;}
extern "C" void SDL_GL_SwapWindow(SDL_Window*){++swaps;}
extern "C" int SDL_RenderCopy(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*){++copies;return 0;}
extern "C" int SDL_RenderCopyEx(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*,double,const SDL_Point*,SDL_RendererFlip){++copies;return 0;}
extern "C" void SDL_DestroyTexture(SDL_Texture* texture){auto t=reinterpret_cast<Texture*>(texture);reinterpret_cast<void(*)(I,const U*)>(gl("glDeleteTextures"))(1,&t->name);delete t;}
extern "C" void SDL_DestroyRenderer(SDL_Renderer*){}
extern "C" SDL_Texture* TestTexture(U name,int w,int h){return reinterpret_cast<SDL_Texture*>(new Texture{name,w,h});}
extern "C" void TestGeometry(float sx,float sy,int x,int y,int w,int h,int cx,int cy,int cw,int ch){scale_x=sx;scale_y=sy;viewport={x,y,w,h};clip={cx,cy,cw,ch};clipped=cw>=0 && ch>=0;}
extern "C" int TestCopies(){return copies;}
extern "C" int TestSwaps(){return swaps;}
// These exports are the genuine downstream calls for the bridge's GL hooks.
extern "C" void glTexImage2D(U t,I l,I i,I w,I h,I b,U f,U type,const void* p){setup();reinterpret_cast<void(*)(U,I,I,I,I,I,U,U,const void*)>(gl("glTexImage2D"))(t,l,i,w,h,b,f,type,p);}
extern "C" void glDrawArrays(U m,I f,I n){setup();reinterpret_cast<void(*)(U,I,I)>(gl("glDrawArrays"))(m,f,n);}
extern "C" void glDeleteTextures(I n,const U* t){reinterpret_cast<void(*)(I,const U*)>(gl("glDeleteTextures"))(n,t);}
extern "C" void glDeleteProgram(U p){reinterpret_cast<void(*)(U)>(gl("glDeleteProgram"))(p);}

extern "C" void glBufferData(U t,std::ptrdiff_t n,const void* p,U u){reinterpret_cast<void(*)(U,std::ptrdiff_t,const void*,U)>(gl("glBufferData"))(t,n,p,u);}
extern "C" void glBufferSubData(U t,std::ptrdiff_t o,std::ptrdiff_t n,const void* p){reinterpret_cast<void(*)(U,std::ptrdiff_t,std::ptrdiff_t,const void*)>(gl("glBufferSubData"))(t,o,n,p);}
extern "C" void glDeleteBuffers(I n,const U* b){reinterpret_cast<void(*)(I,const U*)>(gl("glDeleteBuffers"))(n,b);}

extern "C" int TestCompiles(){return compiles;}
extern "C" U TestProgram(int which){return programs.at(which);}

extern "C" void TestRenderTarget(SDL_Texture* texture){render_target=texture;}

extern "C" SDL_Texture* TestTextureFormat(U name,int w,int h,Uint32 format){return reinterpret_cast<SDL_Texture*>(new Texture{name,w,h,format});}
extern "C" int SDL_GetTextureColorMod(SDL_Texture*,Uint8* r,Uint8* g,Uint8* b){*r=*g=*b=255;return 0;}
extern "C" int SDL_GetTextureAlphaMod(SDL_Texture* texture,Uint8* a){*a=reinterpret_cast<Texture*>(texture)->alpha;return 0;}
extern "C" int SDL_GetTextureBlendMode(SDL_Texture* texture,SDL_BlendMode* b){*b=reinterpret_cast<Texture*>(texture)->blend;return 0;}
extern "C" void TestAlpha(SDL_Texture* texture,Uint8 alpha){auto t=reinterpret_cast<Texture*>(texture);t->alpha=alpha;t->blend=SDL_BLENDMODE_BLEND;}
