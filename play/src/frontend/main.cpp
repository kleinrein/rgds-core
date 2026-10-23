// SPDX-License-Identifier: GPL-3.0-or-later
// rgds-play: the game on one panel, its companion on the other.
//
//   rgds-play --rom GAME --data DIR [--profiles DIR]... [--fonts DIR]
//             [--save-source DIR]... [--fit sharp|integer|smooth] [--second companion|off|swap]
//             [--frame NAME] [--frames DIR]
//
// --fit sharp (the default) fills the panel's width: the game is scaled up by
// a whole number on the GPU without smoothing, then down to the panel with
// it, so pixels stay crisp and the same size. integer keeps whole multiples
// (smaller, pixel exact); smooth fills with plain smoothing.
// --second: the companion on the second screen (default), nothing there
// (off), or the companion on top and the game below (swap).
// --frame: something drawn around the game (ui/frame.hpp), which then places
// it: a built-in style (indigo, glacier, black, platinum, bar, sp-onyx,
// sp-cobalt, sp-platinum) or the name of a picture frame, NAME.png in
// --frames (userdata/frames/<system>). off, or a picture that cannot be read,
// leaves the fit to choose.
//
// Under RG DS Core (RGDS_CORE set, the bridge preloaded) the shared in-game
// menu owns Home and the pad stays shared: F5 saves the machine to
// RGDS_STATE_SLOT, F7 loads it, Tab toggles fast-forward, and the bridge
// changes the view live through rgds_play_set_graphics. Run on its own (a
// desktop, tools/play_device.py) it grabs the RG DS pad, and Home, or
// Select+Start held for a second, saves and quits.
//
// For tests (tools/play_drive.py replays a race through the real player):
// RGDS_PLAY_INPUT names a file of "FRAME BUTTONS" lines (A+LEFT, - for none)
// that replaces the pad from that frame on, RGDS_PLAY_SHOTS a folder that gets
// FRAME.bmp of both panels as presented every RGDS_PLAY_SHOT_EVERY frames
// (600), and RGDS_PLAY_QUIT_AT a frame to save and quit at.
#include "app/session.hpp"
#include "frontend/evdev_pad.hpp"
#include "ui/frame.hpp"
#include "ui/screens.hpp"

#include <SDL2/SDL.h>
#ifdef __linux__
// The xdg_toplevel pointer in SDL's window info needs no Wayland headers.
#ifndef SDL_VIDEO_DRIVER_WAYLAND
#define SDL_VIDEO_DRIVER_WAYLAND 1
#endif
#include <SDL2/SDL_syswm.h>
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace play;

namespace {

// The session runner stops engines with SIGTERM: leave the loop and write the
// save like any other exit.
std::atomic<bool> stop_requested{false};
void request_stop(int) { stop_requested = true; }

enum FitMode { FitSharp = 0, FitInteger = 1, FitSmooth = 2 };
enum SecondMode { SecondCompanion = 0, SecondOff = 1, SecondSwap = 2 };

// Asked for by the bridge from its event hook (the main thread, between
// frames); taken up at the top of the next loop.
std::atomic<int> wanted_fit{-1}, wanted_second{-1};
std::mutex wanted_frame_lock;
std::string wanted_frame; // empty: no change asked for

struct Panel {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* game = nullptr;
    SDL_Texture* sharp = nullptr; // the game at a whole multiple, for sharp fill
    SDL_Texture* companion = nullptr;
    SDL_Texture* frame = nullptr; // the frame as drawn for frame_key (its name and the game's size)
    std::string frame_key;
    bool frame_over = false; // a picture frame: laid over the game, not under it
    SDL_Rect area{}; // where this panel sits in its window
    SDL_GLContext gl = nullptr; // the renderer's own GL context (none for a software renderer)
    bool owns_window = false;
};

uint32_t keyboard_buttons(const Uint8* k) {
    uint32_t b = 0;
    if (k[SDL_SCANCODE_X]) b |= A;
    if (k[SDL_SCANCODE_Z]) b |= B;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= Select;
    if (k[SDL_SCANCODE_RETURN]) b |= Start;
    if (k[SDL_SCANCODE_RIGHT]) b |= Right;
    if (k[SDL_SCANCODE_LEFT]) b |= Left;
    if (k[SDL_SCANCODE_UP]) b |= Up;
    if (k[SDL_SCANCODE_DOWN]) b |= Down;
    if (k[SDL_SCANCODE_S]) b |= R;
    if (k[SDL_SCANCODE_A]) b |= L;
    return b;
}

uint32_t controller_buttons(SDL_GameController* c) {
    if (!c) return 0;
    // By name: the runtime's mapping for the RG DS pad names the button
    // labelled A "a", as melonDS and DSCore read it.
    static const std::pair<SDL_GameControllerButton, uint32_t> map[] = {
        {SDL_CONTROLLER_BUTTON_A, A},
        {SDL_CONTROLLER_BUTTON_B, B},
        {SDL_CONTROLLER_BUTTON_BACK, Select},
        {SDL_CONTROLLER_BUTTON_START, Start},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Right},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, Left},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, Up},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, Down},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L},
        {SDL_CONTROLLER_BUTTON_X, X},
        {SDL_CONTROLLER_BUTTON_Y, Y},
    };
    uint32_t b = 0;
    for (const auto& [button, bit] : map)
        if (SDL_GameControllerGetButton(c, button)) b |= bit;
    return b;
}

bool open_panel(Panel& p, int display, int w, int h, bool fullscreen, const char* title) {
    Uint32 flags = SDL_WINDOW_SHOWN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE);
    p.window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                w, h, flags);
    if (!p.window) return false;
    p.renderer = SDL_CreateRenderer(p.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!p.renderer) p.renderer = SDL_CreateRenderer(p.window, -1, 0);
    p.owns_window = true;
    SDL_RendererInfo info{};
    if (p.renderer && SDL_GetRendererInfo(p.renderer, &info) == 0 && info.name && std::strstr(info.name, "opengl"))
        p.gl = SDL_GL_GetCurrentContext(); // a new GL renderer leaves its context current
    return p.renderer != nullptr;
}

// Two windows, one GL context each, drawn from one thread: on the RG DS (Mali,
// Wayland) SDL's idea of the current context drifts from EGL's, and a panel's
// texture uploads and draws land in the other panel's context (the bottom
// panel kept its first picture for a whole race). Before touching a panel its
// context is bound for real: released, then made current.
void use_panel(const Panel& p) {
    if (!p.gl) return;
    SDL_GL_MakeCurrent(p.window, nullptr);
    SDL_GL_MakeCurrent(p.window, p.gl);
}

// Under RG DS Core, Weston puts a window on a panel by its app id, the roles
// the launcher's own windows use (org.rgds.top, org.rgds.bottom). Without one
// both windows can land on the same panel.
bool set_panel_role(SDL_Window* window, const char* role) {
#ifdef __linux__
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND || !info.info.wl.xdg_toplevel)
        return false;
    void* client = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!client) return false;
    using Marshal = void (*)(void*, unsigned int, ...);
    auto marshal = reinterpret_cast<Marshal>(dlsym(client, "wl_proxy_marshal"));
    if (marshal) marshal(info.info.wl.xdg_toplevel, 3, role); // xdg_toplevel.set_app_id
    dlclose(client);
    return marshal != nullptr;
#else
    (void)window;
    (void)role;
    return false;
#endif
}

SDL_Rect fit_rect(const SDL_Rect& area, int w, int h, bool whole) {
    double s = std::min(double(area.w) / w, double(area.h) / h);
    if (whole && s >= 1) s = std::floor(s);
    int dw = int(w * s), dh = int(h * s);
    return {area.x + (area.w - dw) / 2, area.y + (area.h - dh) / 2, dw, dh};
}

// Game and companion textures live on both panels (a texture belongs to one
// renderer), so swapping the screens is a choice of what each draws.
void make_textures(Panel& p, int gw, int gh, int cw, int ch) {
    p.game = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gw, gh);
    SDL_SetTextureScaleMode(p.game, SDL_ScaleModeNearest);
    if (SDL_RenderTargetSupported(p.renderer)) {
        SDL_Rect dest = fit_rect(p.area, gw, gh, false);
        int k = std::max(1, (dest.w + gw - 1) / gw); // 640 wide: 3x
        p.sharp = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, gw * k, gh * k);
        if (p.sharp) SDL_SetTextureScaleMode(p.sharp, SDL_ScaleModeLinear);
    }
    p.companion = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, cw, ch);
}

// A screen filter chosen in RG DS Core's menu: the bridge's pass replaces the
// plain draw of the game texture (rgds_shader_live answers which one).
bool shader_live() {
#ifdef __linux__
    static auto live = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "rgds_shader_live"));
    return live && live() > 0;
#else
    return false;
#endif
}

// The frame in use: a built-in style, or a picture loaded from the frames folder.
struct FrameChoice {
    std::string name = "off";
    int style = FrameOff;
    PictureFrame picture;
    bool active() const { return style != FrameOff || !picture.px.empty(); }
};
FrameChoice choose_frame(const std::string& name, const std::string& folder) {
    FrameChoice f;
    int style = parse_frame(name);
    if (style >= 0) {
        f.style = style;
        f.name = name;
        return f;
    }
    std::string error;
    if (!folder.empty() && load_picture_frame(folder + "/" + name + ".png", f.picture, error)) f.name = name;
    else std::fprintf(stderr, "rgds-play: frame %s not used: %s\n", name.c_str(), error.empty() ? "no frames folder" : error.c_str());
    return f;
}

void draw_game(Panel& p, const Picture& game, int fit, const FrameChoice& frame, const Font& font,
               const std::string& system) {
    SDL_UpdateTexture(p.game, nullptr, game.px, game.stride * 4);
    SDL_Rect dest = fit_rect(p.area, game.w, game.h, fit == FitInteger);
    bool sharp = fit == FitSharp, smooth = fit == FitSmooth;
    SDL_Texture* over = nullptr;
    if (frame.active()) {
        const bool picture = frame.style == FrameOff;
        FrameLayout layout = picture ? picture_layout(frame.picture, p.area.w, p.area.h, game.w, game.h)
                                     : frame_layout(p.area.w, p.area.h, game.w, game.h, frame.style);
        const std::string key = frame.name + "@" + std::to_string(game.w) + "x" + std::to_string(game.h);
        if (p.frame_key != key) {
            Canvas drawn(p.area.w, p.area.h);
            if (picture) draw_picture_frame(drawn, frame.picture);
            else draw_frame(drawn, layout, frame.style, font, system);
            if (!p.frame)
                p.frame = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, p.area.w,
                                            p.area.h);
            if (p.frame) {
                SDL_UpdateTexture(p.frame, nullptr, drawn.data(), p.area.w * 4);
                SDL_SetTextureBlendMode(p.frame, picture ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
            }
            p.frame_key = key;
            p.frame_over = picture;
        }
        if (p.frame) {
            if (p.frame_over) over = p.frame;
            else SDL_RenderCopy(p.renderer, p.frame, nullptr, &p.area);
            dest = {p.area.x + layout.x, p.area.y + layout.y, layout.w, layout.h};
            sharp = !layout.whole;
            smooth = false;
        }
    }
    if (sharp && p.sharp && !shader_live() && SDL_SetRenderTarget(p.renderer, p.sharp) == 0) {
        SDL_SetTextureScaleMode(p.game, SDL_ScaleModeNearest);
        SDL_RenderCopy(p.renderer, p.game, nullptr, nullptr);
        SDL_SetRenderTarget(p.renderer, nullptr);
        SDL_RenderCopy(p.renderer, p.sharp, nullptr, &dest);
    } else {
        SDL_SetTextureScaleMode(p.game, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        SDL_RenderCopy(p.renderer, p.game, nullptr, &dest);
    }
    if (over) SDL_RenderCopy(p.renderer, over, nullptr, &p.area);
}

int parse_fit(const std::string& s) {
    return s == "integer" ? FitInteger : s == "smooth" || s == "fill" ? FitSmooth : FitSharp;
}
int parse_second(const std::string& s) { return s == "off" ? SecondOff : s == "swap" ? SecondSwap : SecondCompanion; }
const char* fit_name(int f) { return f == FitInteger ? "whole pixels" : f == FitSmooth ? "smooth fill" : "sharp fill"; }
const char* second_name(int s) { return s == SecondOff ? "off" : s == SecondSwap ? "swapped" : "companion"; }

// RGDS_PLAY_INPUT: the buttons from each listed frame on.
std::map<uint64_t, uint32_t> read_inputs(const char* path) {
    std::map<uint64_t, uint32_t> inputs;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        uint64_t frame;
        std::string keys;
        uint32_t buttons;
        if (in >> frame >> keys && parse_buttons(keys, buttons)) inputs[frame] = buttons;
    }
    return inputs;
}

// One panel as it will be presented, into a canvas (RGDS_PLAY_SHOTS).
bool read_panel(const Panel& p, Canvas& out) {
    SDL_Rect area = p.area;
    out = Canvas(area.w, area.h);
    return SDL_RenderReadPixels(p.renderer, &area, SDL_PIXELFORMAT_ABGR8888, out.data(), area.w * 4) == 0;
}

} // namespace

// Called by the RG DS bridge (dlsym) when the in-game menu changes the view.
// fit: 0 sharp, 1 whole pixels, 2 smooth; second: 0 companion, 1 off, 2 swap;
// frame: a built-in style's name or a picture frame's (taken up between frames;
// a picture that cannot be read is logged and drawn as no frame).
extern "C" __attribute__((visibility("default"))) int rgds_play_set_graphics(int fit, int second, const char* frame) {
    if (fit < 0 || fit > 2 || second < 0 || second > 2 || !frame || !*frame || std::strlen(frame) > 64 ||
        std::strchr(frame, '/') || frame[0] == '.')
        return 0;
    {
        std::lock_guard<std::mutex> guard(wanted_frame_lock);
        wanted_frame = frame;
    }
    wanted_fit = fit;
    wanted_second = second;
    return 1;
}

int main(int argc, char** argv) {
    SessionOptions o;
    int fit = FitSharp, second = SecondCompanion;
    std::string frame_choice = "off", frames_folder;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rom") o.rom = next();
        else if (a == "--data") o.data_root = next();
        else if (a == "--profiles") o.profile_dirs.push_back(next());
        else if (a == "--fonts") o.font_dir = next();
        else if (a == "--save-source") o.save_sources.push_back(next());
        else if (a == "--fit") fit = parse_fit(next());
        else if (a == "--second") second = parse_second(next());
        else if (a == "--frame") frame_choice = next();
        else if (a == "--frames") frames_folder = next();
        else if (a == "--swap") second = SecondSwap;
        else if (a == "--no-companion") second = SecondOff;
        else {
            std::fprintf(stderr, "rgds-play: unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (o.rom.empty() || o.data_root.empty()) {
        std::fprintf(stderr, "usage: rgds-play --rom GAME --data DIR [--profiles DIR] [--fonts DIR]\n");
        return 2;
    }
    const char* core_env = std::getenv("RGDS_CORE");
    const bool bridged = core_env && *core_env; // RG DS Core runs us, with its shared menu
    const char* slot_env = std::getenv("RGDS_STATE_SLOT");
    const std::string state_slot = slot_env ? slot_env : "";
    std::signal(SIGTERM, request_stop);
    std::signal(SIGINT, request_stop);
    std::signal(SIGHUP, request_stop);
    Session session;
    std::string error;
    if (!session.open(o, error)) {
        std::fprintf(stderr, "rgds-play: %s\n", error.c_str());
        return 1;
    }
    // Auto-load: the checkpoint the runtime put in the state slot, before the
    // first frame, so the game never shows its boot first (melonDS does the same).
    if (const char* native = std::getenv("RGDS_NATIVE_AUTOLOAD");
        native && std::string(native) == "1" && !state_slot.empty()) {
        std::string why;
        bool ok = session.load_state_file(state_slot, why);
        std::fprintf(stderr, "rgds-play: auto-load %s%s\n",
                     ok ? "from " : "skipped: ", ok ? state_slot.c_str() : why.c_str());
    }
    std::fprintf(stderr, "rgds-play: %s %s \"%s\", companion profile: %s%s\n", session.core().system(),
                 session.core().game_code().c_str(), session.title().c_str(),
                 session.profile() ? session.profile()->file.c_str() : "none", bridged ? ", RG DS Core menu" : "");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "rgds-play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    Panel upper, lower; // the physical panels: display 0 is the top one
    const bool two = SDL_GetNumVideoDisplays() >= 2;
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool wayland = driver && std::strcmp(driver, "wayland") == 0;
    std::fprintf(stderr, "rgds-play: video %s, %d displays\n", driver ? driver : "none", SDL_GetNumVideoDisplays());
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
        SDL_Rect b{};
        SDL_GetDisplayBounds(i, &b);
        const char* name = SDL_GetDisplayName(i);
        std::fprintf(stderr, "rgds-play: display %d %s at %d,%d %dx%d\n", i, name ? name : "?", b.x, b.y, b.w, b.h);
    }
    int pw = 640, ph = 480;
    if (two) {
        if (!open_panel(upper, 0, pw, ph, true, "RG DS Play") ||
            !open_panel(lower, 1, pw, ph, true, "RG DS Play second screen")) {
            std::fprintf(stderr, "rgds-play: window: %s\n", SDL_GetError());
            return 1;
        }
        if (wayland && !(set_panel_role(upper.window, "org.rgds.top") && set_panel_role(lower.window, "org.rgds.bottom")))
            std::fprintf(stderr, "rgds-play: could not give the windows their panels\n");
        int w, h;
        SDL_GetRendererOutputSize(upper.renderer, &w, &h);
        upper.area = {0, 0, w, h};
        SDL_GetRendererOutputSize(lower.renderer, &w, &h);
        lower.area = {0, 0, w, h};
        pw = w;
        ph = h;
    } else {
        if (!open_panel(upper, 0, pw, ph * 2, false, "RG DS Play")) {
            std::fprintf(stderr, "rgds-play: window: %s\n", SDL_GetError());
            return 1;
        }
        SDL_RenderSetLogicalSize(upper.renderer, pw, ph * 2);
        lower.window = upper.window;
        lower.renderer = upper.renderer;
        upper.area = {0, 0, pw, ph};
        lower.area = {0, ph, pw, ph};
    }
    Picture first = session.game();
    use_panel(upper);
    make_textures(upper, first.w, first.h, pw, ph);
    if (lower.renderer != upper.renderer) {
        use_panel(lower);
        make_textures(lower, first.w, first.h, pw, ph);
    }
    else {
        lower.game = upper.game;
        lower.sharp = upper.sharp;
        lower.companion = upper.companion;
    }
    FrameChoice frame = choose_frame(frame_choice, frames_folder);
    // What the black bar says: the system the game is for.
    const std::string system_label = std::string(session.core().system()) == "gbc" ? "GAME BOY COLOR"
                                     : std::string(session.core().system()) == "gb" ? "GAME BOY"
                                                                                     : "GAME BOY ADVANCE";
    std::fprintf(stderr, "rgds-play: game screen %s, second screen %s, frame %s\n", fit_name(fit), second_name(second),
                 frame.name.c_str());

    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    SDL_AudioStream* stream = nullptr;
    unsigned stream_rate = 0;
    if (audio) {
        stream_rate = session.audio_rate();
        stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, int(stream_rate), have.format, have.channels, have.freq);
        SDL_PauseAudioDevice(audio, 0);
        std::fprintf(stderr, "rgds-play: sound %d Hz, %d channels, %d-sample periods (game %u Hz)%s\n", have.freq,
                     have.channels, have.samples, stream_rate, stream ? "" : ", but no converter");
    } else std::fprintf(stderr, "rgds-play: no sound: %s\n", SDL_GetError());
    const bool debug = std::getenv("RGDS_PLAY_DEBUG") != nullptr;
    int peak = 0;
    const Uint32 target_queue = Uint32(have.freq / 20) * 4; // 50 ms of stereo s16

    // On its own on the RG DS the pad is read and grabbed directly. Under RG
    // DS Core it stays shared: the runtime watches it for Home.
    EvdevPad device_pad;
    if (!bridged && device_pad.open())
        std::fprintf(stderr, "rgds-play: buttons from %s (grabbed)\n", device_pad.path().c_str());
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && !pad && !device_pad.active(); ++i)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
    // The handheld's second key device reports some buttons as letters: with
    // the RG DS pad in use (grabbed, or the runtime's mapping), keys are ignored.
    const bool keyboard = !device_pad.active() && !bridged;

    Canvas companion(pw, ph);
    Look look;
    uint64_t drawn_key = ~0ull;
    const double frame_seconds = 1.0 / session.frame_rate();
    const char* input_env = std::getenv("RGDS_PLAY_INPUT");
    const auto inputs = input_env ? read_inputs(input_env) : std::map<uint64_t, uint32_t>{};
    const char* shots_env = std::getenv("RGDS_PLAY_SHOTS");
    const std::string shots = shots_env ? shots_env : "";
    const char* every_env = std::getenv("RGDS_PLAY_SHOT_EVERY");
    const uint64_t shot_every = every_env ? uint64_t(std::max(1, std::atoi(every_env))) : 600;
    uint64_t next_shot = shot_every;
    const char* quit_env = std::getenv("RGDS_PLAY_QUIT_AT");
    const uint64_t quit_at = quit_env ? std::strtoull(quit_env, nullptr, 10) : 0;
    int logged_hud = 0;
    double owed = 0;
    bool fast = false;
    Uint64 last = SDL_GetPerformanceCounter();
    Uint32 quit_held_since = 0;
    std::vector<int16_t> samples(4096 * 2);
    bool running = true;
    auto quit = [&](const char* why) {
        if (running) std::fprintf(stderr, "rgds-play: quit: %s\n", why);
        running = false;
    };
    while (running && !stop_requested) {
        int f = wanted_fit.exchange(-1), s = wanted_second.exchange(-1);
        std::string r;
        {
            std::lock_guard<std::mutex> guard(wanted_frame_lock);
            r.swap(wanted_frame);
        }
        if (f >= 0 || s >= 0 || !r.empty()) {
            if (f >= 0) fit = f;
            if (s >= 0) second = s;
            if (!r.empty() && r != frame.name) frame = choose_frame(r, frames_folder);
            drawn_key = ~0ull;
            std::fprintf(stderr, "rgds-play: game screen %s, second screen %s, frame %s\n", fit_name(fit),
                         second_name(second), frame.name.c_str());
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit(bridged ? "the menu returned home" : "the window was closed");
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                const auto key = e.key.keysym.scancode;
                std::string why;
                if (key == SDL_SCANCODE_F5 && !state_slot.empty()) {
                    // The bridge waits for this file to change, then keeps it as the checkpoint.
                    bool ok = session.save_state_file(state_slot, why);
                    std::fprintf(stderr, "rgds-play: state %s%s\n",
                                 ok ? "saved to " : "not saved: ", ok ? state_slot.c_str() : why.c_str());
                } else if (key == SDL_SCANCODE_F7 && !state_slot.empty()) {
                    bool ok = session.load_state_file(state_slot, why);
                    std::fprintf(stderr, "rgds-play: state %s%s\n",
                                 ok ? "loaded from " : "not loaded: ", ok ? state_slot.c_str() : why.c_str());
                    owed = 0;
                } else if (key == SDL_SCANCODE_TAB) {
                    fast = !fast;
                    std::fprintf(stderr, "rgds-play: fast-forward %s\n", fast ? "on" : "off");
                } else if (key == SDL_SCANCODE_ESCAPE && keyboard) quit("Escape");
            }
            if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !bridged)
                quit("Home");
            if (e.type == SDL_CONTROLLERDEVICEADDED && !pad && !device_pad.active())
                pad = SDL_GameControllerOpen(e.cdevice.which);
        }
        uint32_t buttons = (keyboard ? keyboard_buttons(SDL_GetKeyboardState(nullptr)) : 0) | controller_buttons(pad);
        if (device_pad.active()) {
            bool home = false;
            buttons |= device_pad.poll(home);
            if (home) quit("Home");
        }
        if (!bridged && (buttons & (Select | Start)) == (Select | Start)) {
            if (!quit_held_since) quit_held_since = SDL_GetTicks();
            else if (SDL_GetTicks() - quit_held_since > 1000) quit("Select+Start held");
        } else quit_held_since = 0;

        Uint64 now = SDL_GetPerformanceCounter();
        owed += double(now - last) / SDL_GetPerformanceFrequency() * (fast ? 3 : 1);
        last = now;
        // Keep the sound queue near its target by running a frame early or late.
        if (audio && !fast) {
            Uint32 queued = SDL_GetQueuedAudioSize(audio);
            if (queued < target_queue / 2) owed += frame_seconds * 0.05;
            else if (queued > target_queue * 2) owed -= frame_seconds * 0.05;
        }
        // After a stall (a pause or a lid sleep stops the process), don't race.
        if (owed > frame_seconds * (fast ? 8 : 4)) owed = frame_seconds;
        while (owed >= frame_seconds) {
            uint32_t pressed = buttons;
            if (!inputs.empty()) {
                auto it = inputs.upper_bound(session.frames());
                pressed = it == inputs.begin() ? 0 : std::prev(it)->second;
            }
            session.frame(pressed);
            owed -= frame_seconds;
            // A game can change its sound resolution (and so its rate) while it runs.
            if (stream && session.audio_rate() != stream_rate) {
                stream_rate = session.audio_rate();
                SDL_FreeAudioStream(stream);
                stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, int(stream_rate), have.format, have.channels, have.freq);
            }
            if (stream) {
                size_t n = session.audio(samples.data(), samples.size() / 2);
                if (debug)
                    for (size_t i = 0; i < n * 2; ++i) peak = std::max(peak, std::abs(int(samples[i])));
                if (debug && session.frames() % 60 == 0) {
                    std::fprintf(stderr, "rgds-play: sound queue %u bytes, game %u Hz, peak %d, device %d\n",
                                 SDL_GetQueuedAudioSize(audio), stream_rate, peak,
                                 int(SDL_GetAudioDeviceStatus(audio)));
                    peak = 0;
                }
                if (n && !fast) SDL_AudioStreamPut(stream, samples.data(), int(n * 4));
                int ready;
                while ((ready = SDL_AudioStreamAvailable(stream)) > 0) {
                    std::vector<uint8_t> out(size_t(std::min(ready, 16384)));
                    int got = SDL_AudioStreamGet(stream, out.data(), int(out.size()));
                    if (got <= 0) break;
                    SDL_QueueAudio(audio, out.data(), Uint32(got));
                }
            }
        }

        if (session.companion()) {
            // A line per change, so a log shows when the second screen took the HUD and gave it back.
            auto st = session.companion()->status();
            int hud = st.hud_split ? 2 : st.showing_hud ? 1 : 0;
            if (hud != logged_hud)
                std::fprintf(stderr, "rgds-play: frame %llu, HUD %s (%s)\n", (unsigned long long)session.frames(),
                             hud == 2   ? "on the second screen"
                             : hud == 1 ? "kept, split off"
                                        : "on the game screen",
                             session.companion()->hud_probes(session.core()).c_str());
            logged_hud = hud;
        }
        if (quit_at && session.frames() >= quit_at) quit("RGDS_PLAY_QUIT_AT");
        if (debug && session.frames() % 300 == 0 && upper.window != lower.window)
            std::fprintf(stderr, "rgds-play: game window on display %d, second window on display %d\n",
                         SDL_GetWindowDisplayIndex(upper.window), SDL_GetWindowDisplayIndex(lower.window));

        Panel& game_panel = second == SecondSwap ? lower : upper;
        bool companion_new = false;
        if (second != SecondOff &&
            (session.companion() ? session.companion()->visual_key() != drawn_key : drawn_key != 0)) {
            session.render_companion(companion, look);
            drawn_key = session.companion() ? session.companion()->visual_key() : 0;
            companion_new = true;
        }
        const bool one_window = lower.renderer == upper.renderer;
        const bool shooting = !shots.empty() && session.frames() >= next_shot;
        Canvas shot_top, shot_bottom;
        // Each window in turn with its own context bound: cleared, drawn, read for a shot, presented.
        for (Panel* window : {&upper, &lower}) {
            if (window == &lower && one_window) break;
            use_panel(*window);
            SDL_SetRenderDrawColor(window->renderer, 0, 0, 0, 255);
            SDL_RenderClear(window->renderer);
            for (Panel* p : {&upper, &lower}) {
                if (p != window && !one_window) continue;
                if (p == &game_panel) draw_game(*p, session.game(), fit, frame, session.font(), system_label);
                else if (second != SecondOff) {
                    if (companion_new) SDL_UpdateTexture(p->companion, nullptr, companion.data(), pw * 4);
                    SDL_RenderCopy(p->renderer, p->companion, nullptr, &p->area);
                }
                if (shooting) read_panel(*p, p == &upper ? shot_top : shot_bottom);
            }
            SDL_RenderPresent(window->renderer);
        }
        if (shooting) {
            write_bmp(shots + "/" + std::to_string(session.frames()) + ".bmp", shot_top, &shot_bottom);
            next_shot = session.frames() + shot_every - session.frames() % shot_every;
        }
    }
    if (stop_requested) std::fprintf(stderr, "rgds-play: quit: asked to stop (signal)\n");
    device_pad.close();
    session.flush();
    std::fprintf(stderr, "rgds-play: saved, %llu frames\n", (unsigned long long)session.frames());
    if (audio) SDL_CloseAudioDevice(audio);
    if (stream) SDL_FreeAudioStream(stream);
    if (pad) SDL_GameControllerClose(pad);
    for (Panel* p : {&upper, &lower})
        if (p->owns_window) {
            SDL_DestroyRenderer(p->renderer);
            SDL_DestroyWindow(p->window);
        }
    SDL_Quit();
    return 0;
}
