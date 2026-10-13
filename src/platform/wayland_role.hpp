#pragma once
#include <SDL.h>
// Cross builds use the host SDL headers. Enable the stable Wayland WM-info
// member, whose pointer-only layout does not require Wayland build libraries.
#ifdef __linux__
#ifndef SDL_VIDEO_DRIVER_WAYLAND
#define SDL_VIDEO_DRIVER_WAYLAND 1
#endif
#include <SDL_syswm.h>
#endif
#include <dlfcn.h>

namespace rgds {
inline bool wayland_role(SDL_Window* window, const char* role, void* sdl) {
#ifdef __linux__
    auto info_fn=reinterpret_cast<decltype(&SDL_GetWindowWMInfo)>(dlsym(sdl,"SDL_GetWindowWMInfo"));
    auto version_fn=reinterpret_cast<decltype(&SDL_GetVersion)>(dlsym(sdl,"SDL_GetVersion"));
    if(!info_fn || !version_fn)return false;
    SDL_SysWMinfo info{};version_fn(&info.version);
    if(SDL_VERSIONNUM(info.version.major,info.version.minor,info.version.patch)<SDL_VERSIONNUM(2,0,17))return false;
    if(!info_fn(window,&info) || info.subsystem!=SDL_SYSWM_WAYLAND || !info.info.wl.xdg_toplevel)return false;
    void* client=dlopen("libwayland-client.so.0",RTLD_NOW|RTLD_LOCAL);
    if(!client)return false;
    // xdg_toplevel.set_app_id is request 3 in the stable xdg-shell protocol.
    using Marshal=void(*)(void*,unsigned int,...);
    auto marshal=reinterpret_cast<Marshal>(dlsym(client,"wl_proxy_marshal"));
    if(marshal)marshal(info.info.wl.xdg_toplevel,3,role);
    dlclose(client);return marshal!=nullptr;
#else
    (void)window;(void)role;(void)sdl;return false;
#endif
}
}
