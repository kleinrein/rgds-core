// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Media: films and books on both screens.
//
//   rgds-media --videos DIR... --books DIR... --state DIR [--fonts DIR] [--locales DIR] [--preferences FILE]
//              [--music DIR] [--bt-sink FILE] [--open FILE] [--shelf videos|books] [--script FILE] [--shots DIR] [--quit-after MS]
//
// --open starts on one film or book (the launcher's "Continue"); Back from it then leaves the program.
// --script drives it for tests: lines "MS key A", "MS touch down|move|up X Y" (the lower panel), "MS shot NAME"
// (both panels into DIR/NAME.png, 640 x 960), "MS quit". MS counts from the start.
//
// Under RG DS Core the program gets both panels, like a game: the upper one is display 0. On a desktop it is one
// window with the upper panel above the lower.
#include "media/app.hpp"
#include "media/book.hpp"
#include "media/video.hpp"
#include "frontend/evdev_pad.hpp"
#include "platform/touch.hpp"

#include <SDL.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef __linux__
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
using namespace media;

namespace {

volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

bool set_panel_role(SDL_Window* window, const char* role) {
    // Only where SDL has Wayland (RG DS OS's SDL is KMS only: there are no windows to place).
#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_WAYLAND)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND || !info.info.wl.xdg_toplevel)
        return false;
    void* client = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!client) return false;
    using Marshal = void (*)(void*, unsigned int, ...);
    auto marshal = reinterpret_cast<Marshal>(dlsym(client, "wl_proxy_marshal"));
    if (marshal) marshal(info.info.wl.xdg_toplevel, 3, role);
    dlclose(client);
    return marshal != nullptr;
#else
    (void)window;
    (void)role;
    return false;
#endif
}

bool open_panel(Panel& p, int display, int w, int h, bool fullscreen, const char* title) {
    Uint32 flags = SDL_WINDOW_SHOWN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_ALLOW_HIGHDPI);
    p.window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                w, h, flags);
    if (!p.window) return false;
    p.r = SDL_CreateRenderer(p.window, -1,
                             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!p.r) p.r = SDL_CreateRenderer(p.window, -1, 0);
    p.owns_window = true;
    SDL_RendererInfo info{};
    if (p.r && SDL_GetRendererInfo(p.r, &info) == 0 && info.name && std::strstr(info.name, "opengl"))
        p.gl = SDL_GL_GetCurrentContext();
    return p.r != nullptr;
}

// Each panel's GL context bound for real before touching it (see play/src/frontend/main.cpp: on the RG DS, SDL's
// idea of the current context drifts from EGL's).
void use_panel(const Panel& p) {
    if (!p.gl) return;
    SDL_GL_MakeCurrent(p.window, nullptr);
    SDL_GL_MakeCurrent(p.window, p.gl);
}

Key key_for_scancode(SDL_Scancode s, bool& ok) {
    ok = true;
    switch (s) {
    case SDL_SCANCODE_UP: return KUp;
    case SDL_SCANCODE_DOWN: return KDown;
    case SDL_SCANCODE_LEFT: return KLeft;
    case SDL_SCANCODE_RIGHT: return KRight;
    case SDL_SCANCODE_X:
    case SDL_SCANCODE_SPACE: return KA;
    case SDL_SCANCODE_Z:
    case SDL_SCANCODE_BACKSPACE: return KB;
    case SDL_SCANCODE_S: return KX;
    case SDL_SCANCODE_A: return KY;
    case SDL_SCANCODE_Q: return KL;
    case SDL_SCANCODE_W: return KR;
    case SDL_SCANCODE_1: return KL2;
    case SDL_SCANCODE_2: return KR2;
    case SDL_SCANCODE_RETURN: return KStart;
    case SDL_SCANCODE_RSHIFT: return KSelect;
    case SDL_SCANCODE_ESCAPE: return KHome;
    default: ok = false; return KA;
    }
}

Key key_for_button(int b, bool& ok) {
    ok = true;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return KUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return KDown;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return KLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return KRight;
    case SDL_CONTROLLER_BUTTON_A: return KA;
    case SDL_CONTROLLER_BUTTON_B: return KB;
    case SDL_CONTROLLER_BUTTON_X: return KX;
    case SDL_CONTROLLER_BUTTON_Y: return KY;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return KL;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return KR;
    case SDL_CONTROLLER_BUTTON_START: return KStart;
    case SDL_CONTROLLER_BUTTON_BACK: return KSelect;
    case SDL_CONTROLLER_BUTTON_GUIDE: return KHome;
    default: ok = false; return KA;
    }
}

Key key_by_name(const std::string& n, bool& ok) {
    static const char* names[KCount] = {"Up", "Down", "Left", "Right", "A",     "B",      "X",   "Y",
                                        "L",  "R",    "L2",   "R2",    "Start", "Select", "Home"};
    ok = true;
    for (int i = 0; i < KCount; ++i)
        if (n == names[i]) return Key(i);
    ok = false;
    return KA;
}

struct ScriptLine {
    uint32_t at = 0;
    std::string verb, a, b;
    double x = 0, y = 0;
};

std::vector<ScriptLine> read_script(const std::string& path) {
    std::vector<ScriptLine> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        ScriptLine s;
        in >> s.at >> s.verb;
        if (s.verb == "touch") in >> s.a >> s.x >> s.y;
        else in >> s.a;
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptLine& a, const ScriptLine& b) { return a.at < b.at; });
    return out;
}

// Held keys repeat: after 320 ms, then every 70 ms (the launcher's pace).
struct Repeat {
    bool held[KCount] = {};
    uint32_t next[KCount] = {};
    void down(Key k, uint32_t now) { held[k] = true, next[k] = now + 320; }
    void up(Key k) { held[k] = false; }
    template <class F> void tick(uint32_t now, F&& fire) {
        for (int k = 0; k < KCount; ++k)
            if (held[k] && now >= next[k] && k <= KRight) {
                next[k] = now + 70;
                fire(Key(k));
            }
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string fonts = "assets/fonts", locales = "assets/locales", preferences, open_file, shots, script_file;
    std::string state_dir, music_dir, bt_sink;
    std::vector<std::string> videos, books;
    int shelf = 0;
    uint32_t quit_after = 0;
    bool music_tab = false, index_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--videos") videos.push_back(next());
        else if (a == "--books") books.push_back(next());
        else if (a == "--state") state_dir = next();
        else if (a == "--music") music_dir = next();
        else if (a == "--bt-sink") bt_sink = next();
        else if (a == "--fonts") fonts = next();
        else if (a == "--locales") locales = next();
        else if (a == "--preferences") preferences = next();
        else if (a == "--open") open_file = next();
        else if (a == "--shelf") shelf = next() == "books" ? 1 : 0;
        else if (a == "--shots") shots = next();
        else if (a == "--music-tab") music_tab = true;
        else if (a == "--index") index_only = true;
        else if (a == "--script") script_file = next();
        else if (a == "--quit-after") quit_after = uint32_t(std::atoi(next().c_str()));
        else if (a == "--help" || a == "-h") {
            std::printf("usage: rgds-media --videos DIR --books DIR --state DIR [--open FILE] [--script FILE]\n");
            return 0;
        } else {
            std::fprintf(stderr, "rgds-media: unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (state_dir.empty()) {
        std::fprintf(stderr, "usage: rgds-media --videos DIR --books DIR --state DIR\n");
        return 2;
    }
    // The launcher's Media level: the shelves and their posters, no window (library.cpp).
    if (index_only) return make_index(videos, books, state_dir);
    std::signal(SIGTERM, request_stop);
    std::signal(SIGINT, request_stop);
    std::signal(SIGHUP, request_stop);

    Context c;
    c.state_dir = state_dir;
    c.music_tab = music_tab;
    c.video_dirs = videos;
    c.book_dirs = books;
    std::error_code ec;
    fs::create_directories(state_dir, ec);
    // The launcher's Theme & display; with no file, its defaults (light), as the launcher draws them.
    c.look = play::load_look(preferences.empty() ? state_dir + "/no-preferences.ini" : preferences);
    c.font.load(fonts, c.look.language);
    c.font.set_pixel(c.look.pixel_text);
    c.words.load(locales, c.look.language);
    c.look.words = &c.words;
    c.places.load(state_dir + "/places.tsv");
    // The music player's equaliser settings (its folder's eq.ini) and its engine's control socket.
    c.sound.eq_file = (music_dir.empty() ? state_dir : music_dir) + "/eq.ini";
    if (!music_dir.empty()) c.sound.music_run = music_dir + "/run";
    if (!bt_sink.empty()) c.sound.bt_sink = bt_sink;
    c.sound.poll(1, true);
    c.posters.start(state_dir + "/posters");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "rgds-media: SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    const bool two = SDL_GetNumVideoDisplays() >= 2 && std::getenv("RGDS_MEDIA_WINDOW") == nullptr;
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool wayland = driver && std::strcmp(driver, "wayland") == 0;
    if (two) {
        if (!open_panel(c.upper, 0, PW, PH, true, "RG DS Media") ||
            !open_panel(c.lower, 1, PW, PH, true, "RG DS Media lower")) {
            std::fprintf(stderr, "rgds-media: window: %s\n", SDL_GetError());
            return 1;
        }
        if (wayland &&
            !(set_panel_role(c.upper.window, "org.rgds.top") && set_panel_role(c.lower.window, "org.rgds.bottom")))
            std::fprintf(stderr, "rgds-media: could not give the windows their panels\n");
        for (Panel* p : {&c.upper, &c.lower}) {
            use_panel(*p);
            SDL_RenderSetLogicalSize(p->r, PW, PH);
        }
    } else {
        if (!open_panel(c.upper, 0, PW, PH * 2, false, "RG DS Media")) {
            std::fprintf(stderr, "rgds-media: window: %s\n", SDL_GetError());
            return 1;
        }
        SDL_RenderSetLogicalSize(c.upper.r, PW, PH * 2);
        c.lower = c.upper;
        c.lower.owns_window = false;
        c.upper.viewport = {0, 0, PW, PH};
        c.lower.viewport = {0, PH, PW, PH};
    }

    play::EvdevPad pad_device;
    pad_device.open();
    // The lower panel's touch screen, read directly on the handheld (no bridge runs under RG DS Media). A finger
    // already down when it opens (the tap on the Home tile) is ignored until it lifts.
    rgds::TouchDevice lower_touch;
    if (const char* dev = std::getenv("RGDS_TOUCH_DEV"); dev && *dev && std::string(dev) != "none" && two) {
        if (lower_touch.open_device(dev)) lower_touch.hold_initial_contact();
    }
    // The volume keys are the runner's (runtime/media.py); it writes the level here and we show it.
    const char* hud_env = std::getenv("RGDS_MEDIA_VOLUME_HUD");
    const std::string volume_hud = hud_env ? hud_env : "";
    fs::file_time_type hud_seen{};
    int volume_level = -1;
    uint32_t volume_at = 0, hud_checked = 0;
    Tex volume_tex;
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && !pad && !pad_device.active(); ++i)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

    // ---- Screens and their transitions

    std::vector<std::unique_ptr<Screen>> stack;
    bool running = true;
    Tex snap_upper, snap_lower; // the screen going away, faded over the new one
    uint32_t trans_at = 0;
    bool trans_back = false, trans_live = false;
    auto draw_panel = [&](Panel& p, bool upper, Screen& s) {
        if (upper) s.draw_upper(p.r);
        else s.draw_lower(p.r);
    };
    auto snapshot = [&]() {
        if (stack.empty()) return;
        for (int i = 0; i < 2; ++i) {
            Panel& p = i == 0 ? c.upper : c.lower;
            Tex& t = i == 0 ? snap_upper : snap_lower;
            use_panel(p);
            if (!t) {
                t.t = SDL_CreateTexture(p.r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, PW, PH);
                t.w = PW, t.h = PH;
                SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(t.t, SDL_ScaleModeLinear);
            }
            if (!t.t) continue;
            SDL_SetRenderTarget(p.r, t.t);
            SDL_RenderSetViewport(p.r, nullptr);
            SDL_SetRenderDrawColor(p.r, 0, 0, 0, 255);
            SDL_RenderClear(p.r);
            draw_panel(p, i == 0, *stack.back());
            if (i == 1) c.ripples.live.clear();
            SDL_SetRenderTarget(p.r, nullptr);
        }
        trans_at = SDL_GetTicks();
        trans_live = c.animations();
    };
    c.push = [&](std::unique_ptr<Screen> s) {
        snapshot();
        trans_back = false;
        stack.push_back(std::move(s));
        c.dirty = true;
    };
    c.pop = [&]() {
        if (stack.empty()) return;
        if (stack.size() == 1 || !open_file.empty()) {
            running = false;
            return;
        }
        snapshot();
        trans_back = true;
        stack.back()->leave();
        // Destroyed after this frame's input: a screen may pop itself from inside its own handler.
        static std::vector<std::unique_ptr<Screen>> gone;
        gone.clear();
        gone.push_back(std::move(stack.back()));
        stack.pop_back();
        stack.back()->resumed();
        c.dirty = true;
    };
    c.quit = [&]() { running = false; };

    if (!open_file.empty()) {
        Item item;
        item.path = open_file;
        item.key = key_of(open_file);
        item.name = fs::path(open_file).stem().string();
        item.ext = fs::path(open_file).extension().string();
        for (auto& ch : item.ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (video_ext(item.ext)) {
            item.kind = Kind::Video;
            item.name = tidy_name(item.name);
            std::vector<Item> siblings;
            for (const auto& it : list_folder(fs::path(open_file).parent_path().string(), false))
                if (it.kind == Kind::Video) siblings.push_back(it);
            stack.push_back(make_watch(c, item, siblings));
        } else {
            item.kind = Kind::Book;
            stack.push_back(make_read(c, item));
        }
        trans_live = false;
    } else {
        stack.push_back(make_library(c, shelf));
    }

    // ---- Input

    Repeat repeat;
    auto send_key = [&](Key k, bool rep) {
        if (stack.empty()) return;
        if (k == KHome) {
            running = false;
            return;
        }
        c.now = SDL_GetTicks();
        stack.back()->key(k, rep);
        c.dirty = true;
    };
    auto press = [&](Key k, bool down) {
        if (down) {
            repeat.down(k, SDL_GetTicks());
            send_key(k, false);
        } else {
            repeat.up(k);
        }
    };
    bool finger_down = false;
    auto send_touch = [&](TouchEvent::Kind kind, double x, double y) {
        if (stack.empty()) return;
        if (kind == TouchEvent::Down) finger_down = true;
        if (kind == TouchEvent::Move && !finger_down) return;
        if (kind == TouchEvent::Up) finger_down = false;
        TouchEvent t;
        t.kind = kind;
        t.x = std::clamp(x, 0.0, double(PW - 1));
        t.y = std::clamp(y, 0.0, double(PH - 1));
        t.at = SDL_GetTicks();
        c.now = t.at;
        stack.back()->touch(t);
        c.dirty = true;
    };
    std::vector<ScriptLine> script = script_file.empty() ? std::vector<ScriptLine>{} : read_script(script_file);
    size_t script_at = 0;
    const uint32_t started = SDL_GetTicks();
    auto shoot = [&](const std::string& name) {
        // Both panels drawn again into one picture: upper above lower.
        std::vector<uint32_t> px(size_t(PW) * PH * 2);
        for (int i = 0; i < 2; ++i) {
            Panel& p = i == 0 ? c.upper : c.lower;
            use_panel(p);
            SDL_Texture* t = SDL_CreateTexture(p.r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, PW, PH);
            SDL_SetRenderTarget(p.r, t);
            SDL_RenderSetViewport(p.r, nullptr);
            SDL_SetRenderDrawColor(p.r, 0, 0, 0, 255);
            SDL_RenderClear(p.r);
            draw_panel(p, i == 0, *stack.back());
            if (i == 1) c.ripples.draw(p.r, c.now, c.look.accent, c.animations());
            SDL_Rect all{0, 0, PW, PH};
            SDL_RenderReadPixels(p.r, &all, SDL_PIXELFORMAT_ABGR8888, px.data() + size_t(i) * PW * PH, PW * 4);
            SDL_SetRenderTarget(p.r, nullptr);
            SDL_DestroyTexture(t);
        }
        fs::create_directories(shots, ec);
        write_png(shots + "/" + name + ".png", px.data(), PW, PH * 2);
        std::fprintf(stderr, "rgds-media: shot %s\n", name.c_str());
    };

    uint64_t frames = 0;
    double frame_ms_total = 0;
    while (running && !stop_requested) {
        SDL_Event e;
        const bool busy = c.dirty || trans_live || c.ripples.active() || (!stack.empty() && stack.back()->live());
        if (!busy && script.empty()) {
            if (SDL_WaitEventTimeout(&e, 250)) SDL_PushEvent(&e);
        }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                bool ok;
                Key k = key_for_scancode(e.key.keysym.scancode, ok);
                if (ok) press(k, e.type == SDL_KEYDOWN);
            } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad && !pad_device.active()) {
                pad = SDL_GameControllerOpen(e.cdevice.which);
            } else if ((e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) &&
                       !pad_device.active()) {
                bool ok;
                Key k = key_for_button(e.cbutton.button, ok);
                if (ok) press(k, e.type == SDL_CONTROLLERBUTTONDOWN);
            } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
                // The lower panel's touches: the bridge's reader (touch id -2), or the lower window's own.
                bool lower = e.tfinger.touchId == -2 ||
                             (two && c.lower.window && e.tfinger.windowID == SDL_GetWindowID(c.lower.window));
                if (!lower) continue;
                auto kind = e.type == SDL_FINGERDOWN ? TouchEvent::Down
                            : e.type == SDL_FINGERUP ? TouchEvent::Up
                                                     : TouchEvent::Move;
                send_touch(kind, e.tfinger.x * PW, e.tfinger.y * PH);
            } else if (!two && (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP ||
                                (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)))) {
                // A desktop: the mouse is a finger on the lower half of the window.
                if (e.type != SDL_MOUSEMOTION && e.button.which == SDL_TOUCH_MOUSEID) continue;
                int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                float lx = 0, ly = 0;
                SDL_RenderWindowToLogical(c.upper.r, mx, my, &lx, &ly);
                if (ly < PH && e.type == SDL_MOUSEBUTTONDOWN) continue;
                auto kind = e.type == SDL_MOUSEBUTTONDOWN ? TouchEvent::Down
                            : e.type == SDL_MOUSEBUTTONUP ? TouchEvent::Up
                                                          : TouchEvent::Move;
                send_touch(kind, lx, ly - PH);
            }
        }
        for (const auto& point : lower_touch.poll()) {
            if (point.kind > 2) { // cancelled
                send_touch(TouchEvent::Up, point.x, point.y);
                continue;
            }
            send_touch(point.kind == 0   ? TouchEvent::Down
                       : point.kind == 1 ? TouchEvent::Move
                                         : TouchEvent::Up,
                       point.x, point.y);
        }
        if (!volume_hud.empty() && SDL_GetTicks() - hud_checked > 120) {
            hud_checked = SDL_GetTicks();
            std::error_code hud_error;
            auto stamp = fs::last_write_time(volume_hud, hud_error);
            if (!hud_error && stamp != hud_seen) {
                const bool first = hud_seen == fs::file_time_type{};
                hud_seen = stamp;
                std::ifstream in(volume_hud);
                std::string line;
                while (std::getline(in, line))
                    if (line.rfind("volume=", 0) == 0) volume_level = std::atoi(line.c_str() + 7);
                if (!first) {
                    volume_at = hud_checked;
                    volume_tex.reset();
                    c.dirty = true;
                }
            }
        }
        if (pad_device.active()) {
            bool home = false;
            static uint32_t was = 0;
            uint32_t held = pad_device.poll(home);
            const std::pair<uint32_t, Key> bits[] = {
                {play::Up, KUp}, {play::Down, KDown}, {play::Left, KLeft},   {play::Right, KRight},  {play::A, KA},
                {play::B, KB},   {play::X, KX},       {play::Y, KY},         {play::L, KL},          {play::R, KR},
                {play::L2, KL2}, {play::R2, KR2},     {play::Start, KStart}, {play::Select, KSelect}};
            for (auto [bit, k] : bits)
                if ((held ^ was) & bit) press(k, held & bit);
            was = held;
            if (home) send_key(KHome, false);
        }
        const uint32_t now = SDL_GetTicks();
        repeat.tick(now, [&](Key k) { send_key(k, true); });
        while (script_at < script.size() && now - started >= script[script_at].at) {
            const ScriptLine& s = script[script_at++];
            if (s.verb == "key") {
                bool ok;
                Key k = key_by_name(s.a, ok);
                if (ok) send_key(k, false);
            } else if (s.verb == "touch") {
                send_touch(s.a == "down" ? TouchEvent::Down
                           : s.a == "up" ? TouchEvent::Up
                                         : TouchEvent::Move,
                           s.x, s.y);
            } else if (s.verb == "shot" && !shots.empty() && !stack.empty()) {
                c.now = now;
                stack.back()->update(now);
                shoot(s.a);
            } else if (s.verb == "quit") {
                running = false;
            }
        }
        if (quit_after && now - started > quit_after) running = false;
        if (!running || stack.empty()) break;

        c.now = now;
        c.power.read(now); // every 10 s at most
        Screen& top = *stack.back();
        bool moving = top.update(now);
        const double tt = trans_live ? std::min(1.0, (now - trans_at) / 240.0) : 1.0;
        if (tt >= 1) trans_live = false;
        if (!(moving || c.dirty || trans_live || c.ripples.active() || top.live())) continue;
        c.dirty = false;
        const uint64_t t0 = SDL_GetPerformanceCounter();
        for (int i = 0; i < 2; ++i) {
            Panel& p = i == 0 ? c.upper : c.lower;
            if (!two && i == 1) {
                SDL_RenderSetViewport(p.r, &p.viewport);
            } else {
                use_panel(p);
                SDL_RenderSetViewport(p.r, two ? nullptr : &p.viewport);
                if (two || i == 0) {
                    SDL_SetRenderDrawColor(p.r, 0, 0, 0, 255);
                    SDL_RenderClear(p.r);
                }
            }
            draw_panel(p, i == 0, top);
            if (i == 1) c.ripples.draw(p.r, now, c.look.accent, c.animations());
            // The volume, for a moment after a volume key: a pill at the top of the upper screen.
            if (i == 0 && volume_at && now - volume_at < 1600 && volume_level >= 0) {
                if (!volume_tex) {
                    Canvas cv(260, 52);
                    std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
                    cv.rounded_rect(0, 0, 260, 52, 26, Rgb{0, 0, 0}, 170);
                    glyph(cv, Glyph::Audio, 30, 26, 24, Rgb{255, 255, 255}, Rgb{0, 0, 0});
                    cv.rounded_rect(58, 22, 150, 8, 4, Rgb{255, 255, 255}, 70);
                    cv.rounded_rect(58, 22, std::max(8, 150 * std::clamp(volume_level, 0, 100) / 100), 8, 4,
                                    c.look.accent);
                    std::string pct = std::to_string(volume_level);
                    c.font.draw(cv, 246 - c.font.measure(pct, 20), c.font.centred(20, 26), pct, 20, Rgb{255, 255, 255});
                    upload(p.r, volume_tex, cv, true, true);
                }
                const double t = (now - volume_at) / 1600.0;
                const int alpha = t < .8 ? 255 : int(255 * (1 - (t - .8) / .2));
                draw(p.r, volume_tex, (PW - 260) / 2.0, 18 - 8 * ease_out(1 - std::min(1.0, t * 8)), alpha);
                c.dirty = true; // until it has faded
            }
            // The screen that went: fades and grows a touch (forward) or shrinks back into place (back).
            if (trans_live) {
                Tex& snap = i == 0 ? snap_upper : snap_lower;
                const double e = ease_out(tt);
                const double scale = trans_back ? 1 - .05 * e : 1 + .04 * e;
                const double w = PW * scale, h = PH * scale;
                draw(p.r, snap, (PW - w) / 2, (PH - h) / 2, w, h, int(255 * (1 - e)));
            }
            if (two) SDL_RenderPresent(p.r);
        }
        if (!two) SDL_RenderPresent(c.upper.r);
        frame_ms_total += double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
        ++frames;
    }
    for (auto& s : stack) s->leave();
    stack.clear();
    c.posters.stop();
    if (frames)
        std::fprintf(stderr, "rgds-media: %llu frames, %.2f ms each to draw and present\n",
                     static_cast<unsigned long long>(frames), frame_ms_total / double(frames));
    snap_upper.reset();
    snap_lower.reset();
    volume_tex.reset();
    if (pad) SDL_GameControllerClose(pad);
    if (c.lower.owns_window) {
        SDL_DestroyRenderer(c.lower.r);
        SDL_DestroyWindow(c.lower.window);
    }
    SDL_DestroyRenderer(c.upper.r);
    SDL_DestroyWindow(c.upper.window);
    SDL_Quit();
    return c.exit_code;
}
