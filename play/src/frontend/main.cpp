// SPDX-License-Identifier: GPL-3.0-or-later
// rgds-play: the game on one panel, its companion on the other.
//
//   rgds-play --rom GAME --data DIR [--profiles DIR]... [--fonts DIR]
//             [--save-source DIR]... [--fit sharp|integer|smooth] [--second companion|off|swap|info]
//             [--frame NAME] [--frames DIR] [--palette NAME] [--colour NAME] [--art DIR]
//
// --fit sharp (the default) fills the panel's width: the game is scaled up by
// a whole number on the GPU without smoothing, then down to the panel with
// it, so pixels stay crisp and the same size. integer keeps whole multiples
// (smaller, pixel exact); smooth fills with plain smoothing.
// --second: the companion on the second screen (default), nothing there
// (off), the companion on top and the game below (swap), or the quiet info
// screen with its own controls (info: companion/dock.hpp). The lower panel
// is a touch screen: the companion and the dock take its touches.
// --preferences: userdata/preferences.ini, the launcher's Theme & display
// (and ui.ini, system.ini beside it), which the second screen follows.
// --frame: something drawn around the game (ui/frame.hpp), which then places
// it: a built-in style of the game's own system (ui/frame_styles.hpp: dmg
// and the Pockets for gb, the gbc-* colours, indigo ... sp-platinum for gba,
// bar for all) or the name of a picture frame, NAME.png in --frames
// (userdata/frames/<system>). off, another system's style, or a picture that
// cannot be read, leaves the fit to choose.
// --palette: the handheld an original Game Boy game is shown on (dmg, pocket,
// light, color, greys); --colour: how a Game Boy Color game's screen shows its
// colours (gbc, gbc-dim, vivid). See ui/colour.hpp.
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
#include "app/storage.hpp"
#include "platform/touch.hpp"
#include "ui/gb_palettes.hpp"
#include "frontend/evdev_pad.hpp"
#include "ui/colour.hpp"
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
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
enum SecondMode { SecondCompanion = 0, SecondOff = 1, SecondSwap = 2, SecondInfo = 3 };

// Asked for by the bridge from its event hook (the main thread, between
// frames); taken up at the top of the next loop.
std::atomic<int> wanted_fit{-1}, wanted_second{-1};
std::atomic<int> wanted_colours{-1}; // palette * 16 + correction, set together
std::mutex wanted_frame_lock;
std::string wanted_frame; // empty: no change asked for
// The game, for the bridge's cheat calls (set once the session is open, cleared before it closes).
std::atomic<Session*> cheat_session{nullptr};

struct Panel {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* game = nullptr;
    SDL_Texture* sharp = nullptr; // the game at a whole multiple, for sharp fill
    int game_w = 0, game_h = 0;   // the size `game` (and `sharp`) were made for
    SDL_Texture* companion = nullptr;
    SDL_Texture* frame = nullptr; // the frame as drawn for frame_key (its name and the game's size)
    std::string frame_key;
    bool frame_over = false;    // a picture frame: laid over the game, not under it
    SDL_Rect area{};            // where this panel sits in its window
    SDL_Rect game_dest{};       // where the game was last drawn in it (a light gun aims through this)
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
    // L2 and R2 are axes to SDL (N64's Z is on L2).
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) b |= L2;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) b |= R2;
    return b;
}

// The GLES context a GPU-drawing libretro core gets (core.hpp's GlHost): made on the top panel's window at the
// core's first frame, beside the panel's own context, and only ever drawn into the core's own framebuffer.
SDL_Window* gpu_core_window = nullptr; // set once the panels are open, when their renderer is GL
SDL_GLContext gpu_core_context = nullptr;

bool gpu_core_begin(int major, int minor) {
    if (!gpu_core_window) return false;
    if (!gpu_core_context) {
        int profile = 0, old_major = 0, old_minor = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &old_major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &old_minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        gpu_core_context = SDL_GL_CreateContext(gpu_core_window);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, old_major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, old_minor);
        if (!gpu_core_context) {
            std::fprintf(stderr, "rgds-play: no GLES %d.%d context for the core: %s\n", major, minor, SDL_GetError());
            gpu_core_window = nullptr;
            return false;
        }
        std::fprintf(stderr, "rgds-play: GLES %d.%d context for the core\n", major, minor);
    }
    // Released first, as use_panel does: SDL's idea of the current context drifts from EGL's on the RG DS.
    SDL_GL_MakeCurrent(gpu_core_window, nullptr);
    return SDL_GL_MakeCurrent(gpu_core_window, gpu_core_context) == 0;
}
void gpu_core_end() { SDL_GL_MakeCurrent(gpu_core_window, nullptr); }
void* gpu_core_proc(const char* name) { return SDL_GL_GetProcAddress(name); }
const GlHost gpu_core_host{gpu_core_begin, gpu_core_end, gpu_core_proc};

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
// The game's textures follow its picture's size: a Game Boy HUD band moving
// to the second screen makes it shorter for as long as it is there.
void make_game_textures(Panel& p, int gw, int gh) {
    if (p.game) SDL_DestroyTexture(p.game);
    if (p.sharp) SDL_DestroyTexture(p.sharp);
    p.sharp = nullptr;
    p.game_w = gw;
    p.game_h = gh;
    p.game = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gw, gh);
    SDL_SetTextureScaleMode(p.game, SDL_ScaleModeNearest);
    if (SDL_RenderTargetSupported(p.renderer)) {
        SDL_Rect dest = fit_rect(p.area, gw, gh, false);
        int k = std::max(1, (dest.w + gw - 1) / gw); // 640 wide: 3x
        p.sharp = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, gw * k, gh * k);
        if (p.sharp) SDL_SetTextureScaleMode(p.sharp, SDL_ScaleModeLinear);
    }
}

void make_textures(Panel& p, int gw, int gh, int cw, int ch) {
    make_game_textures(p, gw, gh);
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

// The game's rumble (0-1), to the motor through RG DS Core's bridge, which
// holds the user's Game rumble setting. Nothing happens outside RG DS Core.
void rumble(float level) {
#ifdef __linux__
    static auto motor = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_rumble"));
    static int last = 0;
    const int percent = int(level * 100 + .5f);
    // Each frame while it runs (the bridge re-arms a short effect), once when it stops.
    if (motor && (percent || last)) motor(percent);
    last = percent;
#else
    (void)level;
#endif
}

// The frame in use: a built-in style, or a picture loaded from the frames folder.
struct FrameChoice {
    std::string name = "off";
    int style = FrameOff;
    PictureFrame picture;
    bool active() const { return style != FrameOff || !picture.px.empty(); }
};
FrameChoice choose_frame(const std::string& name, const std::string& folder, const std::string& system) {
    FrameChoice f;
    int style = parse_frame(name, system);
    if (style >= 0) {
        f.style = style;
        f.name = name;
        return f;
    }
    std::string error;
    if (!folder.empty() && load_picture_frame(folder + "/" + name + ".png", f.picture, error)) f.name = name;
    else
        std::fprintf(stderr, "rgds-play: frame %s not used: %s\n", name.c_str(),
                     error.empty() ? "no frames folder" : error.c_str());
    return f;
}

void draw_game(Panel& p, const Picture& game, int fit, const FrameChoice& frame, const Font& font,
               const std::string& system) {
    if (game.w != p.game_w || game.h != p.game_h) make_game_textures(p, game.w, game.h);
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
    p.game_dest = dest;
}

int parse_fit(const std::string& s) {
    return s == "integer" ? FitInteger : s == "smooth" || s == "fill" ? FitSmooth : FitSharp;
}
int parse_second(const std::string& s) {
    return s == "off" ? SecondOff : s == "swap" ? SecondSwap : s == "info" ? SecondInfo : SecondCompanion;
}
const char* fit_name(int f) { return f == FitInteger ? "whole pixels" : f == FitSmooth ? "smooth fill" : "sharp fill"; }
const char* second_name(int s) {
    return s == SecondOff ? "off" : s == SecondSwap ? "swapped" : s == SecondInfo ? "info" : "companion";
}

// The dock's brightness for the second screen, kept between sessions.
int read_brightness(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("brightness=", 0) == 0) return std::atoi(line.c_str() + 11);
    return 100;
}
void write_brightness(const std::string& path, int percent) {
    std::string error;
    const std::string text = "brightness=" + std::to_string(percent) + "\n";
    if (!write_file_atomic(path, std::vector<uint8_t>(text.begin(), text.end()), error))
        std::fprintf(stderr, "rgds-play: second screen brightness not kept: %s\n", error.c_str());
}

// The runtime's own triggers beside the control file, as the Anbernic button
// and the home shortcut write them: ".menu" opens the game switcher, ".home"
// saves and returns home (runtime/game_menu.py).
bool ask_runtime(const char* trigger) {
    const char* control = std::getenv("RGDS_CONTROL_FILE");
    if (!control || !*control) return false;
    const std::string text = std::to_string(SDL_GetTicks()) + "\n";
    std::string error;
    return write_file_atomic(std::string(control) + trigger, std::vector<uint8_t>(text.begin(), text.end()), error);
}

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
// fit: 0 sharp, 1 whole pixels, 2 smooth; second: 0 companion, 1 off, 2 swap, 3 info;
// frame: a built-in style's name or a picture frame's (taken up between frames;
// a picture that cannot be read is logged and drawn as no frame).
extern "C" __attribute__((visibility("default"))) int rgds_play_set_graphics(int fit, int second, const char* frame) {
    if (fit < 0 || fit > 2 || second < 0 || second > 3 || !frame || !*frame || std::strlen(frame) > 64 ||
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

// Called by the bridge when the in-game menu changes the colours (ui/colour.hpp).
extern "C" __attribute__((visibility("default"))) int rgds_play_set_color(int palette, int correction) {
    if (palette < 0 || palette >= play::palette_count() || correction < 0 || correction >= play::correction_count())
        return 0;
    wanted_colours = palette * 16 + correction;
    return 1;
}

// Cheats, called by the bridge as for DSCore's (rgds_cheat_* in its frontend): from its event hook on the main
// thread between frames, so a switch is applied to the core before the call returns. Codes have no groups here.
extern "C" __attribute__((visibility("default"))) int rgds_cheat_count() {
    Session* s = cheat_session.load();
    return s ? int(s->cheats().codes().size()) : 0;
}
extern "C" __attribute__((visibility("default"))) int rgds_cheat_group_count() { return 0; }
extern "C" __attribute__((visibility("default"))) int rgds_cheat_group(int, const char**, int*) { return 0; }
extern "C" __attribute__((visibility("default"))) int
rgds_cheat_entry(int index, const char** name, const char** description, int* group, int* state) {
    Session* s = cheat_session.load();
    if (!s || index < 0 || size_t(index) >= s->cheats().codes().size()) return 0;
    const play::CheatCode& c = s->cheats().codes()[size_t(index)];
    if (name) *name = c.name.c_str();
    if (description) *description = c.description.c_str();
    if (group) *group = -1;
    if (state) *state = c.on ? 1 : 0;
    return 1;
}
extern "C" __attribute__((visibility("default"))) int rgds_set_cheat(int index, int on) {
    Session* s = cheat_session.load();
    if (!s || index < 0 || on < 0 || on > 1) return 0;
    return s->set_cheat(size_t(index), on != 0) ? 1 : 0;
}

int main(int argc, char** argv) {
    SessionOptions o;
    int fit = FitSharp, second = SecondCompanion;
    std::string frame_choice = "off", frames_folder, palette_choice = "default", colour_choice = "off";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rom") o.rom = next();
        else if (a == "--data") o.data_root = next();
        else if (a == "--profiles") o.profile_dirs.push_back(next());
        else if (a == "--fonts") o.font_dir = next();
        else if (a == "--preferences") o.preferences = next();
        else if (a == "--core") o.core_library = next();
        else if (a == "--system") o.system = next();
        else if (a == "--system-dir") o.system_dir = next();
        else if (a == "--art") o.art_dir = next();
        else if (a == "--save-source") o.save_sources.push_back(next());
        else if (a == "--fit") fit = parse_fit(next());
        else if (a == "--second") second = parse_second(next());
        else if (a == "--frame") frame_choice = next();
        else if (a == "--frames") frames_folder = next();
        else if (a == "--palette") palette_choice = next();
        else if (a == "--colour") colour_choice = next();
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
    struct CheatsClosed { // before the session goes, whichever way main returns
        ~CheatsClosed() { cheat_session = nullptr; }
    } cheats_closed;
    std::string error;
    set_gl_host(&gpu_core_host);
    if (!session.open(o, error)) {
        std::fprintf(stderr, "rgds-play: %s\n", error.c_str());
        return 1;
    }
    cheat_session = &session;
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
        if (wayland &&
            !(set_panel_role(upper.window, "org.rgds.top") && set_panel_role(lower.window, "org.rgds.bottom")))
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
    if (upper.gl) gpu_core_window = upper.window;
    Picture first = session.game();
    use_panel(upper);
    make_textures(upper, first.w, first.h, pw, ph);
    if (lower.renderer != upper.renderer) {
        use_panel(lower);
        make_textures(lower, first.w, first.h, pw, ph);
    } else {
        lower.game = upper.game;
        lower.sharp = upper.sharp;
        lower.companion = upper.companion;
    }
    FrameChoice frame = choose_frame(frame_choice, frames_folder, session.core().system());
    Colour colour;
    colour.set(session.core(), parse_palette(palette_choice), parse_correction(colour_choice));
    if (session.companion()) session.companion()->set_colour(colour);
    // What the black bar says: the system the game is for.
    const std::string system_label = std::string(session.core().system()) == "gbc"  ? "GAME BOY COLOR"
                                     : std::string(session.core().system()) == "gb" ? "GAME BOY"
                                                                                    : "GAME BOY ADVANCE";
    // A light-gun game is played by touching it: it starts on the touch screen.
    // A light gun or a mouse game is played on the touch screen: it starts there, a finger aims or draws.
    const bool lightgun = session.profile() && (session.profile()->lightgun || session.profile()->mouse);
    if (lightgun && second == SecondCompanion) second = SecondSwap;
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
#ifdef __linux__
    // RGDS_PLAY_CPU=N (RG DS OS): the main loop, which runs the core and draws, gets a core of its own at
    // round-robin priority; the threads already started (SDL's audio) keep the session's CPUs.
    if (const char* pin = std::getenv("RGDS_PLAY_CPU"); pin && *pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(std::atoi(pin), &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::perror("rgds-play: sched_setaffinity");
        sched_param sp{};
        sp.sched_priority = 5;
        if (sched_setscheduler(0, SCHED_RR, &sp) != 0) std::perror("rgds-play: sched_setscheduler");
    }
#endif
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
    Look look = session.look();
    // A game's art on the second screen in the palette the top screen shows it in.
    auto shade_look = [&]() {
        const int p = colour.palette();
        for (int i = 0; i < 4; ++i)
            look.game_shades[i] =
                rgb_to_px(std::strcmp(session.core().system(), "gb") == 0 && p > 0 ? gb_palettes[p].shade[i]
                                                                                   : gb_palettes[0].shade[i]);
    };
    shade_look();
    // The second screen's touches: under RG DS Core the bridge reads the
    // lower panel (Weston does not deliver it reliably) and sends them as
    // finger events with touchId -2; on its own the player reads it itself.
    // A finger at x, y of the lower panel (its 640x480 pixels) aims the light gun at the game pixel under it.
    auto aim = [&](int x, int y, bool down) {
        const SDL_Rect& d = lower.game_dest;
        const Picture game = session.core().picture();
        if (d.w <= 0 || d.h <= 0 || game.w <= 0 || game.h <= 0) return;
        const int wx = lower.area.x + x * lower.area.w / std::max(1, pw),
                  wy = lower.area.y + y * lower.area.h / std::max(1, ph);
        const bool inside = wx >= d.x && wy >= d.y && wx < d.x + d.w && wy < d.y + d.h;
        session.core().set_pointer(inside ? (wx - d.x) * game.w / d.w : -1, inside ? (wy - d.y) * game.h / d.h : -1,
                                   down);
    };
    rgds::TouchDevice panel_touch;
    const bool panel_touch_open =
        !bridged && panel_touch.open_device(std::getenv("RGDS_TOUCH_DEV") ? std::getenv("RGDS_TOUCH_DEV") : "");
    const std::string brightness_file = o.data_root + "/second-screen.ini";
    if (session.companion()) {
        session.companion()->dock().set_brightness(read_brightness(brightness_file));
        session.companion()->dock().brightness_changed();
        session.companion()->dock().set_twelve_hour(look.twelve_hour);
    }
    Uint32 brightness_at = 0;
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
    // While RG DS Core's menu is up (the runtime's ".blocked" beside the control file) the game holds still and
    // quiet, and the frames go on: on RG DS OS the bridge draws the menu into them (runtime/emulators.py).
    const char* control_env = std::getenv("RGDS_CONTROL_FILE");
    const std::string menu_block = bridged && control_env && *control_env ? std::string(control_env) + ".blocked" : "";
    bool held = false;
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
            if (!r.empty() && r != frame.name) frame = choose_frame(r, frames_folder, session.core().system());
            drawn_key = ~0ull;
            std::fprintf(stderr, "rgds-play: game screen %s, second screen %s, frame %s\n", fit_name(fit),
                         second_name(second), frame.name.c_str());
        }
        if (int both = wanted_colours.exchange(-1); both >= 0) {
            const int pal = both / 16, col = both % 16;
            colour.set(session.core(), pal, col);
            if (session.companion()) session.companion()->set_colour(colour);
            shade_look();
            drawn_key = ~0ull;
            std::fprintf(stderr, "rgds-play: palette %s, colour %s\n", palette_slug(pal), correction_slug(col));
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
            // With the screens swapped a light-gun or mouse game is on the touch screen: a finger aims and fires, or
            // draws.
            if (lightgun && second == SecondSwap) {
                if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
                    int x = -1, y = -1;
                    if (e.tfinger.touchId == -2) {
                        x = int(e.tfinger.x * pw), y = int(e.tfinger.y * ph);
                    } else if (lower.window && e.tfinger.windowID == SDL_GetWindowID(lower.window)) {
                        int ww = 0, wh = 0;
                        SDL_GetWindowSize(lower.window, &ww, &wh);
                        x = (int(e.tfinger.x * ww) - lower.area.x) * pw / std::max(1, lower.area.w);
                        y = (int(e.tfinger.y * wh) - lower.area.y) * ph / std::max(1, lower.area.h);
                    }
                    aim(x, y, e.type != SDL_FINGERUP);
                } else if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) &&
                           e.button.which != SDL_TOUCH_MOUSEID && lower.window &&
                           e.button.windowID == SDL_GetWindowID(lower.window)) {
                    aim((e.button.x - lower.area.x) * pw / std::max(1, lower.area.w),
                        (e.button.y - lower.area.y) * ph / std::max(1, lower.area.h), e.type == SDL_MOUSEBUTTONDOWN);
                } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK) &&
                           e.motion.which != SDL_TOUCH_MOUSEID && lower.window &&
                           e.motion.windowID == SDL_GetWindowID(lower.window)) {
                    // A desktop's mouse dragged: a finger drawing (Mario Paint).
                    aim((e.motion.x - lower.area.x) * pw / std::max(1, lower.area.w),
                        (e.motion.y - lower.area.y) * ph / std::max(1, lower.area.h), true);
                }
                continue;
            }
            // The second screen is the touch screen while it shows the companion or the info screen.
            if (!session.companion() || (second != SecondCompanion && second != SecondInfo)) continue;
            Touch t;
            bool have = false;
            if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
                t.kind = e.type == SDL_FINGERDOWN ? Touch::Down : e.type == SDL_FINGERUP ? Touch::Up : Touch::Move;
                if (e.tfinger.touchId == -2) { // the bridge's reader: the lower panel, 640x480
                    t.x = int(e.tfinger.x * pw);
                    t.y = int(e.tfinger.y * ph);
                    have = true;
                } else if (lower.window && e.tfinger.windowID == SDL_GetWindowID(lower.window)) {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(lower.window, &ww, &wh);
                    t.x = (int(e.tfinger.x * ww) - lower.area.x) * pw / std::max(1, lower.area.w);
                    t.y = (int(e.tfinger.y * wh) - lower.area.y) * ph / std::max(1, lower.area.h);
                    have = true;
                }
            } else if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP ||
                        (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK))) &&
                       e.button.which != SDL_TOUCH_MOUSEID && lower.window &&
                       e.button.windowID == SDL_GetWindowID(lower.window)) {
                // A desktop: the mouse stands in for a finger.
                const int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x,
                          my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                t.kind = e.type == SDL_MOUSEBUTTONDOWN ? Touch::Down
                         : e.type == SDL_MOUSEBUTTONUP ? Touch::Up
                                                       : Touch::Move;
                t.x = (mx - lower.area.x) * pw / std::max(1, lower.area.w);
                t.y = (my - lower.area.y) * ph / std::max(1, lower.area.h);
                have = t.y >= 0 && t.y < ph;
            }
            if (have && session.companion()->touch(t)) drawn_key = ~0ull;
        }
        if (panel_touch_open && lightgun && second == SecondSwap)
            for (const auto& point : panel_touch.poll())
                if (point.kind < 3) aim(point.x, point.y, point.kind != 2);
        if (panel_touch_open && session.companion() && (second == SecondCompanion || second == SecondInfo))
            for (const auto& point : panel_touch.poll())
                if (point.kind < 3) {
                    Touch t{point.kind == 0   ? Touch::Down
                            : point.kind == 1 ? Touch::Move
                                              : Touch::Up,
                            point.x, point.y};
                    if (session.companion()->touch(t)) drawn_key = ~0ull;
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
        if (const bool hold = !menu_block.empty() && access(menu_block.c_str(), F_OK) == 0; hold != held) {
            held = hold;
            if (audio) {
                if (hold) SDL_ClearQueuedAudio(audio);
                SDL_PauseAudioDevice(audio, hold ? 1 : 0);
            }
            std::fprintf(stderr, "rgds-play: %s\n", hold ? "held under the menu" : "playing again");
        }
        if (held) owed = 0;
        if (pad)
            session.core().set_sticks(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX),
                                      SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY),
                                      SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX),
                                      SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
        while (owed >= frame_seconds) {
            uint32_t pressed = buttons;
            if (!inputs.empty()) {
                auto it = inputs.upper_bound(session.frames());
                pressed = it == inputs.begin() ? 0 : std::prev(it)->second;
            }
            // Off or the info screen: the game keeps its whole picture, HUD and all.
            if (session.companion()) session.companion()->set_shown(second == SecondCompanion || second == SecondSwap);
            session.frame(pressed);
            rumble(session.core().rumble());
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

        if (Companion* c = session.companion()) {
            Dock& dock = c->dock();
            c->set_time(SDL_GetTicks());
            c->set_info(second == SecondInfo);
            dock.set_reachable(second != SecondSwap);
            switch (dock.take_action()) {
            case Dock::Action::Switcher:
                // On its own the player has no other games to switch to.
                if (!bridged || !ask_runtime(".menu")) dock.report("Not available");
                break;
            case Dock::Action::Home:
                // The runtime saves first, as the menu's Return home does; on its own the player saves on quitting.
                if (!bridged || !ask_runtime(".home")) quit("Return home on the second screen");
                break;
            case Dock::Action::None: break;
            }
            if (dock.brightness_changed()) brightness_at = SDL_GetTicks() | 1;
            if (brightness_at && SDL_GetTicks() - brightness_at > 800) {
                write_brightness(brightness_file, dock.brightness());
                brightness_at = 0;
            }
        }
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
                if (p == &game_panel)
                    draw_game(*p, colour.apply(session.game(shader_live())), fit, frame, session.font(), system_label);
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
    close_gpu_core(); // while its context can still be made current
    if (gpu_core_context) SDL_GL_DeleteContext(gpu_core_context);
    for (Panel* p : {&upper, &lower})
        if (p->owns_window) {
            SDL_DestroyRenderer(p->renderer);
            SDL_DestroyWindow(p->window);
        }
    SDL_Quit();
    return 0;
}
