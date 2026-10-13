#pragma once
#include "platform/overlay_frame.hpp"
#include "platform/volume_hud.hpp"
#include "platform/frame_stats.hpp"
#include <SDL.h>
#include <dlfcn.h>
#include <cstdlib>
#include <map>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <chrono>
#include <array>

namespace rgds {
inline void* overlay_library(){
    static void* library=[](){const char* path=std::getenv("RGDS_SDL_LIBRARY");return dlopen(path?path:"libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL);}();return library;
}
template<class T>T overlay_sdl(const char* name){
    RGDS_PROFILE(sdl_lookups,1);auto* library=overlay_library();return library?reinterpret_cast<T>(dlsym(library,name)):nullptr;
}
class EmulatorOverlay {
    std::mutex mutex;
    OverlayFrame frame;
    struct Textures{SDL_Texture* texture[2]{};bool straight[2]{};uint32_t version[2]{};std::vector<unsigned char> conversion;};
    std::map<SDL_Renderer*,Textures> textures;
    struct Gpu {unsigned int program=0,texture[2]{},buffer=0;uint32_t version[2]{};int image=-1;bool vertices=false;std::array<void*,39> functions{};};
    std::map<void*,Gpu> contexts;
    std::string capture_token;
    bool captured[2]{},capture_active=false;
    unsigned long long capture_poll_at=0;
    std::vector<unsigned char> composite;
    uint32_t composite_version=0,menu_version=0;
    bool menu_visible=false,hud_visible=false,fps_visible=false,ff_visible=false,ff_active=false;
    double fps_value=-1,drawn_fps=-1;
    bool fps_game=false;
    int volume_level=-1,drawn_volume=-1;
    unsigned long long volume_until=0;
    static unsigned long long clock(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    bool ready(bool visible){
        const char* path=std::getenv("RGDS_OVERLAY_FILE");
        bool menu=visible && path && frame.open(path,false) && frame.refresh();
        bool hud=volume_level>=0 && clock()<volume_until;
        bool fps=fps_value>=0 && !menu;
        bool ff=ff_active && !menu;
        if(!menu && !hud && !fps && !ff){menu_visible=hud_visible=fps_visible=ff_visible=false;return false;}
        if(menu!=menu_visible || hud!=hud_visible || fps!=fps_visible || ff!=ff_visible || (fps && drawn_fps!=fps_value) ||
           (menu && menu_version!=frame.version()) || (hud && drawn_volume!=volume_level)){
            // A plain menu already has stable owned pixels. Only a HUD needs
            // composition, and a HUD alone has no bottom panel to copy/draw.
            if(hud || fps || ff){
                composite.resize(overlay_panel_bytes*(menu?2:1));RGDS_PROFILE(composites,1);
                if(menu)std::memcpy(composite.data(),frame.panel(0),composite.size());
                else std::fill(composite.begin(),composite.end(),0);
                if(hud)draw_volume_hud(composite.data(),volume_level);
                if(ff)draw_fast_forward_hud(composite.data());
                if(fps)draw_fps_hud(composite.data(),fps_value,fps_game);
            }
            menu_visible=menu;hud_visible=hud;fps_visible=fps;ff_visible=ff;drawn_fps=fps_value;menu_version=frame.version();drawn_volume=volume_level;
            ++composite_version;
        }
        return true;
    }
    const unsigned char* pixels(int index)const{return (hud_visible || fps_visible || ff_visible)?composite.data()+index*overlay_panel_bytes:frame.panel(index);}
    bool has_panel(int index)const{return menu_visible || ((hud_visible || fps_visible || ff_visible) && index==0);}
    void acknowledge(){if(menu_visible)frame.acknowledge();}
    int panel(SDL_Window* window){
        static auto display=overlay_sdl<decltype(&SDL_GetWindowDisplayIndex)>("SDL_GetWindowDisplayIndex");
        static auto title=overlay_sdl<decltype(&SDL_GetWindowTitle)>("SDL_GetWindowTitle");
        const char* name=window && title?title(window):nullptr;
        return (window && display && display(window)==1) || (name && (std::strstr(name,"Bottom") || std::strstr(name,"bottom")))?1:0;
    }
public:
    void fps(double value,bool game=false){std::lock_guard<std::mutex> guard(mutex);if(fps_game!=game)drawn_fps=-1;fps_value=value;fps_game=game;}
    void fast_forward(bool active){std::lock_guard<std::mutex> guard(mutex);ff_active=active;}
    void volume(int level){
        std::lock_guard<std::mutex> guard(mutex);
        volume_level=std::clamp(level,0,100);volume_until=clock()+1800;
    }
    // Ordinary captures omit UI for save thumbnails. A diagnostic hud- token
    // captures after composition, allowing on-device verification of owned HUDs.
    void capture(SDL_Renderer* renderer,bool after_hud=false){
        std::lock_guard<std::mutex> guard(mutex);
        if(after_hud && !capture_active)return;
        const char* control=std::getenv("RGDS_CONTROL_FILE");if(!control)return;
        static const std::string request=std::string(control)+".capture";
        auto time=clock();
        if(!after_hud && (!capture_poll_at || time-capture_poll_at>=100)){
            capture_poll_at=time;RGDS_PROFILE(capture_checks,1);std::ifstream input(request);std::string token;
            capture_active=bool(input>>token);
            if(capture_active && token!=capture_token){capture_token=token;captured[0]=captured[1]=false;}
        }
        if(!capture_active || (capture_token.rfind("hud-",0)==0)!=after_hud)return;
        static auto size=overlay_sdl<decltype(&SDL_GetRendererOutputSize)>("SDL_GetRendererOutputSize");
        static auto create=overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat");
        static auto read=overlay_sdl<decltype(&SDL_RenderReadPixels)>("SDL_RenderReadPixels");static auto file=overlay_sdl<decltype(&SDL_RWFromFile)>("SDL_RWFromFile");static auto save=overlay_sdl<decltype(&SDL_SaveBMP_RW)>("SDL_SaveBMP_RW");static auto release=overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface");
        if(!size || !create || !read || !file || !save || !release)return;
        int w=0,h=0;if(size(renderer,&w,&h)!=0 || w<=0 || h<=0 || w*h>4096*4096)return;
        using Window=SDL_Window*(*)(SDL_Renderer*);static auto window=overlay_sdl<Window>("SDL_RenderGetWindow");bool combined=w>=h*2 || h>=w*3/2;int index=combined?0:panel(window?window(renderer):nullptr);if(captured[index])return;
        auto* bitmap=create(0,w,h,32,SDL_PIXELFORMAT_RGBA32);if(!bitmap)return;
        std::string output=std::string(control)+(combined?".capture-combined.bmp":index?".capture-bottom.bmp":".capture-top.bmp");
        if(read(renderer,nullptr,bitmap->format->format,bitmap->pixels,bitmap->pitch)==0){auto* stream=file((output+".pending").c_str(),"wb");if(stream && save(bitmap,stream,1)==0){rename((output+".pending").c_str(),output.c_str());captured[index]=true;if(combined)captured[1]=true;}}
        release(bitmap);if(captured[0] && captured[1]){unlink(request.c_str());capture_active=false;}
    }
    void capture_gl(SDL_Window* window){
        std::lock_guard<std::mutex> guard(mutex);
        const char* control=std::getenv("RGDS_CONTROL_FILE");if(!control)return;
        auto time=clock();if(!capture_poll_at || time-capture_poll_at>=100){
            capture_poll_at=time;std::ifstream input(std::string(control)+".capture");std::string token;capture_active=bool(input>>token);
            if(capture_active && token!=capture_token){capture_token=token;captured[0]=captured[1]=false;}
        }
        if(!capture_active)return;
        static auto proc=overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
        static auto size=overlay_sdl<decltype(&SDL_GL_GetDrawableSize)>("SDL_GL_GetDrawableSize");
        static auto create=overlay_sdl<decltype(&SDL_CreateRGBSurfaceWithFormat)>("SDL_CreateRGBSurfaceWithFormat");
        static auto file=overlay_sdl<decltype(&SDL_RWFromFile)>("SDL_RWFromFile");
        static auto save=overlay_sdl<decltype(&SDL_SaveBMP_RW)>("SDL_SaveBMP_RW");
        static auto release=overlay_sdl<decltype(&SDL_FreeSurface)>("SDL_FreeSurface");
        if(!proc || !size || !create || !file || !save || !release)return;
        using Read=void(*)(int,int,int,int,unsigned,unsigned,void*);using Get=void(*)(unsigned,int*);using Set=void(*)(unsigned,int);
        auto read=reinterpret_cast<Read>(proc("glReadPixels"));auto get=reinterpret_cast<Get>(proc("glGetIntegerv"));auto set=reinterpret_cast<Set>(proc("glPixelStorei"));
        if(!read || !get || !set)return;
        int w=0,h=0;size(window,&w,&h);if(w<=0 || h<=0 || w>2560 || h>1920)return;
        bool combined=w>=h*2 || h>=w*3/2;int index=combined?0:panel(window);if(captured[index])return;
        auto* bitmap=create(0,w,h,32,SDL_PIXELFORMAT_RGBA32);if(!bitmap)return;
        int alignment=4;get(0x0d05,&alignment);set(0x0d05,4);read(0,0,w,h,0x1908,0x1401,bitmap->pixels);set(0x0d05,alignment);
        auto* bytes=static_cast<unsigned char*>(bitmap->pixels);
        for(int y=0;y<h/2;y++)for(int x=0;x<bitmap->pitch;x++)std::swap(bytes[y*bitmap->pitch+x],bytes[(h-1-y)*bitmap->pitch+x]);
        std::string output=std::string(control)+(combined?".capture-combined.bmp":index?".capture-bottom.bmp":".capture-top.bmp");
        auto* stream=file((output+".pending").c_str(),"wb");if(stream && save(bitmap,stream,1)==0){rename((output+".pending").c_str(),output.c_str());captured[index]=true;if(combined)captured[1]=true;}
        release(bitmap);if(captured[0] && captured[1]){unlink((std::string(control)+".capture").c_str());capture_active=false;}
    }
    void renderer(SDL_Renderer* renderer,bool visible){
        std::lock_guard<std::mutex> guard(mutex);
        if(!ready(visible))return;
#define FN(name) static auto name=overlay_sdl<decltype(&SDL_##name)>("SDL_" #name)
        FN(GetRendererOutputSize);FN(RenderGetLogicalSize);FN(RenderGetScale);FN(RenderGetViewport);FN(RenderGetClipRect);FN(RenderIsClipEnabled);
        FN(RenderSetLogicalSize);FN(RenderSetScale);FN(RenderSetViewport);FN(RenderSetClipRect);
        FN(GetRenderTarget);FN(SetRenderTarget);FN(RenderGetIntegerScale);FN(RenderSetIntegerScale);
        FN(CreateTexture);FN(UpdateTexture);FN(SetTextureBlendMode);FN(ComposeCustomBlendMode);FN(RenderCopy);
#undef FN
        if(!GetRendererOutputSize || !RenderGetLogicalSize || !RenderGetScale || !RenderGetViewport || !RenderGetClipRect || !RenderIsClipEnabled || !RenderSetLogicalSize || !RenderSetScale || !RenderSetViewport || !RenderSetClipRect || !GetRenderTarget || !SetRenderTarget || !CreateTexture || !UpdateTexture || !SetTextureBlendMode || !ComposeCustomBlendMode || !RenderCopy)return;
        int w=0,h=0,lw=0,lh=0;float sx=1,sy=1;SDL_Rect viewport{},clip{};
        if(GetRendererOutputSize(renderer,&w,&h)!=0 || w<=0 || h<=0)return;
        using Window=SDL_Window*(*)(SDL_Renderer*);static auto window=overlay_sdl<Window>("SDL_RenderGetWindow");
        auto* output_window=window?window(renderer):nullptr;auto* target=GetRenderTarget(renderer);
        int single=!target && w<h*2 && h<w*3/2?panel(output_window):-1;
        auto required=[&](int index){return has_panel(index) && (single<0 || single==index);};
        if(!required(0) && !required(1))return;
        RenderGetLogicalSize(renderer,&lw,&lh);RenderGetScale(renderer,&sx,&sy);RenderGetViewport(renderer,&viewport);RenderGetClipRect(renderer,&clip);bool clipped=RenderIsClipEnabled(renderer);
        bool integer=RenderGetIntegerScale && RenderGetIntegerScale(renderer);
        auto& cached=textures[renderer];
        static auto blend=ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD,SDL_BLENDFACTOR_ONE,SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,SDL_BLENDOPERATION_ADD);
        for(int i=0;i<2;i++)if(required(i)){
            if(!cached.texture[i]){cached.texture[i]=CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,640,480);if(cached.texture[i] && SetTextureBlendMode(cached.texture[i],blend)!=0){cached.straight[i]=true;SetTextureBlendMode(cached.texture[i],SDL_BLENDMODE_BLEND);}}
            if(!cached.texture[i])return;
            if(cached.version[i]!=composite_version){
                const unsigned char* data=pixels(i);
                if(cached.straight[i]){
                    auto& straight=cached.conversion;straight.assign(data,data+overlay_panel_bytes);
                    for(size_t n=0;n<straight.size();n+=4){
                        unsigned alpha=straight[n+3];if(alpha==255)continue;
                        if(!alpha){straight[n]=straight[n+1]=straight[n+2]=0;continue;}
                        for(int c=0;c<3;c++)straight[n+c]=std::min(255u,unsigned(straight[n+c])*255/alpha);
                    }data=straight.data();
                }
                RGDS_PROFILE(texture_uploads,1);if(UpdateTexture(cached.texture[i],nullptr,data,640*4)!=0)return;cached.version[i]=composite_version;
            }
        }
        if(SetRenderTarget(renderer,nullptr)!=0)return;
        if(GetRendererOutputSize(renderer,&w,&h)!=0 || w<=0 || h<=0){SetRenderTarget(renderer,target);return;}
        if(RenderSetIntegerScale)RenderSetIntegerScale(renderer,SDL_FALSE);
        RenderSetLogicalSize(renderer,0,0);RenderSetScale(renderer,1,1);RenderSetViewport(renderer,nullptr);RenderSetClipRect(renderer,nullptr);
        bool ok=true;
        if(w>=h*2){for(int i=0;i<2;i++)if(has_panel(i)){SDL_Rect box{i*w/2,0,w/2,h};RGDS_PROFILE(draws,1);ok=RenderCopy(renderer,cached.texture[i],nullptr,&box)==0 && ok;}}
        else if(h>=w*3/2){for(int i=0;i<2;i++)if(has_panel(i)){SDL_Rect box{0,i*h/2,w,h/2};RGDS_PROFILE(draws,1);ok=RenderCopy(renderer,cached.texture[i],nullptr,&box)==0 && ok;}}
        else {
            int index=panel(output_window);
            if(has_panel(index)){SDL_Rect box{0,0,w,h};RGDS_PROFILE(draws,1);ok=RenderCopy(renderer,cached.texture[index],nullptr,&box)==0;}
        }
        SetRenderTarget(renderer,target);if(RenderSetIntegerScale)RenderSetIntegerScale(renderer,integer?SDL_TRUE:SDL_FALSE);
        RenderSetLogicalSize(renderer,lw,lh);RenderSetScale(renderer,sx,sy);RenderSetViewport(renderer,&viewport);RenderSetClipRect(renderer,clipped?&clip:nullptr);
        if(ok)acknowledge();
    }
    void destroy(SDL_Renderer* renderer){
        std::lock_guard<std::mutex> guard(mutex);
        auto found=textures.find(renderer);if(found==textures.end())return;
        static auto release=overlay_sdl<decltype(&SDL_DestroyTexture)>("SDL_DestroyTexture");
        if(release)for(auto texture:found->second.texture)if(texture)release(texture);
        textures.erase(found);
    }
    void destroy_gl(SDL_GLContext context){std::lock_guard<std::mutex> guard(mutex);contexts.erase(context);}
    void gl(SDL_Window* window,bool visible){
        std::lock_guard<std::mutex> guard(mutex);
        if(!ready(visible))return;
        using U=unsigned int;using I=int;using B=unsigned char;using P=std::ptrdiff_t;
        static auto proc=overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
        static auto context=overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext");
        static auto drawable=overlay_sdl<decltype(&SDL_GL_GetDrawableSize)>("SDL_GL_GetDrawableSize");
        if(!proc || !context || !drawable)return;
        int w=0,h=0;drawable(window,&w,&h);if(w<=0 || h<=0)return;
        bool combined=w>=h*2 || h>=w*3/2;int single=combined?-1:panel(window);
        auto required=[&](int index){return has_panel(index) && (single<0 || single==index);};
        if(!required(0) && !required(1))return;
        void* current=context();if(!current)return;auto& gpu=contexts[current];size_t function=0;
#define GL(name,ret,args) auto& entry_##name=gpu.functions[function++];if(!entry_##name){RGDS_PROFILE(gl_lookups,1);entry_##name=proc("gl" #name);}auto name=reinterpret_cast<ret(*)args>(entry_##name);if(!name)return
        GL(GetIntegerv,void,(U,I*));GL(GetBooleanv,void,(U,B*));GL(IsEnabled,B,(U));GL(Enable,void,(U));GL(Disable,void,(U));
        GL(CreateShader,U,(U));GL(ShaderSource,void,(U,I,const char* const*,const I*));GL(CompileShader,void,(U));GL(GetShaderiv,void,(U,U,I*));GL(DeleteShader,void,(U));
        GL(CreateProgram,U,());GL(AttachShader,void,(U,U));GL(BindAttribLocation,void,(U,U,const char*));GL(LinkProgram,void,(U));GL(GetProgramiv,void,(U,U,I*));GL(UseProgram,void,(U));GL(GetUniformLocation,I,(U,const char*));GL(Uniform1i,void,(I,I));
        GL(GenTextures,void,(I,U*));GL(BindTexture,void,(U,U));GL(TexParameteri,void,(U,U,I));GL(TexImage2D,void,(U,I,I,I,I,I,U,U,const void*));GL(ActiveTexture,void,(U));GL(PixelStorei,void,(U,I));
        GL(TexSubImage2D,void,(U,I,I,I,I,I,U,U,const void*));
        GL(GenBuffers,void,(I,U*));GL(BindBuffer,void,(U,U));GL(BufferData,void,(U,P,const void*,U));
        GL(GetVertexAttribiv,void,(U,U,I*));GL(GetVertexAttribPointerv,void,(U,U,void**));GL(VertexAttribPointer,void,(U,I,U,B,I,const void*));GL(EnableVertexAttribArray,void,(U));GL(DisableVertexAttribArray,void,(U));
        GL(BlendFuncSeparate,void,(U,U,U,U));GL(BlendEquationSeparate,void,(U,U));GL(ColorMask,void,(B,B,B,B));GL(Viewport,void,(I,I,I,I));GL(BindFramebuffer,void,(U,U));GL(DrawArrays,void,(U,I,I));
#undef GL
        if(!gpu.program){
            const char* vertex="attribute vec2 position;attribute vec2 uv;varying vec2 texcoord;void main(){texcoord=uv;gl_Position=vec4(position,0.0,1.0);}";
            const char* fragment="precision mediump float;varying vec2 texcoord;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,texcoord);}";
            auto compile=[&](U type,const char* source){U shader=CreateShader(type);ShaderSource(shader,1,&source,nullptr);CompileShader(shader);I ok=0;GetShaderiv(shader,0x8b81,&ok);if(!ok){DeleteShader(shader);return U(0);}return shader;};
            U vs=compile(0x8b31,vertex),fs=compile(0x8b30,fragment);if(!vs || !fs){if(vs)DeleteShader(vs);if(fs)DeleteShader(fs);return;}
            gpu.program=CreateProgram();AttachShader(gpu.program,vs);AttachShader(gpu.program,fs);BindAttribLocation(gpu.program,0,"position");BindAttribLocation(gpu.program,1,"uv");LinkProgram(gpu.program);DeleteShader(vs);DeleteShader(fs);
            I linked=0;GetProgramiv(gpu.program,0x8b82,&linked);if(!linked){gpu.program=0;return;}
            GenTextures(2,gpu.texture);GenBuffers(1,&gpu.buffer);
            gpu.image=GetUniformLocation(gpu.program,"image");
        }
        I old_program=0,old_active=0,old_texture=0,old_buffer=0,old_framebuffer=0,old_unpack=0,viewport[4]{},src_rgb=0,dst_rgb=0,src_alpha=0,dst_alpha=0,eq_rgb=0,eq_alpha=0;
        B mask[4]{};GetIntegerv(0x8b8d,&old_program);GetIntegerv(0x84e0,&old_active);ActiveTexture(0x84c0);GetIntegerv(0x8069,&old_texture);GetIntegerv(0x8894,&old_buffer);GetIntegerv(0x8ca6,&old_framebuffer);GetIntegerv(0x0cf5,&old_unpack);GetIntegerv(0x0ba2,viewport);GetBooleanv(0x0c23,mask);
        GetIntegerv(0x80c9,&src_rgb);GetIntegerv(0x80c8,&dst_rgb);GetIntegerv(0x80cb,&src_alpha);GetIntegerv(0x80ca,&dst_alpha);GetIntegerv(0x8009,&eq_rgb);GetIntegerv(0x883d,&eq_alpha);
        const U modes[5]={0x0be2,0x0c11,0x0b71,0x0b90,0x0b44};B enabled[5]{};for(int i=0;i<5;i++)enabled[i]=IsEnabled(modes[i]);
        struct Attribute{I size,type,normalized,stride,buffer,enabled;void* pointer;};Attribute attributes[2]{};
        for(U i=0;i<2;i++){auto& a=attributes[i];GetVertexAttribiv(i,0x8623,&a.size);GetVertexAttribiv(i,0x8625,&a.type);GetVertexAttribiv(i,0x886a,&a.normalized);GetVertexAttribiv(i,0x8624,&a.stride);GetVertexAttribiv(i,0x889f,&a.buffer);GetVertexAttribiv(i,0x8622,&a.enabled);GetVertexAttribPointerv(i,0x8645,&a.pointer);}
        BindFramebuffer(0x8d40,0);UseProgram(gpu.program);Uniform1i(gpu.image,0);Enable(0x0be2);for(int i=1;i<5;i++)Disable(modes[i]);ColorMask(1,1,1,1);BlendEquationSeparate(0x8006,0x8006);BlendFuncSeparate(1,0x0303,1,0x0303);PixelStorei(0x0cf5,1);
        const float vertices[]={-1,-1,0,1,1,-1,1,1,-1,1,0,0,1,1,1,0};BindBuffer(0x8892,gpu.buffer);if(!gpu.vertices){BufferData(0x8892,sizeof(vertices),vertices,0x88e4);gpu.vertices=true;}
        VertexAttribPointer(0,2,0x1406,0,4*sizeof(float),nullptr);VertexAttribPointer(1,2,0x1406,0,4*sizeof(float),reinterpret_cast<void*>(2*sizeof(float)));EnableVertexAttribArray(0);EnableVertexAttribArray(1);
        for(int i=0;i<2;i++)if(required(i) && gpu.version[i]!=composite_version){
            BindTexture(0x0de1,gpu.texture[i]);
            if(!gpu.version[i]){TexParameteri(0x0de1,0x2801,0x2601);TexParameteri(0x0de1,0x2800,0x2601);TexParameteri(0x0de1,0x2802,0x812f);TexParameteri(0x0de1,0x2803,0x812f);TexImage2D(0x0de1,0,0x1908,640,480,0,0x1908,0x1401,pixels(i));}
            else TexSubImage2D(0x0de1,0,0,0,640,480,0x1908,0x1401,pixels(i));
            RGDS_PROFILE(texture_uploads,1);gpu.version[i]=composite_version;
        }
        if(w>=h*2){for(int i=0;i<2;i++)if(has_panel(i)){Viewport(i*w/2,0,w/2,h);BindTexture(0x0de1,gpu.texture[i]);DrawArrays(5,0,4);RGDS_PROFILE(draws,1);}}
        else if(h>=w*3/2){for(int i=0;i<2;i++)if(has_panel(i)){Viewport(0,(1-i)*h/2,w,h/2);BindTexture(0x0de1,gpu.texture[i]);DrawArrays(5,0,4);RGDS_PROFILE(draws,1);}}
        else{int index=panel(window);if(has_panel(index)){Viewport(0,0,w,h);BindTexture(0x0de1,gpu.texture[index]);DrawArrays(5,0,4);RGDS_PROFILE(draws,1);}}
        for(U i=0;i<2;i++){auto& a=attributes[i];BindBuffer(0x8892,a.buffer);VertexAttribPointer(i,a.size,a.type,a.normalized,a.stride,a.pointer);if(a.enabled)EnableVertexAttribArray(i);else DisableVertexAttribArray(i);}
        BindBuffer(0x8892,old_buffer);BindTexture(0x0de1,old_texture);ActiveTexture(old_active);PixelStorei(0x0cf5,old_unpack);UseProgram(old_program);BindFramebuffer(0x8d40,old_framebuffer);Viewport(viewport[0],viewport[1],viewport[2],viewport[3]);ColorMask(mask[0],mask[1],mask[2],mask[3]);BlendFuncSeparate(src_rgb,dst_rgb,src_alpha,dst_alpha);BlendEquationSeparate(eq_rgb,eq_alpha);for(int i=0;i<5;i++){if(enabled[i])Enable(modes[i]);else Disable(modes[i]);}
        acknowledge();
    }
};
}
