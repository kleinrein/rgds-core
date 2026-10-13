#pragma once
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <dlfcn.h>
#include <cstdlib>
#include <stdexcept>
#include <string>

// Load the device's vendor SDL rather than shipping a replacement display stack.
// Only APIs present in SDL 2.0.10 and later are used.
#define RGDS_SDL_FUNCTIONS(X) \
 X(InitSubSystem) X(QuitSubSystem) X(OpenAudioDevice) X(CloseAudioDevice) X(PauseAudioDevice) X(LockAudioDevice) X(UnlockAudioDevice) X(LoadWAV_RW) X(FreeWAV) \
 X(Init) X(Quit) X(SetMainReady) X(GetError) X(ClearError) X(GetVersion) X(GetCurrentVideoDriver) \
 X(GetNumVideoDisplays) X(GetDisplayName) X(GetDisplayBounds) X(CreateWindow) \
 X(DestroyWindow) X(GetWindowID) X(GetWindowSize) X(SetWindowFullscreen) X(GL_SetAttribute) \
 X(CreateRenderer) X(CreateSoftwareRenderer) X(GetRendererInfo) X(DestroyRenderer) X(RenderSetLogicalSize) X(SetRenderDrawColor) X(SetRenderDrawBlendMode) \
 X(RenderClear) X(RenderFillRect) X(RenderDrawRect) X(RenderDrawLine) X(RenderPresent) X(RenderFlush) \
 X(RenderReadPixels) X(GetRendererOutputSize) X(CreateRGBSurfaceWithFormat) X(FreeSurface) X(RWFromFile) \
 X(SaveBMP_RW) X(WaitEventTimeout) X(PollEvent) X(Delay) X(GetTicks) X(NumJoysticks) \
 X(JoystickOpen) X(JoystickClose) X(JoystickName) X(SetHint) \
 X(CreateTexture) X(SetRenderTarget) X(RenderCopyEx) X(CreateTextureFromSurface) X(DestroyTexture) X(SetTextureBlendMode) X(SetTextureColorMod) X(RenderCopy) X(PushEvent) X(GetKeyFromName) \
 X(LoadBMP_RW) X(SetTextureAlphaMod) X(StartTextInput) X(StopTextInput) X(RenderSetClipRect)

struct Sdl {
    void* handle = nullptr;
#define RGDS_DECLARE(name) decltype(&SDL_##name) name = nullptr;
    RGDS_SDL_FUNCTIONS(RGDS_DECLARE)
#undef RGDS_DECLARE
    Sdl() {
        const char* custom = std::getenv("RGDS_SDL_LIBRARY");
        const char* candidates[] = {custom, "libSDL2-2.0.so.0", "libSDL2.so",
            "/opt/homebrew/lib/libSDL2.dylib", "/usr/local/lib/libSDL2.dylib", "libSDL2.dylib"};
        for (auto path : candidates) {
            if (path && (handle = dlopen(path, RTLD_NOW | RTLD_LOCAL))) break;
        }
        if (!handle) throw std::runtime_error("Cannot load SDL2. Set RGDS_SDL_LIBRARY to its full path.");
#define RGDS_LOAD(name) \
        name = reinterpret_cast<decltype(name)>(dlsym(handle, "SDL_" #name)); \
        if (!name) { dlclose(handle); handle = nullptr; \
            throw std::runtime_error("Vendor SDL2 lacks SDL_" #name ". Capture the inventory first."); }
        RGDS_SDL_FUNCTIONS(RGDS_LOAD)
#undef RGDS_LOAD
    }
    ~Sdl() { if (handle) dlclose(handle); }
    Sdl(const Sdl&) = delete;
    Sdl& operator=(const Sdl&) = delete;
};
