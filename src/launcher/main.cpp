#include "launcher/model.hpp"
#include "platform/sdl_dynamic.hpp"
#include "platform/event_wait.hpp"
#include "platform/wayland_role.hpp"
#include "platform/ui_trace.hpp"
#include "platform/system_controls.hpp"
#include "platform/lid_sleep.hpp"
#include "launcher/settings.hpp"
#include "launcher/i18n.hpp"
#include "launcher/collection_filter.hpp"
#include "launcher/keyboard.hpp"
#include "launcher/library_summary.hpp"
#include "launcher/settings_visual.hpp"
#include "launcher/pixel_icons.hpp"
#include "launcher/font.hpp"
#include "launcher/pixel_text.hpp"
#include "launcher/line_break.hpp"
#include "launcher/carousel.hpp"
#include "launcher/view_motion.hpp"
#include "launcher/menu_audio.hpp"
#include "launcher/accent.hpp"
#include "platform/alsa_mixer.hpp"
#include "platform/net_probe.hpp"
#include "launcher/library_viewport.hpp"
#include "launcher/media.hpp"
#include "launcher/game_options.hpp"
#include "launcher/shader_preview.hpp"
#include "launcher/library_cache.hpp"
#include "launcher/game_info.hpp"
#include "launcher/pictochat_app.hpp"
#include "platform/touch.hpp"
#include "platform/volume_keys.hpp"
#include "launcher/repeat.hpp"
#include "platform/overlay_frame.hpp"
#include "platform/control_mapping.hpp"
#include "platform/mainline_controls.hpp"
#include <array>
#include <csignal>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <optional>
#include <functional>
#include <sstream>
#include <set>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <poll.h>
#include <unistd.h>

using namespace rgds;
static volatile sig_atomic_t stopping = 0;
static void on_signal(int) { stopping = 1; }

// Opt-in startup evidence. Buffer milestones and write once after presentation;
// logging must not add disk IO between SDL/window/font initialization stages.
struct LaunchProfile {
    struct Mark {
        const char* stage;
        uint64_t us;
        int panel;
    };
    std::array<Mark, 32> marks{};
    size_t count = 0;
    bool enabled = false, finished = false;
    uint64_t began = 0;
    static uint64_t clock() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return uint64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    }
    void start() {
        const char* option = std::getenv("RGDS_LAUNCH_PROFILE");
        enabled = option && std::string(option) == "1";
        if (enabled) began = clock();
    }
    void mark(const char* stage, int panel = -1) {
        if (enabled && !finished && count < marks.size()) marks[count++] = {stage, clock() - began, panel};
    }
    void finish(bool menu) {
        if (!enabled || finished) return;
        mark("first-present");
        finished = true;
        std::ostringstream output;
        output << "{\"type\":\"rgds-launch-profile\",\"pid\":" << getpid()
               << ",\"game_menu\":" << (menu ? "true" : "false") << ",\"milestones\":[";
        for (size_t i = 0; i < count; i++) {
            if (i) output << ',';
            output << "{\"stage\":\"" << marks[i].stage << "\",\"elapsed_us\":" << marks[i].us
                   << ",\"panel\":" << marks[i].panel << '}';
        }
        output << "]}\n";
        std::cerr << output.str();
    }
};
static LaunchProfile launch_profile;

// Sleep until a child exits instead of waking the CPU every 10 ms while an
// emulator owns the screens. Signals still interrupt poll(2).
static void wait_child_exit(pid_t pid, int timeout_ms) {
#if defined(__linux__) && defined(SYS_pidfd_open)
    if (pid > 0) {
        int fd = int(syscall(SYS_pidfd_open, pid, 0));
        if (fd >= 0) {
            pollfd item{fd, POLLIN, 0};
            poll(&item, 1, timeout_ms);
            ::close(fd);
            return;
        }
    }
#endif
    (void)pid;
    (void)timeout_ms;
    usleep(10000);
}

// UI reads may materialize an empty optional field. Missing and empty fields
// render identically, so that must not turn an unchanged status file into a repaint.
static bool same_fields(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
    for (const auto& pair : a)
        if (!pair.second.empty()) {
            auto found = b.find(pair.first);
            if (found == b.end() || found->second != pair.second) return false;
        }
    for (const auto& pair : b)
        if (!pair.second.empty()) {
            auto found = a.find(pair.first);
            if (found == a.end() || found->second != pair.second) return false;
        }
    return true;
}
static bool same_hardware(const SystemStatus& a, const SystemStatus& b) {
    return a.brightness == b.brightness && a.power == b.power && a.sleep == b.sleep && a.lid == b.lid &&
           a.charging == b.charging && a.external_power == b.external_power &&
           a.brightness_percent == b.brightness_percent && a.battery == b.battery && a.volume == b.volume &&
           a.volume_percent == b.volume_percent && a.poweroff == b.poweroff;
}

// Original, deliberately small probe font. UTF-8 paths are retained by the model;
// full Unicode text rendering is a later UI milestone.
static const std::map<char, std::array<unsigned char, 7>> glyphs = {{'A', {14, 17, 17, 31, 17, 17, 17}},
                                                                    {'B', {30, 17, 17, 30, 17, 17, 30}},
                                                                    {'C', {14, 17, 16, 16, 16, 17, 14}},
                                                                    {'D', {30, 17, 17, 17, 17, 17, 30}},
                                                                    {'E', {31, 16, 16, 30, 16, 16, 31}},
                                                                    {'F', {31, 16, 16, 30, 16, 16, 16}},
                                                                    {'G', {14, 17, 16, 23, 17, 17, 15}},
                                                                    {'H', {17, 17, 17, 31, 17, 17, 17}},
                                                                    {'I', {14, 4, 4, 4, 4, 4, 14}},
                                                                    {'J', {7, 2, 2, 2, 18, 18, 12}},
                                                                    {'K', {17, 18, 20, 24, 20, 18, 17}},
                                                                    {'L', {16, 16, 16, 16, 16, 16, 31}},
                                                                    {'M', {17, 27, 21, 21, 17, 17, 17}},
                                                                    {'N', {17, 25, 21, 19, 17, 17, 17}},
                                                                    {'O', {14, 17, 17, 17, 17, 17, 14}},
                                                                    {'P', {30, 17, 17, 30, 16, 16, 16}},
                                                                    {'Q', {14, 17, 17, 17, 21, 18, 13}},
                                                                    {'R', {30, 17, 17, 30, 20, 18, 17}},
                                                                    {'S', {15, 16, 16, 14, 1, 1, 30}},
                                                                    {'T', {31, 4, 4, 4, 4, 4, 4}},
                                                                    {'U', {17, 17, 17, 17, 17, 17, 14}},
                                                                    {'V', {17, 17, 17, 17, 17, 10, 4}},
                                                                    {'W', {17, 17, 17, 21, 21, 21, 10}},
                                                                    {'X', {17, 17, 10, 4, 10, 17, 17}},
                                                                    {'Y', {17, 17, 10, 4, 4, 4, 4}},
                                                                    {'Z', {31, 1, 2, 4, 8, 16, 31}},
                                                                    {'0', {14, 17, 19, 21, 25, 17, 14}},
                                                                    {'1', {4, 12, 4, 4, 4, 4, 14}},
                                                                    {'2', {14, 17, 1, 2, 4, 8, 31}},
                                                                    {'3', {30, 1, 1, 14, 1, 1, 30}},
                                                                    {'4', {2, 6, 10, 18, 31, 2, 2}},
                                                                    {'5', {31, 16, 16, 30, 1, 1, 30}},
                                                                    {'6', {14, 16, 16, 30, 17, 17, 14}},
                                                                    {'7', {31, 1, 2, 4, 8, 8, 8}},
                                                                    {'8', {14, 17, 17, 14, 17, 17, 14}},
                                                                    {'9', {14, 17, 17, 15, 1, 1, 14}},
                                                                    {'.', {0, 0, 0, 0, 0, 6, 6}},
                                                                    {':', {0, 6, 6, 0, 6, 6, 0}},
                                                                    {'-', {0, 0, 0, 31, 0, 0, 0}},
                                                                    {'/', {1, 2, 2, 4, 8, 8, 16}},
                                                                    {'+', {0, 4, 4, 31, 4, 4, 0}},
                                                                    {'?', {14, 17, 1, 2, 4, 0, 4}},
                                                                    {'!', {4, 4, 4, 4, 4, 0, 4}},
                                                                    {'=', {0, 31, 0, 31, 0, 0, 0}},
                                                                    {'%', {25, 25, 2, 4, 8, 19, 19}},
                                                                    {'[', {14, 8, 8, 8, 8, 8, 14}},
                                                                    {']', {14, 2, 2, 2, 2, 2, 14}},
                                                                    {'(', {2, 4, 8, 8, 8, 4, 2}},
                                                                    {')', {8, 4, 2, 2, 2, 4, 8}},
                                                                    {'<', {1, 2, 4, 8, 4, 2, 1}},
                                                                    {'>', {16, 8, 4, 2, 4, 8, 16}},
                                                                    {'_', {0, 0, 0, 0, 0, 0, 31}},
                                                                    {' ', {0, 0, 0, 0, 0, 0, 0}},
                                                                    {char(0xdf), {14, 17, 17, 22, 17, 17, 22}},
                                                                    {char(0xc6), {15, 20, 20, 30, 20, 20, 23}},
                                                                    {char(0xd8), {15, 19, 21, 21, 25, 17, 30}},
                                                                    {char(0xc5), {4, 10, 4, 14, 17, 31, 17}},
                                                                    {'&', {12, 18, 20, 8, 21, 18, 13}},
                                                                    {'\'', {4, 4, 8, 0, 0, 0, 0}},
                                                                    {char(0xb7), {0, 0, 0, 6, 6, 0, 0}},
                                                                    {',', {0, 0, 0, 0, 0, 4, 8}}};
struct Section {
    const char* title;
};
static const std::array<Section, 10> sections = {{{"Theme & display"},
                                                  {"Power"},
                                                  {"Library"},
                                                  {"Sound"},
                                                  {"Connectivity"},
                                                  {"System"},
                                                  {"Controls"},
                                                  {"Statistics"},
                                                  {"Power off"},
                                                  {"Updates"}}};

struct Options {
    bool independent = false, mainline = false;
    bool preview = false, probe = false, allow_launch = false, watch_assets = false, settings = false,
         game_menu = false, game_switcher = false, home = false, library = false, boot_warning = false,
         boot_startup = false, card_repair = false;
    int frames = 0, seconds = 0, boot_ready_fd = -1;
    std::string top = "DSI-1", bottom = "DSI-2";
    fs::path theme, themes, screenshot, app, data, state, preferences, system_preferences, input_script, cache_report,
        touch_recording;
    std::string mode, font, section;
    std::vector<fs::path> roots;
    std::vector<std::pair<std::string, fs::path>> system_roots; // --system-dir gba=PATH: RG DS Play's games
    fs::path game_rom;
    std::string game_core = "dsperate";
    fs::path executable;
};
struct Surface {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Uint32 id = 0;
    SDL_Surface* bitmap = nullptr;
};
class App {
    Sdl s;
    MenuAudio sounds{s};
    Options o;
    Model model;
    bool session_initialized = false, library_ready = false, boot_displayed = false, boot_touch_candidate = false,
         boot_touch_wait_release = false, boot_hardware_ready = false;
    Uint32 boot_displayed_at = 0, boot_scene_at = 0;
    std::set<SDL_Keycode> boot_blocked_keys;
    std::set<std::pair<SDL_JoystickID, Uint8>> boot_blocked_buttons;
    pid_t library_child = -1;
    int library_pipe = -1;
    bool library_eof = false, library_scan_started = false;
    std::string library_bytes;
    std::vector<Game> all_games;
    // Show all systems: the DS games as scanned, the systems listed on the Systems
    // page (the DS first, then the others that have games) and whose games All shows.
    std::vector<Game> ds_games;
    std::vector<std::string> listed_systems = {"nds"};
    std::string shown_system = "nds";
    // The Systems step: Home > Games with Show all systems on. It is the library's
    // carousel (as Home's and the cartridges'), its tiles the systems with games.
    bool systems_menu = false;
    size_t systems_selected = 0;
    struct SystemStats {
        size_t games = 0;
        uint64_t sessions = 0, seconds = 0;
    };
    std::vector<SystemStats> system_stats;
    int play_fit = 0, play_second = 0; // RG DS Play's view of the running game (play.ini)
    std::string play_frame = "off";
    GameOptions game_preferences;
    std::vector<Shader> shaders;
    std::vector<int> shader_choices;
    bool shader_per_game = false;
    int shader_core = 0;
    Page shader_return = Page::LibraryOptions;
    int shader_preview_scene = 0;
    ShaderPreviewMode shader_preview_mode = ShaderPreviewMode::Split;
    float shader_preview_split = .5f;
    // The picker reuses the Home carousel and rail, with its own position.
    Carousel shader_carousel;
    bool shader_drag = false, shader_thumbs_pending = false;
    ControlMapping edited_controls;
    Page controls_return = Page::Settings;
    bool controls_per_game = false, controls_inherit = false, control_capture = false;
    int capture_logical = -1;
    Uint32 capture_started = 0, capture_clock = 0;
    std::string controls_notice;
    Uint32 pointer_at = 0, release_at = 0, launch_at = 0, feedback_frame_at = 0;
    int contact_x = 0, contact_y = 0;
    bool touch_tail = false, options_ready = false, launching = false, launch_preview = false;
    unsigned launching_core = 0;
    int launch_drawn_head = -1;
    enum class LaunchPhase { None, Waiting, Preparing, Exiting, Playing };
    LaunchPhase launch_phase = LaunchPhase::None;
    pid_t launch_child = -1;
    int launch_channel = -1, launch_result = 0;
    bool launch_cancelled = false, launch_handoff = false;
    Uint32 launch_exit_at = 0, launch_cancel_at = 0;
    double launch_exit_opacity = 1;
    Carousel carousel;
    ViewMotion view_motion;
    int view_touch_dx = 0, view_touch_dy = 0;
    MotionHits motion_hits;
    ViewTravel view_route = ViewTravel::Forward;
    bool motion_touch = false, drawing_elements = false;
    int element_origin_y = 0;
    Uint32 view_drawn_at = 0;
    uint64_t element_moves = 0, back_transitions = 0;
    // Animations page: four 72 px cards, and three 104 px choices in a card.
    static int motion_row(int index) { return 100 + index * 82; }
    static int motion_segment_x(int index) { return 270 + index * 112; }
    bool view_transitions_enabled() const {
        return preferences.animations && preferences.view_animations && !probe && !launching &&
               settings.page != Page::BootWarning;
    }
    void motion_input(int code) {
        view_route = (code == 3 || (code == 7 && settings.page != Page::Library) ||
                      (code == 2 && settings.page == Page::Display) || (code == 4 && settings.page == Page::Keyboard))
                         ? ViewTravel::Back
                     : code == 0 ? ViewTravel::Left
                     : code == 1 ? ViewTravel::Right
                                 : ViewTravel::Forward;
        if (view_transitions_enabled() && preferences.view_style == ViewStyle::Spring)
            view_motion.interact(s.GetTicks(), motion_touch ? contact_x : 320, motion_touch ? contact_y : 240);
    }
    void move_element(uint32_t id, int& x, int& y, int width, int height, bool focused, int clip_top = 0,
                      int clip_bottom = 480, int clip_left = 0, int clip_right = 640) {
        if (!drawing_elements || !view_transitions_enabled() || layered(preferences.view_style)) return;
        MotionRect bounds{x, y - element_origin_y, width, height};
        auto offset = view_motion.element(id, bounds, focused, view_drawn_at);
        if (pointer_down && bounds.contains(contact_x, contact_y)) offset.y += 3;
        motion_hits.add(bounds, offset.x, offset.y, clip_top, clip_bottom, clip_left, clip_right);
        x += offset.x;
        y += offset.y;
        if (offset.x || offset.y) element_moves++;
    }
    SDL_Texture* view_canvas = nullptr;
    SDL_Texture *view_previous = nullptr, *view_presented = nullptr;
    bool view_snapshot_ready = false, view_previous_ready = false;
    ViewStyle last_view_style = ViewStyle::Spring;
    std::vector<std::pair<int, bool>> view_levels;
    uint64_t push_animation_frames = 0;
    void clear_view_canvases() {
        for (auto** texture : {&view_canvas, &view_previous, &view_presented})
            if (*texture) {
                s.DestroyTexture(*texture);
                *texture = nullptr;
            }
        view_snapshot_ready = view_previous_ready = false;
    }
    void start_view_motion(Uint32 now, ViewTravel route = ViewTravel::Forward) {
        // Push composites the view captured just before the input that
        // changed it; nothing captured (no input) means no transition.
        if (layered(preferences.view_style) && !view_previous_ready) {
            view_motion.stop();
            return;
        }
        view_motion.start(now, route, preferences.view_style);
    }
    SDL_Renderer* bottom_renderer() { return surfaces.size() > 1 ? surfaces[1].renderer : surfaces[0].renderer; }
    uint64_t view_capture_frame = ~uint64_t(0);
    // The outgoing image for a Push transition, captured once per displayed
    // frame when an input arrives. Idle frames never render offscreen.
    void capture_view_for_input(const SDL_Event& e) {
        if (!view_transitions_enabled() || !layered(preferences.view_style) || view_canvas_failed ||
            view_motion.running || surfaces.empty())
            return;
        const bool trigger = e.type == SDL_KEYDOWN || e.type == SDL_JOYBUTTONDOWN ||
                             (e.type == SDL_JOYHATMOTION && e.jhat.value) || e.type == SDL_MOUSEBUTTONUP ||
                             e.type == SDL_FINGERUP;
        if (!trigger || view_capture_frame == rendered_frames) return;
        SDL_Renderer* renderer = bottom_renderer();
        if (!view_previous)
            view_previous = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
        if (!view_previous || s.SetRenderTarget(renderer, view_previous) != 0) {
            view_canvas_failed = true;
            s.ClearError();
            return;
        }
        s.RenderSetLogicalSize(renderer, 640, 480);
        s.RenderSetClipRect(renderer, nullptr);
        s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        s.SetRenderDrawColor(renderer, 0, 0, 0, 0);
        s.RenderClear(renderer);
        paint_bottom(renderer, 0);
        if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore capture canvas");
        s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
        s.RenderSetClipRect(renderer, nullptr);
        view_previous_ready = true;
        view_capture_frame = rendered_frames;
    }
    bool view_canvas_failed = false, view_seen = false;
    std::tuple<int, bool, int, int> last_view;
    uint64_t view_transitions = 0, view_animation_frames = 0;
    void sync_view_motion(Uint32 now) {
        auto key = std::make_tuple(int(settings.page), home_menu, preferences.view, preferences.layout);
        bool enabled = view_transitions_enabled();
        view_motion.pace = view_pace(preferences.view_speed);
        if (last_view_style != preferences.view_style) {
            clear_view_canvases();
            view_motion.stop();
            view_motion.style = preferences.view_style;
            view_canvas_failed = false;
            last_view_style = preferences.view_style;
        }
        auto level = std::make_pair(int(settings.page), home_menu);
        if (view_seen && level != std::make_pair(std::get<0>(last_view), std::get<1>(last_view))) {
            auto parent = std::find(view_levels.begin(), view_levels.end(), level);
            if (parent != view_levels.end()) {
                view_route = ViewTravel::Back;
                view_levels.erase(parent + 1, view_levels.end());
            } else if (view_route == ViewTravel::Back) view_levels = {level};
            else {
                view_route = ViewTravel::Forward;
                view_levels.push_back(level);
            }
        } else if (!view_seen) view_levels = {level};
        const bool same_page = settings.page == Page::Library && std::get<0>(last_view) == int(Page::Library) &&
                               home_menu == std::get<1>(last_view);
        // A choice on the view sheet changes the page under the sheet, and the
        // sheet's own slide away is the whole transition. A crossfade as well
        // would keep the old frame, sheet and all, over the closing sheet until
        // the fade ran out.
        if (view_seen && key != last_view && enabled && !(same_page && (picker_open || sheet_leaving()))) {
            // Any other layout or collection change stays in place: a quick
            // crossfade, never a sideways slide of the whole screen.
            if (same_page) view_route = ViewTravel::Fade;
            start_view_motion(now, view_route);
            if (view_route == ViewTravel::Back) back_transitions++;
            view_route = ViewTravel::Forward;
            view_transitions++;
        }
        last_view = key;
        view_seen = true;
        if (!enabled) {
            if (view_motion.active()) dirty = true;
            view_motion.stop();
            motion_hits.clear();
            clear_view_canvases();
            view_canvas_failed = false;
        }
    }
    struct SoundState {
        Page page;
        bool home, picker;
        int view, layout, focus, pick, row, col, volume, brightness;
        size_t game, text;
        unsigned toggles;
        bool favourite, launch;
    };
    unsigned sound_toggles() const {
        return unsigned(preferences.dark) | (unsigned(preferences.pixel) << 1) |
               (unsigned(preferences.animations) << 2) | (unsigned(preferences.view_animations) << 3) |
               (unsigned(preferences.touch_indicator) << 4) | (unsigned(preferences.auto_load) << 5) |
               (unsigned(preferences.auto_save) << 6) | (unsigned(preferences.menu_sounds) << 7) |
               (unsigned(system_preferences.lid) << 8);
    }
    SoundState sound_state() const {
        return {settings.page,
                home_menu,
                picker_open,
                preferences.view,
                preferences.layout,
                settings.page == Page::Settings ? settings.section : settings.focus,
                picker_focus,
                key_row,
                key_col,
                hardware.volume_percent,
                system_preferences.brightness,
                navigation_selected(),
                edit_buffer.size(),
                sound_toggles(),
                game_preferences.favourite,
                launching};
    }
    unsigned sound_depth = 0;
    int sound_code = 2;
    SoundState sound_before{};
    std::optional<MenuCue> explicit_sound;
    std::string menu_sound_action, departure_action;
    Uint32 departure_at = 0;
    bool departure_dispatch = false, sleep_audio_parked = false;
    void configure_sounds() {
        if (!sounds.opened) sounds.opened = [this] { restore_dac_level(); };
        sounds.configure(preferences.menu_sounds && !probe && !sleep_audio_parked && settings.page != Page::Microphone,
                         preferences.sound_volume, asset_path("sounds"));
        // Music belongs to the home, library and settings; not to the in-game
        // menu, the microphone test, sleep or a launch in progress.
        sounds.configure_music(preferences.menu_music && !probe && !o.game_menu && !sleep_audio_parked &&
                                   settings.page != Page::Microphone && !launching,
                               preferences.music_volume, asset_path("music") / "menu.wav");
    }
    // Every new sink can reset the RK817 DAC, including the boot screen. The
    // menu sink stays paused until the saved level is back; a direct control
    // write avoids a helper process on each reopen. Status may not exist yet.
    void restore_dac_level() {
        if (o.preview) return;
        int level = hardware.volume ? hardware.volume_percent : -1;
        if (level < 0) {
            auto saved = fields(o.data / "volume.ini");
            try {
                if (saved["schema"] == "1") level = std::clamp(std::stoi(saved["volume"]), 0, 100);
            } catch (const std::exception&) {
                level = -1;
            }
        }
        if (level >= 0 && write_mixer_percent(dac_percent_for_level(level))) return;
        if (!system.sync_volume()) {
            sounds.close();
            sounds.error = "Menu audio is unavailable on this device.";
        }
    }
    bool emit_sound(MenuCue cue, int direction = 0) {
        if (probe || settings.page == Page::Microphone || (hardware.volume && hardware.volume_percent == 0))
            return false;
        return sounds.play(cue, s.GetTicks(), direction,
                           motion_touch ? (contact_x / 640.f - .5f) * .5f : direction * .12f);
    }
    void sound_intent(MenuCue cue) {
        if (sound_depth) explicit_sound = cue;
        else emit_sound(cue);
    }
    void finish_sound_action() {
        if (--sound_depth) return;
        if (!departure_action.empty()) {
            explicit_sound.reset();
            return;
        }
        const auto after = sound_state();
        auto before = sound_before;
        int direction = sound_code == 0 || sound_code == 8 ? -1 : 1;
        if (explicit_sound) {
            emit_sound(*explicit_sound);
            explicit_sound.reset();
            return;
        }
        if (before.launch || after.launch) return;
        if (before.page == Page::GameOptions && after.page == Page::GameOptions &&
            before.favourite != after.favourite) {
            emit_sound(after.favourite ? MenuCue::Favourite : MenuCue::Unfavourite);
            return;
        }
        if (before.toggles != after.toggles) {
            emit_sound((after.toggles & ~before.toggles) ? MenuCue::Enable : MenuCue::Disable);
            return;
        }
        if (before.page != after.page || before.home != after.home || before.picker != after.picker) {
            bool back = sound_code == 3 || view_route == ViewTravel::Back;
            emit_sound(back ? MenuCue::Back : before.picker && !after.picker ? MenuCue::Confirm : MenuCue::Open);
            return;
        }
        if (before.view != after.view || before.layout != after.layout) {
            emit_sound(MenuCue::Open, direction);
            return;
        }
        if (before.volume != after.volume || before.brightness != after.brightness) {
            emit_sound(MenuCue::Adjust, direction);
            return;
        }
        if (before.text != after.text) {
            emit_sound(MenuCue::Type);
            return;
        }
        if (before.game != after.game || before.focus != after.focus || before.pick != after.pick ||
            before.row != after.row || before.col != after.col)
            emit_sound(MenuCue::Navigate, direction);
    }
    struct SoundAction {
        App& app;
        SoundAction(App& value, int code) : app(value) {
            if (!app.sound_depth++) {
                app.sound_before = app.sound_state();
                app.sound_code = code;
                app.explicit_sound.reset();
            } else if (code == 3) app.sound_code = 3;
        }
        ~SoundAction() { app.finish_sound_action(); }
    };
    void menu_sounds_action(int code) {
        if (code == 3) {
            settings.back();
            dirty = true;
            return;
        }
        if (code == 8 || code == 9) {
            settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 4);
            dirty = true;
            return;
        }
        if (code != 0 && code != 1 && code != 2 && code != 4) return;
        if (settings.focus == 0) {
            preferences.menu_sounds = (code == 2 || code == 4) ? !preferences.menu_sounds : code == 1;
            remember_appearance();
        } else if (settings.focus == 1) {
            if (code == 0 || code == 1) {
                preferences.sound_volume = std::clamp(preferences.sound_volume + (code == 0 ? -5 : 5), 0, 100);
                remember_appearance();
                sound_intent(MenuCue::Adjust);
            } else
                show_picker("Effects volume", {"Mute", "25%", "50%", "75%", "100%"},
                            (preferences.sound_volume + 12) / 25, [this](int n) {
                                preferences.sound_volume = n * 25;
                                remember_appearance();
                                sound_intent(MenuCue::Adjust);
                            });
        } else if (settings.focus == 2) {
            preferences.menu_music = (code == 2 || code == 4) ? !preferences.menu_music : code == 1;
            remember_appearance();
            sound_intent(preferences.menu_music ? MenuCue::Enable : MenuCue::Disable);
        } else if (settings.focus == 3) {
            if (code == 0 || code == 1) {
                preferences.music_volume = std::clamp(preferences.music_volume + (code == 0 ? -5 : 5), 0, 100);
                remember_appearance();
            } else
                show_picker("Music volume", {"20%", "40%", "60%", "80%", "100%"},
                            std::clamp((preferences.music_volume + 10) / 20 - 1, 0, 4), [this](int n) {
                                preferences.music_volume = (n + 1) * 20;
                                remember_appearance();
                            });
        } else {
            std::vector<std::string> names(menu_cue_labels.begin(), menu_cue_labels.end());
            show_picker("Preview sound", names, 0, [this](int n) { sound_intent(static_cast<MenuCue>(n)); });
        }
        dirty = true;
    }
    TouchDevice touchscreen;
    VolumeKeys volume_keys;
    HoldRepeat hold;
    Theme theme;
    Theme light_theme = default_theme(), dark_theme = default_dark_theme();
    RoundedFont rounded;
    Media media;
    ShaderPreview shader_preview;
    LibraryCache library_cache;
    Uint32 library_poll = 0;
    uint64_t rendered_frames = 0, render_file_reads = 0, render_stamp_checks = 0;
    UiTrace ui_trace;
    CollectionFilter collection_filter, search_preview;
    std::vector<Game> search_matches;
    bool search_preview_waiting = false, search_preview_loading = false;
    Uint32 search_preview_at = 0;
    LibrarySummary library_summary;
    LibrarySummary::Result summary;
    bool summary_ready = false;
    bool collection_loading = false;
    std::string collection_selection;
    Preferences preferences;
    Locale locale;
    Settings settings;
    SystemControls system;
    SystemStatus hardware;
    OverlayFrame overlay;
    bool embedded_menu = false;
    bool home_menu = true, scroll_drag = false, scroll_hover = false, list_touch = false, list_swipe = false,
         tap_pending = false;
    int gesture_scroll = 0, scroll_grab = 0;
    Uint32 scroll_cue_at = 0;
    bool picker_open = false, picker_detail = false, detail_drag = false, detail_rail = false;
    std::vector<std::string> detail_lines;
    int detail_scroll = 0;
    std::string picker_title;
    std::vector<std::string> picker_options;
    int picker_focus = 0, picker_selected = 0, picker_core_offset = -1;
    std::vector<int> picker_cores; // emulator picker rows -> core indices
    int picker_scroll = 0, picker_grab = 0, picker_scroll_start = 0;
    bool picker_rail = false, picker_swipe = false;
    Uint32 picker_at = 0, picker_closed_at = 0;
    bool picker_was_open = false;
    // The sheet leaves with a short slide; input already belongs to the page.
    bool picker_closing() const {
        return preferences.animations && !picker_open && picker_closed_at && s.GetTicks() - picker_closed_at < 140;
    }
    // Closed since the last frame, or still sliding away.
    bool sheet_leaving() const { return (picker_was_open && !picker_open) || picker_closing(); }
    std::function<void(int)> picker_apply;
    struct ToggleMotion {
        bool value = false;
        float from = 0;
        Uint32 at = 0;
    };
    std::map<std::pair<int, std::string>, ToggleMotion> toggle_motion;
    Uint32 micro_until = 0;
    // A segmented control's lit pill glides to the new choice instead of
    // jumping (140 ms ease-out), keyed by page and row like the switches.
    struct SegmentMotion {
        int value = -1;
        float from = 0;
        Uint32 at = 0;
    };
    std::map<std::pair<int, int>, SegmentMotion> segment_motion;
    float segment_position(int row, int value) {
        const auto now = s.GetTicks();
        auto& motion = segment_motion[{int(settings.page), row}];
        auto position = [&]() {
            float t = preferences.animations ? std::min(1.f, (now - motion.at) / 140.f) : 1.f;
            t = 1 - (1 - t) * (1 - t) * (1 - t);
            return motion.from + (float(motion.value) - motion.from) * t;
        };
        if (motion.value < 0) motion = {value, float(value), now - 200};
        else if (motion.value != value) {
            motion.from = position();
            motion.value = value;
            motion.at = now;
            micro_until = std::max(micro_until, now + 150);
        }
        return position();
    }
    enum class LibraryPress {
        None,
        Back,
        View,
        Options,
        Search,
        Clear,
        LeftArrow,
        RightArrow,
        Horizontal,
        Vertical,
        Row
    };
    LibraryPress library_press = LibraryPress::None;
    std::array<RailPress, 3> rail_press; // Left, right, captured thumb.
    bool rail_feedback_active = false;
    int library_press_row = -1;
    Uint32 library_press_at = 0;
    size_t home_selected = 1;
    std::vector<Game> switch_games;
    int list_scroll = 0, list_focus = -1;
    Page list_page = Page::Library;
    static constexpr int list_top = 96, list_height = 316, list_stride = 82;
    // Sound and brightness: the slider card's vertical span (touch and drawing agree).
    std::pair<int, int> slider_band() const {
        return settings.page == Page::Sound ? std::make_pair(96, 186) : std::make_pair(163, 285);
    }
    static int sound_row(int index) { return 198 + index * 74; }
    int list_count() const {
        switch (settings.page) {
        case Page::Settings: return 10;
        case Page::MenuSounds: return 5;
        case Page::Cheats: return int(cheat_rows.size());
        case Page::GameSwitcher: return int(switch_games.size()) + 1;
        case Page::Graphics: return 8;
        case Page::Power: return 6;
        case Page::LibraryOptions: return 11;
        case Page::Appearance: return 11;
        case Page::Scraper: return 5;
        case Page::DeviceInfo: return 5;
        case Page::System: return 8;
        case Page::GameOptions:
            if (other_system_selected()) return 7; // the rows every engine has; DS-only ones read Unavailable
            if (experimental_game()) return 8;
            return (game_preferences.core < 0 ? int(preferences.core) : game_preferences.core) == 0
                       ? (superdrastic_game() ? 10 : 9)
                       : 7;
        case Page::GameArtwork: return 4;
        case Page::Maintenance: return 7;
        case Page::Network: return 5;
        case Page::Startup: return 4;
        case Page::SaveGallery: return 9;
        case Page::Discover: return 4;
        case Page::MoreFilters: return 10;
        case Page::RomSetup: return 3;
        case Page::DSControls: return 5;
        case Page::EmulatorOptions: return o.game_core == "drastic" ? 12 : 10;
        case Page::ShaderPicker: return int(shader_choices.size());
        case Page::Controls: return 19;
        case Page::Wifi:
            return std::max(
                0, std::atoi(device_status.count("network_count") ? device_status.at("network_count").c_str() : "0"));
        case Page::Timezone: return int(timezones.size());
        case Page::PlayGraphics: return 4;
        default: return 0;
        }
    }
    // Settings > Library: "Show all systems" (row 10) is shown first, the others in their order below it.
    static int library_row(int shown) { return shown == 0 ? 10 : shown - 1; }
    int list_selected() const {
        return settings.page == Page::Settings ? section_row(settings.section) : settings.focus;
    }
    // Back: the row you left `target` from, kept within its current list.
    static constexpr SDL_Rect rail_left_bounds() { return {0, 409, RailGeometry::left, 71}; }
    static constexpr SDL_Rect rail_right_bounds() {
        return {RailGeometry::left + RailGeometry::width, 409, 640 - RailGeometry::left - RailGeometry::width, 71};
    }
    void back_to(Page target) {
        settings.page = target;
        int row = settings.recall(target);
        int count = list_count();
        settings.focus = count > 0 ? std::clamp(row, 0, count - 1) : row;
    }
    int list_max_scroll() const { return std::max(0, list_count() * list_stride - 16 - list_height); }
    void sync_list() {
        if (settings.page != list_page) {
            list_page = settings.page;
            list_scroll = 0;
            list_focus = -1;
        }
        int focus = list_selected();
        if (focus != list_focus) {
            list_focus = focus;
            int top = focus * list_stride, bottom = top + 66;
            if (top < list_scroll) list_scroll = top;
            if (bottom > list_scroll + list_height) list_scroll = bottom - list_height;
        }
        list_scroll = std::clamp(list_scroll, 0, list_max_scroll());
    }
    void scroll_to(int y) {
        int count = list_count(), content = count * list_stride - 16;
        int thumb =
            content > 0 ? std::max(54, list_height * list_height / std::max(list_height, content)) : list_height;
        list_scroll = std::clamp(int(std::round((y - list_top - scroll_grab) * double(list_max_scroll()) /
                                                std::max(1, list_height - thumb))),
                                 0, list_max_scroll());
        dirty = true;
    }
    // Home tiles: Settings, Games, Favourites, Recents, then the console's PictoChat.
    static constexpr size_t home_count = 6, pictochat_tile = 4, last_played_tile = 5;
    // The carousel shows five tiles; the DSi column adds the last played game.
    size_t home_tiles() const { return preferences.home_layout == 1 ? home_count : home_count - 1; }
    size_t navigation_count() const {
        return home_menu ? home_tiles() : systems_menu ? listed_systems.size() : model.games.size();
    }
    // The library's layout; the Systems step is always the carousel.
    int browse_layout() const { return systems_menu ? 0 : preferences.layout; }
    void home_layout_changed() {
        home_selected = std::min(home_selected, home_tiles() - 1);
        carousel.reset(navigation_selected());
        cancel_navigation();
    }
    size_t navigation_selected() const {
        return home_menu ? home_selected : systems_menu ? systems_selected : model.selected;
    }
    void select_navigation(size_t index, bool animate = true) {
        if (systems_menu) {
            systems_selected = std::min<size_t>(index, listed_systems.size() - 1);
            selected_at = s.GetTicks();
            if (animate && preferences.animations) carousel.select(systems_selected, selected_at);
            else carousel.reset(systems_selected);
            dirty = true;
            return;
        }
        if (!home_menu) {
            select_game(index, animate);
            return;
        }
        home_selected = std::min<size_t>(index, home_tiles() - 1);
        selected_at = s.GetTicks();
        if (animate && preferences.animations) carousel.select(home_selected, selected_at);
        else carousel.reset(home_selected);
        dirty = true;
    }
    // Pages where a held Left/Right (keys, d-pad, stick, L/R) repeats: the
    // Home rail, sliders and the shader carousel.
    bool horizontal_hold_page() const {
        return settings.page == Page::Library || settings.page == Page::Display || settings.page == Page::Sound ||
               settings.page == Page::ShaderPicker || settings.page == Page::Timezone;
    }
    void cancel_navigation() {
        dirty = true;
        touch_tail = false;
        options_ready = false;
        pointer_down = false;
        pointer_source = PointerSource::None;
        carousel.dragging = false;
        browser_list.moving = false;
        scroll_drag = false;
        picker_rail = picker_swipe = false;
        detail_drag = detail_rail = false;
        list_touch = list_swipe = tap_pending = false;
        browser_list_touch = browser_rail = false;
        shader_drag = false;
        shader_carousel.dragging = false;
        rail_press = {};
        hold.clear();
    }
    void load_library(std::vector<Game> games) {
        model.games = std::move(games);
        if (o.preview && model.games.empty())
            for (const char* name :
                 {"A little adventure", "Puzzle collection", "Racing club", "Garden stories", "Pocket exploration"})
                model.games.push_back({name, {}, true});
        ds_games = model.games;
        refresh_other_systems();
        library_ready = true;
        if (!o.game_menu) {
            summary_ready = false;
            library_summary.request(o.data, all_games);
        }
        device_status = fields(o.data / "device/status.ini");
        if (!o.game_menu && !o.boot_startup) library_view(preferences.view);
        if (o.game_menu) {
            if (std::find(std::begin(core_ids), std::end(core_ids), o.game_core) == std::end(core_ids))
                throw std::runtime_error("Unknown game engine");
            if (!o.game_rom.empty()) o.game_rom = fs::weakly_canonical(o.game_rom);
            model.games = {
                {o.game_rom.empty() ? "Preview game" : o.game_rom.stem().string(), o.game_rom, o.game_rom.empty()}};
            // Its system, for its icon and the Graphics it offers (a zipped game in RG DS Play is a GBA one).
            model.games[0].system = system_for_path(o.game_rom);
            if (model.games[0].system == "nds" && o.game_core == "rgds-play") model.games[0].system = "gba";
            model.selected = 0;
            home_menu = false;
            settings.page = Page::QuickMenu;
            menu_status = {{"state", "paused"}, {"checkpoint", o.preview ? "1" : "0"}};
            load_cheats();
        }
        if (o.game_switcher) {
            load_switcher();
            settings.page = Page::GameSwitcher;
        }
        restore_state();
        if (o.library || o.game_menu) home_menu = false;
        else if (o.home || o.boot_startup) {
            home_menu = true;
            settings.page = Page::Library;
        }
        home_selected = std::min(home_selected, home_tiles() - 1);
        carousel.reset(navigation_selected());
        browser_list.reveal(model.selected, model.games.size());
        theme_source = read_theme_source();
        if (o.settings) {
            settings.open();
            if (!o.section.empty()) {
                if (o.section == "scraper") open_scraper();
                else if (o.section == "updates") open_updates(Page::Settings);
                else {
                    static const std::array<std::string, 8> ids = {"theme",        "power",  "library",  "sound",
                                                                   "connectivity", "system", "controls", "statistics"};
                    auto found = std::find(ids.begin(), ids.end(),
                                           o.section == "brightness" || o.section == "motion" ? "theme" : o.section);
                    if (found == ids.end()) throw std::runtime_error("Unknown settings section");
                    settings.section = int(found - ids.begin());
                    settings.enter();
                    if (o.section == "brightness") settings.page = Page::Display;
                    else if (o.section == "motion") settings.page = Page::Motion;
                    else if (o.section == "controls") open_controls(false);
                }
            }
        }
    }
    void stop_library_scan() {
        if (library_child > 0) {
            kill(library_child, SIGTERM);
            for (int i = 0; i < 30; i++) {
                if (waitpid(library_child, nullptr, WNOHANG) != 0) {
                    library_child = -1;
                    break;
                }
                usleep(10000);
            }
            if (library_child > 0) {
                kill(library_child, SIGKILL);
                waitpid(library_child, nullptr, 0);
            }
        }
        library_child = -1;
        if (library_pipe >= 0) ::close(library_pipe);
        library_pipe = -1;
    }
    void begin_library_scan() {
        library_scan_started = true;
        if (o.roots.empty()) {
            load_library({});
            return;
        }
        int channel[2];
        if (pipe(channel) != 0) {
            status = "Could not start library scan";
            library_ready = true;
            return;
        }
        if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
            ::close(channel[0]);
            ::close(channel[1]);
            status = "Could not start library scan";
            library_ready = true;
            return;
        }
        std::vector<std::string> arguments = {o.executable.string(), "--scan-library"};
        for (const auto& root : o.roots) {
            arguments.push_back("--rom-dir");
            arguments.push_back(root.string());
        }
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        library_child = fork();
        if (library_child == 0) {
            ::close(channel[0]);
            if (dup2(channel[1], STDOUT_FILENO) < 0) _exit(127);
            ::close(channel[1]);
            execv(argv[0], argv.data());
            _exit(127);
        }
        ::close(channel[1]);
        if (library_child < 0) {
            ::close(channel[0]);
            status = "Could not start library scan";
            library_ready = true;
            return;
        }
        library_pipe = channel[0];
    }
    void poll_library_scan() {
        if (library_pipe < 0) return;
        char bytes[4096];
        ssize_t count;
        while ((count = read(library_pipe, bytes, sizeof(bytes))) > 0) {
            library_bytes.append(bytes, size_t(count));
            if (library_bytes.size() > 64 * 1024 * 1024) {
                stop_library_scan();
                status = "Library scan exceeded its limit";
                library_ready = true;
                return;
            }
        }
        if (count == 0) library_eof = true;
        else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            stop_library_scan();
            status = "Could not read library scan";
            library_ready = true;
            return;
        }
        if (library_child > 0) {
            int result = 0;
            auto done = waitpid(library_child, &result, WNOHANG);
            if (done == 0) return;
            if (done < 0 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
                stop_library_scan();
                status = "Library scan failed";
                library_ready = true;
                return;
            }
            library_child = 0;
        }
        if (!library_eof) return;
        ::close(library_pipe);
        library_pipe = -1;
        library_child = -1;
        std::vector<Game> games;
        size_t at = 0;
        while (at < library_bytes.size()) {
            auto path_end = library_bytes.find('\0', at);
            if (path_end == std::string::npos) break;
            auto title_end = library_bytes.find('\0', path_end + 1);
            if (title_end == std::string::npos) break;
            games.push_back({library_bytes.substr(path_end + 1, title_end - path_end - 1),
                             fs::path(library_bytes.substr(at, path_end - at)), false});
            at = title_end + 1;
        }
        library_bytes.clear();
        auto previous = settings;
        bool previous_home = home_menu;
        auto previous_category = home_selected;
        load_library(std::move(games));
        if (o.boot_startup) {
            settings = previous;
            home_menu = previous_home;
            home_selected = previous_category;
            if (!home_menu) library_view(preferences.view);
            carousel.reset(navigation_selected());
        } else if (scan_stay) {
            if (!previous_home) library_view(preferences.view);
            settings = previous;
            home_menu = previous_home;
            home_selected = previous_category;
            carousel.reset(navigation_selected());
        }
        scan_stay = false;
        if (!o.roots.empty()) start_worker("index");
        dirty = true;
    }
    void start_boot_session() {
        if (session_initialized) return;
        load_system_configuration();
        if (!preferences.pixel) check_fonts();
        shaders =
            load_shaders(o.app.empty() ? fs::path("assets/shaders/catalog.tsv") : o.app / "assets/shaders/catalog.tsv");
        cancel_navigation();
        boot_touch_candidate = false;
        session_initialized = true;
        settings.page = Page::Library;
        home_menu = !o.library;
        selected_at = page_at = s.GetTicks();
        if (o.preview) {
            refresh_system();
            boot_hardware_ready = true;
        } else system.request_status();
        begin_library_scan();
        dirty = true;
    }
    void continue_boot() {
        if (settings.page == Page::BootWarning && boot_displayed) start_boot_session();
    }
    void snapshot_boot_input() {
        if (!o.boot_startup || session_initialized) return;
        auto pump = reinterpret_cast<decltype(&SDL_PumpEvents)>(dlsym(s.handle, "SDL_PumpEvents"));
        if (pump) pump();
        auto keys = reinterpret_cast<decltype(&SDL_GetKeyboardState)>(dlsym(s.handle, "SDL_GetKeyboardState"));
        if (keys) {
            int length = 0;
            auto state = keys(&length);
            for (auto item : {std::make_pair(SDLK_a, SDL_SCANCODE_A), std::make_pair(SDLK_RETURN, SDL_SCANCODE_RETURN)})
                if (state && item.second < length && state[item.second]) boot_blocked_keys.insert(item.first);
        }
        auto mouse = reinterpret_cast<decltype(&SDL_GetMouseState)>(dlsym(s.handle, "SDL_GetMouseState"));
        if (mouse && (mouse(nullptr, nullptr) & SDL_BUTTON_LMASK)) boot_touch_wait_release = true;
        auto touch_count =
            reinterpret_cast<decltype(&SDL_GetNumTouchDevices)>(dlsym(s.handle, "SDL_GetNumTouchDevices"));
        auto touch_id = reinterpret_cast<decltype(&SDL_GetTouchDevice)>(dlsym(s.handle, "SDL_GetTouchDevice"));
        auto fingers = reinterpret_cast<decltype(&SDL_GetNumTouchFingers)>(dlsym(s.handle, "SDL_GetNumTouchFingers"));
        if (touch_count && touch_id && fingers)
            for (int i = 0; i < touch_count(); i++)
                if (fingers(touch_id(i)) > 0) boot_touch_wait_release = true;
        boot_touch_wait_release = boot_touch_wait_release || touchscreen.initially_pressed();
        auto update = reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle, "SDL_JoystickUpdate"));
        auto button = reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle, "SDL_JoystickGetButton"));
        auto instance = reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle, "SDL_JoystickInstanceID"));
        if (update) update();
        const char* a = std::getenv("RGDS_BUTTON_A");
        int accept = a ? std::atoi(a) : 0;
        if (button && instance)
            for (auto* joystick : joysticks)
                for (int index : {accept, 7}) {
                    bool mainline = o.mainline && mainline_pads.count(instance(joystick));
                    int raw = mainline ? (index == 7 ? 9 : 1) : index;
                    if (button(joystick, raw)) boot_blocked_buttons.insert({instance(joystick), Uint8(index)});
                }
    }
    std::set<SDL_JoystickID> menu_held_hats;
    // A fresh pause menu would read the game's held controls as new presses:
    // SDL posts every joystick button already down on its first update, and a
    // stylus resting on the panel decodes as a new contact. A held A (accelerate),
    // B, Start, Select or Menu, or a stylus over Resume, would close the menu at
    // once, so each control held at open is ignored until it is released.
    void snapshot_menu_input() {
        if (!o.game_menu) return;
        std::string held;
        if (touchscreen.initially_pressed()) {
            touchscreen.hold_initial_contact();
            held = " touch";
        }
        auto update = reinterpret_cast<decltype(&SDL_JoystickUpdate)>(dlsym(s.handle, "SDL_JoystickUpdate"));
        auto count = reinterpret_cast<decltype(&SDL_JoystickNumButtons)>(dlsym(s.handle, "SDL_JoystickNumButtons"));
        auto button = reinterpret_cast<decltype(&SDL_JoystickGetButton)>(dlsym(s.handle, "SDL_JoystickGetButton"));
        auto hat = reinterpret_cast<decltype(&SDL_JoystickGetHat)>(dlsym(s.handle, "SDL_JoystickGetHat"));
        auto instance = reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle, "SDL_JoystickInstanceID"));
        if (update && count && button && hat && instance) {
            update();
            for (auto* joystick : joysticks) {
                auto id = instance(joystick);
                bool mainline = o.mainline && mainline_pads.count(id);
                for (int raw = 0; raw < count(joystick); raw++)
                    if (button(joystick, raw)) {
                        int index = mainline ? MainlineControls::button(raw) : raw;
                        if (index >= 0) {
                            boot_blocked_buttons.insert({id, Uint8(index)});
                            held += " button " + std::to_string(index);
                        }
                    }
                if (hat(joystick, 0) != SDL_HAT_CENTERED) {
                    menu_held_hats.insert(id);
                    held += " hat";
                }
            }
        }
        if (!held.empty())
            std::cout << "RGDS menu helper: held at open:" << held << "; ignored until released" << std::endl;
    }
    // The column's top tile: straight back into the last game.
    void open_last_played() {
        const auto& last = summary.groups[0].last_game;
        if (!summary_ready || !last) {
            status = summary_ready ? "Nothing played yet" : "Preparing…";
            dirty = true;
            return;
        }
        library_view(0);
        remember_appearance();
        // The game starts over Home itself. Games opens instead only while a
        // very large card is still being listed, or if the game has left it.
        if (!collection_loading)
            for (size_t i = 0; i < model.games.size(); i++)
                if (model.games[i].path == last->path) {
                    select_game(i, false);
                    carousel.reset(home_selected); // the rail is Home's while Home is up
                    launch(true);
                    return;
                }
        home_menu = false;
    }
    void open_category() {
        cancel_navigation();
        if (home_selected == last_played_tile) open_last_played();
        else if (home_selected == pictochat_tile) open_pictochat();
        else if (home_selected == 0) settings.open();
        else if (home_selected == 1 && preferences.show_systems) open_systems();
        else {
            home_menu = false;
            library_view(int(home_selected) - 1);
            remember_appearance();
        }
        dirty = true;
    }
    // ---- Show all systems (Settings > Library): other systems' games are played by
    // RG DS Play. Games opens the Systems page; Favourites and Recents span every system.
    bool other_system_selected() const {
        return !o.game_menu && model.selected < model.games.size() && model.games[model.selected].system != "nds";
    }
    void refresh_other_systems() {
        all_games = ds_games;
        listed_systems = {"nds"};
        if (preferences.show_systems && !o.game_menu)
            for (const auto& info : systems()) {
                if (std::string(info.id) == "nds") continue;
                std::vector<fs::path> roots;
                for (const auto& [id, path] : o.system_roots)
                    if (id == info.id) roots.push_back(path);
                auto games = scan_system(roots, info.id);
                if (games.empty()) continue;
                listed_systems.push_back(info.id);
                all_games.insert(all_games.end(), games.begin(), games.end());
            }
        if (std::find(listed_systems.begin(), listed_systems.end(), shown_system) == listed_systems.end())
            shown_system = "nds";
    }
    std::vector<Game> view_source(int view) const {
        if (view != 0 || !preferences.show_systems) return all_games;
        std::vector<Game> games;
        for (const auto& game : all_games)
            if (game.system == shown_system) games.push_back(game);
        return games;
    }
    size_t system_games(const std::string& id) const {
        return size_t(std::count_if(all_games.begin(), all_games.end(), [&](const Game& g) { return g.system == id; }));
    }
    void open_systems() {
        cancel_navigation();
        settings.page = Page::Library;
        home_menu = false;
        systems_menu = true;
        auto at = std::find(listed_systems.begin(), listed_systems.end(), shown_system);
        systems_selected = at == listed_systems.end() ? 0 : size_t(at - listed_systems.begin());
        // Each card's numbers, as Home's collections show theirs.
        system_stats.assign(listed_systems.size(), {});
        for (const auto& game : all_games)
            for (size_t i = 0; i < listed_systems.size(); i++)
                if (game.system == listed_systems[i] && !game.demo) {
                    const auto& played = library_cache.stats(game, true);
                    system_stats[i].games++;
                    system_stats[i].sessions += played.sessions;
                    system_stats[i].seconds += played.seconds;
                }
        selected_at = s.GetTicks();
        carousel.reset(systems_selected);
        status.clear();
        dirty = true;
    }
    void choose_system(size_t index) {
        if (index >= listed_systems.size()) return;
        shown_system = listed_systems[index];
        systems_menu = false;
        library_view(0);
        remember_appearance();
        dirty = true;
    }
    // A system's tile: a cartridge and its badge, drawn in the space a game's icon takes.
    void system_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
        const auto& info = system_info(index < listed_systems.size() ? listed_systems[index] : "nds");
        ui_icon(r, SettingsIcon::Cartridge, x + size / 2, y + size * 2 / 5, size * 5 / 8, "accent");
        const std::string badge = info.badge;
        raw_label(r, badge, x + (size - label_width(badge, 16, false)) / 2, y + size * 3 / 4, 16, "ink", size);
    }
    // RG DS Play's view of the running game, as the runtime stores it beside game.ini.
    fs::path play_view_path() { return item_folder(0) / "play.ini"; }
    void load_play_view() {
        auto values = fields(play_view_path());
        static const std::array<std::string, 3> fits = {"sharp", "integer", "smooth"},
                                                seconds = {"companion", "off", "swap"};
        play_fit = int(std::find(fits.begin(), fits.end(), values["fit"]) - fits.begin()) % 3;
        play_second = int(std::find(seconds.begin(), seconds.end(), values["second"]) - seconds.begin()) % 3;
        auto choices = play_frame_choices();
        play_frame =
            std::find(choices.begin(), choices.end(), values["frame"]) == choices.end() ? "off" : values["frame"];
    }
    // What RG DS Play can draw around the game (play/src/ui/frame.hpp), by their play.ini names: its own
    // styles, then the owner's picture frames, userdata/frames/<system>/<name>.png.
    static constexpr std::array<const char*, 9> play_frames = {"off", "indigo",  "glacier",   "black",      "platinum",
                                                               "bar", "sp-onyx", "sp-cobalt", "sp-platinum"};
    static constexpr std::array<const char*, 9> play_frame_names = {
        "Off", "Indigo", "Glacier", "Black", "Platinum", "Black bar", "SP Onyx", "SP Cobalt", "SP Platinum"};
    fs::path play_frames_folder() {
        const auto& game = model.games.at(0);
        std::string system = game.system;
        if (system == "nds") system = system_for_path(game.path);
        return o.data / "frames" / (system == "nds" ? "gba" : system);
    }
    std::vector<std::string> play_frame_choices() {
        std::vector<std::string> choices(play_frames.begin(), play_frames.end()), pictures;
        std::error_code ec;
        for (fs::directory_iterator it(play_frames_folder(), ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().stem().string();
            if (lower(it->path().extension().string()) == ".png" && it->is_regular_file(ec) && !name.empty() &&
                name.size() <= 64 && name[0] != '.' && std::find(choices.begin(), choices.end(), name) == choices.end())
                pictures.push_back(name);
        }
        std::sort(pictures.begin(), pictures.end());
        choices.insert(choices.end(), pictures.begin(), pictures.end());
        return choices;
    }
    std::string play_frame_label(const std::string& frame) const {
        auto found = std::find(play_frames.begin(), play_frames.end(), frame);
        return found == play_frames.end() ? frame : play_frame_names[size_t(found - play_frames.begin())];
    }
    void apply_play_view(int fit, int second, const std::string& frame) {
        if (fit == play_fit && second == play_second && frame == play_frame) return;
        if (o.game_menu && !o.preview) {
            menu_request("play-graphics:" + std::to_string(fit) + ":" + std::to_string(second) + ":" + frame);
            return;
        }
        static const std::array<const char*, 3> fits = {"sharp", "integer", "smooth"},
                                                seconds = {"companion", "off", "swap"};
        std::error_code ec;
        fs::create_directories(play_view_path().parent_path(), ec);
        auto pending = fs::path(play_view_path().string() + ".pending");
        std::ofstream(pending) << "schema=1\nfit=" << fits[size_t(fit)] << "\nsecond=" << seconds[size_t(second)]
                               << "\nframe=" << frame << '\n';
        fs::rename(pending, play_view_path(), ec);
        play_fit = fit;
        play_second = second;
        play_frame = frame;
        status = "Graphics applied (preview).";
        dirty = true;
    }
    const char* play_fit_name(int fit) const {
        return fit == 1 ? "Whole pixels" : fit == 2 ? "Smooth fill" : "Sharp fill";
    }
    const char* play_second_name(int second) const {
        return second == 1 ? "Off" : second == 2 ? "Swap screens" : "Companion";
    }
    void play_graphics_action() {
        if (settings.focus == 0)
            show_picker("Game screen", {"Sharp fill", "Whole pixels", "Smooth fill"}, play_fit,
                        [this](int n) { apply_play_view(n, play_second, play_frame); });
        else if (settings.focus == 1)
            show_picker("Second screen", {"Companion", "Off", "Swap screens"}, play_second,
                        [this](int n) { apply_play_view(play_fit, n, play_frame); });
        else if (settings.focus == 2) {
            auto choices = play_frame_choices();
            std::vector<std::string> labels;
            for (const auto& choice : choices) labels.push_back(play_frame_label(choice));
            int selected = int(std::find(choices.begin(), choices.end(), play_frame) - choices.begin());
            show_picker("Frame", labels, selected < int(choices.size()) ? selected : 0, [this, choices](int n) {
                if (n >= 0 && n < int(choices.size())) apply_play_view(play_fit, play_second, choices[size_t(n)]);
            });
        } else {
            // The same shader carousel as the DS engines: the bridge's pass draws GBA and Game Boy screens too.
            open_shader_picker(true);
            shader_return = Page::PlayGraphics;
        }
    }
    void return_home() {
        view_route = ViewTravel::Back;
        cancel_navigation();
        home_menu = true;
        home_selected = size_t(preferences.view) + 1;
        carousel.reset(home_selected);
        selected_at = s.GetTicks();
        dirty = true;
    }
    SystemPreferences system_preferences;
    bool system_configuration_loaded = false;
    Uint32 last_activity = 0, system_poll = 0, system_retry = 0, lid_poll = 0, brightness_changed = 0,
           connection_poll = 0;
    int brightness_applied = 60;
    bool brightness_unsaved = false;
    int volume_applied = 25;
    bool volume_unsaved = false;
    pid_t return_control_child = -1;
    std::optional<int> return_volume, return_brightness, return_power, return_lid, active_return_volume,
        active_return_brightness;
    bool return_status_needed = false, return_audio_failed = false;
    bool return_writes_pending() const {
        return return_control_child > 0 || return_volume || return_brightness || return_power || return_lid;
    }
    void begin_return_controls() {
        if (return_control_child > 0 || system.volume_pending() || system.brightness_pending()) return;
        if (!return_volume && !return_brightness && !return_power && !return_lid) return;
        std::vector<std::string> arguments = {o.executable.string(),
                                              "--restore-controls",
                                              o.app.string(),
                                              o.data.string(),
                                              std::to_string(return_volume.value_or(-1)),
                                              std::to_string(return_brightness.value_or(-1)),
                                              std::to_string(return_power.value_or(-1)),
                                              std::to_string(return_lid.value_or(-1))};
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        active_return_volume = return_volume;
        active_return_brightness = return_brightness;
        return_volume.reset();
        return_brightness.reset();
        return_power.reset();
        return_lid.reset();
        system.discard_status();
        return_control_child = fork();
        if (return_control_child == 0) {
            execv(argv[0], argv.data());
            _exit(127);
        }
        if (return_control_child < 0) {
            return_audio_failed = active_return_volume.has_value();
            status = "ERROR: Could not restore settings";
        }
    }
    void poll_return_controls(bool wait = false) {
        if (return_control_child > 0) {
            int result = 0;
            pid_t completed;
            do {
                completed = waitpid(return_control_child, &result, wait ? 0 : WNOHANG);
            } while (completed < 0 && errno == EINTR);
            if (completed == 0) return;
            return_control_child = -1;
            int failure = completed > 0 && WIFEXITED(result) ? WEXITSTATUS(result) : 127;
            if (active_return_volume) {
                return_audio_failed = (failure & 1) != 0;
                if (!return_audio_failed) volume_applied = *active_return_volume;
            }
            if (active_return_brightness && !(failure & 2)) brightness_applied = *active_return_brightness;
            if (failure) status = "ERROR: Could not restore settings - see log";
            if (!return_volume && active_return_volume) {
                if (return_audio_failed) hardware.volume_percent = confirmed_volume();
                volume_unsaved = false;
            }
            if (!return_brightness && active_return_brightness) {
                if (failure & 2) {
                    system_preferences.brightness = brightness_applied;
                    hardware.brightness_percent = brightness_applied;
                }
                if (brightness_unsaved) {
                    try {
                        save_system_preferences(o.system_preferences, system_preferences);
                    } catch (const std::exception& error) {
                        status = error.what();
                    }
                }
                brightness_unsaved = false;
            }
            active_return_volume.reset();
            active_return_brightness.reset();
            dirty = true;
        }
        begin_return_controls();
        if (!wait && !return_writes_pending() && return_status_needed && system.request_status()) {
            return_status_needed = false;
            system_poll = s.GetTicks();
        }
    }
    void flush_return_controls() {
        while (return_writes_pending()) {
            poll_return_controls(true);
            if (return_control_child <= 0 && (system.volume_pending() || system.brightness_pending())) {
                system.flush_volume();
                system.flush_brightness();
            }
        }
    }
    LidSleep lid_sleep;
    bool font_ready = true, font_checked = false;
    void check_fonts(bool first_frame = false) {
        if (!font_ready || font_checked) return;
        try {
            if (first_frame) {
                if (o.boot_warning) rounded.check({16, 20, 28, 36});
                else rounded.check({16, 20, 28});
            } else {
                rounded.check();
                font_checked = true;
            }
        } catch (const std::exception& e) {
            font_ready = false;
            preferences.pixel = true;
            std::cerr << "Font fallback: " << e.what() << std::endl;
        }
    }
    Uint32 selected_at = 0, page_at = 0, service_poll = 0, clock_poll = 0;
    Uint32 cover_motion_at = 0;   // Only a changed selection animates the top-screen cover.
    std::time_t drawn_second = 0; // the column's clock ticks once a second
    std::string drawn_status_stamp;
    int drawn_charge_phase = -1;
    pid_t worker = -1;
    bool worker_ok = false;
    Uint32 input_guard_until = 0;
    std::string worker_action, username, password, edit_buffer, service_notice;
    int edit_field = 0, key_row = 0, key_col = 0, key_layer = 0;
    bool key_caps = false;
    size_t edit_cursor = 0;
    bool service_loaded = false;
    std::map<std::string, std::string> service;
    bool pointer_down = false;
    enum class EdgeGesture { None, Bottom, Left };
    EdgeGesture edge_gesture = EdgeGesture::None;
    void gesture_intro() {
        remember_appearance();
        show_detail(
            "Touch gestures",
            locale.text("Swipe up from the bottom edge to go Home from anywhere.") + "\n" +
                locale.text("Swipe in from the left edge to go Back.") + "\n" +
                locale.text(
                    "A small handle at the bottom of the lower screen shows gestures are on; it rises with your finger, and a ring marks the spot when a gesture fires."));
    }
    Uint32 gesture_flash_at = 0;
    int gesture_flash_kind = 0;
    // The handle at the bottom edge, the Home pill that rises with the finger,
    // the Back disc that slides in from the left, and a ring where one fired.
    void gesture_cues(SDL_Renderer* r, int y) {
        if (!gestures_available()) return;
        const Uint32 now = s.GetTicks();
        const bool bottom = pointer_down && edge_gesture == EdgeGesture::Bottom,
                   left = pointer_down && edge_gesture == EdgeGesture::Left;
        const double pb = bottom ? std::clamp((down_y - contact_y) / 72.0, 0.0, 1.0) : 0,
                     pl = left ? std::clamp((contact_x - down_x) / 72.0, 0.0, 1.0) : 0;
        {
            const int w = 72 + int(48 * pb), lift = int(26 * pb);
            alpha_round(r, 320 - w / 2, y + 471 - lift, w, 5, 2, theme.at(pb > 0 ? "accent" : "ink"),
                        int((preferences.dark ? 90 : 110) + 140 * pb));
            if (pb > .2) {
                const std::string text = locale.text("Home");
                const int pw = label_width(text, 16, false) + 52;
                alpha_round(r, 320 - pw / 2, y + 428 - lift, pw, 34, 17, theme.at("accent"),
                            int(250 * std::min(1.0, (pb - .2) * 2)));
                if (pb > .5) {
                    ui_icon(r, SettingsIcon::Home, 320 - pw / 2 + 20, y + 445 - lift, 18, "on_accent");
                    raw_label_colour(r, text, 320 - pw / 2 + 36, centred_label_y(text, 16, y + 445 - lift), 16,
                                     theme.at("on_accent"), pw);
                }
            }
        }
        if (pl > 0) {
            const int cx = -44 + int(80 * pl);
            alpha_round(r, cx - 40, y + 200, 80, 80, 40, theme.at("accent"), int(250 * pl));
            if (pl > .4) ui_icon(r, SettingsIcon::Back, cx + 14, y + 240, 24, "on_accent");
        }
        if (preferences.animations && gesture_flash_at && now - gesture_flash_at < 260) {
            const double t = (now - gesture_flash_at) / 260.0;
            feedback_arc(r, gesture_flash_kind ? 24 : 320, y + (gesture_flash_kind ? 240 : 468), int(18 + 56 * t), 0, 1,
                         theme.at("accent"), int(220 * (1 - t)), 4);
        }
    }
    bool gestures_available() const {
        return preferences.gestures && !probe && !(chat_open && chat.view == pictochat::View::Room) && !launching &&
               settings.page != Page::BootWarning && settings.page != Page::Controls && !o.game_menu;
    }
    // A swipe that started on an edge and travelled far enough: act once, then swallow the rest of the touch.
    bool edge_gesture_move(int x, int y) {
        if (edge_gesture == EdgeGesture::None) return false;
        const bool home = edge_gesture == EdgeGesture::Bottom && down_y - y >= 72 && std::abs(x - down_x) < 120,
                   back = edge_gesture == EdgeGesture::Left && x - down_x >= 72 && std::abs(y - down_y) < 120;
        if (!home && !back) return false;
        edge_gesture = EdgeGesture::None;
        cancel_navigation();
        brightness_drag = volume_drag = false;
        gesture_flash_at = s.GetTicks();
        gesture_flash_kind = home ? 0 : 1;
        micro_until = gesture_flash_at + 270;
        if (home) {
            const bool chatting = chat_open;
            close_pictochat();
            picker_open = false;
            if (settings.page != Page::Library) {
                settings.page = Page::Library;
                settings.focus = 0;
            }
            if (!home_menu) return_home();
            if (!chatting) sound_intent(MenuCue::Back);
        } else action(3);
        dirty = true;
        return true;
    }
    enum class PointerSource { None, Mouse, SDL, Raw };
    PointerSource pointer_source = PointerSource::None;
    Uint32 raw_touch_at = 0, sdl_touch_at = 0;
    bool raw_touch_seen = false, sdl_touch_seen = false;
    SDL_FingerID active_finger = 0;
    int down_x = 0, down_y = 0;
    size_t pressed_game = 0;
    std::map<std::string, std::string> menu_status;
    std::map<std::string, std::string> device_status, update_status;
    bool update_health_sent = false;
    uint64_t update_health_at = 0, update_health_started = 0;
    bool device_loaded = false;
    std::string network_signature;
    int probed_signal = -1;
    Uint32 device_info_at = 0;
    // Spawn the full status helper only for a changed link, an open network
    // page or a five-minute safety refresh. Quality comes from the kernel.
    void refresh_network(Uint32 now) {
        connection_poll = now;
        auto probe = o.preview ? NetworkProbe{} : probe_network();
        bool details = settings.page == Page::Network || settings.page == Page::Wifi ||
                       settings.page == Page::DeviceInfo || settings.page == Page::Startup ||
                       settings.page == Page::Maintenance;
        if (!device_loaded || o.preview || details || probe.signature != network_signature ||
            now - device_info_at >= 300000) {
            start_worker("device.info");
            device_loaded = true;
            device_info_at = now;
            network_signature = probe.signature;
            probed_signal = -1;
            return;
        }
        int level = probe.signal >= 67   ? 3
                    : probe.signal >= 34 ? 2
                    : probe.signal > 0   ? 1
                                         : 0,
            previous = probed_signal >= 67   ? 3
                       : probed_signal >= 34 ? 2
                       : probed_signal > 0   ? 1
                                             : 0;
        if (probed_signal < 0 || level != previous) dirty = true;
        probed_signal = probe.signal;
    }
    std::string network_id, confirmation;
    std::string startup_mode = "stock";
    bool startup_warning = true;
    Page confirm_return = Page::System;
    Uint32 menu_poll = 0;
    unsigned menu_sequence = 0;
    std::string menu_pending_token;
    Uint32 menu_request_at = 0;
    void confirm(const std::string& operation, Page parent) {
        confirmation = operation;
        confirm_return = parent;
        settings.page = Page::Confirm;
        settings.focus = 0;
        dirty = true;
    }
    void save_startup() {
        std::ostringstream payload;
        payload << "{\"mode\":\"" << startup_mode << "\",\"warning\":" << (startup_warning ? "true" : "false") << '}';
        start_worker("device.startup", payload.str());
    }
    void change_startup(int direction, bool toggle) {
        if (settings.focus == 3) {
            save_experience("quick_resume", experience["quick_resume"] != "1");
            return;
        }
        if (o.independent) {
            if (settings.focus == 2) confirm("reboot", Page::Startup);
            else if (settings.focus == 1) {
                startup_mode = "rgds";
                startup_warning = toggle ? device_status["boot_warning"] != "1" : direction > 0;
                save_startup();
            }
            return;
        }
        if (worker > 0) return;
        startup_mode = device_status["boot_mode"] == "rgds" ? "rgds" : "stock";
        startup_warning = device_status["boot_warning"] == "1";
        if (settings.focus == 2) {
            confirm("reboot", Page::Startup);
            return;
        }
        if (settings.focus == 1) {
            startup_warning = toggle ? !startup_warning : direction > 0;
            save_startup();
            return;
        }
        auto previous = startup_mode;
        startup_mode = toggle ? (startup_mode == "stock" ? "rgds" : "stock") : direction > 0 ? "rgds" : "stock";
        if (startup_mode == previous) return;
        if (startup_mode == "rgds") confirm("startup-rgds", Page::Startup);
        else save_startup();
    }
    void wifi_choose() {
        if (worker > 0) return;
        auto prefix = "network_" + std::to_string(settings.focus) + "_";
        network_id = device_status[prefix + "id"];
        if (network_id.empty()) return;
        if (device_status[prefix + "connected"] == "1") {
            settings.page = Page::Network;
            service_notice = "Already connected.";
            dirty = true;
            return;
        }
        if (device_status[prefix + "saved"] == "1") {
            auto selected_network = network_id;
            show_picker("Saved network", {"Connect", "Enter password"}, 0, [this, selected_network](int selected) {
                network_id = selected_network;
                if (selected == 1) {
                    begin_edit(2);
                    return;
                }
                std::ostringstream payload;
                payload << "{\"network\":" << std::quoted(network_id) << ",\"use_saved\":true}";
                start_worker("device.connect", payload.str());
                settings.page = Page::Network;
                settings.focus = 1;
            });
            return;
        }
        if (device_status[prefix + "security"].find("802.1X") != std::string::npos) {
            service_notice = "This network requires an enterprise profile.";
            dirty = true;
            return;
        }
        if (device_status[prefix + "security"].empty()) {
            std::ostringstream payload;
            payload << "{\"network\":" << std::quoted(network_id) << ",\"password\":\"\"}";
            start_worker("device.connect", payload.str());
            settings.page = Page::Network;
        } else begin_edit(2);
        dirty = true;
    }
    std::string menu_input = "no input";
    void note_menu_input(const SDL_Event& e) {
        if (e.type == SDL_JOYBUTTONDOWN) menu_input = "joystick button " + std::to_string(e.jbutton.button);
        else if (e.type == SDL_JOYHATMOTION && e.jhat.value)
            menu_input = "joystick hat " + std::to_string(e.jhat.value);
        else if (e.type == SDL_KEYDOWN) menu_input = "key " + std::to_string(e.key.keysym.sym);
        else if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN) menu_input = "SDL pointer";
    }
    void menu_request(const std::string& action) {
        if (menu_status["state"] == "busy" || !menu_pending_token.empty()) {
            dirty = true;
            return;
        }
        if (o.preview && action.rfind("switch:", 0) == 0) {
            std::cout << "Game switch (preview): " << action << std::endl;
            menu_status["message"] = "Saving before switching games…";
            dirty = true;
            return;
        }
        if (action == "resume" && !departure_dispatch && preferences.menu_sounds) {
            if (emit_sound(MenuCue::Resume)) {
                explicit_sound.reset();
                departure_action = action;
                departure_at = s.GetTicks() + 180;
                return;
            }
        }
        menu_sound_action = action;
        if (action == "save" || action == "load" || action.rfind("slot-", 0) == 0) sound_intent(MenuCue::Confirm);
        // The supervisor logs <pid>-<n> tokens; this names the input behind each one.
        if (o.game_menu)
            std::cout << "RGDS menu helper: " << getpid() << '-' << menu_sequence + 1 << ' ' << action << " after "
                      << menu_input << " at " << s.GetTicks() << " ms" << std::endl;
        if (o.preview) {
            if (action == "save" || action.rfind("slot-save:", 0) == 0) sound_intent(MenuCue::Save);
            else if (action == "load" || action.rfind("slot-load:", 0) == 0) sound_intent(MenuCue::Load);
            if (action == "threaded:0" || action == "threaded:1") menu_status["threaded"] = action.substr(9);
            if (action == "fast-forward") menu_status["fast_forward"] = menu_status["fast_forward"] == "1" ? "0" : "1";
            menu_status["message"] = action == "save"                  ? "Checkpoint saved (preview)"
                                     : action == "load"                ? "Checkpoint loaded (preview)"
                                     : action.rfind("shader:", 0) == 0 ? "Shader change requested (preview)"
                                                                       : "";
            if (action.rfind("shader:", 0) == 0) std::cout << "Shader request (preview): " << action << std::endl;
            if (action == "resume" || action == "quit" || action == "force-quit" || action == "restart") quit = true;
            dirty = true;
            return;
        }
        auto path = o.data / "session/game-menu.request", pending = fs::path(path.string() + ".pending");
        fs::create_directories(path.parent_path());
        std::ofstream file(pending);
        auto request_token = std::to_string(getpid()) + '-' + std::to_string(++menu_sequence);
        file << "token=" << request_token << "\naction=" << action << '\n';
        file.close();
        if (!file) {
            status = "Could not send game action";
            dirty = true;
            return;
        }
        fs::rename(pending, path);
        if (action == "resume") quit = true;
        else {
            menu_pending_token = request_token;
            menu_request_at = s.GetTicks();
            menu_status["state"] = "busy";
            menu_status["message"] = action == "save"   ? "Saving checkpoint…"
                                     : action == "load" ? "Loading checkpoint…"
                                     : (action.rfind("graphics:", 0) == 0 || action.rfind("filter2d:", 0) == 0 ||
                                        action.rfind("adaptive:", 0) == 0)
                                         ? "Applying graphics…"
                                     : action.rfind("shader:", 0) == 0 ? "Applying shader…"
                                     : action == "restart"             ? "Saving before restarting…"
                                     : action == "quit"                ? "Returning home…"
                                                                       : "Working…";
        }
        dirty = true;
    }
    std::map<std::string, std::string> slots, experience;
    // The slot page is opened for one purpose: saving, loading, or (from the
    // library) exporting. That purpose names the page and the A button.
    enum class GalleryMode { Save, Load, Manage };
    GalleryMode gallery_mode = GalleryMode::Manage;
    Page gallery_return = Page::GameOptions, ds_return = Page::GameOptions, discover_return = Page::Library;
    int gallery_core = 1, discovery_filter = 0, resume_core = -1;
    std::string search_query;
    // Library filters from the game information (game_facets.hpp), and the counts that fill their pickers.
    Facets discovery_facets;
    FacetIndex facet_index;
    void open_more_filters();
    void facet_picker(int row);
    void reset_facets();
    Page search_return = Page::Discover;
    Uint32 automatic_update_at = 0;
    bool resume_checked = false, setup_checked = false, resume_launch = false;
    fs::path gallery_root() { return item_folder(model.selected) / "slots" / core_ids[gallery_core]; }
    void save_experience(const std::string& key, int value) {
        std::ostringstream body;
        body << "{\"action\":\"settings\",\"" << key << "\":" << value << '}';
        start_worker("device.library", body.str());
        experience[key] = std::to_string(value);
        dirty = true;
    }
    void open_gallery(GalleryMode mode = GalleryMode::Manage) {
        if (model.games.empty()) return;
        gallery_return = settings.page;
        gallery_mode = o.game_menu ? mode : GalleryMode::Manage;
        auto local = library_cache.options(model.games[model.selected]);
        gallery_core = o.game_menu               ? active_engine()
                       : other_system_selected() ? play_core
                       : local.core < 0          ? int(preferences.core)
                                                 : local.core;
        settings.page = Page::SaveGallery;
        settings.focus = 0;
        slots = fields(gallery_root() / "index.ini");
        dirty = true;
        if (!o.game_menu) slot_worker("slots", 0);
    }
    void slot_worker(const std::string& action, int slot, const std::string& file = "", bool overwrite = false) {
        std::ostringstream body;
        body << "{\"action\":" << std::quoted(action)
             << ",\"rom\":" << std::quoted(model.games[model.selected].path.string())
             << ",\"core\":" << std::quoted(core_ids[gallery_core]) << ",\"slot\":" << slot
             << ",\"file\":" << std::quoted(file) << ",\"overwrite\":" << (overwrite ? "true" : "false") << '}';
        start_worker("device.library", body.str());
    }
    // Checkpoint slots: the same three verbs on every slot. A does what the page
    // was opened for (save here, load, or export from the library), X deletes,
    // Y holds the rare actions. Anything that replaces progress asks once first,
    // with Cancel selected.
    const char* gallery_title() const {
        return gallery_mode == GalleryMode::Save   ? "Save checkpoint"
               : gallery_mode == GalleryMode::Load ? "Load checkpoint"
                                                   : "Save gallery";
    }
    SettingsIcon gallery_icon() const {
        return gallery_mode == GalleryMode::Load ? SettingsIcon::Load : SettingsIcon::Save;
    }
    bool slot_occupied(int slot) const {
        auto found = slots.find("slot_" + std::to_string(slot));
        return found != slots.end() && !found->second.empty();
    }
    bool slot_primary_enabled(int slot) const { return gallery_mode == GalleryMode::Save || slot_occupied(slot); }
    const char* slot_primary_name() const {
        return gallery_mode == GalleryMode::Save ? "Save here" : gallery_mode == GalleryMode::Load ? "Load" : "Export";
    }
    const char* slot_hint(bool occupied) const {
        if (gallery_mode == GalleryMode::Save)
            return occupied ? "A replaces this slot with your current progress. X deletes it."
                            : "A saves your current progress here.";
        if (gallery_mode == GalleryMode::Load)
            return occupied ? "A loads this slot. Undo last load, under Y, brings back the progress you have now."
                            : "Nothing to load here yet.";
        return occupied ? "A exports this slot to userdata/exports. X deletes it." : "Y imports a save into this slot.";
    }
    std::string slot_question(const char* verb, int slot) const {
        return std::string(verb) + " slot " + std::to_string(slot + 1) + "?";
    }
    void slot_primary() {
        const int slot = settings.focus;
        const bool occupied = slot_occupied(slot);
        if (gallery_mode == GalleryMode::Save) {
            if (!occupied) {
                menu_request("slot-save:" + std::to_string(slot));
                return;
            }
            show_picker(slot_question("Replace", slot), {"Cancel", "Replace"}, 0, [this, slot](int yes) {
                if (yes) menu_request("slot-save:" + std::to_string(slot) + ":yes");
            });
        } else if (gallery_mode == GalleryMode::Load) {
            if (occupied)
                show_picker(slot_question("Load", slot), {"Cancel", "Load"}, 0, [this, slot](int yes) {
                    if (yes) menu_request("slot-load:" + std::to_string(slot));
                });
        } else if (occupied) slot_worker("export", slot);
    }
    void slot_delete() {
        const int slot = settings.focus;
        if (!slot_occupied(slot)) return;
        show_picker(slot_question("Delete", slot), {"Cancel", "Delete"}, 0, [this, slot](int yes) {
            if (!yes) return;
            if (o.game_menu) menu_request("slot-delete:" + std::to_string(slot));
            else slot_worker("delete", slot);
        });
    }
    void slot_import(int slot) {
        std::vector<std::string> names;
        std::error_code ec;
        auto folder = o.data / "imports";
        if (fs::exists(folder))
            for (const auto& file : fs::directory_iterator(folder, ec)) {
                auto name = file.path().filename().string();
                if (file.is_regular_file() && name.size() > 12 && name.substr(name.size() - 12) == ".rgdssave.zip" &&
                    names.size() < 100)
                    names.push_back(name);
            }
        std::sort(names.begin(), names.end());
        if (names.empty()) {
            show_detail(
                "Import slot",
                locale.text(
                    "Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));
            return;
        }
        const bool occupied = slot_occupied(slot);
        show_picker("Import slot", names, 0, [this, names, slot, occupied](int n) {
            auto name = names.at(n);
            if (occupied)
                show_picker(slot_question("Replace", slot), {"Cancel", "Replace"}, 0, [this, name, slot](int yes) {
                    if (yes) slot_worker("import", slot, name, true);
                });
            else slot_worker("import", slot, name);
        });
    }
    void slot_more() {
        const int slot = settings.focus;
        std::vector<std::string> actions;
        if (o.game_menu) {
            if (slot_occupied(slot)) actions.push_back("Export slot");
            actions.push_back("Undo last slot change");
            actions.push_back("Undo last load");
        } else actions = {"Import into this slot", "Undo last slot change", "Import help"};
        show_picker(locale.text("Slot") + " " + std::to_string(slot + 1), actions, 0, [this, slot, actions](int choice) {
            const auto& action = actions.at(choice);
            if (action == "Export slot") menu_request("slot-export:" + std::to_string(slot));
            else if (action == "Undo last slot change") {
                if (o.game_menu) menu_request("slot-undo");
                else slot_worker("undo", slot);
            } else if (action == "Undo last load") menu_request("slot-undo-load");
            else if (action == "Import into this slot") slot_import(slot);
            else
                show_detail(
                    "Import help",
                    locale.text(
                        "Copy .rgdssave.zip files into userdata/imports. Imports must match this game and emulator."));
        });
    }
    pid_t microphone_worker = -1;
    int microphone_pipe = -1, microphone_level = 0, microphone_peak = 0, microphone_db = -60;
    std::string microphone_bytes, microphone_state;
    Uint32 microphone_update = 0;
    void stop_microphone() {
        if (microphone_worker > 0) {
            kill(-microphone_worker, SIGTERM);
            kill(microphone_worker, SIGTERM);
            for (int i = 0; i < 30; i++) {
                if (waitpid(microphone_worker, nullptr, WNOHANG) != 0) {
                    microphone_worker = -1;
                    break;
                }
                usleep(10000);
            }
            if (microphone_worker > 0) {
                kill(microphone_worker, SIGKILL);
                waitpid(microphone_worker, nullptr, 0);
                microphone_worker = -1;
            }
        }
        if (microphone_pipe >= 0) {
            ::close(microphone_pipe);
            microphone_pipe = -1;
        }
        microphone_bytes.clear();
    }
    void open_microphone() {
        if (o.game_menu) {
            show_detail("Test microphone", locale.text("Return home before testing the physical microphone."));
            return;
        }
        stop_microphone();
        microphone_level = microphone_peak = 0;
        microphone_db = -60;
        microphone_state = o.preview ? "Test the microphone on your handheld." : "Listening…";
        settings.page = Page::Microphone;
        settings.focus = 0;
        configure_sounds();
        dirty = true;
        if (o.preview) return;
        int channel[2];
        if (pipe(channel) != 0) {
            microphone_state = "Microphone unavailable";
            return;
        }
        auto helper = (o.app / "runtime/microphone_test.py").string(), data = o.data.string();
        auto child = fork();
        if (child == 0) {
            setpgid(0, 0);
            ::close(channel[0]);
            dup2(channel[1], STDOUT_FILENO);
            ::close(channel[1]);
            execlp("python3", "python3", "-B", helper.c_str(), "--data", data.c_str(), nullptr);
            _exit(127);
        }
        ::close(channel[1]);
        if (child < 0) {
            ::close(channel[0]);
            microphone_state = "Microphone unavailable";
            return;
        }
        setpgid(child, child);
        microphone_worker = child;
        microphone_pipe = channel[0];
        fcntl(microphone_pipe, F_SETFL, fcntl(microphone_pipe, F_GETFL) | O_NONBLOCK);
        fcntl(microphone_pipe, F_SETFD, FD_CLOEXEC);
        microphone_update = s.GetTicks();
    }
    void poll_microphone(Uint32 now) {
        if (settings.page != Page::Microphone) {
            if (microphone_worker > 0 || microphone_pipe >= 0) stop_microphone();
            return;
        }
        if (microphone_pipe < 0) return;
        char buffer[512];
        ssize_t size;
        while ((size = read(microphone_pipe, buffer, sizeof(buffer))) > 0) microphone_bytes.append(buffer, size);
        size_t newline;
        while ((newline = microphone_bytes.find('\n')) != std::string::npos) {
            auto line = microphone_bytes.substr(0, newline);
            microphone_bytes.erase(0, newline + 1);
            std::istringstream values(line);
            int level, peak, db;
            if (values >> level >> peak >> db) {
                microphone_level = std::clamp(level, 0, 100);
                microphone_peak = std::clamp(peak, 0, 100);
                microphone_db = std::clamp(db, -60, 0);
                microphone_state = "Listening…";
                microphone_update = now;
            } else {
                microphone_state =
                    line == "busy" ? "Return home before testing the physical microphone." : "Microphone unavailable";
                microphone_level = microphone_peak = 0;
            }
            dirty = true;
        }
        if (microphone_bytes.size() > 4096) microphone_bytes.clear();
        if (microphone_worker > 0 && waitpid(microphone_worker, nullptr, WNOHANG) != 0) {
            microphone_worker = -1;
            ::close(microphone_pipe);
            microphone_pipe = -1;
            if (microphone_state == "Listening…") microphone_state = "Microphone unavailable";
            microphone_level = microphone_peak = 0;
            dirty = true;
        }
        if (now - microphone_update > 3000 && microphone_state == "Listening…") {
            microphone_state = "Microphone unavailable";
            microphone_level = microphone_peak = 0;
            dirty = true;
        }
    }
    void microphone_screen(SDL_Renderer* r, int y) {
        settings_header(r, y, "Test microphone");
        round_rect(r, 28, y + 105, 584, 250, 12, "panel");
        ui_icon(r, SettingsIcon::Volume, 70, y + 145, 30, "accent");
        label(r, "Live input", 100, y + 133, 24, "ink", 280);
        raw_label(r, std::to_string(microphone_db) + " dBFS", 424, y + 136, 22, "accent", 160);
        for (int i = 0; i < 24; i++)
            round_rect(r, 52 + i * 22, y + 207, 17, 52, 4, i * 100 / 24 < microphone_level ? "accent" : "slot");
        int peak = 52 + std::clamp(microphone_peak * 523 / 100, 0, 523);
        rect(r, peak, y + 198, 3, 70, "ink");
        label(r, "Quiet", 52, y + 275, 16, "muted", 200);
        label(r, "Loud", 526, y + 275, 16, "muted", 70);
        wrapped(r, microphone_state, 52, y + 312, 18, 536, 2);
        label(r, "Audio is not recorded or saved.", 40, y + 381, 18, "muted", 560);
    }
    void open_discover() {
        discover_return = settings.page;
        settings.page = Page::Discover;
        settings.focus = 0;
        status.clear();
        dirty = true;
    }
    void launcher_search_shortcut() {
        if (o.game_menu || !session_initialized || settings.page == Page::BootWarning) return;
        cancel_navigation();
        picker_open = false;
        picker_apply = {};
        picker_detail = false;
        probe = false;
        if (settings.page == Page::Keyboard) finish_edit(false);
        if (settings.page == Page::Microphone) {
            stop_microphone();
            settings.page = Page::Sound;
            configure_sounds();
        }
        if (settings.page == Page::Confirm) settings.page = confirm_return;
        control_capture = false;
        capture_logical = -1;
        if (settings.page != Page::Discover) open_discover();
        else {
            settings.focus = 0;
            dirty = true;
        }
    }
    void save_switcher_library() {
        auto path = o.data / "session/switcher-library.ini", pending = fs::path(path.string() + ".pending");
        fs::create_directories(path.parent_path());
        std::ofstream out(pending);
        std::set<fs::path> seen;
        int count = 0;
        auto add = [&](const Game& game) {
            auto rom = game.path.string();
            if (game.demo || count >= 9 || rom.find_first_of("\r\n") != std::string::npos ||
                !seen.insert(game.path).second)
                return;
            auto title = library_title(game.title);
            std::replace(title.begin(), title.end(), '\n', ' ');
            std::replace(title.begin(), title.end(), '\r', ' ');
            out << "game_" << count << "_rom=" << rom << "\ngame_" << count << "_title=" << title << '\n';
            count++;
        };
        if (!model.games.empty()) add(model.games[model.selected]);
        for (const auto& game : summary.recent_games) add(game);
        // A fresh library can still switch games before it has play history.
        for (const auto& game : model.games) {
            if (count >= 9) break;
            add(game);
        }
        out << "count=" << count << '\n';
        out.close();
        if (out) fs::rename(pending, path);
    }
    void load_switcher() {
        switch_games.clear();
        auto values = fields(o.data / "session/game-switcher.ini");
        for (int i = 0; i < std::clamp(std::atoi(values["count"].c_str()), 0, 9); i++) {
            auto prefix = "game_" + std::to_string(i);
            fs::path path = values[prefix + "_rom"];
            if (!path.empty()) {
                std::error_code ec;
                auto canonical = fs::weakly_canonical(path, ec);
                if (!ec) path = canonical;
            }
            if (!path.empty())
                switch_games.push_back(
                    {values[prefix + "_title"].empty() ? path.stem().string() : values[prefix + "_title"], path, false,
                     system_for_path(path)});
        }
        if (switch_games.empty() && !model.games.empty()) switch_games.push_back(model.games[0]);
    }
    bool discovery_active() const { return !search_query.empty() || discovery_filter != 0 || discovery_facets.any(); }
    std::string discovery_filter_name() const {
        static const std::array<std::string, 4> names = {"All games", "Unplayed", "Played", "Missing artwork"};
        return names.at(discovery_filter);
    }
    // What the list is filtered by, apart from the search words: "Unplayed  ·  Puzzle  ·  2 players".
    std::string discovery_filters_text() const {
        std::string value = discovery_filter ? locale.text(discovery_filter_name()) : "";
        if (discovery_facets.any()) value += (value.empty() ? "" : "  ·  ") + discovery_facets.summary();
        return value;
    }
    std::string discovery_description() const {
        std::string value = search_query;
        const auto filters = discovery_filters_text();
        if (!filters.empty()) value += (value.empty() ? "" : "  ·  ") + filters;
        return value;
    }
    void apply_discovery() {
        home_menu = false;
        settings.page = Page::Library;
        settings.focus = 0;
        library_view(preferences.view);
        dirty = true;
    }
    void clear_discovery() {
        search_query.clear();
        discovery_filter = 0;
        discovery_facets = {};
        apply_discovery();
    }
    void quick_search() {
        picker_open = false;
        picker_apply = {};
        discover_return = Page::Library;
        begin_edit(4);
    }
    static std::string trimmed_query(const std::string& value) {
        auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? "" : value.substr(first, last - first + 1);
    }
    void search_changed() {
        if (edit_field != 4) return;
        search_preview.cancel();
        search_matches.clear();
        search_preview_waiting = true;
        search_preview_loading = true;
        search_preview_at = s.GetTicks();
        dirty = true;
    }
    void poll_search_preview(Uint32 now) {
        if (settings.page != Page::Keyboard || edit_field != 4) return;
        try {
            if (search_preview_waiting && now - search_preview_at >= 150 && library_ready) {
                search_preview_waiting = false;
                search_preview.request(o.data, all_games, preferences.view, trimmed_query(edit_buffer),
                                       discovery_filter, discovery_facets);
            }
            if (auto result = search_preview.poll()) {
                search_matches = std::move(result->games);
                search_preview_loading = false;
                dirty = true;
            }
        } catch (const std::exception&) {
            search_preview_loading = false;
            dirty = true;
        }
    }
    void open_ds_controls() {
        ds_settings = fields(item_folder(model.selected) / "ds-input.ini");
        ds_return = settings.page;
        settings.page = Page::DSControls;
        settings.focus = 0;
        dirty = true;
    }
    std::map<std::string, std::string> ds_settings;
    bool ds_supported() {
        int core = o.game_menu                 ? active_engine()
                   : game_preferences.core < 0 ? int(preferences.core)
                                               : game_preferences.core;
        return core == 1 || core == 3 || core == 4;
    }
    void ds_controls_row(SDL_Renderer* r, int y, int i) {
        const std::array<std::string, 5> names = {"Blow into microphone", "Close and open DS lid", "DS language",
                                                  "Real microphone", "DS nickname"};
        const std::array<std::string, 6> languages = {"Japanese", "English", "French", "German", "Italian", "Spanish"};
        int language = ds_settings.count("language") ? std::clamp(std::atoi(ds_settings["language"].c_str()), 0, 5) : 1;
        list_row(r, y, i, names[i],
                 !ds_supported() ? "Engine managed"
                 : i == 4        ? (ds_settings.count("nickname") ? ds_settings["nickname"] : "RG DS Core")
                 : i == 2        ? languages[language]
                 : i == 3        ? ds_settings["microphone"] == "0" ? "Off" : "On"
                 : o.game_menu   ? "2 seconds"
                                 : "In game",
                 ds_supported() && (i >= 2 || o.game_menu));
    }
    void ds_controls_action() {
        if (!ds_supported()) return;
        if (settings.focus < 2) {
            if (o.game_menu) menu_request(settings.focus == 0 ? "ds:blow" : "ds:lid");
            return;
        }
        if (settings.focus == 2)
            show_picker("DS language", {"Japanese", "English", "French", "German", "Italian", "Spanish"},
                        ds_settings.count("language") ? std::atoi(ds_settings["language"].c_str()) : 1,
                        [this](int n) { save_ds_setting("language", n); });
        else if (settings.focus == 4) begin_edit(5);
        else save_ds_setting("microphone", ds_settings["microphone"] == "0");
    }
    void save_ds_setting(const std::string& key, int value) { save_ds_setting(key, std::to_string(value)); }
    void save_ds_setting(const std::string& key, const std::string& value) {
        try {
            ds_settings[key] = value;
            auto folder = item_folder(model.selected);
            fs::create_directories(folder);
            auto path = folder / "ds-input.ini", pending = fs::path(path.string() + ".pending");
            std::ofstream out(pending);
            for (auto& pair : ds_settings) out << pair.first << '=' << pair.second << '\n';
            out.close();
            if (out) {
                fs::rename(pending, path);
                status = "Restart the game to apply DS settings.";
            }
        } catch (const std::exception&) {
            status = "Could not save DS settings.";
        }
        dirty = true;
    }
    bool scan_stay = false; // a scan from Add your games keeps that page open
    void rescan_games(bool stay = false) {
        stop_library_scan();
        library_bytes.clear();
        library_eof = false;
        library_ready = false;
        scan_stay = stay;
        if (!stay) {
            home_menu = true;
            settings.page = Page::Library;
        }
        begin_library_scan();
        dirty = true;
    }
    // Add your games: opened by itself on a first start (Done then goes to the
    // library) or from Library settings (Done goes back there). A scan from the
    // page stays on it and reports what it found.
    bool setup_first_run = false, setup_scanned = false;
    bool backup_requested = false; // Back up all saves was chosen; its outcome shows on the Library page
    void confirm_reset_state() {
        show_picker("Restart without loading a state?", {"Cancel", "Reset state"}, 0, [this](int n) {
            if (n) menu_request("reset-state");
        });
    }
    // Cheats: the Action Replay codes DSCore loaded for this game, from the list
    // the bridge writes beside the control file (session/emulator.control.cheats)
    // when the menu opens and after each toggle. Databases run to hundreds or
    // thousands of codes (Mario Kart DS 483 in 31 groups, Pokémon HeartGold
    // 1174 in 55), so the page opens on the groups, lists one group at a time
    // with Left/Right stepping between groups, and offers a search (X) and the
    // codes that are on (Y). The preview shows a sample set.
    struct CheatCode {
        std::string name, description;
        int group = -1, state = 0;
    }; // state: -1 a note, 0 off, 1 on
    struct CheatGroup {
        std::string name;
        bool exclusive = false;
    };
    struct CheatRow {
        bool header;
        int index;
    }; // the groups overview: a group (-1 = codes outside any group); otherwise a code (into cheat_codes)
    // The tabs: All (with X's search as its filter), Groups (an overview, then one group) and On.
    enum class CheatView { Groups, Group, All, On };
    std::vector<CheatCode> cheat_codes;
    std::vector<CheatGroup> cheat_groups;
    std::vector<CheatRow> cheat_rows;
    std::vector<int> cheat_group_order; // groups in order of first appearance; -1 for codes outside any group
    CheatView cheat_view = CheatView::Groups;
    int cheat_group = -1, cheat_groups_focus = 0;
    std::string cheat_query;
    // Game menu layout: Resume and the switcher on top, a 3x3 grid, the Cheats bar under it.
    static constexpr int quick_top = 96, quick_wide_height = 52, quick_grid_top = 156, quick_grid_pitch = 68,
                         quick_tile_height = 60, quick_bottom_top = 360;
    int cheats_on() const;
    int cheats_in(int group, bool only_on = false) const;
    bool cheats_available() const { return experimental_game() && !cheat_codes.empty(); }
    // Two or more groups get the overview; a flat database opens on its codes.
    bool cheat_overview() const { return cheat_group_order.size() > 1; }
    bool cheat_togglable(int row) const;
    std::string cheats_note() const;
    std::string cheat_group_name(int group) const;
    // The header's title: the tabs say which view is open, so only a group names itself.
    std::string cheats_title() const;
    // What the view holds, for the top screen.
    std::string cheats_context() const;
    static std::string lowered(std::string value) {
        for (auto& c : value)
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
        return value;
    }
    static bool cheat_matches(const CheatCode& code, const std::string& query) {
        return lowered(code.name).find(query) != std::string::npos ||
               lowered(code.description).find(query) != std::string::npos;
    }
    int cheat_match_count(const std::string& text) const;
    void build_cheat_rows();
    void show_cheat_view(CheatView view, int group = -1);
    void cheat_home_view() { show_cheat_view(cheat_overview() ? CheatView::Groups : CheatView::All); }
    // The tab strip: All, Groups (when the database has any) and On, in that order.
    std::vector<CheatView> cheat_tabs() const;
    static const char* cheat_tab_name(CheatView view) {
        return view == CheatView::All ? "All" : view == CheatView::On ? "On" : "Groups";
    }
    int cheat_tab() const;
    // Opening a tab leaves the search behind: the filter belongs to All while it is open.
    void cheat_tab_select(int index);
    void cheat_tab_step(int direction);
    void load_cheats();
    void open_cheats();
    void cheat_move(int step);
    // Left/Right on the pad: the previous or next group while a group is open;
    // a page of rows in All or On, where one list can run long. L and R change tabs.
    void cheat_side(int direction);
    // B goes up one step: a group to the overview, a search to all codes, else the menu.
    void cheat_back();
    // A, Select or a tap opens a group from the overview, or flips the code. The
    // engine applies it (cheat:<index>:<0|1>) and the list comes back with its answer.
    void cheat_action(int code);
    // The header: the title, the tab strip standing on the header's rule and
    // the search key at the right end. Fixed columns, right to left: the search
    // key (X and its label, or the query while one filters All), a 12 px gap,
    // then the tabs, 72 px each, ending where the search key begins.
    static constexpr int cheat_search_x = 500, cheat_search_w = 116, cheat_tab_w = 72;
    int cheat_tabs_left() const { return cheat_search_x - 12 - int(cheat_tabs().size()) * cheat_tab_w; }
    bool cheat_header_hit(int x, int y) const { return y >= 24 && y <= 77 && x >= cheat_tabs_left(); }
    // A tab strip: folder tabs with rounded tops standing on a rule, the open
    // one taller and in accent glass with the rule turning accent under it,
    // the others in panel glass a step lower. `rule` is the rule's own line.
    void tab_strip(SDL_Renderer* r, int x, int rule, int w, const std::vector<std::string>& names, int selected,
                   int pressed = -1);
    void cheat_header(SDL_Renderer* r, int y);
    void cheat_header_tap(int x);
    void cheat_row(SDL_Renderer* r, int y, int i);
    // The top screen: where you are, the focused group or code with its
    // description, the totals and the keys that move around.
    void cheats_top(SDL_Renderer* r, int y);
    // A legend bar: a sunken track split into equal cells, each an accent key badge and what the key does.
    template <class Keys> void key_bar(SDL_Renderer* r, int x, int y, int w, int h, const Keys& keys) {
        sunken_track(r, x, y, w, h, 13);
        const int cells = int(keys.size()), cell = (w - 8) / std::max(1, cells);
        for (int i = 0; i < cells; i++) {
            const int left = x + 4 + i * cell;
            if (i) rect(r, left, y + 10, 1, h - 20, "line");
            const std::string key = keys[size_t(i)].key, value = locale.text(keys[size_t(i)].text);
            const int badge = key_group(nullptr, 0, 0, key),
                      text = std::min(cell - badge - 30, label_width(value, 16, false));
            const int start = left + (cell - badge - 10 - text) / 2;
            key_group(r, start, y + h / 2, key);
            raw_label(r, value, start + badge + 10, centred_label_y(value, 16, y + h / 2), 16, "ink", text + 4);
        }
    }
    // The top screen while typing a cheat search: the live match count and the first names.
    void cheat_search_top(SDL_Renderer* r, int y);
    void quick_action() {
        if (menu_status["state"] == "busy") return;

        if (settings.focus == 0) menu_request("resume");
        else if (settings.focus == 1 || settings.focus == 2)
            open_gallery(settings.focus == 1 ? GalleryMode::Save : GalleryMode::Load);
        else if (settings.focus == 3) {
            game_preferences = library_cache.reload_options(model.games[0]);
            if (active_engine() == play_core) {
                load_play_view();
                settings.page = Page::PlayGraphics;
                settings.focus = 0;
                status.clear();
            } else if (experimental_game()) {
                settings.page = Page::Graphics;
                settings.focus = 0;
                status.clear();
            } else {
                open_shader_picker(true);
                shader_return = Page::QuickMenu;
            }
        } else if (settings.focus == 4) open_emulator_options();
        else if (settings.focus == 5) {
            settings.section = 3;
            settings.page = Page::Sound;
        } else if (settings.focus == 6) {
            settings.section = 0;
            settings.page = Page::Display;
        } else if (settings.focus == 7) menu_request("restart");
        else if (settings.focus == 9) open_controls(true, Page::QuickMenu);
        else if (settings.focus == 10) {
            load_switcher();
            settings.page = Page::GameSwitcher;
            settings.focus = 0;
        } else if (settings.focus == 11) open_cheats();
        else if (menu_status["state"] == "error") {
            settings.page = Page::LeaveGame;
            settings.focus = 0;
        } else menu_request("quit");
        dirty = true;
    }
    int active_engine() const {
        return int(std::find(std::begin(core_ids), std::end(core_ids), o.game_core) - std::begin(core_ids));
    }
    void open_emulator_options() {
        game_preferences = library_cache.reload_options(model.games[0]);
        status.clear();
        settings.page = Page::EmulatorOptions;
        settings.focus = 0;
        dirty = true;
    }
    std::string active_graphics_name() const {
        auto found = menu_status.find("active_scale");
        if (found == menu_status.end()) return "Waiting for renderer";
        int scale = std::clamp(std::atoi(found->second.c_str()), 1, 5);
        return scale == 1 ? "Native / software"
                          : std::to_string(scale) + "x Vulkan / " + std::to_string(scale * 256) + " x " +
                                std::to_string(scale * 192);
    }
    void apply_experimental_graphics(const GameOptions& next) {
        if (!o.game_menu || o.preview) {
            apply_game_preferences(next, o.game_menu ? "Graphics applied (preview)." : "Graphics saved.");
            if (o.game_menu) {
                menu_status["active_scale"] = std::to_string(next.rgds_scale);
                menu_status["active_smooth"] = std::to_string(next.rgds_smooth);
                menu_status["active_filter2d"] = std::to_string(next.rgds_2d);
            }
            return;
        }
        menu_request("graphics:" + std::to_string(next.rgds_scale) + ":" + std::to_string(next.rgds_smooth) + ":" +
                     std::to_string(std::max(0, next.frameskip)));
    }
    static std::string filter2d_name(int mode) {
        return mode == 1   ? "Sharp"
               : mode == 2 ? "Smooth"
               : mode == 3 ? "Crisp"
               : mode == 4 ? "HD"
               : mode == 5 ? "HD smooth"
                           : "Native pixels";
    }
    void apply_filter2d(int mode) {
        auto next = game_preferences;
        next.rgds_2d = mode;
        if (!o.game_menu || o.preview) {
            apply_game_preferences(next, o.game_menu ? "2D scaling applied (preview)." : "2D scaling saved.");
            if (o.game_menu) menu_status["active_filter2d"] = std::to_string(mode);
            return;
        }
        menu_request("filter2d:" + std::to_string(mode));
    }
    void apply_adaptive(bool on) {
        // In a game the engine switches live (adaptive:N); the runtime saves the
        // choice once it is applied. Outside, or in a preview, only save it.
        if (!o.game_menu || o.preview) {
            auto next = game_preferences;
            next.rgds_adaptive = on;
            apply_game_preferences(next, o.game_menu ? "Heavy 3D scenes applied (preview)." : "Heavy 3D scenes saved.");
            return;
        }
        menu_request(std::string("adaptive:") + (on ? "1" : "0"));
    }
    void graphics_action() {
        if (settings.focus == 0) show_experimental_resolution_picker();
        else if (settings.focus == 1)
            show_picker("3D edge smoothing", {"Sharp", "Smooth (3x and above)"}, game_preferences.rgds_smooth,
                        [this](int n) {
                            auto next = game_preferences;
                            next.rgds_smooth = n;
                            apply_experimental_graphics(next);
                        });
        else if (settings.focus == 2)
            show_picker("Heavy 3D scenes (2x and above)", {"Adaptive (holds 60 fps)", "Always sharp"},
                        game_preferences.rgds_adaptive ? 0 : 1, [this](int n) { apply_adaptive(n == 0); });
        else if (settings.focus == 3)
            show_picker("2D scaling", {"Native pixels", "Sharp", "Smooth", "Crisp", "HD", "HD smooth"},
                        game_preferences.rgds_2d, [this](int n) { apply_filter2d(n); });
        else if (settings.focus == 4) {
            open_shader_picker(true);
            shader_return = Page::Graphics;
        } else if (settings.focus == 5)
            show_picker("Frameskip", {"Off (smooth)", "1 frame", "2 frames", "3 frames"},
                        std::max(0, game_preferences.frameskip), [this](int n) {
                            auto next = game_preferences;
                            next.frameskip = n;
                            apply_experimental_graphics(next);
                        });
        else if (settings.focus == 7) menu_request("restart");
        dirty = true;
    }
    bool emulator_option_enabled(int row) {
        // RG DS Play: no DS engine to switch to and no DS to control.
        if (active_engine() == play_core && (row == 6 || row == list_count() - 2)) return false;
        if (row >= (active_engine() == 0 ? 10 : 8) && row < list_count()) return true;
        if (superdrastic_game() && (row == 0 || row == 9)) return false;
        if (row == 2) return hardware.power;
        if (row == 3) return active_engine() == 0 || active_engine() == 3;
        if (row == 4) return active_engine() == 0 || active_engine() == 2 || active_engine() == 4;
        if (row == 5) return active_engine() == 0 || active_engine() == 1 || active_engine() == 4;
        return row >= 0 && (row < 8 || ((row == 8 || row == 9) && active_engine() == 0));
    }
    void change_emulator_option(int direction) {
        if (settings.focus == list_count() - 1) {
            confirm_reset_state();
            return;
        }
        if (settings.focus == list_count() - 2) {
            open_ds_controls();
            return;
        }
        if (!emulator_option_enabled(settings.focus)) return;
        int row = settings.focus - 1, engine = active_engine();
        if (row < 0) {
            if (o.preview) {
                menu_status["fps"] = menu_status["fps"] == "1" ? "0" : "1";
                dirty = true;
            } else menu_request(menu_status["fps"] == "1" ? "fps:0" : "fps:1");
            return;
        }
        if (row == 0) {
            menu_request("fast-forward");
            return;
        }
        if (row == 1) {
            change_system(0, direction);
            return;
        }
        if (row == 2 && engine == 3) {
            menu_request(menu_status["threaded"] == "0" ? "threaded:1" : "threaded:0");
            return;
        }
        auto next = game_preferences;
        if (row == 2 && engine == 0) next.threaded = next.threaded == 0 ? 1 : 0;
        else if (row == 3) next.hires = next.hires == 1 ? 0 : 1;
        else if (row == 4) {
            if (!direction) return;
            next.frameskip = std::clamp((next.frameskip < 0 ? 0 : next.frameskip) + direction, 0, 3);
        } else if (row == 6) {
            menu_request("restart");
            return;
        } else return;
        if (experimental_game()) apply_experimental_graphics(next);
        else apply_game_preferences(next, "Saved. Save & restart applies this change.");
    }
    void watch_menu(Uint32 now) {
        // Pending actions should acknowledge promptly; idle menus keep their
        // slower poll. The first frame starts with the supervisor's real state.
        if (!o.game_menu || o.preview ||
            (rendered_frames &&
             now - menu_poll < (menu_pending_token.empty() && menu_status["state"] != "busy" ? 100u : 20u)))
            return;
        menu_poll = now;
        auto value = fields(o.data / "session/game-menu.status");
        if (value["view_request"] != menu_status["view_request"] && std::atoi(value["view_request"].c_str()) > 0) {
            picker_open = false;
            cancel_navigation();
            if (value["view"] == "switcher") {
                load_switcher();
                settings.page = Page::GameSwitcher;
            } else settings.page = Page::QuickMenu;
            settings.focus = 0;
            load_cheats();
            dirty = true;
        }
        // The old status can still be on disk after a tap. Keep the local
        // busy state until the supervisor acknowledges this exact request.
        if (menu_pending_token.empty() || value["request_token"] == menu_pending_token) {
            if (!menu_pending_token.empty() && value["state"] != "busy") {
                if (value["state"] == "error") emit_sound(MenuCue::Error);
                // The direct present has no GL pass: the filter is saved and needs the renderer, so offer the restart.
                else if (menu_sound_action.rfind("shader:", 0) == 0 && value["message"].rfind("Shader saved", 0) == 0) {
                    settings.page = Page::ApplyShader;
                    settings.focus = 0;
                } else if (menu_sound_action == "save" || menu_sound_action.rfind("slot-save:", 0) == 0)
                    emit_sound(MenuCue::Save);
                else if (menu_sound_action == "load" || menu_sound_action.rfind("slot-load:", 0) == 0)
                    emit_sound(MenuCue::Load);
                // The supervisor saves live graphics choices after the engine accepts them; show what it stored.
                if ((menu_sound_action.rfind("graphics:", 0) == 0 || menu_sound_action.rfind("filter2d:", 0) == 0 ||
                     menu_sound_action.rfind("adaptive:", 0) == 0) &&
                    value["state"] != "error")
                    game_preferences = library_cache.reload_options(model.games[0]);
                // The engine rewrote its cheat list before answering; show that, whichever way it went.
                if (menu_sound_action.rfind("cheat:", 0) == 0) load_cheats();
                // RG DS Play's view: what the runtime stored once the player applied it.
                if (menu_sound_action.rfind("play-graphics:", 0) == 0) load_play_view();
                menu_sound_action.clear();
            }
            if (value["state"] != "busy") menu_pending_token.clear();
            if (!same_fields(value, menu_status)) {
                menu_status = std::move(value);
                dirty = true;
            }
        } else if (now - menu_request_at > (menu_sound_action.rfind("graphics:", 0) == 0 ? 50000u : 15000u)) {
            menu_pending_token.clear();
            emit_sound(MenuCue::Error);
            menu_status["state"] = "error";
            menu_status["message"] = "The game menu did not respond. Resume or return without saving.";
            dirty = true;
        }
        auto volume = fields(o.data / "volume.ini");
        if (volume.count("volume")) {
            int level = std::clamp(std::atoi(volume["volume"].c_str()), 0, 100);
            volume_applied = level;
            if (!volume_unsaved && !system.volume_pending() && level != hardware.volume_percent) {
                hardware.volume_percent = level;
                if (level > 0) unmuted_volume = level;
                volume_toast = now + 1800;
                dirty = true;
            }
        }
        if (menu_status["state"] == "home") quit = true;
    }
    void start_worker(const std::string& operation, const std::string& payload = "") {
        if (worker > 0 && (worker_action == "device.info" || worker_action == "device.update-auto") &&
            operation != "device.info")
            stop_worker();
        if (worker > 0 && worker_action == "index" && operation.rfind("device.", 0) == 0) stop_worker();
        if (worker > 0) return;
        bool busy_visible = operation != "device.info" || settings.page == Page::Startup ||
                            settings.page == Page::Wifi || !service_notice.empty();
        service_notice.clear();
        bool device = operation.rfind("device.", 0) == 0;
        auto folder = o.data / (device ? "device" : "scraper");
        fs::create_directories(folder);
        fs::path script = fs::path("runtime") / (device ? "device_services.py" : "scraper.py");
        std::vector<std::string> args = {"python3",
                                         "-B",
                                         (o.app.empty() ? script : o.app / script).string(),
                                         device ? operation.substr(7) : operation,
                                         "--data",
                                         o.data.string()};
        if (device) {
            args.push_back("--app");
            args.push_back((o.app.empty() ? fs::current_path() : o.app).string());
            if (o.preview) args.push_back("--preview");
        } else {
            static const std::array<const char*, 3> providers = {"auto", "gametdb", "libretro"};
            args.push_back("--provider");
            args.push_back(operation == "scrape" && settings.page == Page::GameArtwork
                               ? "auto"
                               : providers.at(preferences.artwork_provider));
            if (operation == "scrape" && settings.page == Page::GameArtwork && !model.games.empty()) {
                args.push_back("--rom");
                args.push_back(model.games[model.selected].path.string());
                args.push_back("--force");
            }
            // Game info and extra pictures ride along with the covers when they are switched on.
            std::string extras = preferences.game_info ? "info" : "";
            if (preferences.extra_pictures & 1) extras += std::string(extras.empty() ? "" : ",") + "snap";
            if (preferences.extra_pictures & 2) extras += std::string(extras.empty() ? "" : ",") + "title";
            if (operation == "scrape" && !extras.empty()) {
                args.push_back("--extras");
                args.push_back(extras);
            }
            for (auto root : o.roots) {
                args.push_back("--rom-dir");
                args.push_back(root.string());
            }
            if (preferences.show_systems)
                for (const auto& [system, root] : o.system_roots) {
                    args.push_back("--system-dir");
                    args.push_back(system + "=" + root.string());
                }
        }
        int channel[2];
        if (pipe(channel) != 0) {
            status = "Could not start artwork service";
            return;
        }
        auto child = fork();
        if (child == 0) {
            ::close(channel[1]);
            dup2(channel[0], STDIN_FILENO);
            ::close(channel[0]);
            int log = ::open((folder / "worker.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
                ::close(log);
            }
            std::vector<char*> argv;
            for (auto& arg : args) argv.push_back(arg.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        ::close(channel[0]);
        if (!payload.empty() && child > 0) {
            auto sent = write(channel[1], payload.data(), payload.size());
            (void)sent;
        }
        ::close(channel[1]);
        if (child < 0) {
            status = "Could not start artwork service";
            return;
        }
        worker = child;
        worker_action = operation;
        worker_ok = false;
        if (busy_visible) dirty = true;
    }
    void stop_worker() {
        if (worker <= 0) return;
        if (waitpid(worker, nullptr, WNOHANG) == 0) {
            kill(worker, SIGTERM);
            for (int i = 0; i < 100; i++) {
                if (waitpid(worker, nullptr, WNOHANG) != 0) {
                    worker = -1;
                    return;
                }
                usleep(10000);
            }
            kill(worker, SIGKILL);
            waitpid(worker, nullptr, 0);
        }
        worker = -1;
    }
    Uint32 worker_finished_at = 0;
    void poll_service(Uint32 now) {
        bool artwork_finished = false;
        if (worker > 0) {
            int result = 0;
            auto finished = waitpid(worker, &result, WNOHANG);
            if (finished > 0) {
                worker_ok = WIFEXITED(result) && WEXITSTATUS(result) == 0;
                worker = -1;
                worker_finished_at = now;
                service_poll = 0;
                artwork_finished = worker_action == "index" || worker_action == "scrape";
                if (worker_action != "device.info" || settings.page == Page::Startup || settings.page == Page::Wifi)
                    dirty = true;
            }
        }
        if (artwork_finished) {
            if (settings.page == Page::GameArtwork && !model.games.empty())
                artwork_result = fields(item_folder(model.selected) / "artwork-status.ini");
            library_cache.invalidate_metadata();
            if (media.refresh()) library_cache.invalidate_titles();
        }
        // Status files change only through workers; poll quickly while one runs.
        bool quick = worker > 0 || now - worker_finished_at < 1000 || settings.page == Page::SaveGallery ||
                     !menu_pending_token.empty();
        if (now - service_poll < (quick ? 250u : 2000u)) return;
        service_poll = now;
        auto next = fields(o.data / "scraper/status.ini");
        if (!same_fields(next, service)) {
            service = std::move(next);
            library_cache.invalidate_metadata();
            if (media.refresh()) library_cache.invalidate_titles();
            dirty = true;
        }
        auto device = fields(o.data / "device/status.ini");
        if (!same_fields(device, device_status)) {
            device_status = std::move(device);
            dirty = true;
        }
        experience = fields(o.data / "experience.ini");
        if (!experience.count("auto_update")) experience["auto_update"] = "1";
        if (settings.page == Page::SaveGallery && !model.games.empty()) {
            auto next = fields(gallery_root() / "index.ini");
            if (next != slots) {
                slots = std::move(next);
                media.refresh();
                dirty = true;
            }
        }
        auto update = fields(o.data / "update/status.ini");
        if (!same_fields(update, update_status)) {
            update_status = std::move(update);
            dirty = true;
        }
        if (worker <= 0 && worker_ok && worker_action == "device.update-apply" &&
            update_status["state"] == "restarting")
            quit = true;
        if (shutting_down() && worker <= 0 && !worker_ok) {
            shutdown_at = 0;
            shutdown_kind.clear();
            dirty = true;
        }
        if (worker <= 0 && !worker_ok && worker_action.rfind("device.", 0) == 0 && device_status["state"] != "error") {
            device_status["state"] = "error";
            device_status["message"] = "This request did not finish. Please try again.";
            dirty = true;
        }
        if (worker <= 0 && worker_ok && (worker_action == "device.poweroff" || worker_action == "device.reboot") &&
            device_status["state"] == "ready") {
            system_exit_ack = !o.preview;
            quit = true;
        }
    }
    void select_game(size_t index, bool animate = true) {
        if (model.games.empty()) return;
        model.selected = std::min(index, model.games.size() - 1);
        selected_at = s.GetTicks();
        if (animate) cover_motion_at = selected_at;
        browser_list.select(model.selected, model.games.size(), selected_at, animate && preferences.animations);
        if (animate && preferences.animations) carousel.select(model.selected, selected_at);
        else carousel.reset(model.selected);
        dirty = true;
    }
    void navigate(int delta) {
        SoundAction sound_action(*this, delta < 0 ? 0 : 1);
        motion_input(delta < 0 ? 0 : 1);
        if (!navigation_count()) return;
        long count = static_cast<long>(navigation_count()), previous = static_cast<long>(navigation_selected());
        size_t next = static_cast<size_t>((previous + delta % count + count) % count);
        if (next != size_t(previous)) sound_intent(MenuCue::Navigate);
        select_navigation(next, std::abs(previous - static_cast<long>(next)) <= 3);
    }
    // A held Left or Right steps one game at a time; from 2.4 s on its stride
    // doubles every 600 ms, so a card of thousands of games is crossed in a
    // few seconds. The run stops at either end instead of wrapping round.
    static int hold_stride(unsigned held, size_t count) {
        if (held < 2400 || count < 24) return 1;
        const int cap = int(std::clamp<size_t>(count / 40, 1, 128));
        return std::min(cap, 1 << std::min(7u, 1 + (held - 2400) / 600));
    }
    void hold_navigate(int direction, unsigned held) {
        const int stride = home_menu ? 1 : hold_stride(held, navigation_count());
        if (stride == 1) {
            navigate(direction);
            return;
        }
        const long count = long(navigation_count()), at = long(navigation_selected()),
                   next = std::clamp(at + direction * stride, 0L, count - 1);
        if (next != at) navigate(int(next - at));
    }
    void library_view(int view) {
        cancel_navigation();
        std::string selected = model.games.empty()                ? ""
                               : model.games[model.selected].demo ? "demo:" + model.games[model.selected].title
                                                                  : model.games[model.selected].path.string();
        preferences.view = view;
        collection_filter.cancel();
        collection_loading = false;
        if ((view != 0 && all_games.size() > 256) || !search_query.empty() || discovery_filter ||
            discovery_facets.any()) {
            collection_selection = selected;
            collection_loading = true;
            collection_filter.request(o.data, view_source(view), view, search_query, discovery_filter,
                                      discovery_facets);
            model.games.clear();
            model.selected = 0;
            carousel.reset(0);
            browser_list.scroll = 0;
            dirty = true;
            return;
        }
        model.games = view_source(view);
        if (view == 1)
            model.games.erase(
                std::remove_if(model.games.begin(), model.games.end(),
                               [&](const Game& game) { return !library_cache.options(game, true).favourite; }),
                model.games.end());
        if (view == 2) {
            // Snapshot each date once. Sorting a large collection never performs
            // filesystem reads or churns the bounded detail cache in a comparator.
            std::vector<std::pair<Game, std::string>> recent;
            recent.reserve(model.games.size());
            for (const auto& game : model.games) {
                auto date = library_cache.stats(game, true).last_played;
                if (!date.empty()) recent.emplace_back(game, std::move(date));
            }
            std::stable_sort(recent.begin(), recent.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
            model.games.clear();
            model.games.reserve(recent.size());
            for (auto& item : recent) model.games.push_back(std::move(item.first));
        }
        model.selected = 0;
        if (!selected.empty())
            for (size_t i = 0; i < model.games.size(); i++)
                if ((model.games[i].demo ? "demo:" + model.games[i].title : model.games[i].path.string()) == selected)
                    model.selected = i;
        carousel.reset(model.selected);
        browser_list.scroll = 0;
        browser_list.reveal(model.selected, model.games.size());
        dirty = true;
    }
    void poll_collection() {
        try {
            auto result = collection_filter.poll();
            if (!result || !collection_loading) return;
            model.games = std::move(result->games);
            collection_loading = false;
            model.selected = 0;
            for (size_t i = 0; i < model.games.size(); i++)
                if (model.games[i].path.string() == collection_selection) {
                    model.selected = i;
                    break;
                }
            carousel.reset(model.selected);
            browser_list.reveal(model.selected, model.games.size());
            dirty = true;
        } catch (const std::exception&) {
            collection_loading = false;
            status = "Could not read this collection. Try again.";
            dirty = true;
        }
    }
    void open_game_options() {
        if (model.games.empty()) return;
        game_preferences = library_cache.options(model.games[model.selected], true);
        status.clear();
        settings.page = Page::GameOptions;
        settings.focus = 0;
        settings.section = 2;
        dirty = true;
    }
    bool apply_game_preferences(const GameOptions& next, const char* message) {
        try {
            save_game_options(item_folder(model.selected) / "game.ini", next);
            game_preferences = next;
            library_cache.saved_options(model.games[model.selected], next);
            if (!o.game_menu) library_summary.request(o.data, all_games);
            status = message;
        } catch (const std::exception& e) {
            status = "Could not save game settings. Previous values kept.";
            std::cerr << e.what() << std::endl;
            dirty = true;
            return false;
        }
        dirty = true;
        return true;
    }
    void choose_game_emulator(int core) {
        if (core == game_preferences.core) return;
        auto next = game_preferences;
        next.core = core;
        apply_game_preferences(next,
                               o.game_menu ? "Saved. Save & restart changes the emulator." : "Game emulator saved.");
    }
    void change_game_option(int delta, bool toggle = false) {
        auto next = game_preferences;
        if (settings.focus == 0) next.favourite = toggle ? !next.favourite : delta > 0;
        else if (settings.focus == 1) {
            int& value = next.core;
            const int step = toggle || delta > 0 ? 1 : -1;
            do
                value = toggle ? (value >= ds_core_count - 1 ? -1 : value + 1)
                               : std::clamp(value + step, -1, ds_core_count - 1);
            while (value == retired_core);
        } else {
            open_shader_picker(true);
            return;
        }
        apply_game_preferences(next, "Game options saved");
    }
    std::string shader_name(int value) {
        return value < 0 ? "Default" : shaders.at(std::clamp(value, 0, shader_count - 1)).name;
    }
    bool experimental_game() const {
        if (o.game_menu) return o.game_core == "rgds-experimental";
        if (other_system_selected()) return false;
        return (game_preferences.core < 0 ? int(preferences.core) : game_preferences.core) == 4;
    }
    std::string experimental_resolution_name() const {
        return game_preferences.rgds_scale == 1 ? "Native" : std::to_string(game_preferences.rgds_scale) + "x";
    }
    void show_experimental_resolution_picker() {
        show_picker("DSCore 3D resolution",
                    {"Native (fastest)", "2x sharper", "3x sharper", "4x (experimental)", "5x (experimental)"},
                    game_preferences.rgds_scale - 1, [this](int n) {
                        auto next = game_preferences;
                        next.rgds_scale = n + 1;
                        apply_experimental_graphics(next);
                    });
    }
    bool superdrastic_game() const {
        int core = game_preferences.core < 0 ? int(preferences.core) : game_preferences.core;
        int backend =
            game_preferences.drastic_backend < 0 ? preferences.drastic_backend : game_preferences.drastic_backend;
        if (o.game_menu) return o.game_core == "drastic" && std::getenv("RGDS_SD_SHARED_MENU");
        return core == 0 && backend == 1 && !other_system_selected();
    }
    int superdrastic_resolution() const {
        return game_preferences.hires == 1 ? (game_preferences.gengis_scale >= 2 ? game_preferences.gengis_scale : 1)
                                           : 0;
    }
    const char* superdrastic_resolution_name() const {
        static const char* names[] = {"Native", "2x", "2x Gengis", "3x Gengis"};
        return names[superdrastic_resolution()];
    }
    void show_superdrastic_resolution_picker() {
        show_picker("3D resolution", {"Native", "2x (DraStic)", "2x Gengis (experimental)", "3x Gengis (experimental)"},
                    superdrastic_resolution(), [this](int n) {
                        auto next = game_preferences;
                        next.hires = n > 0;
                        next.gengis_scale = n >= 2 ? n : 0;
                        apply_game_preferences(next, "Saved. Applies on your next SuperDrastic launch.");
                    });
    }
    void open_shader_picker(bool per_game);
    // The system of the game a shader is picked for (Settings' default is the DS games').
    std::string shader_system = "nds";
    std::string picker_game_system();
    // A screen preset is made for one system: DS screens for DS games, GBA screens for GBA games
    // (runtime/play_options.py keeps the same rule at launch).
    static bool shader_fits_system(int id, const std::string& system) {
        const std::string family = shader_family_names[shader_family(id)];
        if (family == "DS screens") return system == "nds";
        if (family == "GBA screens") return system == "gba";
        return true;
    }
    void close_shader_picker();
    fs::path shader_preview_folder() const;
    int preview_shader() const;
    void change_shader_preview(int code);
    int shader_choice_family(int index) const;
    void focus_shader(int index);
    // Up/Down, L/R and the group chips jump to the first preset of a group.
    void shader_family_jump(int family);
    void shader_family_step(int direction);
    // A chip per group this carousel has (a system's screens are only offered for its own games).
    std::vector<int> shader_families_shown();
    static SDL_Rect shader_chip(int place) { return {24 + place * 99, 86, 95, 32}; }
    // Touch on the carousel or its rail. Taps elsewhere use the shared tap path.
    bool shader_pointer_down(int x, int y);
    fs::path controls_file(bool per_game);
    static std::string control_title(int source) {
        static const std::array<const char*, 17> labels = {
            "A",    "B",    "X",     "Y",  "L",  "R",          "Start",       "Select",    "Up",
            "Down", "Left", "Right", "L2", "R2", "Left stick", "Right stick", "Unassigned"};
        return source >= 0 && source < int(labels.size()) ? labels[source] : "Unassigned";
    }
    void load_controls_editor();
    void open_controls(bool per_game, Page parent = Page::Settings);
    void close_controls();
    bool save_controls_editor();
    void capture_control(int source);
    std::string controls_row_name(int row);
    std::string controls_row_value(int row);
    void controls_action(int code);
    void render_shader_preview(SDL_Renderer* r, int y);
    void render_shader_carousel(SDL_Renderer* r, int y);
    void choose_shader();
    // The Game info page (game_info_screen.hpp): facts, description and the pictures kept for the selected game.
    GameFacts info_facts;
    std::vector<GalleryPicture> info_pictures;
    int info_picture = 0, info_scroll = 0, info_scroll_max = 0;
    Page info_return = Page::GameArtwork;
    void open_game_info(Page back);
    void game_info_key(int code);
    void game_info_touch(int x, int y);
    void game_info_step_picture(int step);
    void game_info_scroll_by(int delta);
    void game_info_top(SDL_Renderer* r, int y);
    void game_info_bottom(SDL_Renderer* r, int y);
    Page artwork_return = Page::GameOptions;
    std::string artwork_match;
    std::map<std::string, std::string> artwork_result;
    void open_game_artwork(Page back = Page::GameOptions) {
        if (model.games.empty()) return;
        artwork_return = back;
        artwork_match = fields(item_folder(model.selected) / "artwork.ini")["match_title"];
        artwork_result = fields(item_folder(model.selected) / "artwork-status.ini");
        service_notice.clear();
        settings.page = Page::GameArtwork;
        settings.focus = 0;
        dirty = true;
    }
    bool save_artwork_match(const std::string& title) {
        try {
            auto path = item_folder(model.selected) / "artwork.ini";
            fs::create_directories(path.parent_path());
            auto pending = path.string() + ".pending";
            std::ofstream file(pending);
            std::string clean = title;
            std::replace(clean.begin(), clean.end(), '\n', ' ');
            std::replace(clean.begin(), clean.end(), '\r', ' ');
            file << "schema=1\nmatch_title=" << clean << '\n';
            file.close();
            if (!file) throw std::runtime_error("Could not save matching title.");
            fs::rename(pending, path);
            artwork_match = clean;
            return true;
        } catch (const std::exception&) {
            service_notice = "Could not save matching title.";
            return false;
        }
    }
    void review_artwork() {
        std::vector<std::string> names;
        std::vector<size_t> targets;
        for (size_t i = 0; i < model.games.size(); i++) {
            auto result = fields(item_folder(i) / "artwork-status.ini");
            std::error_code ec;
            if (!fs::exists(item_folder(i) / "box.bmp", ec) || result["state"] == "error" ||
                result["state"] == "missing") {
                names.push_back(game_title(i));
                targets.push_back(i);
            }
        }
        if (names.empty()) {
            service_notice =
                model.games.empty() ? "No games in this collection." : "Every game in this collection has artwork.";
            dirty = true;
            return;
        }
        show_picker("Unmatched artwork", names, 0, [this, targets](int n) {
            select_game(targets.at(n), false);
            open_game_artwork(Page::Scraper);
        });
    }
    void game_artwork_action() {
        if (settings.focus == 3) {
            open_game_info(Page::GameArtwork);
            return;
        }
        if (worker > 0 && worker_action == "scrape") {
            if (settings.focus == 1) {
                std::ofstream(o.data / "scraper/cancel").put('\n');
                service_notice = "Stopping after this request.";
            } else service_notice = "Stop fetching before editing the match.";
            dirty = true;
            return;
        }
        if (settings.focus == 0) {
            begin_edit(3);
            return;
        }
        if (settings.focus == 2) {
            if (save_artwork_match("")) service_notice = "Using the ROM's game code and filename.";
            dirty = true;
            return;
        }
        if (worker > 0 && (worker_action == "index" || worker_action == "device.info" || worker_action == "status"))
            stop_worker();
        if (model.games[model.selected].demo) {
            service_notice = "Add your DS games to fetch their artwork.";
            dirty = true;
            return;
        }
        start_worker("scrape");
    }
    void open_scraper() {
        settings.section = 2;
        settings.page = Page::Scraper;
        settings.focus = 0;
        if (worker <= 0) start_worker("status");
        service_loaded = true;
        dirty = true;
    }
    void begin_edit(int field) {
        if (field == 4) search_return = settings.page;
        edit_field = field;
        edit_buffer = field == 5   ? (ds_settings.count("nickname") ? ds_settings["nickname"] : "RG DS Core")
                      : field == 4 ? search_query
                      : field == 6 ? cheat_query
                      : field == 0 ? username
                      : field == 1 ? password
                      : field == 3 ? artwork_match
                                   : "";
        key_row = 1;
        key_col = 1;
        key_layer = 0;
        key_caps = false;
        edit_cursor = edit_buffer.size();
        settings.page = Page::Keyboard;
        s.StartTextInput();
        search_changed();
        dirty = true;
    }
    void finish_edit(bool commit) {
        if (edit_field == 6) {
            // An empty search lists every code flat, which has its own use.
            if (commit) {
                cheat_query = trimmed_query(edit_buffer);
                show_cheat_view(CheatView::All);
            }
            edit_buffer.clear();
            s.StopTextInput();
            settings.page = Page::Cheats;
            dirty = true;
            return;
        }
        if (edit_field == 5) {
            if (commit) save_ds_setting("nickname", edit_buffer.empty() ? std::string("RG DS Core") : edit_buffer);
            edit_buffer.clear();
            s.StopTextInput();
            settings.page = Page::DSControls;
            settings.focus = 4;
            dirty = true;
            return;
        }
        if (edit_field == 4) {
            search_preview.cancel();
            search_preview_waiting = search_preview_loading = false;
            search_matches.clear();
            if (commit) {
                search_query = trimmed_query(edit_buffer);
                apply_discovery();
            } else {
                settings.page = search_return;
                settings.focus = 0;
            }
            edit_buffer.clear();
            s.StopTextInput();
            dirty = true;
            return;
        }
        if (edit_field == 3) {
            if (commit && save_artwork_match(edit_buffer))
                service_notice = "Matching title saved. Choose Fetch artwork.";
            edit_buffer.clear();
            s.StopTextInput();
            settings.page = Page::GameArtwork;
            settings.focus = 0;
            dirty = true;
            return;
        }
        if (edit_field == 2) {
            if (commit) {
                std::ostringstream payload;
                payload << "{\"network\":" << std::quoted(network_id) << ",\"password\":" << std::quoted(edit_buffer)
                        << '}';
                start_worker("device.connect", payload.str());
            }
            std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
            edit_buffer.clear();
            s.StopTextInput();
            settings.page = Page::Network;
            settings.focus = 1;
            dirty = true;
            return;
        }
        if (commit) {
            if (edit_field == 0) username = edit_buffer;
            else password = edit_buffer;
        }
        std::fill(edit_buffer.begin(), edit_buffer.end(), '\0');
        edit_buffer.clear();
        s.StopTextInput();
        settings.page = Page::Scraper;
        dirty = true;
    }
    void move_edit_cursor(int direction) {
        if (direction < 0 && edit_cursor) {
            edit_cursor--;
            while (edit_cursor && (static_cast<unsigned char>(edit_buffer[edit_cursor]) & 0xc0) == 0x80) edit_cursor--;
        } else if (direction > 0 && edit_cursor < edit_buffer.size()) {
            edit_cursor++;
            while (edit_cursor < edit_buffer.size() &&
                   (static_cast<unsigned char>(edit_buffer[edit_cursor]) & 0xc0) == 0x80)
                edit_cursor++;
        }
        dirty = true;
    }
    void insert_text(const std::string& value) {
        if (edit_field == 6 && RoundedFont::codepoints(edit_buffer + value).size() > 16) return;
        if (edit_field == 5) {
            auto text = edit_buffer;
            text.insert(edit_cursor, value);
            auto points = RoundedFont::codepoints(text);
            size_t units = 0;
            for (auto c : points) units += c > 0xFFFF ? 2 : 1;
            if (units > 10) return;
        }
        if (edit_buffer.size() + value.size() <= 256 && value.find_first_of("\r\n\t") == std::string::npos) {
            edit_buffer.insert(edit_cursor, value);
            edit_cursor += value.size();
            search_changed();
        }
        dirty = true;
    }
    void erase_character() {
        if (!edit_cursor) return;
        size_t end = edit_cursor;
        move_edit_cursor(-1);
        edit_buffer.erase(edit_cursor, end - edit_cursor);
        search_changed();
        dirty = true;
    }
    void delete_character() {
        if (edit_cursor >= edit_buffer.size()) return;
        size_t begin = edit_cursor;
        move_edit_cursor(1);
        edit_buffer.erase(begin, edit_cursor - begin);
        edit_cursor = begin;
        search_changed();
        dirty = true;
    }
    void scraper_action() {
        if (settings.focus == 3) {
            preferences.game_info = !preferences.game_info;
            remember_appearance();
            return;
        }
        if (settings.focus == 4) {
            open_setting_picker();
            return;
        }
        if (settings.focus == 2) {
            review_artwork();
            return;
        }
        if (settings.focus == 0) {
            open_setting_picker();
            return;
        }
        if (worker > 0 && (worker_action == "index" || worker_action == "device.info" || worker_action == "status"))
            stop_worker();
        if (worker > 0) {
            std::ofstream(o.data / "scraper/cancel").put('\n');
            service_notice = "Stopping after this request";
            return;
        }
        start_worker("scrape");
    }
    bool brightness_drag = false, volume_drag = false;
    int unmuted_volume = 25;
    Uint32 volume_toast = 0;
    struct Input {
        Uint32 time;
        SDL_Event event;
    };
    std::vector<Input> inputs;
    size_t next_input = 0;
    std::vector<Surface> surfaces;
    std::vector<SDL_Joystick*> joysticks;
    InputWake input_wake;
    // An idle menu sleeps until input arrives; only held controls, pending
    // hardware writes and timed UI keep a short cadence.
    int idle_wait_ms(Uint32 now) {
        if (!input_wake.available()) return touchscreen.available() || volume_keys.available() ? 8 : 50;
        if (hold.active() || volume_keys.holding() || pointer_down || carousel.dragging || volume_drag ||
            brightness_drag || touch_tail)
            return 8;
        int wait = 250;
        if (!library_ready || settings.page == Page::Microphone) wait = 16;
        if (system.volume_pending() || system.brightness_pending() || volume_unsaved || brightness_unsaved)
            wait = std::min(wait, 20);
        if (worker > 0 || launching) wait = std::min(wait, 50);
        if (artwork_busy() && preferences.animations) wait = std::min(wait, 55);
        if (volume_toast) wait = std::min(wait, std::max(1, int(volume_toast - now)));
        if (hardware.charging) wait = std::min(wait, int(420 - now % 420) + 1);
        return wait;
    }
    bool wait_event(SDL_Event* event, int timeout) {
        return wait_ui_event(s, event, timeout,
                             !joysticks.empty() || touchscreen.available() || volume_keys.available(), &input_wake);
    }
    bool quit = false, probe, dirty = true, system_exit_ack = false;
    std::string status = "READY", last_input = "WAITING FOR INPUT";
    std::string top_name, bottom_name;
    int touch_x = -1, touch_y = -1;
    Uint32 last_axis = 0;
    Uint32 theme_poll = 0, theme_changed = 0;
    bool theme_pending = false;
    std::map<fs::path, std::optional<std::string>> theme_source;
    std::map<fs::path, std::optional<std::string>> read_theme_source() {
        std::map<fs::path, std::optional<std::string>> result;
        for (auto path : {o.themes / "light.ini", o.themes / "dark.ini", o.theme}) {
            if (path.empty()) continue;
            std::ifstream file(path, std::ios::binary);
            result[path] =
                file ? std::optional<std::string>(std::string(std::istreambuf_iterator<char>(file), {})) : std::nullopt;
        }
        return result;
    }
    // The selected light/dark palette with the optional DS favourite colour.
    void select_theme() {
        theme = preferences.dark ? dark_theme : light_theme;
        apply_accent(theme, preferences.accent, preferences.dark);
        if (preferences.high_contrast) apply_high_contrast(theme, preferences.dark);
    }
    void reload_themes() {
        auto light = fs::exists(o.themes / "light.ini") ? load_theme(o.themes / "light.ini") : default_theme();
        auto dark = fs::exists(o.themes / "dark.ini") ? load_theme(o.themes / "dark.ini", default_dark_theme())
                                                      : default_dark_theme();
        if (!o.theme.empty()) {
            bool is_dark = dark_theme_file(o.theme);
            auto custom = load_theme(o.theme, is_dark ? dark : light);
            if (is_dark) dark = std::move(custom);
            else light = std::move(custom);
        }
        light_theme = std::move(light);
        dark_theme = std::move(dark);
        select_theme();
    }
    void remember_appearance() {
        select_theme();
        try {
            save_preferences(o.preferences, preferences);
            status = "Settings saved";
        } catch (const std::exception& e) {
            status = "Could not save all settings. Check storage.";
            std::cerr << e.what() << std::endl;
            // Base and additive UI preferences have separate atomic files.
            // A partial failure must display what actually reached storage.
            try {
                preferences = load_preferences(o.preferences);
                select_theme();
            } catch (const std::exception&) {}
        }
        locale.load(asset_path("locales"), preferences.language);
        rounded.prefer(preferences.language);
        configure_sounds();
        dirty = true;
    }
    // Theme & display uses the shared settings list, four roomy rows per
    // screen: 0 colour mode, 1 accent colour, 2 high contrast, 3 typeface,
    // 4 icon style, 5 screen brightness, 6 animations, 7 touch indicator.
    // The two-choice rows show both options in place (tap a side, Left/Right,
    // or A to swap).
    using SegmentNames = std::array<const char*, 2>;
    static constexpr SegmentNames mode_names{"Light", "Dark"}, typeface_names{"DS rounded", "Pixel classic"};
    SDL_Rect segment_bounds(int index, int option) {
        const auto& names = index == 0 ? mode_names : typeface_names;
        int width = std::clamp(std::max(label_width(names[0], 18), label_width(names[1], 18)) + 34, 100, 150),
            left = 574 - 2 * width - 8;
        return {left + 4 + option * width, list_top + index * list_stride - list_scroll + 17, width, 32};
    }
    void segment_row(SDL_Renderer* r, int y, int index, const std::string& title, bool second) {
        int row = y + list_top + index * list_stride - list_scroll;
        bool focused = index == list_selected();
        int dx = 24;
        move_element(uint32_t(index), dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
        dx -= 24;
        settings_card(r, 24 + dx, row, 566, 66, focused);
        round_rect(r, 38 + dx, row + 11, 44, 44, 12, focused ? "accent" : "slot");
        ui_icon(r, index == 0 ? (second ? SettingsIcon::Moon : SettingsIcon::Sun) : SettingsIcon::Type, 60 + dx,
                row + 33, 25, focused ? "on_accent" : "accent");
        const auto& names = index == 0 ? mode_names : typeface_names;
        auto first = segment_bounds(index, 0);
        first.x += dx;
        first.y += y;
        label(r, title, 98 + dx, row + 19, 24, "ink", first.x - 16 - (98 + dx));
        sunken_track(r, first.x - 4, first.y - 4, first.w * 2 + 8, first.h + 8, 13);
        const float slide = segment_position(index, second ? 1 : 0);
        raised_pill(r, first.x + int(std::lround(slide * first.w)), first.y, first.w, first.h, 10, theme.at("accent"),
                    0, false);
        for (int option = 0; option < 2; option++) {
            SDL_Rect box{first.x + option * first.w, first.y, first.w, first.h};
            bool on = std::abs(slide - option) < .5f;
            int width = std::min(box.w - 12, label_width(names[size_t(option)], 18));
            label(r, names[size_t(option)], box.x + (box.w - width) / 2,
                  centred_label_y(locale.text(names[size_t(option)]), 18, box.y + box.h / 2), 18,
                  on ? "on_accent" : "ink", box.w - 12);
        }
    }
    void appearance_row(SDL_Renderer* r, int y, int i) {
        switch (i) {
        case 0: segment_row(r, y, i, "Colour mode", preferences.dark); break;
        case 1: {
            Color swatch = theme.at("accent");
            list_row(r, y, i, "Accent colour", accent_name(preferences.accent), true, &swatch);
            break;
        }
        case 2: list_row(r, y, i, "High contrast", preferences.high_contrast ? "On" : "Off"); break;
        case 3: segment_row(r, y, i, "Typeface", preferences.pixel); break;
        case 4: list_row(r, y, i, "Icon style", preferences.rich_icons ? "Rich" : "Simple pixel"); break;
        case 5: list_row(r, y, i, "Screen brightness", std::to_string(system_preferences.brightness) + "%"); break;
        // Animations opens its own page, so even "Off" is a value, not a switch.
        case 6:
            list_row(r, y, i, "Animations",
                     !preferences.animations       ? "Off"
                     : preferences.view_animations ? (preferences.view_style == ViewStyle::Push   ? "Push"
                                                      : preferences.view_style == ViewStyle::Zoom ? "Zoom"
                                                                                                  : "Spring")
                                                   : "Subtle",
                     true, nullptr, false);
            break;
        case 7: list_row(r, y, i, "Touch indicator", preferences.touch_indicator ? "On" : "Off"); break;
        case 8: list_row(r, y, i, "Home layout", preferences.home_layout == 1 ? "Column" : "Carousel"); break;
        case 9: list_row(r, y, i, "Background", preferences.background == 1 ? "Grid" : "Plain"); break;
        default: list_row(r, y, i, "Touch gestures", preferences.gestures ? "On" : "Off"); break;
        }
    }
    static std::string accent_name(int index) {
        return index >= 0 && index < int(ds_colours.size()) ? ds_colours[size_t(index)].name : "Classic";
    }
    void accent_swatch(SDL_Renderer* r, int cx, int cy, int radius, Color fill) {
        for (int dy = -radius; dy <= radius; dy++) {
            int half = int(std::sqrt(double(radius * radius - dy * dy)));
            tint_rect(r, cx - half, cy + dy, half * 2 + 1, 1, fill);
        }
        ring(r, cx, cy, radius, "line");
    }
    // Off, an open share that appears in Finder/Explorer like a NAS, or the
    // previous one-time password login. Both shares are advertised by name.
    void show_sharing_picker() {
        int current = device_status["sharing"] != "1" ? 0 : device_status["sharing_mode"] == "open" ? 1 : 2;
        show_picker(
            "Network file sharing", {"Off", "Open on this Wi-Fi", "Password protected"}, current,
            [this, current](int n) {
                if (n == current) return;
                start_worker(n == 0 ? "device.sharing-off" : n == 1 ? "device.sharing-open" : "device.sharing-on");
            });
    }
    // Theme & display > Accent colour: the DSi profile's 4x4 favourite-colour
    // grid. Tiles show the original DS colour and its name (never colour
    // alone); the interface uses the contrast-adjusted variant. Focus 0-15 are
    // tiles, 16 Classic blue, 17 High contrast.
    static constexpr int colour_grid_x = 28, colour_grid_y = 102, colour_tile_w = 137, colour_tile_h = 56,
                         colour_gap = 12, colour_row_pitch = 66;
    void open_colours();
    SDL_Rect colour_tile(int index) const;
    SDL_Rect colour_option(int index) const;
    void choose_colour(int index);
    void colours_action(int code);
    void colours_touch(int x, int y);
    // Pick whichever of white or near-black reads better on a raw DS colour.
    static Color tile_ink(Color fill) {
        const Color white{255, 255, 255}, ink{18, 24, 30};
        return contrast_ratio(white, fill) >= contrast_ratio(ink, fill) ? white : ink;
    }
    void colours_screen(SDL_Renderer* r, int y);
    void colours_top(SDL_Renderer* r, int y);
    // Left/Right (choice 0/1) set a row's value directly; A (choice 2)
    // swaps two-choice rows, flips switches and opens the rest.
    void choose_appearance(int choice) {
        const int row = settings.focus;
        const bool step = choice != 2;
        switch (row) {
        case 0: preferences.dark = step ? choice == 1 : !preferences.dark; break;
        case 1:
            if (!step) {
                open_colours();
                return;
            }
            preferences.accent =
                std::clamp(preferences.accent + (choice == 1 ? 1 : -1), -1, int(ds_colours.size()) - 1);
            break;
        case 2:
            preferences.high_contrast = step ? choice == 1 : !preferences.high_contrast;
            emit_sound(preferences.high_contrast ? MenuCue::Enable : MenuCue::Disable);
            break;
        case 3: {
            bool pixel = step ? choice == 1 : !preferences.pixel;
            if (!pixel) check_fonts();
            if (!pixel && !font_ready) {
                status = "ROUNDED FONT UNAVAILABLE";
                dirty = true;
                return;
            }
            preferences.pixel = pixel;
            break;
        }
        case 4:
            if (!step) {
                show_picker("Icon style", {"Simple pixel", "Rich"}, preferences.rich_icons ? 1 : 0, [this](int n) {
                    preferences.rich_icons = n == 1;
                    remember_appearance();
                });
                return;
            }
            preferences.rich_icons = choice == 1;
            break;
        case 5:
            if (step) {
                change_system(4, choice == 0 ? -1 : 1);
                return;
            }
            settings.enter();
            dirty = true;
            return;
        case 6:
            settings.page = Page::Motion;
            settings.focus = 0;
            dirty = true;
            return;
        case 7: preferences.touch_indicator = step ? choice == 1 : !preferences.touch_indicator; break;
        case 8:
            if (!step) {
                show_picker("Home layout", {"Carousel", "Column"}, preferences.home_layout, [this](int n) {
                    preferences.home_layout = n;
                    home_layout_changed();
                    remember_appearance();
                });
                return;
            }
            preferences.home_layout = choice;
            home_layout_changed();
            break;
        case 9:
            if (!step) {
                show_picker("Background", {"Plain", "Grid"}, preferences.background, [this](int n) {
                    preferences.background = n;
                    remember_appearance();
                });
                return;
            }
            preferences.background = choice;
            break;
        case 10:
            preferences.gestures = step ? choice == 1 : !preferences.gestures;
            if (preferences.gestures) gesture_intro();
            break;
        default: return;
        }
        remember_appearance();
    }
    void refresh_system() {
        hardware = system.status();
        dirty = true;
        if (o.preview) {
            std::ifstream file(o.data / "volume.ini");
            std::string line;
            while (std::getline(file, line))
                if (line.rfind("volume=", 0) == 0)
                    hardware.volume_percent = std::clamp(std::atoi(line.c_str() + 7), 0, 100);
        }
        if (hardware.volume_percent > 0) unmuted_volume = hardware.volume_percent;
        volume_applied = hardware.volume_percent;
    }
    void save_preview_volume(int value) {
        if (!o.preview) return;
        fs::create_directories(o.data);
        auto path = o.data / "volume.ini", pending = fs::path(path.string() + ".tmp");
        std::ofstream file(pending);
        file << "schema=1\nvolume=" << value << '\n';
        file.close();
        if (!file) throw std::runtime_error("Could not save volume");
        fs::rename(pending, path);
    }
    int confirmed_volume() {
        if (!o.preview) {
            auto fields = rgds::fields(o.data / "volume.ini");
            if (fields.count("volume")) {
                try {
                    size_t end = 0;
                    int value = std::stoi(fields["volume"], &end);
                    if (end == fields["volume"].size() && value >= 0 && value <= 100) return value;
                } catch (...) {}
            }
        }
        return system.cached().volume_percent;
    }
    void poll_volume() {
        if (auto update = system.poll_volume()) {
            if (update->success) {
                volume_applied = update->value;
                return_audio_failed = false;
            }
            if (update->settled && update->value == hardware.volume_percent) {
                if (!update->success) {
                    hardware.volume_percent = volume_applied = confirmed_volume();
                    status = "Could not set volume - see log";
                }
                try {
                    save_preview_volume(volume_applied);
                } catch (const std::exception& error) {
                    status = error.what();
                }
                volume_unsaved = false;
                dirty = true;
            }
        }
    }
    void flush_volume() {
        if (!volume_unsaved && !system.volume_pending()) return;
        bool ok = system.flush_volume();
        volume_applied = ok ? system.cached().volume_percent : confirmed_volume();
        if (!ok) {
            hardware.volume_percent = volume_applied;
            status = "Could not set volume - see log";
        }
        try {
            save_preview_volume(volume_applied);
        } catch (const std::exception& error) {
            status = error.what();
        }
        volume_unsaved = false;
        poll_volume();
    }
    void toggle_mute() {
        hold.clear();
        volume_drag = false;
        change_volume(hardware.volume_percent == 0 ? unmuted_volume : 0);
    }
    void change_volume(int value) {
        if (!hardware.volume) {
            status = "Volume control unavailable";
            dirty = true;
            return;
        }
        value = std::clamp(value, 0, 100);
        if (value == hardware.volume_percent && !return_audio_failed) {
            volume_toast = s.GetTicks() + 1800;
            last_activity = s.GetTicks();
            dirty = true;
            return;
        }
        if (return_writes_pending()) return_volume = value;
        else system.request_volume(value);
        volume_unsaved = true;
        hardware.volume_percent = value;
        if (value > 0) unmuted_volume = value;
        volume_toast = s.GetTicks() + 1800;
        last_activity = s.GetTicks();
        dirty = true;
    }
    void apply_timezone() {
        setenv("TZ", system_preferences.timezone.c_str(), 1);
        tzset();
        dirty = true;
    }
    // UTC offsets per zone, read from tzdata by switching TZ for a moment and
    // putting the chosen zone back. Only rows on screen ask, and the cache
    // empties every quarter hour so a daylight-saving change shows up.
    std::map<std::string, long> zone_offsets;
    long zone_offsets_bucket = -1;
    long zone_offset(const std::string& zone) {
        auto now = std::time(nullptr);
        if (now / 900 != zone_offsets_bucket) {
            zone_offsets.clear();
            zone_offsets_bucket = now / 900;
        }
        auto found = zone_offsets.find(zone);
        if (found != zone_offsets.end()) return found->second;
        std::tm local{};
        long offset = 0;
        if (zone != "UTC") {
            setenv("TZ", zone.c_str(), 1);
            tzset();
            if (localtime_r(&now, &local)) offset = local.tm_gmtoff;
            setenv("TZ", system_preferences.timezone.c_str(), 1);
            tzset();
        }
        return zone_offsets[zone] = offset;
    }
    static std::string offset_text(long seconds) {
        if (!seconds) return "UTC";
        auto minutes = std::abs(seconds) / 60;
        std::string text = std::string("UTC") + (seconds < 0 ? "-" : "+") + std::to_string(minutes / 60);
        if (minutes % 60) {
            char part[8];
            std::snprintf(part, sizeof(part), ":%02ld", minutes % 60);
            text += part;
        }
        return text;
    }
    std::string zone_time(const std::string& zone) {
        auto shifted = std::time(nullptr) + zone_offset(zone);
        std::tm t{};
        gmtime_r(&shifted, &t);
        return time_text(t);
    }
    std::string timezone_summary() {
        const auto& zone = system_preferences.timezone;
        return zone == "UTC" ? zone : timezone_place(zone) + " · " + offset_text(zone_offset(zone));
    }
    std::string timezone_title() {
        auto region = timezone_region(timezones.at(size_t(std::clamp(settings.focus, 0, int(timezones.size()) - 1))));
        return region == "UTC" ? locale.text("Timezone")
                               : locale.text("Timezone") + " · " + locale.text(timezone_region_name(region));
    }
    // The regions L and R (and the d-pad's Left and Right) jump to; empty at either end.
    std::pair<std::string, std::string> timezone_neighbours() {
        const int count = int(timezones.size()), focus = std::clamp(settings.focus, 0, count - 1);
        auto region = [&](int i) { return timezone_region(timezones[size_t(i)]); };
        int start = focus;
        while (start > 0 && region(start - 1) == region(focus)) start--;
        const int next = timezone_region_jump(focus, 1);
        return {start > 0 ? locale.text(timezone_region_name(region(start - 1))) : std::string(),
                region(next) != region(focus) ? locale.text(timezone_region_name(region(next))) : std::string()};
    }
    void load_system_configuration() {
        if (system_configuration_loaded) return;
        system_configuration_loaded = true;
        try {
            if (fs::exists(o.system_preferences)) {
                system_preferences = load_system_preferences(o.system_preferences);
                if (!probe && !o.game_menu) {
                    if (hardware.power && system_preferences.power > 0 &&
                        !system.set("power", system_preferences.power))
                        status = "Could not apply saved power mode";
                    if (hardware.brightness) {
                        if (system.set("brightness", system_preferences.brightness)) {
                            brightness_applied = system_preferences.brightness;
                            hardware.brightness_percent = brightness_applied;
                        } else {
                            system_preferences.brightness = brightness_applied;
                            status = "Could not apply saved brightness";
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "System preferences fallback: " << e.what() << std::endl;
        }
        if (!probe && !o.game_menu && hardware.lid && system_preferences.lid && !system.set("lid", 1))
            status = "Could not enable lid wake";
        apply_timezone();
    }
    void save_date_format(int value) {
        auto previous = system_preferences.date_format;
        system_preferences.date_format = value;
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            dirty = true;
        } catch (const std::exception& e) {
            system_preferences.date_format = previous;
            status = "Could not save date format";
            std::cerr << e.what() << std::endl;
            dirty = true;
        }
    }
    void save_time_format(int value) {
        auto previous = system_preferences.time_format;
        system_preferences.time_format = value;
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            dirty = true;
        } catch (const std::exception& e) {
            system_preferences.time_format = previous;
            status = "Could not save time format";
            std::cerr << e.what() << std::endl;
            dirty = true;
        }
    }
    void save_timezone() {
        auto previous = system_preferences.timezone;
        system_preferences.timezone = timezones.at(settings.focus);
        try {
            save_system_preferences(o.system_preferences, system_preferences);
            apply_timezone();
        } catch (const std::exception& e) {
            system_preferences.timezone = previous;
            status = "Could not save timezone";
            std::cerr << e.what() << std::endl;
            dirty = true;
        }
    }
    bool apply_control(int focus, const SystemPreferences& value) {
        if (return_writes_pending()) {
            if (focus == 0) return_power = value.power;
            else if (focus == 2) return_lid = value.lid;
            else if (focus == 4) return_brightness = value.brightness;
            return true;
        }
        if (focus == 0) return system.set("power", value.power);
        if (focus == 2) return system.set("lid", value.lid);
        if (focus == 4) return system.set("brightness", value.brightness);
        return true;
    }
    void poll_brightness() {
        if (auto result = system.poll_brightness()) {
            if (result->success) {
                brightness_applied = result->value;
                hardware.brightness_percent = result->value;
            } else if (result->settled && result->value == system_preferences.brightness) {
                system_preferences.brightness = brightness_applied;
                status = "Could not apply brightness";
                dirty = true;
            }
        }
        if (brightness_unsaved && !return_writes_pending() && !system.brightness_pending() && !brightness_drag &&
            s.GetTicks() - brightness_changed >= 180) {
            try {
                save_system_preferences(o.system_preferences, system_preferences);
                brightness_unsaved = false;
            } catch (const std::exception& e) {
                status = "Could not save brightness";
                brightness_unsaved = false;
                std::cerr << e.what() << std::endl;
                dirty = true;
            }
        }
    }
    void flush_brightness() {
        bool success = system.flush_brightness();
        if (auto result = system.poll_brightness()) {
            if (result->success) brightness_applied = result->value;
        }
        if (!success) {
            system_preferences.brightness = brightness_applied;
            status = "Could not apply brightness";
        }
        if (brightness_unsaved) {
            try {
                save_system_preferences(o.system_preferences, system_preferences);
                brightness_unsaved = false;
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
            }
        }
    }
    void change_system(int focus, int delta, bool cycle = false) {
        auto previous = system_preferences, next = previous;
        bool available = focus == 0   ? hardware.power
                         : focus == 1 ? hardware.sleep
                         : focus == 2 ? hardware.lid
                         : focus == 3 ? hardware.battery >= 0
                                      : hardware.brightness;
        if (!available) {
            status = "Control unavailable on this device";
            dirty = true;
            return;
        }
        if (focus == 0) next.power = cycle ? (next.power + 1) % 4 : std::clamp(next.power + delta, 0, 3);
        else if (focus == 1) next.idle = cycle ? (next.idle + 1) % 5 : std::clamp(next.idle + delta, 0, 4);
        else if (focus == 2) next.lid = cycle ? !next.lid : delta > 0;
        else if (focus == 3) next.battery_percent = cycle ? !next.battery_percent : delta > 0;
        else if (focus == 4)
            next.brightness = cycle ? (next.brightness >= 100 ? 5 : std::min(100, (next.brightness / 5 + 1) * 5))
                                    : std::clamp((next.brightness / 5 + delta) * 5, 5, 100);
        if (next.power == previous.power && next.idle == previous.idle && next.lid == previous.lid &&
            next.brightness == previous.brightness && next.battery_percent == previous.battery_percent)
            return;
        if (focus == 4) {
            system_preferences.brightness = next.brightness;
            if (return_writes_pending()) return_brightness = next.brightness;
            else system.request_brightness(next.brightness);
            brightness_changed = s.GetTicks();
            brightness_unsaved = true;
            dirty = true;
            return;
        }
        if (!apply_control(focus, next)) {
            status = "Could not apply setting - see log";
            dirty = true;
            return;
        }
        try {
            save_system_preferences(o.system_preferences, next);
            system_preferences = next;
            status = o.preview ? "Saved - desktop simulation" : "Settings saved";
        } catch (const std::exception& e) {
            if (!apply_control(focus, previous))
                std::cerr << "Could not restore previous hardware setting" << std::endl;
            status = "Could not save settings";
            std::cerr << e.what() << std::endl;
        }
        dirty = true;
    }
    void reset_after_sleep() {
        top_seen.clear();
        // Discard pre-suspend SDL/raw presses, including a held browser gesture.
        SDL_Event pending{};
        while (s.PollEvent(&pending)) {
            if (pending.type == SDL_QUIT) quit = true;
        }
        touchscreen.discard_until_release();
        cancel_navigation();
        brightness_drag = volume_drag = false;
        carousel.reset(navigation_selected());
        last_activity = s.GetTicks();
        dirty = true;
    }
    void restore_sleep_audio() { configure_sounds(); }
    void sleep_now(bool lid = false) {
        if (!hardware.sleep) {
            status = "Sleep unavailable on this device";
            dirty = true;
            return;
        }
        flush_return_controls();
        flush_volume();
        flush_brightness();
        save_state();
        status = o.preview ? "Sleep simulated - press any control" : "Sleeping - use Power to wake";
        dirty = true;
        draw();
        sounds.stop();
        // The private shared PCM can survive suspend just like an engine's
        // PCM. Keep it warm: reopening the codec can swallow a short cue and
        // delays the wake sound. Exclusive/custom configurations still release it.
        const char* shared_audio = std::getenv("RGDS_SHARED_AUDIO");
        bool warm_audio = !o.preview && shared_audio && std::string(shared_audio) == "1" && sounds.available();
        bool close_played = false;
        if (warm_audio || o.preview) close_played = emit_sound(MenuCue::LidClose);
        if (o.preview && close_played) {
            auto began = s.GetTicks();
            while (sounds.active() && s.GetTicks() - began < 400) s.Delay(5);
        }
        if (!warm_audio) sounds.close();
        bool ok = system.sleep(lid, close_played);
        status =
            ok ? (o.preview ? "Sleep simulated - settings retained" : "Welcome back") : "Could not sleep - see log";
        reset_after_sleep();
        // Stock resume hooks may reset the mixer even with the sink retained.
        if (warm_audio && !system.sync_volume()) sounds.close();
        restore_sleep_audio();
        if (ok) emit_sound(MenuCue::Unlock);
    }
    void watch_system(Uint32 now) {
        if (!return_writes_pending() && now - system_poll >= 10000 && now - system_retry >= 500) {
            system_retry = now;
            if (system.request_status()) system_poll = now;
        }
        // The game runner owns lid sleep while playing, including its overlay.
        if (o.preview || probe || o.game_menu) return;
        if (now - lid_poll >= 50) {
            lid_poll = now;
            auto closed = system.lid_closed();
            if (lid_sleep.poll(closed, system_preferences.lid && hardware.lid, system.lid_generation())) {
                sleep_now(true);
                return;
            }
        }
        if (!pointer_down && system_preferences.idle > 0 && hardware.sleep &&
            now - last_activity >= static_cast<Uint32>(idle_minutes[system_preferences.idle]) * 60000)
            sleep_now();
    }
    void watch_theme(Uint32 now) {
        if (!o.watch_assets || o.theme.empty() || now - theme_poll < 100) return;
        theme_poll = now;
        auto source = read_theme_source();
        if (source != theme_source) {
            theme_source = std::move(source);
            theme_changed = now;
            theme_pending = true;
        }
        // Editors may truncate then write, or replace the file via rename.
        if (theme_pending && now - theme_changed >= 200) {
            theme_pending = false;
            action(6);
        }
    }
    void restore_state() {
        if (o.state.empty()) return;
        std::ifstream navigation(o.state);
        std::string navigation_line;
        bool saved_home = false;
        while (std::getline(navigation, navigation_line)) {
            std::istringstream row(navigation_line);
            std::string marker;
            int category = 1;
            if (row >> marker >> category && marker == "home" && category >= 0 && category < int(home_count)) {
                saved_home = true;
                home_menu = true;
                home_selected = size_t(category);
            }
        }
        std::ifstream file(o.state);
        unsigned schema = 0, core = 0;
        size_t selected = 0;
        std::string path;
        if (!(file >> schema >> selected >> core >> std::quoted(path)) || schema != 1 || core >= core_count) return;
        if (!saved_home) home_menu = false;
        model.core = core;
        if (!model.games.empty()) {
            model.selected = std::min(selected, model.games.size() - 1);
            for (size_t i = 0; i < model.games.size(); i++)
                if (!path.empty() && model.games[i].path == fs::path(path)) {
                    model.selected = i;
                    break;
                }
        }
        int page = 0, section = 0, focus = 0;
        if (file >> page >> section >> focus && page == int(Page::ShaderPicker) && focus >= 0 &&
            focus <= shader_count) {
            std::string marker;
            int per_game = 0;
            if (file >> marker >> per_game && marker == "shader" && (per_game == 0 || per_game == 1) &&
                (!per_game || !model.games.empty())) {
                if (per_game) game_preferences = library_cache.options(model.games[model.selected]);
                open_shader_picker(per_game);
                if (o.game_menu) shader_return = Page::QuickMenu;
                settings.focus = std::min(focus, int(shader_choices.size()) - 1);
                shader_carousel.reset(size_t(std::max(0, settings.focus)));
                int scene = 0, mode = 2;
                float split = .5f;
                if (file >> marker >> scene >> mode >> split && marker == "shader-preview" && scene >= 0 &&
                    scene < ShaderPreview::scene_count && mode >= 0 && mode <= 2 && split >= .1f && split <= .9f) {
                    shader_preview_scene = scene;
                    shader_preview_mode = ShaderPreviewMode(mode);
                    shader_preview_split = split;
                }
                return;
            }
        }
        if (file && page == int(Page::Controls) && focus >= 0 && focus <= 18) {
            std::string marker, tail;
            int per_game = 0, parent = int(o.game_menu ? Page::QuickMenu : Page::Settings), saved_parent = parent;
            file >> marker >> per_game;
            std::getline(file, tail);
            std::istringstream extra(tail);
            if (extra >> saved_parent && (saved_parent == int(Page::Settings) || saved_parent == int(Page::QuickMenu) ||
                                          saved_parent == int(Page::GameOptions)))
                parent = saved_parent;
            open_controls(marker == "controls" && per_game == 1, static_cast<Page>(parent));
            settings.focus = focus;
            return;
        }
        if (file && page >= 0 &&
            (page <= int(Page::GameOptions) || page == int(Page::Startup) || page == int(Page::Statistics) ||
             page == int(Page::RomSetup) || page == int(Page::Discover) || page == int(Page::Motion) ||
             page == int(Page::Colours) || page == int(Page::MenuSounds)) &&
            page != int(Page::Keyboard) && page != int(Page::QuickMenu) && page != int(Page::LeaveGame) &&
            page != int(Page::Confirm) && section >= 0 && section <= 9 && focus >= 0 &&
            focus <= (page == int(Page::Motion)           ? 3
                      : page == int(Page::Timezone)       ? int(timezones.size()) - 1
                      : page == int(Page::Maintenance)    ? 6
                      : page == int(Page::LibraryOptions) ? 10
                      : page == int(Page::Appearance)     ? 10
                      : page == int(Page::System)         ? 7
                      : page == int(Page::Wifi) ? std::max(0, std::atoi(device_status["network_count"].c_str()) - 1)
                                                : 4))
            settings = {static_cast<Page>(page), section, focus};
        if (settings.page == Page::Power) {
            settings.section = 1;
        }
        if (settings.page == Page::Placeholder && settings.section == 1) settings.page = Page::Power;
        if (settings.page == Page::Placeholder && settings.section == 0) settings.page = Page::Appearance;
        if (settings.page == Page::Placeholder && settings.section >= 2)
            settings.page = settings.section == 2   ? Page::LibraryOptions
                            : settings.section == 3 ? Page::Sound
                            : settings.section == 4 ? Page::Network
                                                    : Page::System;
        if (settings.page == Page::Appearance || settings.page == Page::Display || settings.page == Page::Motion ||
            settings.page == Page::Colours)
            settings.section = 0;
        if (settings.page == Page::Display) settings.focus = 0;
        if (settings.page == Page::Sound || settings.page == Page::MenuSounds) {
            settings.section = 3;
            settings.focus = 0;
        }
        if (settings.page == Page::System || settings.page == Page::Timezone) settings.section = 5;
        if (settings.page == Page::System) settings.focus = std::min(settings.focus, 7);
        if (settings.page == Page::Startup) {
            settings.section = 5;
            settings.focus = std::min(settings.focus, 3);
            start_worker("device.startup-info");
        }
        if (settings.page == Page::Network) settings.focus = std::min(settings.focus, 4);
        if (settings.page == Page::Maintenance) settings.focus = std::min(settings.focus, 6);
        if (settings.page == Page::Statistics) settings.section = 7;
        if (settings.page == Page::GameOptions) {
            if (model.games.empty()) settings.page = Page::Library;
            else game_preferences = library_cache.options(model.games[model.selected]);
            settings.focus = std::min(settings.focus, 5);
        }
    }
    void save_state() {
        if (o.state.empty() || !session_initialized || !library_ready || settings.page == Page::BootWarning) return;
        try {
            if (!o.state.parent_path().empty()) fs::create_directories(o.state.parent_path());
            auto pending = o.state.string() + ".tmp";
            std::ofstream file(pending);
            file << 1 << ' ' << model.selected << ' ' << model.core << ' '
                 << std::quoted(model.games.empty() ? std::string() : model.games[model.selected].path.string())
                 << '\n';
            if (settings.page != Page::Library && settings.page != Page::Keyboard)
                file << static_cast<int>(settings.page) << ' ' << settings.section << ' ' << settings.focus << '\n';
            if (settings.page == Page::ShaderPicker) file << "shader " << shader_per_game << '\n';
            if (settings.page == Page::ShaderPicker)
                file << "shader-preview " << shader_preview_scene << ' ' << int(shader_preview_mode) << ' '
                     << shader_preview_split << '\n';
            if (settings.page == Page::Controls)
                file << "controls " << controls_per_game << ' ' << int(controls_return) << '\n';
            if (home_menu && !o.game_menu) file << "home " << home_selected << '\n';
            file.close();
            if (!file) throw std::runtime_error("Cannot save launcher state");
            fs::rename(pending, o.state);
        } catch (const std::exception& e) {
            std::cerr << "State: " << e.what() << std::endl;
        }
    }
    void color(SDL_Renderer* r, const std::string& key) {
        auto c = theme.at(key);
        s.SetRenderDrawColor(r, c.r, c.g, c.b, o.game_menu && key == "background" ? 56 : 255);
    }
    void rect(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, bool outline = false) {
        color(r, key);
        SDL_Rect box{x, y, w, h};
        if (outline) s.RenderDrawRect(r, &box);
        else s.RenderFillRect(r, &box);
    }
    Color blend(Color a, Color b, int amount) {
        return {static_cast<unsigned char>((a.r * (100 - amount) + b.r * amount) / 100),
                static_cast<unsigned char>((a.g * (100 - amount) + b.g * amount) / 100),
                static_cast<unsigned char>((a.b * (100 - amount) + b.b * amount) / 100)};
    }
    void tint_rect(SDL_Renderer* r, int x, int y, int w, int h, Color value) {
        s.SetRenderDrawColor(r, value.r, value.g, value.b, 255);
        SDL_Rect box{x, y, w, h};
        s.RenderFillRect(r, &box);
    }
    void sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength = 12) {
        auto base = theme.at(key);
        for (int i = 0; i < 12; i++) {
            int top = y + h * i / 12, bottom = y + h * (i + 1) / 12;
            auto shade =
                blend(base, i < 6 ? theme.at("panel") : theme.at("background"), strength * std::abs(6 - i) / 6);
            tint_rect(r, x, top, w, bottom - top, shade);
        }
    }
    void horizontal_sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength = 12) {
        auto base = theme.at(key), highlight = theme.at("panel");
        int span = std::max(1, w - 1);
        for (int column = 0; column < w; column++)
            tint_rect(r, x + column, y, 1, h, blend(base, highlight, strength * (w - 1 - column) / span));
    }
    void triangle(SDL_Renderer* r, int x, int y, int half, int height, const std::string& key, bool down = true) {
        color(r, key);
        for (int i = 0; i < height; i++) {
            int width = half * (height - i) / height;
            int row = down ? y + i : y + height - 1 - i;
            s.RenderDrawLine(r, x - width, row, x + width, row);
        }
    }
    void battery_icon(SDL_Renderer* r, int y) {
        const int x = 576, top = y + 14;
        round_rect(r, x, top, 38, 20, 3, "muted");
        round_rect(r, x + 2, top + 2, 34, 16, 2, "slot");
        rect(r, x + 38, top + 6, 3, 8, "muted");
        int bars = hardware.battery < 0    ? 0
                   : hardware.battery > 75 ? 4
                   : hardware.battery > 45 ? 3
                   : hardware.battery > 15 ? 2
                   : hardware.battery > 0  ? 1
                                           : 0;
        drawn_charge_phase = hardware.charging ? int((s.GetTicks() / 420) % 4) : -1;
        if (hardware.charging) bars = 1 + drawn_charge_phase;
        auto level = hardware.battery >= 0 && hardware.battery <= 15 && !hardware.charging ? Color{239, 129, 75}
                                                                                           : theme.at("accent");
        for (int i = 0; i < 4; i++)
            tint_rect(r, x + 5 + i * 7, top + 5, 5, 10,
                      i < bars ? level : blend(theme.at("slot"), theme.at("muted"), 16));
        if (hardware.charging) {
            color(r, "accent");
            s.RenderDrawLine(r, x - 10, top + 1, x - 16, top + 10);
            s.RenderDrawLine(r, x - 16, top + 10, x - 9, top + 10);
            s.RenderDrawLine(r, x - 9, top + 10, x - 15, top + 19);
        } else if (hardware.external_power) {
            color(r, "accent");
            s.RenderDrawLine(r, x - 15, top + 3, x - 15, top + 8);
            s.RenderDrawLine(r, x - 10, top + 3, x - 10, top + 8);
            s.RenderDrawLine(r, x - 17, top + 8, x - 8, top + 8);
            s.RenderDrawLine(r, x - 17, top + 8, x - 17, top + 12);
            s.RenderDrawLine(r, x - 8, top + 8, x - 8, top + 12);
            s.RenderDrawLine(r, x - 17, top + 12, x - 8, top + 12);
            s.RenderDrawLine(r, x - 13, top + 12, x - 13, top + 18);
        }
    }
    fs::path status_asset(const std::string& name) {
        return asset_path(std::string("status/") + (preferences.dark ? "dark-" : "light-") + name + ".bmp");
    }
    std::string wifi_asset() {
        if (device_status["wifi"] != "1") return "wifi-off";
        int signal = -1;
        if (!device_status["wifi_signal"].empty()) signal = std::atoi(device_status["wifi_signal"].c_str());
        if (probed_signal >= 0 && device_status["wifi_connected"] == "1") signal = probed_signal;
        int level = signal >= 67 ? 3 : signal >= 34 ? 2 : signal > 0 ? 1 : 0;
        return "wifi-" + std::to_string(level);
    }
    std::string sound_asset() { return hardware.volume && hardware.volume_percent == 0 ? "muted" : "sound"; }
    // Covers are being fetched: a small spinner and the count beside the
    // brand, on every screen, until the artwork worker reports it is done.
    bool artwork_busy() const {
        auto found = service.find("state");
        return found != service.end() && found->second == "scraping";
    }
    int loader_phase(Uint32 now) const { return preferences.animations ? int((now / 110) % 8) : 0; }
    void artwork_loader(SDL_Renderer* r, int y) {
        static const int dx[8] = {0, 5, 7, 5, 0, -5, -7, -5}, dy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
        const int head = drawn_loader_phase = loader_phase(s.GetTicks()), cx = 232, cy = y + 24;
        const Color ink = theme.at("accent");
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        for (int i = 0; i < 8; i++) {
            static const Uint8 tail[8] = {255, 200, 150, 105, 70, 50, 40, 40};
            s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, tail[(head - i + 8) % 8]);
            SDL_Rect dot{cx + dx[i] - 2, cy + dy[i] - 2, 4, 4};
            s.RenderFillRect(r, &dot);
        }
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        auto count = [&](const char* key) {
            auto found = service.find(key);
            return found == service.end() ? std::string("0") : found->second;
        };
        const std::string progress = count("completed") + "/" + count("total");
        label(r, progress, 246, status_label_y(progress, 16, y), 16, "muted", 110, false);
    }
    int drawn_loader_phase = -1;
    static int pixel_scale(int size) { return std::max(1, (size + 4) / 10); }
    // Classic-text width in 6-dot cells; kana and kanji take two.
    static int pixel_cells(const std::string& value, int limit = 1000) {
        int cells = 0;
        for (auto cp : RoundedFont::codepoints(value, limit)) cells += pixel_wide(cp) ? 2 : 1;
        return cells;
    }
    int label_width(const std::string& source, int size, bool translate = true) {
        auto value = translate ? locale.text(source) : source;
        return preferences.pixel ? pixel_cells(value) * pixel_scale(size) * 6
                                 : int(std::ceil(rounded.width(value, size)));
    }
    std::string status_stamp() {
        auto now = std::time(nullptr);
        char stamp[32];
        const auto& local = *std::localtime(&now);
        std::strftime(stamp, sizeof(stamp), date_format_patterns.at(system_preferences.date_format), &local);
        return std::string(stamp) + "  " + time_text(local);
    }
    // The label y that puts this text's ink (capitals and digits) on `cy`.
    int centred_label_y(const std::string& value, int size, int cy) {
        auto bounds = preferences.pixel ? std::pair<int, int>{pixel_scale(size) * 3, pixel_scale(size) * 10}
                                        : rounded.ink_bounds(value, size);
        return cy - (bounds.first + bounds.second + 1) / 2;
    }
    int status_label_y(const std::string& value, int size, int y) { return centred_label_y(value, size, y + 24); }
    void status_bar(SDL_Renderer* r, int y) {
        sheen(r, 0, y, 640, 48, "background", 24);
        if (!preferences.rich_icons) {
            // The pixel style's own status symbols, at the same 24 px as its icons.
            draw_pixel_glyph(s, r, pixel_status_glyph(sound_asset()), 24, y + 24, 24, theme.at("ink"));
            draw_pixel_glyph(s, r, pixel_status_glyph(wifi_asset()), 58, y + 24, 24, theme.at("ink"));
        } else {
            media.draw(r, status_asset(sound_asset()), {10, y + 10, 28, 28}, false);
            media.draw(r, status_asset(wifi_asset()), {44, y + 10, 28, 28}, false);
        }
        // Rendered at its drawn size; its capitals match the clock's (14 px),
        // centred on the bar's text line (y + 24) like the labels.
        media.draw(r, asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"),
                   {84, y + 13, 129, 22}, false);
        if (artwork_busy()) artwork_loader(r, y);
        drawn_status_stamp = status_stamp();
        const auto& stamp = drawn_status_stamp;
        int right = hardware.battery >= 0 && system_preferences.battery_percent ? 494 : 550;
        label(r, stamp, right - label_width(stamp, 20), status_label_y(stamp, 20, y), 20, "ink", right - 180);
        update_status_chip(r, y, right - label_width(stamp, 20) - 14);
        battery_icon(r, y);
        if (system_preferences.battery_percent && hardware.battery >= 0) {
            auto percent = std::to_string(hardware.battery) + "%";
            label(r, percent, 554 - label_width(percent, 20), status_label_y(percent, 20, y), 20, "muted", 65);
        }
        rect(r, 0, y + 47, 640, 1, "line");
    }
    int point_size(int size) { return size == 1 ? 16 : size == 2 ? 20 : size == 3 ? 28 : 36; }
    void text(SDL_Renderer* r, std::string value, int x, int y, int size, const std::string& key, int max_chars = 80,
              bool translate = true) {
        if (translate) value = locale.text(value);
        if (!preferences.pixel && font_ready) {
            rounded.draw(r, value, x, y, point_size(size), theme.at(key), max_chars, std::max(1, 612 - x));
            return;
        }
        y += size * 3; // Match the rounded font's visible top, rather than its line-box top.
        color(r, key);
        int n = 0;
        for (unsigned c : RoundedFont::codepoints(value)) {
            if (++n > max_chars) break;
            if (pixel_wide(c)) {
                // The rounded glyph, centred in two cells and standing on the dot baseline.
                try {
                    const auto glyph = utf8_encode(c);
                    const int points = size * 10;
                    const int w = static_cast<int>(std::lround(rounded.width(glyph, points)));
                    rounded.draw(r, glyph, x + size * 6 - w / 2, y + size * 7 - rounded.ascent(points), points,
                                 theme.at(key), 1, 1000);
                } catch (const std::exception&) {}
                x += size * 12;
                continue;
            }
            unsigned accent = 0;
            auto vietnamese = vietnamese_pixel_glyph(c);
            if (vietnamese.base) c = vietnamese.base;
            if (c >= 0xe0 && c <= 0xfe && c != 0xf7) c -= 32;
            auto extended = extended_pixel_glyph(c);
            if (extended.base) {
                c = extended.base;
                accent = static_cast<unsigned>(extended.accent);
            }
            switch (c) {
            case 0xc0:
            case 0xc1:
            case 0xc2:
            case 0xc3:
            case 0xc4:
                accent = c - 0xc0 + 1;
                c = 'A';
                break;
            case 0xc8:
            case 0xc9:
            case 0xca:
            case 0xcb:
                accent = c == 0xcb ? 5 : c - 0xc8 + 1;
                c = 'E';
                break;
            case 0xcc:
            case 0xcd:
            case 0xce:
            case 0xcf:
                accent = c == 0xcf ? 5 : c - 0xcc + 1;
                c = 'I';
                break;
            case 0xd2:
            case 0xd3:
            case 0xd4:
            case 0xd5:
            case 0xd6:
                accent = c - 0xd2 + 1;
                c = 'O';
                break;
            case 0xd9:
            case 0xda:
            case 0xdb:
            case 0xdc:
                accent = c == 0xdc ? 5 : c - 0xd9 + 1;
                c = 'U';
                break;
            case 0xd1:
                accent = 4;
                c = 'N';
                break;
            case 0xc7:
                accent = 6;
                c = 'C';
                break;
            case 0xdd:
                accent = 2;
                c = 'Y';
                break;
            case 0xa1: c = '!'; break;
            case 0xbf: c = '?'; break;
            default: break;
            }
            if (accent) {
                auto dot = [&](int col, int row) {
                    SDL_Rect p{x + col * size, y + row * size, size, size};
                    s.RenderFillRect(r, &p);
                };
                if (accent == 1) {
                    dot(1, -2);
                    dot(2, -1);
                } else if (accent == 2) {
                    dot(3, -2);
                    dot(2, -1);
                } else if (accent == 3) {
                    dot(2, -2);
                    dot(1, -1);
                    dot(3, -1);
                } else if (accent == 4) {
                    dot(1, -2);
                    dot(2, -2);
                    dot(2, -1);
                    dot(3, -1);
                } else if (accent == 5) {
                    dot(1, -2);
                    dot(3, -2);
                } else if (accent == 6) {
                    dot(2, 7);
                    dot(1, 8);
                } else if (accent == 7) {
                    dot(3, 7);
                    dot(4, 8);
                } else if (accent == 8) {
                    dot(2, -2);
                } else if (accent == 9) {
                    dot(1, -2);
                    dot(2, -1);
                    dot(3, -2);
                } else if (accent == 10) {
                    dot(1, 3);
                    dot(2, 2);
                }
            }
            if (vietnamese.base) {
                auto dot = [&](int col, int row) {
                    SDL_Rect p{x + col * size, y + row * size, size, size};
                    s.RenderFillRect(r, &p);
                };
                if (vietnamese.shape == 1) {
                    dot(2, -2);
                    dot(1, -1);
                    dot(3, -1);
                } else if (vietnamese.shape == 2) {
                    dot(1, -2);
                    dot(2, -1);
                    dot(3, -2);
                } else if (vietnamese.shape == 3) {
                    dot(4, -1);
                    dot(5, -2);
                } else if (vietnamese.shape == 4) {
                    for (int col = 0; col < 4; col++) dot(col, 3);
                }
                int lift = vietnamese.shape && vietnamese.shape != 4 ? -2 : 0;
                if (vietnamese.tone == 1) {
                    dot(1, -2 + lift);
                    dot(2, -1 + lift);
                } else if (vietnamese.tone == 2) {
                    dot(3, -2 + lift);
                    dot(2, -1 + lift);
                } else if (vietnamese.tone == 3) {
                    dot(2, -3 + lift);
                    dot(3, -2 + lift);
                    dot(2, -1 + lift);
                } else if (vietnamese.tone == 4) {
                    dot(1, -2 + lift);
                    dot(2, -2 + lift);
                    dot(2, -1 + lift);
                    dot(3, -1 + lift);
                } else if (vietnamese.tone == 5) dot(2, 8);
            }
            if (c == 0xe6) c = 0xc6;
            else if (c == 0xf8) c = 0xd8;
            else if (c == 0xe5) c = 0xc5;
            else if (c == 0xe9) c = 'E';
            else if (c == 0x2019) c = '\'';
            else if (c == 0x2026) c = '.';
            else if (c == 0x2013 || c == 0x2014) c = '-';
            else if (c == '"' || c == 0xab || c == 0xbb || (c >= 0x201c && c <= 0x201e)) c = '\'';
            const auto* bitmap = cyrillic_pixel_bitmap(c);
            if (!bitmap) {
                char ch = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
                auto it = glyphs.find(ch);
                if (it == glyphs.end()) it = glyphs.find('?');
                bitmap = &it->second;
            }
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++) {
                    if ((*bitmap)[row] & (1 << (4 - col))) {
                        SDL_Rect p{x + col * size, y + row * size, size, size};
                        s.RenderFillRect(r, &p);
                    }
                }
            x += size * 6;
        }
    }
    void centered(SDL_Renderer* r, const std::string& source, int y, int size, const std::string& key, int limit = 48) {
        auto value = locale.text(source);
        auto count = std::min(static_cast<int>(RoundedFont::codepoints(value).size()), limit);
        if (!preferences.pixel && font_ready) {
            auto width = std::min(rounded.width(value, point_size(size), limit), 584.0f);
            text(r, value, static_cast<int>(320 - width / 2), y, size, key, limit);
            return;
        }
        while (count > 0 && pixel_cells(value, count) * size * 6 > 584) count--;
        text(r, value, 320 - pixel_cells(value, count) * size * 3, y, size, key, count);
    }
    void label(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, const std::string& key,
               int width = 560, bool translate = true) {
        auto value = translate ? locale.text(source) : source;
        if (!preferences.pixel && font_ready) {
            rounded.draw(r, value, x, y, pixels, theme.at(key), 1000, width);
            return;
        }
        int scale = pixel_scale(pixels);
        text(r, value, x, y, scale, key, std::min(80, width / (scale * 6)), false);
    }
    void raw_label(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, const std::string& key,
                   int width = 560) {
        label(r, value, x, y, pixels, key, width, false);
    }
    void raw_wrapped(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, int width, int rows = 3) {
        wrapped(r, value, x, y, pixels, width, rows, false);
    }
    // The DSi's START frame around a selected tile: one accent body in one
    // continuous shade, the tile set into it and the word centred in the
    // space below over a soft engraved shadow.
    void start_frame(SDL_Renderer* r, int x, int row, int tile, const std::string& word, int band = 45) {
        const Color accent = theme.at("accent");
        raised_pill(r, x - 9, row - 9, tile + 18, tile + band + 17, 8, accent);
        fill_round(r, x - 3, row - 3, tile + 6, tile + 8, 6, blend(accent, Color{0, 0, 0}, 30));
        round_rect(r, x - 1, row - 1, tile + 2, tile + 2, 4, "panel");
        const int width = label_width(word, 20),
                  word_y = centred_label_y(locale.text(word), 20, row + tile + 8 + band / 2);
        label_colour(r, word, x + (tile - width) / 2, word_y + 1, 20, blend(accent, Color{0, 0, 0}, 40), tile);
        label(r, word, x + (tile - width) / 2, word_y, 20, "on_accent", tile);
    }
    std::string category_title(size_t index) {
        static const std::array<std::string, home_count> titles = {"Settings", "Games",     "Favourites",
                                                                   "Recents",  "PictoChat", "Last played"};
        return titles.at(index);
    }
    void category_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
        static const std::array<SettingsIcon, home_count> symbols = {SettingsIcon::Sliders, SettingsIcon::Cartridge,
                                                                     SettingsIcon::Heart,   SettingsIcon::Clock,
                                                                     SettingsIcon::Chat,    SettingsIcon::Play};
        ui_icon(r, symbols.at(index), x + size / 2, y + size / 2, size, "accent");
    }

    void settings_footer(SDL_Renderer* r, int y) {
        if (settings.page == Page::ShaderPicker) return;
        rect(r, 0, y + 424, 640, 56, "background");
        rect(r, 24, y + 424, 592, 1, "line");
        const bool pressed = footer_pressed(true), a_pressed = footer_pressed(false);
        footer_button(r, 12, y + 432, 160, "Back", "B", false, pressed);
        if (settings.page == Page::Keyboard) {
            const bool searching = edit_field == 4 || edit_field == 6;
            key_hints(r, 320, y + 452, {{"X", "Delete"}, {"Start", searching ? "Search" : "Done"}}, 0, 16, 280);
            footer_button(r, 468, y + 432, 160, searching ? "Search" : "Done", "", true, a_pressed);
            return;
        }
        if (settings.page == Page::Statistics || settings.page == Page::Microphone) return;
        if (settings.page == Page::GameInfo) {
            if (!picker_open) key_hints(r, 320, y + 452, {{"L R", "Picture"}, {"A", "Next picture"}}, 0, 16, 280);
            return;
        }
        if (settings.page == Page::GameSwitcher) key_hints(r, 320, y + 452, {{"X", "Game menu"}}, 0, 16, 280);
        if (settings.page == Page::SaveGallery) {
            if (!picker_open) {
                std::vector<KeyHint> keys{{"Y", "More"}};
                if (slot_occupied(settings.focus)) keys.insert(keys.begin(), {"X", "Delete"});
                key_hints(r, 320, y + 452, keys, 0, 16, 280);
            }
            if (slot_primary_enabled(settings.focus))
                footer_button(r, 468, y + 432, 160, slot_primary_name(), "A", true, a_pressed);
            return;
        }
        if (settings.page == Page::Cheats && !picker_open)
            key_hints(r, 320, y + 452, {{"L R", "Tabs"}, {"X", "Search"}}, 0, 16, 280);
        if (settings.page == Page::Maintenance && !picker_open) {
            const auto stage = update_stage();
            if (stage != UpdateStage::None && stage != UpdateStage::Running)
                hotkey_pill(r, 184, y + 432, 272, 40, update_action_name(stage), KeyLook::Panel,
                            pointer_down && contact_y >= 424 && contact_x >= 200 && contact_x < 440);
        }
        if ((settings.page == Page::EmulatorOptions && !emulator_option_enabled(settings.focus)) ||
            (settings.page == Page::Cheats && cheat_view != CheatView::Groups && !cheat_togglable(settings.focus)) ||
            (settings.page == Page::GameOptions && settings.focus == 2 && superdrastic_game()))
            return;
        footer_button(r, 468, y + 432, 160,
                      settings.page == Page::ShaderPicker ? "Apply"
                      : settings.page == Page::Cheats     ? (cheat_view == CheatView::Groups ? "Open" : "Toggle")
                      : settings.page == Page::Settings   ? "Open"
                      : settings.page == Page::DeviceInfo ? "View"
                      : settings.page == Page::Sound      ? (settings.focus == 2            ? "Open"
                                                             : settings.focus == 1          ? "Test"
                                                             : hardware.volume_percent == 0 ? "Unmute"
                                                                                            : "Mute")
                      : settings.page == Page::Display    ? "Done"
                                                          : "Select",
                      "A", true, a_pressed);
    }
    static const char* icon_asset(SettingsIcon symbol) {
        switch (symbol) {
        case SettingsIcon::Play: return "play";
        case SettingsIcon::Save: return "save";
        case SettingsIcon::Load: return "load";
        case SettingsIcon::Shader: return "shaders";
        case SettingsIcon::Sun: return "brightness";
        case SettingsIcon::Restart: return "restart";
        case SettingsIcon::Home: return "home";
        case SettingsIcon::Art: return "artwork";
        case SettingsIcon::Library: return "library";
        case SettingsIcon::Sliders: return "settings";
        case SettingsIcon::Cartridge: return "games";
        case SettingsIcon::Heart: return "favorites";
        case SettingsIcon::Clock: return "recent";
        case SettingsIcon::Display: return "display";
        case SettingsIcon::Power: return "power";
        case SettingsIcon::Volume: return "sound";
        case SettingsIcon::Wifi: return "wifi";
        case SettingsIcon::System: return "system";
        case SettingsIcon::Controller: return "controls";
        case SettingsIcon::Shutdown: return "shutdown";
        case SettingsIcon::Chat: return "pictochat";
        default: return nullptr;
        }
    }
    void ui_icon(SDL_Renderer* r, SettingsIcon symbol, int x, int y, int size, const std::string& key) {
        if (drawing_elements && view_transitions_enabled() && view_motion.active()) {
            double pop = view_motion.accent(uint32_t(symbol), view_drawn_at);
            size = int(std::round(size * (1 + .14 * pop)));
            y -= int(std::round(3 * pop));
        }
        if (!preferences.rich_icons && draw_pixel_icon(s, r, symbol, x, y, size, theme.at(key))) return;
        auto name = icon_asset(symbol);
        if (preferences.rich_icons && name && size >= 20 &&
            media.draw(r, asset_path(std::string("icons/") + name + ".bmp"), {x - size / 2, y - size / 2, size, size},
                       false))
            return;
        draw_settings_icon(s, r, symbol, x, y, size, theme.at(key));
    }
    void settings_card(SDL_Renderer* r, int x, int y, int w, int h, bool focused = false) {
        round_rect(r, x, y + 3, w, h, 14, "slot");
        round_rect(r, x, y, w, h, 14, focused ? "accent" : "panel");
        if (focused) round_rect(r, x + 2, y + 2, w - 4, h - 4, 12, "focus");
    }
    void list_row(SDL_Renderer* r, int y, int index, const std::string& title, const std::string& value = "",
                  bool enabled = true, const Color* swatch = nullptr, bool switches = true) {
        int row = y + list_top + index * list_stride - list_scroll;
        bool focused = index == list_selected();
        int dx = 24;
        move_element(uint32_t(index), dx, row, 566, 66, focused, list_top, list_top + list_height, 20, 595);
        dx -= 24;
        settings_card(r, 24 + dx, row, 566, 66, focused);
        round_rect(r, 38 + dx, row + 11, 44, 44, 12, focused && enabled ? "accent" : "slot");
        ui_icon(r,
                settings.page == Page::SaveGallery    ? gallery_icon()
                : settings.page == Page::ShaderPicker ? SettingsIcon::Shader
                : settings.page == Page::Cheats       ? SettingsIcon::Spark
                : settings.page == Page::Timezone     ? SettingsIcon::Clock
                                                      : settings_icon_for(title),
                60 + dx, row + 33, 25, enabled ? (focused ? "on_accent" : "accent") : "muted");
        const bool toggle = switches && (value == "On" || value == "Off");
        const int dot = swatch ? 22 : 0;
        const int value_width = value.empty() ? 0
                                : toggle      ? 82
                                              : std::min(224, label_width(value, 18) + (enabled ? 46 : 28) + dot);
        const int text_width = 464 - (value_width ? value_width + 16 : 28);
        label(r, title, 98 + dx, row + 19, 24, enabled ? "ink" : "muted", text_width,
              settings.page != Page::Wifi && settings.page != Page::Cheats &&
                  settings.page != Page::Timezone); // network and cheat names are data
        if (toggle) {
            draw_toggle(r, 524 + dx, row + 19, title, value == "On", enabled);
        } else if (!value.empty()) {
            // Even 14 px margins; text, swatch and chevron on one centre line.
            int x = 574 - value_width + dx;
            round_rect(r, x, row + 18, value_width, 30, 15, focused ? "panel" : "slot");
            if (swatch) accent_swatch(r, x + 22, row + 33, 8, *swatch);
            label(r, value, x + 14 + dot, centred_label_y(locale.text(value), 18, row + 33), 18,
                  enabled ? "accent" : "muted", value_width - (enabled ? 46 : 28) - dot);
            if (enabled) ui_icon(r, SettingsIcon::Chevron, x + value_width - 17, row + 33, 12, "accent");
        } else if (enabled) ui_icon(r, SettingsIcon::Chevron, 570 + dx, row + 33, 14, focused ? "accent" : "muted");
    }
    void draw_toggle(SDL_Renderer* r, int x, int y, const std::string& name, bool value, bool enabled = true) {
        auto key = std::make_pair(int(settings.page), name);
        auto now = s.GetTicks();
        auto found = toggle_motion.find(key);
        if (found == toggle_motion.end())
            found = toggle_motion.emplace(key, ToggleMotion{value, float(value), now - 100}).first;
        auto& motion = found->second;
        auto position = [&]() {
            float t = preferences.animations ? std::min(1.f, (now - motion.at) / 90.f) : 1.f;
            t = 1 - (1 - t) * (1 - t);
            return motion.from + (float(motion.value) - motion.from) * t;
        };
        if (motion.value != value) {
            motion.from = position();
            motion.value = value;
            motion.at = now;
            micro_until = now + 100;
        }
        sunken_track(r, x, y, 44, 28, 14);
        if (enabled && value) raised_pill(r, x + 1, y + 1, 42, 26, 13, theme.at("accent"), 0, false);
        const int knob = x + 5 + int(14 * position());
        alpha_round(r, knob, y + 7, 18, 18, 9, shadow_colour(), preferences.dark ? 90 : 40);
        round_rect(r, knob, y + 5, 18, 18, 9, "panel");
        alpha_round(r, knob + 4, y + 7, 10, 5, 2, Color{255, 255, 255}, preferences.dark ? 40 : 150);
        label(r, value ? "On" : "Off", x - 42, centred_label_y(locale.text(value ? "On" : "Off"), 16, y + 14), 16,
              enabled ? "ink" : "muted", 36);
    }
    void list_scrollbar(SDL_Renderer* r, int y);
    // The vertical rail after the horizontal one: a recessed groove with a
    // pip per page, a glass window under the thumb, and a glossy thumb with
    // an engraved grip that widens while it is held.
    // The vertical rail: a quiet slot with a recessed groove down its middle
    // and a glossy thumb with an engraved grip. Nothing changes on hover;
    // dragging only adds a soft accent glow, so the thumb never jumps.
    void draw_scrollbar(SDL_Renderer* r, int x, int y, int height, int thumb, int offset, bool dragging, bool hover,
                        int width = 36) {
        (void)hover;
        const int cx = x + width / 2;
        round_rect(r, x, y, width, height, width / 2, "slot");
        round_rect(r, cx - 4, y + 10, 8, height - 20, 4, "line");
        round_rect(r, cx - 3, y + 11, 6, height - 22, 3, "background");
        const int top = y + offset, tw = width - 10, left = cx - tw / 2;
        alpha_round(r, left, top + 3, tw, thumb, tw / 2, shadow_colour(), preferences.dark ? 70 : 30);
        if (dragging)
            alpha_round(r, left - 3, top - 3, tw + 6, thumb + 6, tw / 2 + 3, theme.at("accent"),
                        preferences.dark ? 70 : 50);
        glossy_pill(r, left, top, tw, thumb, tw / 2, theme.at("accent"), dragging ? .5 : 0);
        for (int line = -1; line <= 1; line++) {
            tint_rect(r, cx - 6, top + thumb / 2 + line * 6 + 1, 12, 2, blend(theme.at("accent"), Color{0, 0, 0}, 28));
            round_rect(r, cx - 6, top + thumb / 2 + line * 6, 12, 2, 1, "on_accent");
        }
    }
    void round_rect(SDL_Renderer* r, int x, int y, int w, int h, int radius, const std::string& key) {
        color(r, key);
        SDL_Rect center{x + radius, y, w - 2 * radius, h};
        s.RenderFillRect(r, &center);
        for (int dy = 0; dy < radius; dy++) {
            int inset =
                radius - static_cast<int>(std::sqrt(radius * radius - (radius - dy - 0.5) * (radius - dy - 0.5)));
            SDL_Rect a{x + inset, y + dy, w - inset * 2, 1}, b{x + inset, y + h - 1 - dy, w - inset * 2, 1};
            s.RenderFillRect(r, &a);
            s.RenderFillRect(r, &b);
        }
        rect(r, x, y + radius, w, h - radius * 2, key);
    }
    // Explicit colours (DS colour tiles) reuse the themed drawing paths.
    void fill_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill) {
        theme["swatch"] = fill;
        round_rect(r, x, y, w, h, radius, "swatch");
    }
    void label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width) {
        theme["swatch_ink"] = ink;
        label(r, value, x, y, pixels, "swatch_ink", width);
    }
    void raw_label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width) {
        theme["swatch_ink"] = ink;
        raw_label(r, value, x, y, pixels, "swatch_ink", width);
    }
    void check_mark(SDL_Renderer* r, int cx, int cy, Color ink) {
        s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, 255);
        for (int w = 0; w < 2; w++) {
            s.RenderDrawLine(r, cx - 5, cy + w, cx - 1, cy + 4 + w);
            s.RenderDrawLine(r, cx - 1, cy + 4 + w, cx + 6, cy - 4 + w);
        }
    }
    void ring(SDL_Renderer* r, int x, int y, int radius, const std::string& key) {
        color(r, key);
        for (int i = 0; i < 48; i++) {
            double a = i * 6.2831853 / 48, b = (i + 1) * 6.2831853 / 48;
            s.RenderDrawLine(r, x + int(std::cos(a) * radius), y + int(std::sin(a) * radius),
                             x + int(std::cos(b) * radius), y + int(std::sin(b) * radius));
        }
    }
    void icon(SDL_Renderer* r, int kind, int x, int y, const std::string& key) {
        color(r, key);
        if (kind == 0) {
            ring(r, x, y, 11, key);
            ring(r, x - 4, y - 3, 2, key);
            ring(r, x + 4, y - 3, 2, key);
            ring(r, x, y + 5, 2, key);
        } else if (kind == 1) {
            ring(r, x, y + 2, 10, key);
            rect(r, x - 3, y - 12, 6, 15, "panel");
            color(r, key);
            s.RenderDrawLine(r, x, y - 12, x, y + 1);
        } else if (kind == 2) {
            rect(r, x - 11, y - 9, 24, 18, key, true);
            s.RenderDrawLine(r, x - 4, y - 9, x - 4, y + 9);
            s.RenderDrawLine(r, x + 4, y - 9, x + 4, y + 9);
        } else if (kind == 3) {
            rect(r, x - 10, y - 4, 5, 8, key);
            s.RenderDrawLine(r, x - 5, y - 4, x + 1, y - 9);
            s.RenderDrawLine(r, x + 1, y - 9, x + 1, y + 9);
            s.RenderDrawLine(r, x + 1, y + 9, x - 5, y + 4);
            s.RenderDrawLine(r, x + 6, y - 7, x + 10, y);
            s.RenderDrawLine(r, x + 10, y, x + 6, y + 7);
        } else if (kind == 4) {
            for (int i = 0; i < 3; i++) {
                int d = 5 + i * 5;
                s.RenderDrawLine(r, x - d, y - i * 5, x, y - 5 - i * 5);
                s.RenderDrawLine(r, x, y - 5 - i * 5, x + d, y - i * 5);
            }
            rect(r, x - 1, y + 5, 3, 3, key);
        } else {
            ring(r, x, y, 10, key);
            s.RenderDrawLine(r, x, y - 1, x, y + 6);
            rect(r, x - 1, y - 6, 2, 2, key);
        }
    }
    void gear(SDL_Renderer* r, int x, int y) {
        ring(r, x, y, 9, "accent");
        ring(r, x, y, 3, "accent");
        color(r, "accent");
        for (int i = 0; i < 8; i++) {
            double a = i * 6.2831853 / 8;
            s.RenderDrawLine(r, x + int(std::cos(a) * 9), y + int(std::sin(a) * 9), x + int(std::cos(a) * 13),
                             y + int(std::sin(a) * 13));
        }
    }
    void choose_browser_layout() {
        show_picker("Browser layout", {"Cartridges", "Covers", "List"}, preferences.layout, [this](int n) {
            preferences.layout = n;
            cancel_navigation();
            remember_appearance();
        });
    }
    void choose_library_view() {
        show_picker("Library view",
                    {"All games", "Favourites", "Recents", "Cartridges", "Covers", "List", "Jump to letter",
                     "Search games", "Clear filters"},
                    preferences.view, [this](int n) {
                        if (n < 3) {
                            library_view(n);
                            remember_appearance();
                        } else if (n < 6) {
                            preferences.layout = n - 3;
                            cancel_navigation();
                            remember_appearance();
                        } else if (n == 6) jump_to_letter();
                        else if (n == 7) open_discover();
                        else clear_discovery();
                    });
    }
    // Three sections of three equal tiles with room between them.
    static SDL_Rect library_view_choice(int index) {
        return {40 + (index % 3) * 191, 120 + (index / 3) * 100, 178, 52};
    }
    void library_choice_icon(SDL_Renderer* r, int index, int x, int y, int size, const std::string& ink) {
        if (index == 5) {
            for (int i = -1; i <= 1; i++) {
                round_rect(r, x - size / 2, y + i * size / 3 - 1, 3, 3, 1, ink);
                round_rect(r, x - size / 2 + 7, y + i * size / 3 - 1, size - 7, 3, 1, ink);
            }
            return;
        }
        if (index == 7) {
            search_icon(r, x, y, size, ink);
            return;
        }
        static constexpr std::array<SettingsIcon, 9> icons = {
            SettingsIcon::Cartridge, SettingsIcon::Heart,   SettingsIcon::Clock,
            SettingsIcon::Cartridge, SettingsIcon::Art,     SettingsIcon::Library,
            SettingsIcon::Type,      SettingsIcon::Library, SettingsIcon::Refresh};
        ui_icon(r, icons.at(index), x, y, size, ink);
    }
    static std::string initial_letter(const std::string& title) {
        auto first = title.find_first_not_of(" ");
        if (first == std::string::npos) return "#";
        unsigned char c = title[first];
        if (c >= 'a' && c <= 'z') c -= 32;
        return c >= 'A' && c <= 'Z' ? std::string(1, char(c)) : "#";
    }
    // The displayed title when it is already known; otherwise the file name's.
    // Opening the sheet never reads metadata for every cartridge on the card.
    std::string jump_title(size_t index) {
        const auto& game = model.games[index];
        if (game.demo) return game.title;
        if (const auto* entry = library_cache.peek(game); entry && entry->title_ready) return entry->title;
        return library_title(game.title);
    }
    void jump_to_letter() {
        std::map<std::string, size_t> first;
        for (size_t i = 0; i < model.games.size(); i++) first.emplace(initial_letter(jump_title(i)), i);
        std::vector<std::string> letters;
        std::vector<size_t> targets;
        int selected = 0;
        auto current = model.games.empty() ? "" : initial_letter(jump_title(model.selected));
        for (const auto& entry : first) {
            if (entry.first == current) selected = int(letters.size());
            letters.push_back(entry.first);
            targets.push_back(entry.second);
        }
        show_picker("Jump to letter", letters, selected, [this, targets](int n) {
            if (n >= 0 && n < int(targets.size())) select_game(targets[n], false);
        });
    }
    void show_picker(std::string title, std::vector<std::string> options, int selected, std::function<void(int)> apply);
    void show_emulator_picker(bool per_game);
    const char* drastic_backend_name(int value) const;
    void show_drastic_backend_picker(bool per_game);
    void show_drastic_screen_picker();
    std::string emulator_choice_hint() const;
    bool open_setting_picker();
    std::string device_info_name(int row) const;
    std::string device_info_value(int row);
    void open_device_detail() { show_detail(device_info_name(settings.focus), device_info_value(settings.focus)); }
    void show_detail(const std::string& title, const std::string& value);
    int detail_max_scroll() const { return std::max(0, int(detail_lines.size()) - 8); }
    void scroll_detail_rail(int y);
    int picker_visible() const { return std::min(8, int(picker_options.size())); }
    int picker_stride() const { return std::min(52, 322 / std::max(1, picker_visible())); }
    bool picker_scrollable() const { return !picker_detail && picker_title == "Language"; }
    int picker_height() const { return picker_visible() * picker_stride(); }
    int picker_max_scroll() const;
    int picker_thumb() const;
    int picker_thumb_offset() const;
    void reveal_picker_focus();
    void scroll_picker(int position);
    void seek_picker(int y);
    int picker_first() const;
    bool picker_paged() const;
    int picker_top() const;
    void picker_action(int code);
    void picker_touch(int x, int y);
    void render_picker(SDL_Renderer* r, int y);
    void render_picker_body(SDL_Renderer* r, int y);
    // One back control everywhere: Settings, the in-game menu and the library
    // header share its size, rim, shadow, arrow and pressed colours.
    static constexpr int back_size = 44;
    void back_button(SDL_Renderer* r, int x, int y, bool pressed) {
        round_rect(r, x, y + 3, back_size, back_size, 12, "slot");
        int top = y + (pressed ? 2 : 0);
        round_rect(r, x, top, back_size, back_size, 12, pressed ? "accent" : "line");
        round_rect(r, x + 1, top + 1, back_size - 2, back_size - 2, 11, pressed ? "accent" : "panel");
        ui_icon(r, SettingsIcon::Back, x + back_size / 2, top + back_size / 2, 24, pressed ? "on_accent" : "accent");
    }
    void settings_header(SDL_Renderer* r, int y, const std::string& title, int width = 475, bool translate = true) {
        bool pressed = pointer_down && contact_x < 80 && contact_y < 78;
        back_button(r, 28, y + 20, pressed);
        label(r, title, 88, y + 25, 28, "ink", width, translate);
        rect(r, 24, y + 77, 592, 1, "line");
        rect(r, 24, y + 76, 44, 2, "accent");
    }
    void choice(SDL_Renderer* r, int x, int y, const std::string& title, bool selected, bool focused);
    void settings_top(SDL_Renderer* r, int y);
    // What the radio is doing right now, as a phone would show it: a green
    // card while connected, a spinner while connecting or scanning, a plain
    // note while off or idle, and the service's message when it failed.
    bool network_busy() const {
        return worker > 0 && (worker_action == "device.connect" || worker_action == "device.scan" ||
                              worker_action == "device.wifi-on" || worker_action == "device.wifi-off");
    }
    void network_status_card(SDL_Renderer* r, int y);
    void search_icon(SDL_Renderer* r, int x, int y, int size, const std::string& ink) {
        ring(r, x - 2, y - 2, size / 3, ink);
        color(r, ink);
        for (int w = 0; w < 2; w++) s.RenderDrawLine(r, x + size / 6, y + size / 6 + w, x + size / 2, y + size / 2 + w);
    }
    void keyboard_screen(SDL_Renderer* r, int y);
    void search_keyboard_top(SDL_Renderer* r, int y);
    void settings_bottom(SDL_Renderer* r, int y);
    void cart(SDL_Renderer* r, int x, int y, int width, int height, bool selected, int number) {
        rect(r, x + 4, y + 5, width, height, "line");
        rect(r, x, y, width, height, selected ? "accent" : "slot");
        rect(r, x + 10, y + 12, width - 20, height - 37, "panel");
        rect(r, x + width - 27, y, 27, 14, "background");
        text(r, "DS", x + 18, y + 22, 3, "ink");
        text(r, std::to_string(number), x + 18, y + 56, 2, "muted");
        for (int i = 0; i < 5; i++) rect(r, x + 13 + i * (width - 20) / 5, y + height - 18, 6, 10, "ink");
    }
    // The clock as the user chose it: 24-hour, or 12-hour with AM/PM.
    std::string time_text(const std::tm& t) const {
        char buf[16];
        if (system_preferences.time_format == 1) {
            std::snprintf(buf, sizeof(buf), "%d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min,
                          t.tm_hour < 12 ? "AM" : "PM");
            return buf;
        }
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        return buf;
    }
    std::string clock() {
        auto now = std::time(nullptr);
        return time_text(*std::localtime(&now));
    }
    fs::path folder_for_game(const Game& game) { return library_cache.folder(game); }
    fs::path item_folder(size_t index) { return folder_for_game(model.games[index]); }
    std::string game_title(size_t index) {
        auto& game = model.games[index];
        if (game.demo) return o.game_menu ? locale.text(game.title) : game.title;
        auto& entry = library_cache.metadata(game);
        if (!entry.title_ready) {
            entry.title = entry.info["title"];
            if (entry.title.empty()) entry.title = entry.info["artwork_title"];
            if (entry.title.empty()) entry.title = library_title(game.title);
            if (entry.title.empty()) entry.title = entry.banner["title"];
            if (entry.title.empty()) entry.title = media.banner(game.path).title;
            entry.title_ready = true;
        }
        return entry.title;
    }
    void warm_icon(SDL_Renderer* r, const Game& game) {
        if (!game.demo && !media.warm(r, game.path, true)) media.warm(r, folder_for_game(game) / "icon.bmp");
    }
    void warm_icon(SDL_Renderer* r, size_t index) { warm_icon(r, model.games[index]); }
    // The same icon, read and decoded by the loader thread; it appears on the
    // frame after it arrives, so browsing never waits on the card.
    void request_icon(SDL_Renderer* r, const Game& game) {
        if (game.demo) return;
        media.request(r, game.path, true);
        media.request(r, folder_for_game(game) / "icon.bmp");
    }
    void prepare_render_assets() {
        if (settings.page == Page::GameSwitcher)
            for (auto& panel : surfaces) {
                for (auto& game : switch_games) warm_icon(panel.renderer, game);
                if (settings.focus < int(switch_games.size()))
                    media.warm(panel.renderer, folder_for_game(switch_games[settings.focus]) / "box.bmp");
            }
        // The focused cover is ready for the very first Games frame. Keep it
        // warm while on Home too; neighbours are prepared after presentation.
        if (!o.game_menu && settings.page == Page::Library && !model.games.empty())
            for (auto& panel : surfaces) {
                media.warm(panel.renderer, item_folder(model.selected) / "box.bmp");
                warm_icon(panel.renderer, model.selected);
            }
        if (home_menu && home_selected > 0 && home_selected < pictochat_tile) {
            const auto& last = summary.groups[home_selected > 1 ? home_selected - 1 : 0].last_game;
            if (last)
                for (auto& panel : surfaces) warm_icon(panel.renderer, *last);
        }
        if (home_menu && preferences.home_layout == 1 && summary.groups[0].last_game)
            for (auto& panel : surfaces) warm_icon(panel.renderer, *summary.groups[0].last_game);
        if (settings.page == Page::SaveGallery) {
            auto path = slots["image_" + std::to_string(settings.focus)];
            if (!path.empty())
                for (auto& panel : surfaces) media.warm(panel.renderer, path);
        }
        for (auto& panel : surfaces) {
            for (const char* name : {"brand/mark-48.bmp", "brand/mark-120.bmp"})
                media.warm(panel.renderer, asset_path(name));
            media.warm(panel.renderer,
                       asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"));
            media.warm(panel.renderer,
                       asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"));
        }
        for (auto& panel : surfaces) {
            media.warm(panel.renderer, status_asset(sound_asset()));
            media.warm(panel.renderer, status_asset(wifi_asset()));
        }
        if (preferences.rich_icons && settings.page != Page::BootWarning)
            for (auto& panel : surfaces)
                for (const char* name :
                     {"settings", "games", "favorites", "recent",     "pictochat", "display",  "power",
                      "library",  "sound", "wifi",      "system",     "controls",  "shutdown", "play",
                      "save",     "load",  "shaders",   "brightness", "restart",   "home",     "artwork"}) {
                    if (settings.page == Page::GameSwitcher && std::string(name) != "settings" &&
                        std::string(name) != "games" && std::string(name) != "recent")
                        continue;
                    if (settings.page == Page::Library && std::string(name) != "settings" &&
                        std::string(name) != "games" && std::string(name) != "favorites" &&
                        std::string(name) != "recent" && std::string(name) != "pictochat" &&
                        !(picker_open && (picker_title == "Library view" || picker_title == "Browser layout") &&
                          std::string(name) == "artwork"))
                        continue;
                    media.warm(panel.renderer, asset_path(std::string("icons/") + name + ".bmp"));
                }
        // Statistics uses game identities from the summary, independent of
        // the current library filter. Prepare only the bottom-screen icons.
        if (settings.page == Page::Statistics && summary_ready && !surfaces.empty())
            for (const auto& entry : summary.most_played) warm_icon(surfaces.back().renderer, entry.game);
        carousel.item_spacing = !home_menu && browse_layout() == 1 ? 158 : Carousel::spacing;
        if (settings.page == Page::ShaderPicker && !surfaces.empty())
            shader_preview.warm(surfaces.front().renderer, shader_preview_folder(), shader_preview_scene,
                                preview_shader(), true);
        if (probe || model.games.empty() || home_menu || systems_menu) return;
        // QuickMenu displays its own identity and controls. Defer ROM/banner
        // metadata until a page actually needs the game title.
        if (o.game_menu) {
            game_title(0);
            for (auto& panel : surfaces) media.warm(panel.renderer, o.data / "session/menu-background.bmp");
            return;
        }
        game_title(model.selected);
        if (o.game_menu || settings.page != Page::Library) return;
        library_cache.stats(model.games[model.selected]);
        browser_list.clamp(model.games.size());
        int first = preferences.layout == 2 ? browser_list.first() : int(std::floor(carousel.position)) - 3;
        int end = preferences.layout == 2 ? browser_list.last(model.games.size()) : first + 7;
        // List metadata and icon fallbacks are ready before either draw pass.
        if (preferences.layout == 2)
            for (int index = std::max(0, first); index < std::min(end, int(model.games.size())); index++)
                game_title(size_t(index));
        // Favourite hearts: the visible cartridges' options are read here, never while drawing.
        for (int index = std::max(0, first); index < std::min(end, int(model.games.size())); index++)
            if (!model.games[size_t(index)].demo) library_cache.options(model.games[size_t(index)]);
        if (!model.games[model.selected].demo) library_cache.options(model.games[model.selected]);
        for (size_t panel = 0; panel < surfaces.size(); panel++) {
            auto* renderer = surfaces[panel].renderer;
            // The selected cartridge is prepared in place so its cover and
            // title never pop in; the visible neighbours and a wide window
            // beyond them load on the loader thread, nearest first.
            media.warm(renderer, item_folder(model.selected) / "box.bmp");
            warm_icon(renderer, model.selected);
            if (!o.preview && panel == 0) continue;
            (void)first;
            (void)end;
            const int reach = preferences.layout == 2 ? 14 : 10, centre = int(model.selected),
                      count = int(model.games.size());
            for (int distance = 1; distance <= reach; distance++)
                for (int direction : {1, -1}) {
                    int index = centre + direction * distance;
                    if (index >= 0 && index < count) request_icon(renderer, model.games[size_t(index)]);
                }
        }
    }
    // Covers near the selection load in the background, nearest first; cached
    // misses are remembered too, so missing artwork is never asked for twice.
    void prepare_next_cover() {
        if (o.game_menu || home_menu || settings.page != Page::Library || preferences.layout != 1 ||
            model.games.empty())
            return;
        int center = int(std::lround(carousel.position));
        for (int distance = 1; distance <= 5; distance++)
            for (int direction : {-1, 1}) {
                int index = center + direction * distance;
                if (index < 0 || index >= int(model.games.size())) continue;
                media.request(surfaces.back().renderer, item_folder(size_t(index)) / "box.bmp");
            }
    }
    void watch_library_assets(Uint32 now) {
        if (now - library_poll < 1000) return;
        library_poll = now;
        bool changed = !home_menu && !model.games.empty() && library_cache.refresh(model.games[model.selected]);
        if (media.refresh()) {
            library_cache.invalidate_titles();
            changed = true;
        }
        if (changed) dirty = true;
    }
    void pattern(SDL_Renderer* r, int y, int height, bool lower = false) {
        auto bg = theme.at("background"), fg = theme.at("line");
        if (preferences.background == 1 && lower) {
            // The original DS touch screen: plain square tiles, 8 DS pixels a
            // side (20 px here), each rule with a pale edge after it so the
            // tiles read as pressed into the screen. Quieter than the paper
            // above, so the controls on top own the contrast.
            const Color rule = blend(bg, fg, preferences.dark ? 30 : 34),
                        edge = blend(bg, Color{255, 255, 255}, preferences.dark ? 4 : 55);
            for (int row = y + 19; row < y + height; row += 20) {
                tint_colour(r, rule);
                s.RenderDrawLine(r, 0, row, 639, row);
                if (row + 1 < y + height) {
                    tint_colour(r, edge);
                    s.RenderDrawLine(r, 0, row + 1, 639, row + 1);
                }
            }
            for (int column = 19; column < 640; column += 20) {
                tint_colour(r, rule);
                s.RenderDrawLine(r, column, y, column, y + height - 1);
                tint_colour(r, edge);
                s.RenderDrawLine(r, column + 1, y, column + 1, y + height - 1);
            }
            return;
        }
        if (preferences.background == 1) {
            // The DSi's graph paper: faint rules every 4 px, a light grid every
            // 16 px and a firmer one every 64 px, each major line with a pale
            // edge under it so the paper reads as embossed rather than drawn on.
            auto shade = [&](int parts) {
                return Color{(unsigned char)((bg.r * (parts - 1) + fg.r) / parts),
                             (unsigned char)((bg.g * (parts - 1) + fg.g) / parts),
                             (unsigned char)((bg.b * (parts - 1) + fg.b) / parts)};
            };
            const Color fine = shade(22), minor = shade(9), major = shade(4),
                        edge = blend(bg, preferences.dark ? Color{255, 255, 255} : Color{255, 255, 255},
                                     preferences.dark ? 5 : 60);
            tint_colour(r, fine);
            for (int row = y; row < y + height; row += 4)
                if (row % 16) s.RenderDrawLine(r, 0, row, 639, row);
            tint_colour(r, minor);
            for (int row = y + (16 - y % 16) % 16; row < y + height; row += 16)
                if (row % 64) s.RenderDrawLine(r, 0, row, 639, row);
            for (int column = 16; column < 640; column += 16)
                if (column % 64) s.RenderDrawLine(r, column, y, column, y + height - 1);
            for (int row = y + (64 - y % 64) % 64; row < y + height; row += 64) {
                tint_colour(r, major);
                s.RenderDrawLine(r, 0, row, 639, row);
                if (row + 1 < y + height) {
                    tint_colour(r, edge);
                    s.RenderDrawLine(r, 0, row + 1, 639, row + 1);
                }
            }
            for (int column = 0; column < 640; column += 64) {
                tint_colour(r, major);
                s.RenderDrawLine(r, column, y, column, y + height - 1);
                tint_colour(r, edge);
                s.RenderDrawLine(r, column + 1, y, column + 1, y + height - 1);
            }
            return;
        }
        s.SetRenderDrawColor(r, (bg.r * 19 + fg.r) / 20, (bg.g * 19 + fg.g) / 20, (bg.b * 19 + fg.b) / 20, 255);
        for (int row = y; row < y + height; row += 4) s.RenderDrawLine(r, 0, row, 639, row);
    }
    void game_icon(SDL_Renderer* r, const Game& game, int x, int y, int size, size_t index = 0) {
        SDL_Rect box{x, y, size, size};
        if (!game.demo && ((game.system == "nds" && media.draw(r, game.path, box, true)) ||
                           media.draw(r, folder_for_game(game) / "icon.bmp", box)))
            return;
        if (!game.demo) {
            round_rect(r, x + size / 8, y + size / 8, size * 3 / 4, size * 3 / 4, 4, "slot");
            // The system's badge, as large as fits the slot (GBA in a small switcher row too).
            const std::string badge = system_info(game.system).badge;
            int text = 24;
            while (text > 10 && label_width(badge, text, false) > size * 3 / 4 - 4) text -= 2;
            raw_label(r, badge, x + (size - label_width(badge, text, false)) / 2,
                      centred_label_y(badge, text, y + size / 2), text, "muted", size * 3 / 4);
            return;
        }
        // Original preview symbols, never Nintendo artwork.
        int q = std::max(2, size / 8);
        round_rect(r, x, y, size, size, 4, "focus");
        switch (index % 5) {
        case 0:
            rect(r, x + q * 2, y + q * 2, q * 4, q * 4, "accent");
            rect(r, x + q * 3, y + q, q * 2, q * 6, "ink");
            rect(r, x + q, y + q * 3, q * 6, q * 2, "ink");
            rect(r, x + q * 3, y + q * 3, q * 2, q * 2, "on_accent");
            break;
        case 1:
            for (int i = 0; i < 4; i++)
                rect(r, x + q * (1 + (i % 2) * 3), y + q * (1 + (i / 2) * 3), q * 2, q * 2, i % 2 ? "accent" : "ink");
            break;
        case 2:
            rect(r, x + q * 3, y + q, q * 2, q * 6, "ink");
            for (int i = 0; i < 3; i++) rect(r, x + q * 3, y + q * (1 + i * 2), q * 2, q, "on_accent");
            break;
        case 3:
            ring(r, x + size / 2, y + size / 2, size / 3, "accent");
            rect(r, x + q * 3, y + q * 3, q * 2, q * 2, "ink");
            break;
        default:
            rect(r, x + q * 2, y + q * 2, q * 4, q * 4, "accent");
            rect(r, x + q * 3, y + q * 2, q, q * 4, "on_accent");
            rect(r, x + q * 2, y + q * 3, q * 4, q, "on_accent");
            break;
        }
    }
    void game_icon(SDL_Renderer* r, size_t index, int x, int y, int size) {
        game_icon(r, model.games[index], x, y, size, index);
    }
    bool cover_art(SDL_Renderer* r, const fs::path& path, SDL_Rect box, bool selected) {
        box = media.fitted_box(r, path, box);
        round_rect(r, box.x, box.y + 3, box.w + 6, box.h + 6, 10, "slot");
        round_rect(r, box.x - 3, box.y - 3, box.w + 6, box.h + 6, 10, selected ? "accent" : "line");
        round_rect(r, box.x, box.y, box.w, box.h, 7, "panel");
        return media.draw(r, path, box, false, 255, false, 7);
    }
    void wrapped(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, int width, int rows = 3,
                 bool translate = true) {
        auto value = translate ? locale.text(source) : source;
        std::string line;
        int row = 0;
        auto measure = [&](const std::string& str) {
            return preferences.pixel ? float(pixel_cells(str) * std::max(2, (pixels + 6) / 10) * 6)
                                     : rounded.width(str, pixels);
        };
        for (const auto& piece : line_pieces(value)) {
            std::string candidate = line.empty() ? piece.text : line + (piece.space_before ? " " : "") + piece.text;
            if (!line.empty() && measure(candidate) > width) {
                label(r, line, x, y + row * (pixels + 7), pixels, "ink", width, false);
                if (++row >= rows) return;
                line = piece.text;
            } else line = candidate;
        }
        if (!line.empty() && row < rows) label(r, line, x, y + row * (pixels + 7), pixels, "ink", width, false);
    }
    fs::path asset_path(const std::string& name) const {
        return (o.app.empty() ? fs::path(".") : o.app) / "assets" / name;
    }
    // The tile is drawn from the bitmap rendered at the nearest size above.
    static const char* mark_asset(int size) {
        return size <= 24    ? "brand/mark-24.bmp"
               : size <= 48  ? "brand/mark-48.bmp"
               : size <= 120 ? "brand/mark-120.bmp"
                             : "brand/mark-144.bmp";
    }
    // Powering off or restarting: both screens fade to the background, the
    // mark sits on the upper one inside a turning ring, and nothing accepts
    // input until the system takes over. A failed request lifts the veil.
    Uint32 shutdown_at = 0;
    std::string shutdown_kind;
    bool shutting_down() const { return shutdown_at != 0; }
    void begin_shutdown(const std::string& kind) {
        shutdown_at = s.GetTicks();
        shutdown_kind = kind;
        cancel_navigation();
        picker_open = false;
        micro_until = shutdown_at + 600;
        dirty = true;
    }
    void shutdown_overlay(SDL_Renderer* r, int y, bool upper) {
        if (!shutting_down()) return;
        const Uint32 now = s.GetTicks();
        const double t = preferences.animations ? std::min(1.0, (now - shutdown_at) / 420.0) : 1.0,
                     ease = 1 - (1 - t) * (1 - t);
        const Color bg = theme.at("background");
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, bg.r, bg.g, bg.b, Uint8(std::lround(255 * ease)));
        SDL_Rect veil{0, y, 640, 480};
        s.RenderFillRect(r, &veil);
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        if (ease < .6) return;
        const int alpha = int(std::lround(255 * std::min(1.0, (ease - .6) / .4)));
        if (upper) {
            const int cx = 320, cy = y + 206;
            alpha_round(r, cx - 70, cy - 66, 140, 140, 34, shadow_colour(),
                        int((preferences.dark ? 60 : 18) * alpha / 255));
            round_rect(r, cx - 70, cy - 70, 140, 140, 34, "panel");
            brand(r, cx - 48, cy - 48, 96);
            const double spin = preferences.animations ? (now % 1400) / 1400.0 : 0;
            feedback_arc(r, cx, cy, 86, 0, 1, theme.at("slot"), alpha, 5);
            feedback_arc(r, cx, cy, 86, spin, .28, theme.at("accent"), alpha, 5);
            const std::string text = locale.text(shutdown_kind == "reboot" ? "Restarting…" : "Powering off…");
            raw_label(r, text, 320 - label_width(text, 26, false) / 2, y + 318, 26, "ink", 400);
            const std::string note = locale.text("Your saves are safe. See you soon!");
            raw_label(r, note, 320 - label_width(note, 16, false) / 2, y + 358, 16, "muted", 500);
        } else {
            const double spin = preferences.animations ? (now % 1400) / 1400.0 : 0;
            feedback_arc(r, 320, y + 240, 26, spin, .3, theme.at("accent"), alpha, 4);
        }
    }
    void brand(SDL_Renderer* r, int x, int y, int size) {
        if (!media.draw(r, asset_path(mark_asset(size)), {x, y, size, size}, false))
            ui_icon(r, SettingsIcon::Display, x + size / 2, y + size / 2, size, "accent");
    }
    bool boot_animating(Uint32) const {
        return o.boot_startup && preferences.animations && (settings.page == Page::BootWarning || !library_ready);
    }
    double boot_phase() const { return preferences.animations ? ((s.GetTicks() - boot_scene_at) % 2400) / 2400.0 : 0; }
    void boot_caption(SDL_Renderer* r, const std::string& value, int y, int pixels, const std::string& key) {
        const int width = std::min(576, label_width(value, pixels));
        label(r, value, 320 - width / 2, y, pixels, key, 576);
    }
    void boot_emblem(SDL_Renderer* r, int y) {
        // The identity tile settles in once (a short rise and fade), then
        // rests; a soft floor shadow breathes slowly while the library loads.
        const double t = preferences.animations ? std::min(1.0, (s.GetTicks() - boot_scene_at) / 420.0) : 1.0;
        const double ease = 1 - std::pow(1 - t, 3);
        const int lift = int(std::lround(6 * (1 - ease)));
        const double breath = preferences.animations ? (1 + std::cos(boot_phase() * 6.283185307)) * .5 : .5;
        // A soft floor shadow (always darker than the page, in both modes).
        const Color shade{0, 0, 0};
        for (int i = 0; i < 6; i++) {
            int half = 44 - i * 6 + int(4 * breath);
            tint_rect(r, 320 - half, y + 128 + i, half * 2, 1,
                      blend(theme.at("background"), shade, (preferences.dark ? 5 : 7) - i));
        }
        // Visible from frame one (the identity never pops in from nothing).
        if (!media.draw(r, asset_path("brand/mark-120.bmp"), {260, y + lift, 120, 120}, false,
                        static_cast<unsigned char>(std::lround(255 * (.55 + .45 * ease)))))
            ui_icon(r, SettingsIcon::Display, 320, y + 60, 96, "accent");
    }
    void boot_wordmark(SDL_Renderer* r, int y, int) {
        // Drawn at its rendered size (289 x 48): no scaling, crisp edges.
        if (!media.draw(r, asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"),
                        {176, y, 289, 48}, false))
            boot_caption(r, "RG DS Core", y + 8, 28, "ink");
    }
    void boot_scene(SDL_Renderer* r, int y) {
        rect(r, 0, y, 640, 480, "background");
        boot_emblem(r, y + 112);
        boot_wordmark(r, y + 268, 356);
        // Five soft round dots, a slow wave of light passing through them.
        for (int i = 0; i < 5; i++) {
            double wave = preferences.animations ? (1 + std::cos(boot_phase() * 6.283185307 - i * .65)) * .5 : .4;
            const Color c = blend(theme.at("background"), theme.at("accent"), int(22 + 68 * wave));
            const int cx = 288 + i * 16, cy = y + 368;
            for (int dy = -4; dy <= 4; dy++) {
                int half = int(std::sqrt(std::max(0.0, 16.5 - dy * dy)));
                tint_rect(r, cx - half, cy + dy, half * 2 + 1, 1, c);
            }
        }
        boot_caption(r, "Preparing your library…", y + 403, 16, "muted");
    }
    void boot_top(SDL_Renderer* r, int y) {
        rect(r, 0, y, 640, 480, "background");
        // Small, deliberate safety mark and an open text layout, like the
        // original handheld welcome screen rather than a settings dialog.
        Color amber{239, 178, 65};
        s.SetRenderDrawColor(r, amber.r, amber.g, amber.b, 255);
        for (int row = 0; row < 30; row++) {
            int half = row * 18 / 30;
            s.RenderDrawLine(r, 320 - half, y + 88 + row, 320 + half, y + 88 + row);
        }
        rect(r, 319, y + 98, 3, 10, "background");
        rect(r, 319, y + 111, 3, 3, "background");
        boot_caption(r, "Before you play", y + 150, 28, "ink");
        rect(r, 288, y + 207, 64, 1, "line");
        boot_caption(r, "Play at a comfortable volume.", y + 241, 20, "ink");
        boot_caption(r, "Give your eyes and hands a break.", y + 277, 20, "ink");
        boot_caption(r, "Pause when you need to.", y + 313, 20, "ink");
        boot_caption(r, "RG DS Core", y + 415, 16, "muted");
    }
    void boot_stylus(SDL_Renderer* r, int x, int y) {
        double phase = boot_phase();
        int lift = preferences.animations ? int(3 * (1 - std::cos(phase * 6.283185307))) : 0;
        if (preferences.animations) {
            feedback_arc(r, x, y + 10, 9 + int(phase * 11), 0, 1, theme.at("accent"), int(100 * (1 - phase)), 1);
        } else feedback_arc(r, x, y + 10, 12, 0, 1, theme.at("line"), 255, 1);
        // Closed pen silhouette, a pointed nib and an inset highlight.
        color(r, "ink");
        for (int i = 0; i < 7; i++) s.RenderDrawLine(r, x + 4 + i, y + 3 - lift, x + 29 + i, y - 22 - lift);
        for (int i = 0; i < 6; i++) s.RenderDrawLine(r, x + i, y + 9 - i - lift, x + 4 + i, y + 3 - lift);
        color(r, "background");
        s.RenderDrawLine(r, x + 9, y - lift, x + 31, y - 22 - lift);
    }
    void boot_bottom(SDL_Renderer* r, int y) {
        rect(r, 0, y, 640, 480, "background");
        boot_emblem(r, y + 70);
        boot_wordmark(r, y + 222, 332);
        boot_stylus(r, 309, y + 309);
        boot_caption(r, "Touch to continue", y + 344, 28, "ink");
        key_hints(r, 320, y + 428, {{"A Start", ""}}, 0);
    }
    // A read-only games card breaks saves, volume and launch at once. Explain
    // the automatic check plainly instead of letting every action fail.
    int card_repair = 0;
    bool card_repair_dismissed = false;
    Uint32 card_check_at = 0, card_repair_at = 0;
    void check_card(Uint32 now, bool force = false) {
        if (card_repair || card_repair_dismissed || o.preview || o.game_menu || o.data.empty()) return;
        if (!force && now - card_check_at < 5000) return;
        card_check_at = now;
        struct statvfs st{};
        if (statvfs(o.data.c_str(), &st) != 0 || !(st.f_flag & ST_RDONLY)) return;
        start_card_repair(now);
    }
    void start_card_repair(Uint32 now) {
        auto script = (o.app.empty() ? fs::current_path() : o.app) / "runtime/card_repair.py";
        auto child = fork();
        if (child == 0) {
            int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
            }
            execlp("python3", "python3", "-B", script.c_str(), "start", "--data", o.data.c_str(), nullptr);
            _exit(127);
        }
        int code = -1;
        if (child > 0) {
            int raw = 0;
            if (waitpid(child, &raw, 0) == child && WIFEXITED(raw)) code = WEXITSTATUS(raw);
        }
        if (code == 0 || code == 3) {
            card_repair = code == 0 ? 1 : 2;
            card_repair_at = now;
            stop_worker();
            picker_open = false;
            hold.clear();
            dirty = true;
        }
    }
    void card_top(SDL_Renderer* r, int y) {
        rect(r, 0, y, 640, 480, "background");
        brand(r, 296, y + 96, 48);
        boot_caption(r, card_repair == 1 ? "Checking the memory card" : "The memory card needs attention", y + 178, 28,
                     "ink");
        rect(r, 288, y + 229, 64, 1, "line");
        if (card_repair == 1) {
            boot_caption(r, "The card stopped accepting changes, so", y + 262, 20, "ink");
            boot_caption(r, "RG DS is checking and fixing it now.", y + 296, 20, "ink");
            boot_caption(r, "Your games and saves stay on the card.", y + 330, 20, "muted");
        } else {
            boot_caption(r, "The automatic check did not fix it.", y + 262, 20, "ink");
            boot_caption(r, "Copy your saves to a computer and", y + 296, 20, "ink");
            boot_caption(r, "check the card there.", y + 330, 20, "ink");
        }
    }
    void card_bottom(SDL_Renderer* r, int y) {
        rect(r, 0, y, 640, 480, "background");
        if (card_repair == 1) {
            // Calm progress: eight soft dots, one brighter, no spinner churn.
            const double phase = preferences.animations ? ((s.GetTicks() - card_repair_at) % 1600) / 1600.0 : 0;
            for (int i = 0; i < 8; i++) {
                double angle = i * 0.785398163 - 1.570796327;
                int cx = 320 + int(std::cos(angle) * 30), cy = y + 190 + int(std::sin(angle) * 30);
                int lit = std::abs(i - int(phase * 8)) % 8;
                int strength = lit == 0 ? 100 : lit == 1 || lit == 7 ? 55 : 22;
                for (int dy = -5; dy <= 5; dy++)
                    for (int dx = -5; dx <= 5; dx++)
                        if (dx * dx + dy * dy <= 25)
                            tint_rect(r, cx + dx, cy + dy, 1, 1,
                                      blend(theme.at("background"), theme.at("accent"), strength));
            }
            boot_caption(r, "RG DS restarts by itself when it is done.", y + 268, 20, "ink");
            boot_caption(r, "This usually takes about a minute.", y + 302, 20, "muted");
        } else {
            boot_caption(r, "Games cannot start or save right now.", y + 190, 20, "ink");
            boot_caption(r, "Press any button to continue", y + 404, 16, "muted");
        }
    }
    void top(SDL_Renderer* r, int y);
    // Word-wrapped text centred on the screen, at most `rows` lines.
    void centered_wrapped(SDL_Renderer* r, const std::string& value, int y, int pixels, int width, int rows,
                          const std::string& key) {
        std::istringstream words(value);
        std::string word, line;
        std::vector<std::string> lines;
        while (words >> word) {
            std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && label_width(next, pixels, false) > width) {
                lines.push_back(line);
                line = word;
            } else line = next;
        }
        if (!line.empty()) lines.push_back(line);
        for (int i = 0; i < std::min<int>(rows, int(lines.size())); i++) {
            int w = std::min(width, label_width(lines[size_t(i)], pixels, false));
            raw_label(r, lines[size_t(i)], 320 - w / 2, y + i * (pixels + 8), pixels, key, width);
        }
    }
    // PictoChat's page on Home's top screen: what it is and whether Wi-Fi is up.
    void pictochat_top(SDL_Renderer* r, int y);
    // A heart badge for favourites, on the cover stage and on the tiles below.
    bool stage_favourite = false;
    bool game_loved(size_t index);
    void favourite_badge(SDL_Renderer* r, int cx, int cy, int size);
    // Without a cover: the cartridge's own DS icon, large and crisp on the
    // same card as covers, with its title (the only place it is not repeated).
    void icon_stage(SDL_Renderer* r, int y, const std::string& developer);
    // Translucent rounded fill without overlapping strips (blending stays even).
    void alpha_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, int alpha) {
        radius = std::clamp(radius, 0, std::min(w, h) / 2);
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, fill.r, fill.g, fill.b, Uint8(std::clamp(alpha, 0, 255)));
        for (int dy = 0; dy < radius; dy++) {
            int inset = radius - int(std::sqrt(radius * radius - (radius - dy - 0.5) * (radius - dy - 0.5)));
            SDL_Rect a{x + inset, y + dy, w - inset * 2, 1}, b{x + inset, y + h - 1 - dy, w - inset * 2, 1};
            s.RenderFillRect(r, &a);
            s.RenderFillRect(r, &b);
        }
        SDL_Rect middle{x, y + radius, w, h - radius * 2};
        s.RenderFillRect(r, &middle);
    }
    // DSi / GammaOS-style cover stage: the box art alone, large and centred on
    // a soft shadow and a white card edge. The title already sits in the lower
    // screen's speech bubble. A new selection settles in over 190 ms.
    bool cover_stage(SDL_Renderer* r, int y, const fs::path& path);
    // A shoulder button seen from the front, as the DSi draws L and R: a flat
    // tab with small corners, whose outer top corner sweeps down in a deep
    // quarter ellipse. `right` puts the sweep on the right (R). Blended at `alpha`.
    void shoulder_fill(SDL_Renderer* r, int x, int y, int w, int h, bool right, Color fill, int alpha = 255) {
        const double rx = std::min(w * .45, h * .7), ry = h * .8;
        const int small = std::min(4, h / 2);
        const auto corner = [](double radius, int row) {
            return row >= radius ? 0
                                 : int(radius - std::sqrt(radius * radius - (radius - row - .5) * (radius - row - .5)));
        };
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, fill.r, fill.g, fill.b, Uint8(std::clamp(alpha, 0, 255)));
        for (int dy = 0; dy < h; dy++) {
            const double t = std::max(0.0, (ry - dy - .5) / ry);
            const int sweep = dy < ry ? int(std::lround(rx - rx * std::sqrt(std::max(0.0, 1 - t * t)))) : 0,
                      flat = std::max(corner(small, dy), corner(small, h - 1 - dy)),
                      outer = std::max(sweep, corner(small, h - 1 - dy)), inner = flat;
            SDL_Rect line{x + (right ? inner : outer), y + dy, w - inner - outer, 1};
            s.RenderFillRect(r, &line);
        }
    }
    // Every button tip looks the same: the key as an accent badge with the
    // glass highlight of a DSi key, then what it does. Face buttons are round;
    // L and R are drawn like the DSi draws its shoulders: a flat tab with a
    // deep curve on the outer top corner. "L R" is two badges for either key.
    // Keys are the names printed on the handheld, so they are not translated;
    // `inverse` is the light badge for accent glass.
    struct KeyHint {
        std::string key, text;
    };
    static constexpr int key_badge_size = 22;
    static bool shoulder_key(const std::string& key) { return key == "L" || key == "R" || key == "L2" || key == "R2"; }
    int key_badge_width(const std::string& key) {
        return shoulder_key(key) ? std::max(34, label_width(key, 14, false) + 20)
                                 : std::max(key_badge_size, label_width(key, 14, false) + 14);
    }
    int key_badge(SDL_Renderer* r, int x, int cy, const std::string& key, bool enabled = true, bool inverse = false) {
        const int w = key_badge_width(key), y = cy - key_badge_size / 2;
        const Color fill = theme.at(inverse   ? "on_accent"
                                    : enabled ? "accent"
                                              : "slot"),
                    ink = theme.at(inverse   ? "accent"
                                   : enabled ? "on_accent"
                                             : "muted");
        if (shoulder_key(key)) {
            const bool right = key[0] == 'R';
            shoulder_fill(r, x, y, w, key_badge_size, right, fill);
            if (enabled && !inverse)
                shoulder_fill(r, x + (right ? 3 : 8), y + 2, w - 11, 6, right, Color{255, 255, 255}, 56);
            // The letter leans away from the deep curve, onto the flat part of the tab.
            const int text_x = x + (w - label_width(key, 14, false)) / 2 + (right ? -3 : 3);
            raw_label_colour(r, key, text_x, centred_label_y(key, 14, cy), 14, ink, w);
            return w;
        }
        fill_round(r, x, y, w, key_badge_size, key_badge_size / 2, fill);
        if (enabled && !inverse) alpha_round(r, x + 4, y + 2, w - 8, 7, 3, Color{255, 255, 255}, 56);
        raw_label_colour(r, key, x + (w - label_width(key, 14, false)) / 2, centred_label_y(key, 14, cy), 14, ink, w);
        return w;
    }
    // A key group ("A", "Start", "L R"): its badges side by side.
    int key_group(SDL_Renderer* r, int x, int cy, const std::string& keys, bool enabled = true, bool inverse = false) {
        int w = 0;
        std::istringstream words(keys);
        for (std::string key; words >> key;) {
            if (w) w += 4;
            w += r ? key_badge(r, x + w, cy, key, enabled, inverse) : key_badge_width(key);
        }
        return w;
    }
    int key_hints_width(const std::vector<KeyHint>& hints, int size = 16) {
        int w = 0;
        for (auto& hint : hints)
            w += (w ? 18 : 0) + key_group(nullptr, 0, 0, hint.key) +
                 (hint.text.empty() ? 0 : 8 + label_width(hint.text, size));
        return w;
    }
    // A row of tips from x (align -1), centred on x (0) or ending at x (1),
    // centred on cy; long translations step the text down to fit `limit`.
    int key_hints(SDL_Renderer* r, int x, int cy, const std::vector<KeyHint>& hints, int align = -1, int size = 16,
                  int limit = 584, const std::string& ink = "muted", bool inverse = false) {
        while (size > 12 && key_hints_width(hints, size) > limit) size -= 2;
        const int width = key_hints_width(hints, size);
        int at = align < 0 ? x : align == 0 ? x - width / 2 : x - width;
        for (auto& hint : hints) {
            at += key_group(r, at, cy, hint.key, true, inverse);
            if (!hint.text.empty()) {
                const auto value = locale.text(hint.text);
                const int w = label_width(value, size, false);
                raw_label(r, value, at + 8, centred_label_y(value, size, cy), size, ink, w + 4);
                at += 8 + w;
            }
            at += 18;
        }
        return width;
    }
    // The DSi's note on the top screen: a panel card with an accent edge, the
    // page's icon and a title; the tip sits under it (settings_top draws it).
    // `y` places the icon and title; `screen` is the top screen's own top.
    void dsi_note(SDL_Renderer* r, int y, int screen, SettingsIcon symbol, const std::string& title) {
        const int x = 24, top = screen + 64, w = 592, h = 356;
        alpha_round(r, x, top + 3, w, h, 18, shadow_colour(), preferences.dark ? 60 : 20);
        fill_round(r, x, top, w, h, 18, theme.at("accent"));
        fill_round(r, x + 2, top + 2, w - 4, h - 4, 16, theme.at("panel"));
        ui_icon(r, symbol, 320, y + 118, 40, "accent");
        centered_wrapped(r, title, y + 148, 24, 540, 1, "ink");
    }
    // A shoulder tab in a lower corner of the top screen, flush with its
    // edge: the key badge on the outer end, where the button is, then what it does.
    void shoulder_tab(SDL_Renderer* r, int y, bool right, const std::string& key, const std::string& text) {
        const int h = 36, top = y + 430, cy = top + h / 2, size = 16;
        const int w = std::min(300, 14 + key_badge_width(key) + 8 + label_width(text, size, false) + 16),
                  left = right ? 640 - w : 0;
        alpha_round(r, right ? left : left - 18, top + 3, w + 18, h, 14, shadow_colour(), preferences.dark ? 60 : 20);
        fill_round(r, right ? left : left - 18, top, w + 18, h, 14, theme.at("panel"));
        const int badge = right ? 640 - 14 - key_badge_width(key) : 14;
        key_badge(r, badge, cy, key);
        const int text_x = right ? left + 16 : badge + key_badge_width(key) + 8;
        raw_label(r, text, text_x, centred_label_y(text, size, cy), size, "ink", w - key_badge_width(key) - 38);
    }
    std::string system_row_name(int row) const {
        static const std::array<const char*, 8> names = {
            "Language",           "Timezone",           "Date format",     "Time format",
            "Device information", "Updates & recovery", "Return to stock", "Startup"};
        if (row == 6 && o.independent) return "Restart menu";
        return names[size_t(std::clamp(row, 0, 7))];
    }
    // What the focused row is for, on pages that explain their rows.
    std::string settings_tip(Page page);
    std::string cover_notice();
    // The line along the top screen's lower edge, after the DSi: L (Previous)
    // and R (Next) in the corners like its shoulder tabs, Y (View) beside L
    // and X (Options) beside R, and the status or the play count and time in
    // a pill centred between the two halves. When a long translation leaves
    // no room for the pill, L and R keep their badges and drop their words.
    void cover_line(SDL_Renderer* r, int y, uint64_t seconds);
    std::string caption_plays(uint64_t sessions) const { return std::to_string(sessions); }
    std::string caption_time(uint64_t seconds) const;
    // Play count and time as icon + number (language-neutral); an unplayed game shows nothing.
    int caption_width(uint64_t seconds);
    void cover_caption(SDL_Renderer* r, int x, int cy, uint64_t seconds);
    // Fixed geometry, no textures or per-frame allocations for feedback rings.
    void feedback_arc(SDL_Renderer* r, int x, int y, int radius, double start, double amount, Color ink,
                      int alpha = 255, int weight = 2) {
        static const auto points = []() {
            std::array<std::pair<double, double>, 64> result{};
            for (size_t i = 0; i < result.size(); i++) {
                double a = (double(i) / 64 - .25) * 6.283185307;
                result[i] = {std::cos(a), std::sin(a)};
            }
            return result;
        }();
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        s.SetRenderDrawColor(r, ink.r, ink.g, ink.b, Uint8(std::clamp(alpha, 0, 255)));
        int first = int(start * 64), count = std::clamp(int(std::ceil(amount * 64)), 0, 64);
        for (int i = 0; i < count; i++) {
            auto a = points[(first + i) % 64], b = points[(first + i + 1) % 64];
            for (int w = 0; w < weight; w++)
                s.RenderDrawLine(
                    r, x + int(std::round(a.first * (radius + w))), y + int(std::round(a.second * (radius + w))),
                    x + int(std::round(b.first * (radius + w))), y + int(std::round(b.second * (radius + w))));
        }
    }
    bool options_hold() const {
        return pointer_down && !probe && !home_menu && settings.page == Page::Library && carousel.dragging &&
               !carousel.moved && !carousel.range && !model.games.empty() && pressed_game == model.selected &&
               carousel.tapped(down_x, model.games.size()) == pressed_game &&
               std::abs(carousel.position - pressed_game) < .08;
    }
    bool touch_animating(Uint32 now) const {
        return preferences.touch_indicator && preferences.animations &&
               ((pointer_down && (now - pointer_at < 100 || (options_hold() && now - pointer_at < 600))) ||
                (touch_tail && now - release_at < 140));
    }
    void touch_feedback(SDL_Renderer* r, int y) {
        gesture_cues(r, y);
        if (!preferences.touch_indicator || probe || launching || chat_open || (!pointer_down && !touch_tail)) return;
        auto now = s.GetTicks();
        double release = pointer_down ? 0 : std::min(1.0, (now - release_at) / 140.0);
        if (release >= 1) return;
        int radius =
            preferences.animations
                ? (pointer_down ? 18 + int(4 * std::max(0.0, 1.0 - (now - pointer_at) / 100.0)) : 18 + int(release * 6))
                : 18;
        int alpha = int(220 * (1 - release));
        SDL_Rect clip{0, y, 640, 480};
        s.RenderSetClipRect(r, &clip);
        feedback_arc(r, contact_x, y + contact_y, radius + 2, 0, 1, theme.at("panel"), alpha, 2);
        feedback_arc(r, contact_x, y + contact_y, radius, 0, 1, theme.at("accent"), alpha, 2);
        if (options_hold()) {
            double progress = std::min(1.0, (now - pointer_at) / 600.0);
            feedback_arc(r, contact_x, y + contact_y, radius + 5, 0,
                         preferences.animations ? progress
                         : progress >= 1        ? 1
                                                : 0,
                         theme.at("accent"), 255, 3);
            if (progress >= 1) centered(r, "Release for options", y + 361, 1, "accent");
        }
        s.RenderSetClipRect(r, nullptr);
    }
    double launch_opacity(Uint32 now) const {
        if (!preferences.animations) return launch_phase == LaunchPhase::Exiting ? 0 : 1;
        if (launch_phase == LaunchPhase::Exiting) {
            double t = std::min(1.0, (now - launch_exit_at) / 80.0);
            return launch_exit_opacity * (1 - t * t);
        }
        double t = std::min(1.0, (now - launch_at) / 110.0);
        return 1 - std::pow(1 - t, 3);
    }
    void loading_overlay(SDL_Renderer* r, int y, bool lower) {
        if (!launching) return;
        auto now = s.GetTicks();
        double opacity = launch_opacity(now);
        SDL_Rect panel{0, y, 640, 480};
        s.SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        auto background = theme.at("background");
        double wash = launch_phase == LaunchPhase::Exiting && launch_handoff ? launch_exit_opacity : opacity;
        s.SetRenderDrawColor(r, background.r, background.g, background.b, Uint8((lower ? 224 : 172) * wash));
        s.RenderFillRect(r, &panel);
        if (lower && opacity > 0) {
            // Eight small DS-style blocks: no card, labels, textures or artwork.
            static const std::array<std::pair<int, int>, 8> offsets = {
                {{0, -23}, {16, -16}, {23, 0}, {16, 16}, {0, 23}, {-16, 16}, {-23, 0}, {-16, -16}}};
            int head = preferences.animations ? int((now - launch_at) / 75) % 8 : 0;
            auto accent = theme.at("accent");
            for (int i = 0; i < 8; i++) {
                int age = (head - i + 8) % 8, alpha = preferences.animations ? 255 - age * 27 : 180;
                s.SetRenderDrawColor(r, accent.r, accent.g, accent.b, Uint8(alpha * opacity));
                SDL_Rect block{317 + offsets[i].first, y + 237 + offsets[i].second, 6, 6};
                s.RenderFillRect(r, &block);
            }
        }
        if (launch_phase == LaunchPhase::Exiting && launch_handoff) {
            // Clear the library into the theme's neutral colour for handoff.
            double t = preferences.animations ? std::min(1.0, (now - launch_exit_at) / 80.0) : 1;
            s.SetRenderDrawColor(r, background.r, background.g, background.b, Uint8(255 * t * t));
            s.RenderFillRect(r, &panel);
        }
    }
    struct LibraryHeader {
        SDL_Rect card, back, view, options, title, search, clear;
        int category_x, category_width;
    };
    LibraryHeader library_header_layout();
    static bool point_in(SDL_Rect bounds, int x, int y) {
        return x >= bounds.x && x < bounds.x + bounds.w && y >= bounds.y && y < bounds.y + bounds.h;
    }
    bool library_control_active(LibraryPress control, SDL_Rect bounds) const;
    bool update_rail_feedback(Uint32 now);
    // ---- depth, shared by every control ----
    // A DSi button over any colour: a fine darker rim, then one smooth
    // vertical shade, lighter at the top and a touch deeper at the bottom,
    // with a single bright line just inside the top edge. No band and no
    // hard edge in the middle. `press` turns the shade over, so a held
    // button looks pushed in. No shadow: callers add one.
    void glossy_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color base, double press = 0) {
        radius = std::clamp(radius, 0, std::min(w, h) / 2);
        const Color white{255, 255, 255}, black{0, 0, 0};
        const int lum = (base.r * 3 + base.g * 6 + base.b) / 10;
        // Pale bodies (white panels) need a visible rim; saturated ones a deeper one.
        fill_round(r, x, y, w, h, radius, blend(base, black, lum > 200 ? 16 : 26));
        if (w <= 2 || h <= 2) return;
        fill_round(r, x + 1, y + 1, w - 2, h - 2, std::max(0, radius - 1), base);
        auto inset_at = [&](int row) {
            double dy = row < radius ? radius - row - .5 : row >= h - radius ? row - (h - radius) + .5 : -1;
            return dy < 0 ? 0 : radius - int(std::sqrt(std::max(0.0, radius * radius - dy * dy)));
        };
        const double lift = lum > 200 ? 0.0 : lum < 90 ? 10.0 : 30.0, deepen = lum > 200 ? 9.0 : 14.0;
        for (int row = 1; row < h - 1; row++) {
            int inset = inset_at(row) + 1;
            if (w - 2 * inset <= 0) continue;
            double t = double(row - 1) / std::max(1, h - 3);
            if (press > 0) t = t + (1 - 2 * t) * press * .8;
            const Color c = t < .5 ? blend(base, white, int(std::lround(lift * (1 - 2 * t))))
                                   : blend(base, black, int(std::lround(deepen * (2 * t - 1))));
            tint_rect(r, x + inset, y + row, w - 2 * inset, 1, c);
        }
        int inset = inset_at(1) + 2;
        if (w - 2 * inset > 0 && h > 6)
            tint_rect(r, x + inset, y + 1, w - 2 * inset, 1, blend(base, white, int(55 * (1 - .7 * press))));
    }
    Color shadow_colour() const { return preferences.dark ? Color{0, 0, 0} : Color{24, 40, 60}; }
    // A raised control: soft shadow, then the glass body, sunk 2 px when pressed.
    void raised_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, double press = 0,
                     bool shadow = true) {
        const int sink = int(std::lround(2 * press));
        if (shadow)
            alpha_round(r, x, y + 3, w, h, radius, shadow_colour(),
                        int((preferences.dark ? 70 : 28) * (1 - .5 * press)));
        glossy_pill(r, x, y + sink, w, h, radius, fill, press);
    }
    // A footer or sheet button: a raised glass pill (accent when primary) with an
    // optional round key badge ("B", "A") and a label that sinks while pressed.
    void footer_button(SDL_Renderer* r, int x, int y, int w, const std::string& text, const std::string& badge,
                       bool primary, bool pressed, Color* tint = nullptr) {
        const Color fill = primary ? (tint ? *tint : theme.at("accent")) : theme.at(pressed ? "focus" : "panel"),
                    ink = primary ? (tint ? tile_ink(*tint) : theme.at("on_accent")) : theme.at("ink");
        raised_pill(r, x, y, w, 40, 12, fill, pressed ? 1 : 0);
        const int sink = pressed ? 2 : 0;
        int tx = x + 18;
        if (!badge.empty()) {
            if (primary) {
                fill_round(r, x + w - 36, y + 9 + sink, 22, 22, 11, ink);
                raw_label_colour(r, badge, x + w - 36 + (22 - label_width(badge, 14, false)) / 2,
                                 centred_label_y(badge, 14, y + 20 + sink), 14, fill, 22);
            } else {
                round_rect(r, x + 13, y + 9 + sink, 22, 22, 11, "slot");
                raw_label(r, badge, x + 13 + (22 - label_width(badge, 14, false)) / 2,
                          centred_label_y(badge, 14, y + 20 + sink), 14, "ink", 22);
                tx = x + 48;
            }
        }
        const std::string value = locale.text(text);
        raw_label_colour(r, value, tx, centred_label_y(value, 20, y + 20 + sink), 20, ink,
                         w - (badge.empty() ? 36
                              : primary     ? 58
                                            : 60));
    }
    bool footer_pressed(bool left) const {
        return pointer_down && contact_y >= 424 && (left ? contact_x < 200 : contact_x >= 440);
    }
    // A secondary button: a raised panel pill, pressed in while held.
    void secondary_button(SDL_Renderer* r, int x, int y, int w, int h, int radius, bool pressed, bool lit = false) {
        raised_pill(r, x, y, w, h, radius, theme.at(lit ? "focus" : pressed ? "focus" : "panel"), pressed ? 1 : 0);
    }
    // A recessed track: a rim, the background inside and a shadow along its top.
    void sunken_track(SDL_Renderer* r, int x, int y, int w, int h, int radius) {
        round_rect(r, x, y, w, h, radius, "line");
        round_rect(r, x + 1, y + 1, w - 2, h - 2, std::max(0, radius - 1), "background");
        alpha_round(r, x + 2, y + 2, w - 4, std::min(4, std::max(1, h - 4)), std::max(0, radius - 2), shadow_colour(),
                    preferences.dark ? 50 : 14);
    }
    // ---- updates: from the Home banner to the finished install ----
    // What the owner can do about an update. One whose download stopped (a
    // pause, a restart, Wi-Fi gone) stays offered: its part file resumes.
    enum class UpdateStage { None, Offered, Running, Paused, Ready };
    UpdateStage update_stage();
    std::string update_version() {
        const std::string offer = update_status["offer"];
        return offer.empty() ? update_status["version"] : offer;
    }
    static std::string megabytes(double bytes) {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f", bytes / 1048576.0);
        return text;
    }
    // The current step's progress: bytes while downloading, then the checksum,
    // then the unpacking. The step row above the bar says which one it is, so
    // the percent always matches the megabytes beside it.
    double update_fraction() {
        if (update_stage() == UpdateStage::Ready) return 1;
        const std::string state = update_status["state"];
        const double p = std::clamp(std::atof(update_status["progress"].c_str()) / 100.0, 0.0, 1.0),
                     size = std::atof(update_status["size"].c_str()),
                     received = std::atof(update_status["received"].c_str());
        if (state == "verifying" || state == "staging") return p;
        const double got = size > 0 ? std::clamp(received / size, 0.0, 1.0) : 0;
        return state == "downloading" ? std::max(got, p) : got;
    }
    // Speed and time left while bytes arrive, e.g. "1.8 MB/s · About 2 min left".
    std::string update_speed() {
        if (update_status["state"] != "downloading") return {};
        const double rate = std::atof(update_status["rate"].c_str());
        const int eta = std::atoi(update_status["eta"].c_str());
        if (rate <= 0) return {};
        char speed[32];
        if (rate >= 1048576) std::snprintf(speed, sizeof speed, "%.1f MB/s", rate / 1048576.0);
        else std::snprintf(speed, sizeof speed, "%d KB/s", std::max(1, int(rate / 1024)));
        if (eta < 0) return speed;
        const std::string left = eta < 60     ? locale.text("Less than a minute left")
                                 : eta < 5400 ? locale.text("About " + std::to_string((eta + 30) / 60) + " min left")
                                              : locale.text("About " + std::to_string((eta + 1800) / 3600) + " h left");
        return std::string(speed) + "  ·  " + left;
    }
    // The bar's shown value eases toward the reported one between status polls.
    double update_shown = 0;
    Uint32 update_shown_at = 0, glitter_frame_at = 0;
    int drawn_banner_frame = -1;
    bool glitter_was = false;
    void ease_update(Uint32 now) {
        const double target = update_fraction();
        const bool glitter = glitter_active(), arrived = glitter && !glitter_was;
        glitter_was = glitter;
        if (!glitter || arrived || update_stage() != UpdateStage::Running || target < update_shown - .05)
            update_shown = target;
        else update_shown += (target - update_shown) * std::min(1.0, (now - update_shown_at) / 220.0);
        update_shown_at = now;
    }
    bool glitter_active() {
        return preferences.animations && settings.page == Page::Maintenance && !chat_open &&
               (update_stage() == UpdateStage::Running || update_stage() == UpdateStage::Ready);
    }
    // Home's banner sweeps a light across itself now and then: one 900 ms pass
    // every 6 s. Between passes it is a still frame and costs no redraws.
    static constexpr Uint32 banner_cycle = 6000, banner_sweep = 900;
    bool banner_shown() {
        return settings.page == Page::Library && home_menu && !chat_open && !o.game_menu &&
               update_stage() != UpdateStage::None;
    }
    int banner_frame(Uint32 now) {
        return preferences.animations && banner_shown() && now % banner_cycle < banner_sweep
                   ? int(now % banner_cycle / 33)
                   : -1;
    }
    Uint32 banner_wait(Uint32 now) { return banner_frame(now) >= 0 ? 33 : banner_cycle - now % banner_cycle; }
    static int round_inset(int row, int h, int radius) {
        double dy = row < radius ? radius - row - .5 : row >= h - radius ? row - (h - radius) + .5 : -1;
        return dy < 0 ? 0 : radius - int(std::sqrt(std::max(0.0, radius * radius - dy * dy)));
    }
    // A translucent span; callers switch the renderer to blend mode first.
    void blend_rect(SDL_Renderer* r, int x, int y, int w, int h, Color c, int alpha) {
        if (w <= 0 || h <= 0 || alpha <= 0) return;
        s.SetRenderDrawColor(r, c.r, c.g, c.b, Uint8(std::min(alpha, 255)));
        SDL_Rect b{x, y, w, h};
        s.RenderFillRect(r, &b);
    }
    // A DSi glint: a four-point star with soft arms and a white core.
    void sparkle(SDL_Renderer* r, int cx, int cy, int size, Color ink, int alpha);
    static uint32_t glint_hash(uint32_t v) {
        v ^= v >> 16;
        v *= 0x7feb352dU;
        v ^= v >> 15;
        v *= 0x846ca68bU;
        v ^= v >> 16;
        return v;
    }
    // Glints that come and go: each lives for its own period at a spot picked
    // afresh every cycle inside `box`, growing and fading like the DSi's
    // sparkles. Still, every other glint holds at full size.
    void glints(SDL_Renderer* r, SDL_Rect box, int count, uint32_t seed, Uint32 now, int largest, bool moving);
    // A light band sweeping over a rounded shape, slanted like a glass reflex.
    void light_sweep(SDL_Renderer* r, int x, int y, int h, int radius, int right_edge, double t, int strength);
    // The download bar after the DSi's glass: a recessed groove with tick marks
    // and a fill like a lit tube (a gradient body, slanted stripes that roll
    // forward, a gloss along the top, a travelling light and glints at the
    // leading edge). `moving` animates it; otherwise it is one still frame.
    void glitter_bar(SDL_Renderer* r, int x, int y, int w, int h, double fraction, Uint32 now, bool moving);
    // An arrow into a tray, or a check mark once the update is ready.
    // `size` is the disc's diameter; the glyph fills about 60 % of it.
    void update_glyph(SDL_Renderer* r, int cx, int cy, int size, Color ink, bool ready);
    // A white glass disc holding the glyph; while downloading, a ring around it fills.
    void update_disc(SDL_Renderer* r, int cx, int cy, int size, UpdateStage stage);
    // The hotkey: a glass key with the Y badge, white on the accent glass or
    // accent on a panel, the way the footer's primary button carries its A.
    enum class KeyLook { Accent, White, Panel };
    void hotkey_pill(SDL_Renderer* r, int x, int y, int w, int h, const std::string& text, KeyLook look,
                     bool pressed = false, const std::string& key = "Y", bool translate = true) {
        const Color accent = theme.at("accent"),
                    fill = look == KeyLook::White   ? Color{255, 255, 255}
                           : look == KeyLook::Panel ? theme.at(pressed ? "focus" : "panel")
                                                    : accent,
                    ink = look == KeyLook::Accent ? tile_ink(accent) : accent;
        raised_pill(r, x, y, w, h, h / 2, fill, pressed ? 1 : 0);
        const int sink = pressed ? 2 : 0, badge = h - 14;
        fill_round(r, x + 7, y + 7 + sink, badge, badge, badge / 2, ink);
        raw_label_colour(r, key, x + 7 + (badge - label_width(key, 16, false)) / 2,
                         centred_label_y(key, 16, y + h / 2 + sink), 16, fill, badge);
        const std::string value = translate ? locale.text(text) : text;
        const int room = w - badge - 30;
        int size = h >= 44 ? 20 : 18;
        while (size > 14 && label_width(value, size, false) > room)
            size -= 2; // long translations step down before they clip
        const int tx = x + badge + 14 + std::max(0, (room - label_width(value, size, false)) / 2);
        raw_label_colour(r, value, tx, centred_label_y(value, size, y + h / 2 + sink), size, ink, room);
    }
    std::string update_action_name(UpdateStage stage);
    // Home's notice after the DSi's balloon: an accent glass strip with the
    // release, a white key that says what Y does, glints by the disc and a
    // light that passes over it every few seconds to catch the eye.
    void update_banner(SDL_Renderer* r, int y);
    // The upper Updates screen while an update is offered, moving or ready:
    // a card with an accent glass header (the release), the four steps, the
    // bar and what Y does next.
    void update_card(SDL_Renderer* r, int y);
    // Y on Home or Updates: the shortest way to the newest build. It starts
    // (or resumes) the download, or asks to restart into a ready update.
    bool update_now();
    Page updates_return = Page::Settings; // Home, the Settings list or System: where B leaves Updates for
    void open_updates(Page from);
    // The status bar's update mark beside the brand: an accent disc while an
    // update waits, plus a small glass gauge and the percent while it
    // downloads (or sits paused). It stops short of the clock at `limit`.
    void update_status_chip(SDL_Renderer* r, int y, int limit);
    // The root row's value: the release on offer, or the percent while it downloads.
    std::string update_badge();
    void tint_colour(SDL_Renderer* r, Color value) { s.SetRenderDrawColor(r, value.r, value.g, value.b, 255); }
    // The rail's own glass shelf: a soft vertical sheen over the slot colour.
    Color rail_shelf(int row);
    // The DSi's scroll bar, edge to edge along the bottom with no gaps: an
    // arrow block flush in each corner and, between them, a recessed track
    // with one tick per item under a white glass grabber that shows the
    // ticks through it, the current one lit. One height for everything.
    void draw_rail(SDL_Renderer* r, int y, size_t count, double position, std::array<double, 3> press_state,
                   bool bubble);
    void rail_bar(SDL_Renderer* r, int y);
    // Recessed groove with one pip per item while they stay distinct, drawn
    // only between `from` and `to` (the glass window redraws its part).
    void rail_groove(SDL_Renderer* r, int y, int from, int to, size_t count, const RailGeometry& rail);
    // While the thumb is held: "12 / 62" floats above it, so a long scrub
    // always shows where it will land. Numbers only, no translation needed.
    void rail_bubble(SDL_Renderer* r, int y, int cx, double press);
    void mark_library_press(int x, int y);
    void library_header_action(SDL_Renderer* r, int y, SDL_Rect bounds, const std::string& title,
                               const std::string& button, bool enabled = true);
    // ---- the DSi column: the last game on top, the categories as wide tiles ----
    struct HomeTile {
        SDL_Rect box;
        size_t index;
    };
    static std::array<HomeTile, 6> home_column_tiles() {
        return {{{{24, 20, 592, 88}, last_played_tile},
                 {{24, 122, 288, 92}, 1},
                 {{328, 122, 288, 92}, 2},
                 {{24, 228, 288, 92}, 3},
                 {{328, 228, 288, 92}, pictochat_tile},
                 {{24, 334, 592, 64}, 0}}};
    }
    static constexpr SDL_Rect home_bar_left{12, 418, 80, 54}, home_bar_right{548, 418, 80, 54},
        home_bar_mark{280, 418, 80, 54};
    // Up and down walk the rows, left and right the two tiles of a row.
    int home_column_memory = 0; // the column (Games/Recents or Favourites/PictoChat) you were last in
    bool home_column_move(int code);
    void home_column_touch(int x, int y);
    void home_column_bottom(SDL_Renderer* r, int y);
    // The upper screen of the column: the DSi's clock and calendar.
    void home_column_top(SDL_Renderer* r, int y);
    void bottom(SDL_Renderer* r, int y);
    int display(const std::string& wanted, int fallback) {
        int count = s.GetNumVideoDisplays();
        for (int i = 0; i < count; i++) {
            const char* name = s.GetDisplayName(i);
            if ((name && std::string(name).find(wanted) != std::string::npos) || wanted == std::to_string(i)) return i;
        }
        if (fallback >= count)
            throw std::runtime_error(
                "Two SDL displays required. Use --top-display/--bottom-display after capturing inventory.");
        return fallback;
    }
    Surface create(const char* title, int index, int h, bool device) {
        launch_profile.mark("window-start", int(surfaces.size()));
        Uint32 flags = device ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_SHOWN;
        if (o.game_menu && !o.preview) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        int width = 640, height = h;
        if (device) {
            SDL_Rect bounds{};
            if (s.GetDisplayBounds(index, &bounds) != 0 || bounds.w <= 0 || bounds.h <= 0 || bounds.w > 4096 ||
                bounds.h > 4096)
                throw std::runtime_error("Cannot determine native panel size");
            width = bounds.w;
            height = bounds.h;
            if (width > 640) s.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
        }
        auto* w = s.CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(index), SDL_WINDOWPOS_CENTERED_DISPLAY(index),
                                 width, height, flags);
        if (!w) throw std::runtime_error(std::string("Window creation: ") + s.GetError());
        launch_profile.mark("window-created", int(surfaces.size()));
        auto* r = s.CreateRenderer(
            w, -1, SDL_RENDERER_ACCELERATED | ((!device || surfaces.empty()) ? SDL_RENDERER_PRESENTVSYNC : 0));
        if (!r) r = s.CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
        if (!r) {
            s.DestroyWindow(w);
            throw std::runtime_error(std::string("Renderer creation: ") + s.GetError());
        }
        launch_profile.mark("renderer-created", int(surfaces.size()));
        SDL_RendererInfo info{};
        if (s.GetRendererInfo(r, &info) == 0)
            std::cout << title << " renderer=" << (info.name ? info.name : "unknown") << " flags=" << info.flags
                      << std::endl;
        if (o.independent && device && std::string(s.GetCurrentVideoDriver()) == "wayland") {
            const char* role = std::string(title) == "RG DS Core top" ? "org.rgds.top" : "org.rgds.bottom";
            if (!rgds::wayland_role(w, role, s.handle)) {
                s.DestroyRenderer(r);
                s.DestroyWindow(w);
                throw std::runtime_error("Wayland panel assignment unavailable");
            }
        }
        s.RenderSetLogicalSize(r, 640, h);
        launch_profile.mark("renderer-ready", int(surfaces.size()));
        return {w, r, s.GetWindowID(w)};
    }
    void open() {
        if (o.independent && !o.game_menu) ui_trace.open(std::getenv("RGDS_UI_TRACE"));
        s.SetMainReady();
        s.SetHint("SDL_TOUCH_MOUSE_EVENTS", "0");
        s.SetHint("SDL_MOUSE_TOUCH_EVENTS", "0");
        embedded_menu = o.game_menu && !o.preview && std::getenv("RGDS_OVERLAY_FILE");
        s.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        launch_profile.mark("sdl-init-start");
        if (s.Init((embedded_menu ? 0 : SDL_INIT_VIDEO) | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(s.GetError());
        launch_profile.mark("sdl-init-done");
        if (!o.preview) {
            if (!std::getenv("RGDS_VOLUME_EXTERNAL")) volume_keys.open_devices();
            const char* input = std::getenv("RGDS_TOUCH_DEV");
            if (touchscreen.open_device(input ? input : ""))
                std::cout << "Touch: " << touchscreen.path() << " (logical 640x480; calibrated evdev range)"
                          << std::endl;
            else std::cerr << "Direct touch unavailable; using SDL events" << std::endl;
            input_wake.open_all();
        }
        SDL_version version{};
        s.GetVersion(&version);
        std::cout << "SDL " << int(version.major) << '.' << int(version.minor) << '.' << int(version.patch)
                  << " driver=" << (s.GetCurrentVideoDriver() ? s.GetCurrentVideoDriver() : "unknown") << std::endl;
        for (int i = 0; i < s.GetNumVideoDisplays(); i++) {
            SDL_Rect bounds{};
            s.GetDisplayBounds(i, &bounds);
            std::cout << "display " << i << " " << s.GetDisplayName(i) << " " << bounds.w << "x" << bounds.h
                      << std::endl;
        }
        if (embedded_menu) {
            if (!overlay.open(std::getenv("RGDS_OVERLAY_FILE"), true))
                throw std::runtime_error("Could not open shared menu frame");
            for (int i = 0; i < 2; i++) {
                auto* bitmap = s.CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_RGBA32);
                auto* renderer = bitmap ? s.CreateSoftwareRenderer(bitmap) : nullptr;
                if (!renderer) {
                    if (bitmap) s.FreeSurface(bitmap);
                    throw std::runtime_error("Could not create menu canvas");
                }
                surfaces.push_back({nullptr, renderer, 0, bitmap});
            }
        } else if (o.preview) {
            surfaces.push_back(create("RG DS Core preview", 0, 960, false));
            top_name = "PREVIEW TOP";
            bottom_name = "PREVIEW BOTTOM";
            if (!o.touch_recording.empty() && !touchscreen.open_replay(o.touch_recording.string()))
                throw std::runtime_error("Cannot read Linux touch recording");
        } else {
            int t = display(o.top, 0), b = display(o.bottom, 1);
            if (t == b) throw std::runtime_error("Top and bottom resolved to the same display.");
            top_name = s.GetDisplayName(t);
            bottom_name = s.GetDisplayName(b);
            surfaces.push_back(create("RG DS Core top", t, 480, true));
            surfaces.push_back(create("RG DS Core bottom", b, 480, true));
        }
        // Preview tests: a virtual pad whose listed buttons are already down
        // when the UI opens, like a player still holding A as the menu starts.
        const char* preview_held = o.preview ? std::getenv("RGDS_PREVIEW_HELD_BUTTONS") : nullptr;
        int preview_pad = -1;
        if (preview_held)
            if (auto attach = reinterpret_cast<decltype(&SDL_JoystickAttachVirtual)>(
                    dlsym(s.handle, "SDL_JoystickAttachVirtual")))
                preview_pad = attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0, 17, 1);
        for (int i = 0; i < s.NumJoysticks(); i++) {
            auto* j = s.JoystickOpen(i);
            if (j) {
                joysticks.push_back(j);
                const char* name = s.JoystickName(j);
                std::cout << "joystick " << i << " " << (name ? name : "") << std::endl;
                if (o.mainline && name && std::string(name) == "gpio-keys-control") {
                    auto instance =
                        reinterpret_cast<decltype(&SDL_JoystickInstanceID)>(dlsym(s.handle, "SDL_JoystickInstanceID"));
                    if (instance) mainline_pads.insert(instance(j));
                }
            }
        }
        if (preview_pad >= 0) {
            auto id = reinterpret_cast<decltype(&SDL_JoystickGetDeviceInstanceID)>(
                dlsym(s.handle, "SDL_JoystickGetDeviceInstanceID"));
            auto pad =
                reinterpret_cast<decltype(&SDL_JoystickFromInstanceID)>(dlsym(s.handle, "SDL_JoystickFromInstanceID"));
            auto press = reinterpret_cast<decltype(&SDL_JoystickSetVirtualButton)>(
                dlsym(s.handle, "SDL_JoystickSetVirtualButton"));
            std::istringstream held(preview_held);
            if (id && pad && press)
                if (auto* joystick = pad(id(preview_pad)))
                    for (int button; held >> button; held.ignore(1)) press(joystick, button, SDL_PRESSED);
        }
        snapshot_boot_input();
        snapshot_menu_input();
        launch_profile.mark("input-ready");
    }
    void close() {
        media.stop();
        sounds.close();
        flush_return_controls();
        system.discard_status();
        flush_volume();
        flush_brightness();
        touchscreen.close_device();
        volume_keys.close_devices();
        input_wake.close_all();
        hold.clear();
        pointer_down = false;
        pointer_source = PointerSource::None;
        raw_touch_seen = sdl_touch_seen = false;
        carousel.dragging = false;
        volume_drag = false;
        clear_view_canvases();
        view_canvas_failed = false;
        view_seen = false;
        view_levels.clear();
        view_motion.stop();
        rounded.clear();
        media.clear();
        shader_preview.clear();
        for (auto* j : joysticks) s.JoystickClose(j);
        joysticks.clear();
        mainline_pads.clear();
        mainline_controls.directions.clear();
        overlay.close();
        for (auto p : surfaces) {
            s.DestroyRenderer(p.renderer);
            if (p.window) s.DestroyWindow(p.window);
            if (p.bitmap) s.FreeSurface(p.bitmap);
        }
        surfaces.clear();
        s.Quit();
    }
    void save(SDL_Renderer* r, const fs::path& path) {
        int w = 0, h = 0;
        s.GetRendererOutputSize(r, &w, &h);
        auto* image = s.CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!image) throw std::runtime_error(s.GetError());
        if (s.RenderReadPixels(r, nullptr, image->format->format, image->pixels, image->pitch) != 0) {
            s.FreeSurface(image);
            throw std::runtime_error(s.GetError());
        }
        const auto pending = path.string() + ".tmp";
        auto* file = s.RWFromFile(pending.c_str(), "wb");
        if (!file) {
            s.FreeSurface(image);
            throw std::runtime_error("Cannot write screenshot");
        }
        const int rc = s.SaveBMP_RW(image, file, 1);
        s.FreeSurface(image);
        if (rc != 0) throw std::runtime_error("Screenshot failed");
        fs::rename(pending, path);
    }
    void update_health(bool presented = false) {
        if (o.game_menu || !library_ready || !session_initialized || settings.page == Page::BootWarning) return;
        if (!update_health_sent && !presented) return;
        // SDL shuts down while an emulator owns the screens. Keep the startup
        // pulse on an independent clock so immediate play cannot mimic a hang.
        auto now = uint64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (update_health_sent && (now - update_health_at < 1000 || now - update_health_started > 30000)) return;
        const char* token = std::getenv("RGDS_UPDATE_TOKEN");
        if (!token) return;
        auto path = o.data / "session/update-health.json", pending = fs::path(path.string() + ".pending");
        std::ofstream file(pending);
        file << "{\"build\":" << std::quoted(o.app.filename().string()) << ",\"token\":" << std::quoted(token)
             << ",\"pulse\":" << now << "}\n";
        file.close();
        if (file) {
            std::error_code ec;
            fs::rename(pending, path, ec);
            if (!ec) {
                if (!update_health_sent) update_health_started = now;
                update_health_sent = true;
                update_health_at = now;
            }
        }
    }
    void paint_bottom(SDL_Renderer* renderer, int y) {
        drawing_elements = true;
        element_origin_y = y;
        view_drawn_at = s.GetTicks();
        motion_hits.clear();
        bottom(renderer, y);
        drawing_elements = false;
    }
    void animated_bottom(SDL_Renderer* renderer, int y) {
        if (chat_open) {
            chat_bottom(renderer, y);
            return;
        }
        if (view_transitions_enabled() && layered(preferences.view_style) && !view_canvas_failed) {
            push_bottom(renderer, y);
            return;
        }
        if (!view_motion.running || view_canvas_failed) {
            paint_bottom(renderer, y);
            return;
        }
        if (!view_canvas)
            view_canvas = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
        if (!view_canvas || s.SetRenderTarget(renderer, view_canvas) != 0) {
            view_canvas_failed = true;
            view_motion.stop();
            s.ClearError();
            paint_bottom(renderer, y);
            return;
        }
        // SDL restores the window viewport/logical size when leaving a target.
        s.RenderSetLogicalSize(renderer, 640, 480);
        s.RenderSetClipRect(renderer, nullptr);
        paint_bottom(renderer, 0);
        s.RenderSetClipRect(renderer, nullptr);
        if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore menu canvas");
        s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
        rect(renderer, 0, y, 640, 480, "background");
        SDL_Rect clip{0, y, 640, 480};
        s.RenderSetClipRect(renderer, &clip);
        const auto pose = view_motion.pose(view_drawn_at);
        SDL_Rect destination{int(std::round(320 - 320 * pose.sx + pose.x)),
                             y + int(std::round(240 - 240 * pose.sy + pose.y)), int(std::round(640 * pose.sx)),
                             int(std::round(480 * pose.sy))};
        s.SetTextureBlendMode(view_canvas, SDL_BLENDMODE_BLEND);
        // One stable surface keeps text readable throughout the small spring.
        s.SetTextureColorMod(view_canvas, 255, 255, 255);
        s.SetTextureAlphaMod(view_canvas, 255);
        s.RenderCopyEx(renderer, view_canvas, nullptr, &destination, pose.tilt, nullptr, SDL_FLIP_NONE);
        s.RenderSetClipRect(renderer, nullptr);
        view_animation_frames++;
    }
    void push_bottom(SDL_Renderer* renderer, int y) {
        // Idle: straight to the panel. The capture that a transition needs
        // was taken when the input arrived (capture_view_for_input).
        if (!view_motion.running || !view_previous_ready) {
            view_previous_ready = false;
            paint_bottom(renderer, y);
            return;
        }
        if (!view_canvas)
            view_canvas = s.CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 480);
        auto fallback = [&]() {
            s.ClearError();
            clear_view_canvases();
            view_canvas_failed = true;
            view_motion.stop();
            paint_bottom(renderer, y);
        };
        if (!view_canvas || s.SetRenderTarget(renderer, view_canvas) != 0) {
            fallback();
            return;
        }
        s.RenderSetLogicalSize(renderer, 640, 480);
        s.RenderSetClipRect(renderer, nullptr);
        s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        s.SetRenderDrawColor(renderer, 0, 0, 0, 0);
        s.RenderClear(renderer);
        s.SetRenderDrawBlendMode(renderer, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        paint_bottom(renderer, 0);
        if (s.SetRenderTarget(renderer, nullptr) != 0) throw std::runtime_error("Could not restore push canvas");
        s.RenderSetLogicalSize(renderer, 640, o.preview ? 960 : 480);
        SDL_Rect clip{0, y, 640, 480};
        s.RenderSetClipRect(renderer, &clip);
        rect(renderer, 0, y, 640, 480, "background");
        const auto layers = view_motion.push(view_drawn_at);
        auto layer = [&](SDL_Texture* texture, double x, double shade = 0, double alpha = 1) {
            int light = int(std::round(255 * (1 - shade)));
            s.SetTextureColorMod(texture, light, light, light);
            s.SetTextureAlphaMod(texture, Uint8(std::lround(255 * alpha)));
            s.SetTextureBlendMode(texture, alpha < 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
            SDL_Rect destination{int(std::round(x)), y, 640, 480};
            s.RenderCopy(renderer, texture, nullptr, &destination);
            s.SetTextureColorMod(texture, 255, 255, 255);
            s.SetTextureAlphaMod(texture, 255);
        };
        if (view_motion.style == ViewStyle::Zoom) {
            // Scaled about the panel centre; the outgoing view underneath.
            const auto zoom = view_motion.zoom(view_drawn_at);
            auto scaled = [&](SDL_Texture* texture, double scale, double alpha, double dx) {
                if (alpha <= .004) return;
                s.SetTextureAlphaMod(texture, Uint8(std::lround(255 * std::clamp(alpha, 0.0, 1.0))));
                s.SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                const int w = int(std::lround(640 * scale)), h = int(std::lround(480 * scale));
                SDL_Rect destination{320 - w / 2 + int(std::lround(dx)), y + 240 - h / 2, w, h};
                s.RenderCopy(renderer, texture, nullptr, &destination);
                s.SetTextureAlphaMod(texture, 255);
            };
            scaled(view_previous, zoom.out_scale, zoom.out_alpha, zoom.out_dx);
            scaled(view_canvas, zoom.in_scale, zoom.in_alpha, zoom.in_dx);
        } else if (layers.incoming_above) {
            layer(view_previous, layers.outgoing, layers.shade);
            layer(view_canvas, layers.incoming, 0, layers.alpha);
        } else {
            layer(view_canvas, layers.incoming, layers.shade);
            layer(view_previous, layers.outgoing);
        }
        s.RenderSetClipRect(renderer, nullptr);
        view_animation_frames++;
        push_animation_frames++;
    }
    void draw(bool screenshot = false) {
        if (picker_was_open && !picker_open) picker_closed_at = s.GetTicks();
        picker_was_open = picker_open;
        sync_view_motion(s.GetTicks());
        const auto reads = library_cache.counters.file_reads + media.counters.file_reads +
                           shader_preview.counters.file_reads,
                   checks = library_cache.counters.stamp_checks + media.counters.stamp_checks +
                            shader_preview.counters.stamp_checks;
        auto begin_panel = [&](SDL_Renderer* renderer) {
            // A swapped back buffer has undefined contents. Clear the complete
            // target, independently of the previous page's clip/blend state.
            if (s.RenderSetClipRect(renderer, nullptr) != 0 ||
                s.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) != 0)
                throw std::runtime_error(std::string("Panel state: ") + s.GetError());
            const auto background = theme.at("background");
            s.SetRenderDrawColor(renderer, background.r, background.g, background.b, o.game_menu ? 0 : 255);
            if (s.RenderClear(renderer) != 0) throw std::runtime_error(std::string("Panel clear: ") + s.GetError());
            s.SetRenderDrawBlendMode(renderer, o.game_menu ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        };
        auto flush_panel = [&](SDL_Renderer* renderer) {
            if (s.RenderFlush(renderer) != 0) throw std::runtime_error(std::string("Panel draw: ") + s.GetError());
        };
        auto present_panel = [&](SDL_Renderer* renderer) {
            s.ClearError();
            s.RenderPresent(renderer);
            if (const char* error = s.GetError(); error && *error)
                throw std::runtime_error(std::string("Panel presentation: ") + error);
        };
        if (o.preview) {
            begin_panel(surfaces[0].renderer);
            top(surfaces[0].renderer, 0);
            animated_bottom(surfaces[0].renderer, 480);
            touch_feedback(surfaces[0].renderer, 480);
            loading_overlay(surfaces[0].renderer, 0, false);
            loading_overlay(surfaces[0].renderer, 480, true);
            shutdown_overlay(surfaces[0].renderer, 0, true);
            shutdown_overlay(surfaces[0].renderer, 480, false);
            if (!embedded_menu) volume_feedback(surfaces[0].renderer, 0);
            flush_panel(surfaces[0].renderer);
            if (screenshot) save(surfaces[0].renderer, o.screenshot);
            present_panel(surfaces[0].renderer);
        } else {
            // SDL GLES2 readback/present relies on the current GL context when
            // batching is disabled. Finish each panel before drawing the next
            // so the upper window is never read or swapped with the lower
            // renderer's context still current.
            auto* pixels = embedded_menu ? overlay.begin() : nullptr;
            for (int i = 0; i < 2; i++) {
                auto* renderer = surfaces[i].renderer;
                if (i == 0 && chat_open && (chat.drawing || chat.view == pictochat::View::Lobby) &&
                    chat_top_signature() == chat_top_seen)
                    continue;
                if (i == 0 && !screenshot) {
                    const auto signature = top_signature(s.GetTicks());
                    if (!signature.empty() && signature == top_seen) {
                        top_skips++;
                        continue;
                    }
                    top_seen = signature;
                    top_drawn_at = s.GetTicks();
                }
                ui_trace.mark(i == 0 ? UiTrace::TopDraw : UiTrace::BottomDraw, rendered_frames,
                              uint32_t(settings.page));
                begin_panel(renderer);
                if (i == 0) top(renderer, 0);
                else {
                    animated_bottom(renderer, 0);
                    touch_feedback(renderer, 0);
                }
                loading_overlay(renderer, 0, i == 1);
                shutdown_overlay(renderer, 0, i == 0);
                if (i == 0 && !embedded_menu) volume_feedback(renderer, 0);
                ui_trace.mark(i == 0 ? UiTrace::TopFlush : UiTrace::BottomFlush, rendered_frames,
                              uint32_t(settings.page));
                flush_panel(renderer);
                if (screenshot) save(renderer, o.screenshot.string() + (i == 0 ? "-top.bmp" : "-bottom.bmp"));
                if (embedded_menu) {
                    if (s.RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, pixels + i * overlay_panel_bytes,
                                           640 * 4) != 0)
                        throw std::runtime_error("Menu frame capture failed");
                } else {
                    ui_trace.mark(i == 0 ? UiTrace::TopPresent : UiTrace::BottomPresent, rendered_frames,
                                  uint32_t(settings.page));
                    present_panel(renderer);
                }
            }
            if (embedded_menu) overlay.publish();
        }
        rendered_frames++;
        render_file_reads +=
            library_cache.counters.file_reads + media.counters.file_reads + shader_preview.counters.file_reads - reads;
        render_stamp_checks += library_cache.counters.stamp_checks + media.counters.stamp_checks +
                               shader_preview.counters.stamp_checks - checks;
        ui_trace.mark(UiTrace::FrameDone, rendered_frames, uint32_t(settings.page));
        update_health(true);
        if (rendered_frames == 1) launch_profile.finish(o.game_menu);
        if (rendered_frames == 1 && o.game_menu && !embedded_menu && !o.preview) {
            if (const char* ready = std::getenv("RGDS_MENU_READY_FILE")) {
                auto pending = std::string(ready) + ".pending";
                std::ofstream file(pending);
                file << getpid() << '\n';
                file.close();
                if (file) {
                    std::error_code ec;
                    fs::rename(pending, ready, ec);
                }
            }
        }
        if (o.boot_ready_fd >= 0) {
            const char ready = '1';
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
            const int suppress = 1;
            setsockopt(o.boot_ready_fd, SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
            // The guardian may have exited. A failed notification never interrupts
            // presentation, and the nonblocking socket cannot delay the first frame.
            send(o.boot_ready_fd, &ready, 1, flags);
            ::close(o.boot_ready_fd);
            o.boot_ready_fd = -1;
        }
        if (settings.page == Page::BootWarning && !boot_displayed) {
            boot_displayed = true;
            boot_displayed_at = s.GetTicks();
        }
        dirty = false;
    }
    void volume_feedback(SDL_Renderer* r, int y) {
        if (!volume_toast || s.GetTicks() >= volume_toast) return;
        round_rect(r, 130, y + 60, 380, 62, 10, "focus");
        label(r, hardware.volume_percent == 0 ? "Muted" : "Volume", 151, y + 68, 20, "ink", 175);
        label(r, std::to_string(hardware.volume_percent) + "%", 421, y + 70, 20, "accent", 80);
        round_rect(r, 151, y + 106, 338, 5, 2, "slot");
        rect(r, 151, y + 106, std::max(1, hardware.volume_percent * 338 / 100), 5, "accent");
    }
    // `from_home` starts the selected game under Home instead of opening the
    // selected category.
    void launch(bool from_home = false) {
        if (launching) return;
        if (worker > 0 && (worker_action == "device.reboot" || worker_action == "device.poweroff" ||
                           worker_action == "device.update-apply")) {
            status = "Finishing the system request";
            dirty = true;
            return;
        }
        if (probe) return;
        if (home_menu && !from_home) {
            open_category();
            return;
        }
        if (systems_menu) {
            choose_system(systems_selected);
            return;
        }
        if (return_writes_pending()) {
            status = "Restoring settings";
            dirty = true;
            return;
        }
        // A previous failure must not latch every core off. The runner checks
        // writable storage and reapplies saved volume before opening any audio.
        return_audio_failed = false;
        if (o.boot_startup && (!library_ready || (!o.preview && (!boot_hardware_ready || system.volume_pending())))) {
            status = "Preparing the launcher";
            dirty = true;
            return;
        }
        if (model.games.empty()) return;
        launch_preview = o.preview || model.games[model.selected].demo || !o.allow_launch;
        if (!launch_preview && (o.app.empty() || o.data.empty())) {
            status = "ERROR: Launch needs app and data paths";
            dirty = true;
            return;
        }
        auto local = library_cache.options(model.games[model.selected]);
        launching_core = unsigned(offered_core(local.core < 0 ? int(preferences.core) : local.core));
        if (resume_launch) launching_core = unsigned(resume_core);
        if (model.games[model.selected].system != "nds") launching_core = unsigned(play_core); // RG DS Play
        sound_intent(MenuCue::Launch);
        launching = true;
        launch_at = s.GetTicks();
        feedback_frame_at = launch_at;
        launch_phase = LaunchPhase::Waiting;
        sounds.fade_music();
        launch_cancelled = launch_handoff = false;
        launch_result = 0;
        launch_drawn_head = -1;
        cancel_navigation();
        carousel.reset(model.selected);
        dirty = true;
        if (!launch_preview && worker > 0) {
            kill(worker, SIGTERM);
            worker_action.clear();
        }
    }
    // ---- PictoChat: our own chat pad. Its rules and state live in launcher/pictochat_app.hpp;
    // this is its drawing and its hooks into the launcher's input. It uses the launcher's one keyboard,
    // and it needs no BIOS, firmware or file from Nintendo.
    struct ChatGlyphs : pictochat::GlyphSource {
        RoundedFont& font;
        explicit ChatGlyphs(RoundedFont& f) : font(f) {}
        // A missing font file must not take the launcher down: the text is simply not drawn.
        void draw(const std::string& t, int x, int y, const std::function<void(int, int, uint8_t)>& put) override {
            try {
                font.rasterize(t, 24, x, y, put);
            } catch (const std::exception&) {}
        }
        int width(const std::string& t) override {
            try {
                return int(std::lround(font.width(t, 24)));
            } catch (const std::exception&) {
                return int(RoundedFont::codepoints(t).size()) * 12;
            }
        }
    };
    pictochat::Chat chat;
    ChatGlyphs chat_glyphs{rounded};
    bool chat_open = false;
    int chat_focus = 0; // the lobby row a controller would open (4 is the profile)
    int chat_shoulder = 0;
    Uint32 chat_shoulder_at = 0; // L (+1, older) or R (-1, newer) held on a controller
    double chat_scroll = 0;
    Uint32 chat_scroll_at = 0, chat_repeat_at = 0,
           chat_anim_at = 0; // the log's scroll in pixels, eased towards its goal
    uint32_t chat_seen_serial = 0, chat_slid_serial = 0;
    size_t chat_seen_users = 0;
    bool chat_seen_online = false, chat_seen_up = false;
    std::map<std::pair<SDL_Renderer*, uint32_t>, SDL_Texture*> chat_cards;
    std::map<SDL_Renderer*, SDL_Texture*> chat_pads, chat_sheets;
    bool key_accents = false, key_stamps = false, key_focus_visible = true;
    // Icon sizes in the sheet: interface icons, then the four room plans, then the stamps.
    static constexpr int chat_cells[pictochat::ui_icon_count] = {26, 26, 24, 24, 28, 22, 22,
                                                                 20, 22, 52, 52, 52, 52, 30};
    static constexpr int chat_stamp_cell = 56, chat_tail = 16;
    static Color chat_ink(int index) {
        uint32_t c = pictochat::inks[size_t(index) % pictochat::ink_count];
        return {uint8_t(c & 255), uint8_t((c >> 8) & 255), uint8_t((c >> 16) & 255)};
    }
    static Color chat_hue(float degrees) {
        uint32_t c = pictochat::hue_colour(degrees);
        return {uint8_t(c & 255), uint8_t((c >> 8) & 255), uint8_t((c >> 16) & 255)};
    }
    // The colour that dresses the pad: yours, or the launcher's accent while you have not chosen one.
    Color chat_tint() const { return chat.colour_auto ? theme.at("accent") : ds_colours[size_t(chat.colour)].rgb; }
    int chat_accent_index() const;
    fs::path chat_helper_script() const;
    void chat_save_profile();
    void open_pictochat();
    void close_pictochat();
    // ---- textures: messages, the live pad and one sheet of icons ----
    SDL_Texture* chat_upload(SDL_Renderer* r, const std::vector<uint32_t>& pixels, int w, int h, bool blend);
    SDL_Texture* chat_card(SDL_Renderer* r, const pictochat::Entry& entry);
    SDL_Texture* chat_pad_texture(SDL_Renderer* r);
    static int chat_cell(int id) { return id < pictochat::ui_icon_count ? chat_cells[id] : chat_stamp_cell; }
    static int chat_sheet_left(int id) {
        int x = 0;
        for (int i = 0; i < id; i++) x += chat_cell(i);
        return x;
    }
    // The icons and stamps are drawn once from their outlines as white shapes that take any colour.
    SDL_Texture* chat_sheet(SDL_Renderer* r);
    // id 0-12: interface icons; stamps are drawn with id = ui_icon_count + stamp.
    void chat_icon(SDL_Renderer* r, int id, int cx, int cy, Color ink);
    // ---- drawing helpers ----
    bool chat_down(const pictochat::Box& b) const;
    void chat_brackets(SDL_Renderer* r, int x, int y, int w, int h, Color c, int grow);
    // A raised key or button; `lit` fills it with the pad's colour.
    void chat_key_face(SDL_Renderer* r, const SDL_Rect& b, int radius, bool lit, bool down, bool enabled = true);
    void chat_button(SDL_Renderer* r, int y, const pictochat::Box& box, bool lit, bool enabled, int icon,
                     const std::string& text = "", Color* ink_out = nullptr);
    void chat_footer(SDL_Renderer* r, int y, const std::string& left, const std::string& right,
                     const std::string& middle);
    // ---- the keyboard, shared with search and every text field ----
    std::vector<std::vector<KeyboardKey>> keyboard_rows() const;
    void keyboard_clamp();
    bool key_down(const SDL_Rect& b) const;
    void draw_keyboard(SDL_Renderer* r, int y, const std::vector<std::vector<KeyboardKey>>& rows, bool show_focus);
    void keyboard_apply(const KeyboardKey& key);
    void keyboard_choose();
    // Controller: move over the keys, as in search.
    void keyboard_move(int code);
    bool keyboard_touch(int x, int y);
    // ---- the lobby ----
    void chat_enter_naming();
    void chat_finish_name(bool keep);
    void chat_set_colour(int chip); // chip 0 follows the accent, 1-16 are the DS colour
    void chat_join(int room);
    void chat_leave_room();
    void chat_lobby_bottom(SDL_Renderer* r, int y);
    void chat_naming_bottom(SDL_Renderer* r, int y);
    void chat_lobby_top(SDL_Renderer* r, int y);
    // ---- the room: lower screen ----
    // Screen coordinates on the (enlarged) pad, mapped back to the message canvas.
    std::pair<int, int> chat_pad_point(int x, int y) const;
    void chat_room_bottom(SDL_Renderer* r, int y);
    void chat_bottom(SDL_Renderer* r, int y);
    // ---- the room: upper screen ----
    static constexpr float chat_log_scale = .9f; // message cards on the upper screen
    static int chat_entry_height(const pictochat::Entry& e) { return e.notice.empty() ? 124 : 34; }
    // What the upper screen depends on. While the pad is being drawn on, or the lobby's brackets breathe,
    // the upper screen is skipped as long as this has not changed (and at least every 15 s, for the clock).
    std::string chat_top_seen;
    // The library's upper screen repaints only when something it shows has
    // changed (and at least every two seconds as a safety net): transitions
    // and browsing animate the lower screen alone, so the top panel's draw and
    // present are usually skipped. An empty signature means "always draw".
    std::string top_seen;
    Uint32 top_drawn_at = 0;
    uint64_t top_skips = 0;
    std::string top_signature(Uint32 now);
    std::string chat_top_signature() const;
    double chat_scroll_goal() const;
    double chat_content_height() const;
    void chat_top(SDL_Renderer* r, int y);
    // ---- input ----
    void chat_type(const std::string& piece);
    void chat_send();
    bool chat_event(const SDL_Event& e);
    void chat_press(int x, int y);
    void chat_scroll_step(int direction); // +1 towards older message
    void chat_drag(int x, int y);
    void chat_release();
    void chat_action(int code);
    void poll_chat();
    void begin_launch_exit(bool handoff, Uint32 now) {
        launch_handoff = handoff;
        if (launch_phase == LaunchPhase::Exiting) return;
        launch_exit_opacity = launch_opacity(now);
        launch_exit_at = now;
        launch_phase = LaunchPhase::Exiting;
        dirty = true;
    }
    void cancel_launch() {
        if (!launching || launch_phase == LaunchPhase::Playing || launch_cancelled) return;
        sounds.stop();
        sound_intent(MenuCue::Back);
        launch_cancelled = true;
        launch_cancel_at = s.GetTicks();
        if (launch_child > 0) kill(launch_child, SIGTERM);
        else begin_launch_exit(false, s.GetTicks());
        launch_handoff = false;
    }
    void finish_launch(bool reopen) {
        if (launch_channel >= 0) ::close(launch_channel);
        launch_channel = -1;
        launching = false;
        launch_phase = LaunchPhase::None;
        if (!reopen) configure_sounds(); // a cancelled or preview launch: the music comes back
        if (reopen) {
            library_cache.clear();
            library_summary.request(o.data, all_games);
            open();
            configure_sounds();
            last_activity = s.GetTicks();
            library_view(preferences.view);
            if (home_menu) carousel.reset(home_selected); // back to Home, which the game was started from
            try {
                if (fs::exists(o.system_preferences))
                    system_preferences = load_system_preferences(o.system_preferences);
            } catch (const std::exception& e) {
                std::cerr << "Could not reload system settings: " << e.what() << std::endl;
            }
            if (hardware.volume) {
                hardware.volume_percent = confirmed_volume();
                return_volume = hardware.volume_percent;
                volume_unsaved = true;
            }
            if (hardware.brightness) {
                return_brightness = system_preferences.brightness;
                hardware.brightness_percent = system_preferences.brightness;
                brightness_unsaved = true;
                brightness_changed = last_activity;
            }
            if (hardware.power && system_preferences.power > 0) return_power = system_preferences.power;
            return_status_needed = true;
            begin_return_controls();
        }
        last_activity = s.GetTicks();
        input_guard_until = last_activity + (reopen ? 250 : 100);
        status = launch_cancelled      ? "Launch cancelled"
                 : launch_result == 78 ? "The memory card is read-only"
                 : launch_result       ? "LAUNCH ERROR " + std::to_string(launch_result) + " - SEE LOG"
                 : launch_preview      ? "PREVIEW: " + std::string(core_names[launching_core]) + " SELECTED"
                                       : "BACK HOME";
        if (launch_result == 78) check_card(s.GetTicks(), true);
        if (launch_result && !launch_cancelled) emit_sound(MenuCue::Error);
        else if (reopen) emit_sound(MenuCue::Back);
        dirty = true;
    }
    void start_launch_runner(Uint32 now) {
        try {
            save_switcher_library();
        } catch (const std::exception& error) {
            std::cerr << "Game switcher snapshot: " << error.what() << std::endl;
            std::error_code ec;
            fs::remove(o.data / "session/switcher-library.ini", ec);
        }
        int channel[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0) {
            launch_result = 127;
            begin_launch_exit(false, now);
            return;
        }
        if (fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
            ::close(channel[0]);
            ::close(channel[1]);
            launch_result = 127;
            begin_launch_exit(false, now);
            return;
        }
#ifdef SO_NOSIGPIPE
        const int suppress = 1;
        setsockopt(channel[0], SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
#endif
        std::vector<std::string> args = {"python3",
                                         (o.app / "runtime/emulators.py").string(),
                                         "launch",
                                         "--app",
                                         o.app.string(),
                                         "--data",
                                         o.data.string(),
                                         "--core",
                                         core_ids[launching_core],
                                         "--rom",
                                         model.games[model.selected].path.string(),
                                         "--display-handoff-fd",
                                         std::to_string(channel[1])};
        save_state();
        launch_child = fork();
        if (launch_child == 0) {
            if (resume_launch) setenv("RGDS_FORCE_AUTOLOAD", "1", 1);
            ::close(channel[0]);
            if (fcntl(channel[1], F_SETFD, 0) < 0) _exit(127);
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        resume_launch = false;
        ::close(channel[1]);
        launch_channel = channel[0];
        launch_phase = LaunchPhase::Preparing;
        if (launch_child < 0) {
            launch_result = 127;
            begin_launch_exit(false, now);
        }
    }
    void poll_launch(Uint32 now) {
        if (!launching) return;
        if (launch_child > 0) {
            int result = 0;
            auto finished = waitpid(launch_child, &result, WNOHANG);
            if (finished == launch_child || (finished < 0 && errno != EINTR)) {
                launch_child = -1;
                launch_result = finished < 0        ? 127
                                : WIFEXITED(result) ? WEXITSTATUS(result)
                                                    : 128 + (WIFSIGNALED(result) ? WTERMSIG(result) : 0);
                if (launch_phase == LaunchPhase::Playing) {
                    finish_launch(true);
                    return;
                }
                begin_launch_exit(false, now);
            } else if (launch_cancelled && now - launch_cancel_at >= 5000) kill(launch_child, SIGKILL);
        }
        if (launch_phase == LaunchPhase::Playing) return;
        if (launch_preview) {
            if (launch_phase != LaunchPhase::Exiting && now - launch_at >= 500) begin_launch_exit(false, now);
        } else if (launch_phase == LaunchPhase::Waiting) {
            // Reap helpers and drain pending controls via the ordinary event loop.
            // No sleeps, synchronous worker shutdown or control flush on START.
            if (worker > 0) {
                auto done = waitpid(worker, nullptr, WNOHANG);
                if (done > 0 || (done < 0 && errno != EINTR)) worker = -1;
                else if (now - launch_at >= 1000) kill(worker, SIGKILL);
            }
            if (worker <= 0 && !system.volume_pending() && !system.brightness_pending() && !volume_unsaved &&
                !brightness_unsaved)
                start_launch_runner(now);
        } else if (launch_phase == LaunchPhase::Preparing && !launch_cancelled) {
            char ready = 0;
            auto received = recv(launch_channel, &ready, 1, MSG_DONTWAIT);
            if (received == 1 && ready == 'R') begin_launch_exit(true, now);
        }
        if (launch_phase == LaunchPhase::Exiting && (!preferences.animations || now - launch_exit_at >= 80)) {
            if (launch_handoff && launch_child > 0) {
                // Let the finite launch sparkle finish while input/cancel remains live.
                if (sounds.active() && now - launch_at < 1200) return;
                // Present the final fade frame, release SDL, then acknowledge ownership.
                draw(!o.screenshot.empty());
                close();
                launch_phase = LaunchPhase::Playing;
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                const char released = 'C';
                if (send(launch_channel, &released, 1, flags) != 1) {
                    kill(launch_child, SIGTERM);
                }
                ::close(launch_channel);
                launch_channel = -1;
            } else if (launch_child <= 0) finish_launch(false);
        }
    }
    void stop_launch() {
        if (launch_channel >= 0) ::close(launch_channel);
        launch_channel = -1;
        if (launch_child > 0) {
            kill(launch_child, SIGTERM);
            for (int i = 0; i < 600; i++) {
                auto done = waitpid(launch_child, nullptr, WNOHANG);
                if (done == launch_child || (done < 0 && errno != EINTR)) {
                    launch_child = -1;
                    break;
                }
                usleep(10000);
            }
            if (launch_child > 0) {
                kill(launch_child, SIGKILL);
                while (waitpid(launch_child, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
        launch_child = -1;
    }
    void action(int code);
    void touch(int x, int y);
    int cover_hit(int x, int y);
    LibraryViewport browser_list;
    bool browser_list_touch = false, browser_rail = false, browser_hover = false;
    int browser_scroll_start = 0, browser_grab = 0;
    void pointer_begin(int x, int y);
    void pointer_move(int x, int y);
    void pointer_end(int x, int y);
    bool mouse_position(Uint32 window, int& x, int& y);
    void raw_touch(const TouchPoint& point, Uint32 now);
    MainlineControls mainline_controls;
    std::set<SDL_JoystickID> mainline_pads;
    void event(SDL_Event e);
    void handle_input(const SDL_Event& e);

public:
    App(Options options)
        : o(std::move(options)), theme(default_theme()),
          rounded(s, o.app.empty() ? fs::path("assets/fonts") : o.app / "assets/fonts"), media(s), shader_preview(s),
          system(o.preview, o.app, o.data), probe(o.probe) {
        launch_profile.mark("sdl-library-loaded");
        if (o.themes.empty()) o.themes = o.app.empty() ? fs::path("themes/default") : o.app / "themes/default";
        if (!o.boot_startup)
            shaders = load_shaders(o.app.empty() ? fs::path("assets/shaders/catalog.tsv")
                                                 : o.app / "assets/shaders/catalog.tsv");
        if (o.preferences.empty())
            o.preferences = o.data.empty() ? fs::path("build/dev/preferences.ini") : o.data / "preferences.ini";
        if (o.data.empty()) o.data = o.preferences.parent_path();
        library_cache.reset(o.data);
        if (o.system_preferences.empty()) o.system_preferences = o.preferences.parent_path() / "system.ini";
        if (!o.boot_startup && (!o.game_menu || o.preview)) {
            refresh_system();
            system_preferences.brightness = hardware.brightness_percent;
            brightness_applied = hardware.brightness_percent;
        }
        if (!o.boot_startup && !o.preview && !probe && !o.game_menu && hardware.volume &&
            !system.set("volume", hardware.volume_percent))
            status = "Could not apply volume";
        if (!o.boot_warning) load_system_configuration();
        preferences.dark = dark_theme_file(o.theme);
        try {
            preferences = load_preferences(o.preferences);
        } // reads ui.ini even without the base file
        catch (const std::exception& e) {
            std::cerr << "Preferences fallback: " << e.what() << std::endl;
            preferences = Preferences{};
            try {
                load_ui_preferences(o.preferences.parent_path() / "ui.ini", preferences);
            } catch (const std::exception&) {}
        }
        locale.load(asset_path("locales"), preferences.language);
        rounded.prefer(preferences.language);
        if (!o.mode.empty()) preferences.dark = o.mode == "dark";
        if (!o.font.empty()) preferences.pixel = o.font == "pixel";
        if (!o.boot_startup || !preferences.pixel) check_fonts(o.boot_startup || o.game_menu);
        try {
            reload_themes();
        } catch (const std::exception& e) {
            std::cerr << "Theme fallback: " << e.what() << std::endl;
            theme = preferences.dark ? default_dark_theme() : default_theme();
            status = "DEFAULT THEME - CHECK LOG";
        }
        if (o.boot_startup) {
            settings.page = o.boot_warning ? Page::BootWarning : Page::Library;
            home_menu = true;
        } else {
            session_initialized = true;
            load_library(o.game_menu ? std::vector<Game>{} : scan(o.roots));
        }
        if (o.card_repair) {
            card_repair = 1;
            card_repair_at = s.GetTicks();
        }
        launch_profile.mark("configuration-ready");
    }
    ~App() {
        stop_microphone();
        stop_launch();
        if (o.boot_ready_fd >= 0) ::close(o.boot_ready_fd);
        stop_library_scan();
        stop_worker();
        std::fill(password.begin(), password.end(), '\0');
        close();
    }
    void finish_managed_menu(int result) {
#ifdef __linux__
        const char* driver = s.GetCurrentVideoDriver();
        if (!o.game_menu || o.preview || embedded_menu || !std::getenv("RGDS_SYSTEM_MANAGED") || !driver ||
            std::string(driver) != "wayland")
            return;
        // The vendor EGL driver waits a full second per window for buffers
        // during destruction. This is a terminating, session-owned helper:
        // finish durable work and reap children, then let process exit close
        // its Wayland connection and release its GPU resources.
        // The parent retains game-input ownership until it reaps this process.
        stop_launch();
        stop_library_scan();
        stop_worker();
        flush_return_controls();
        flush_brightness();
        flush_volume();
        system.finish();
        overlay.close();
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
#else
        (void)result;
#endif
    }
    int run() {
        open();
        configure_sounds();
        view_motion.seed = ViewMotion::hash(uint32_t(getpid()) ^ uint32_t(std::time(nullptr)));
        for (auto& panel : surfaces) {
            for (const char* name : {"brand/mark-48.bmp", "brand/mark-120.bmp"})
                media.warm(panel.renderer, asset_path(name));
            media.warm(panel.renderer,
                       asset_path(preferences.dark ? "brand/status-dark.bmp" : "brand/status-light.bmp"));
            media.warm(panel.renderer,
                       asset_path(preferences.dark ? "brand/wordmark-dark.bmp" : "brand/wordmark-light.bmp"));
        }
        const auto began = s.GetTicks();
        boot_scene_at = began;
        last_activity = began;
        int frames = 0;
        std::set<int> visited_pages;
        if (!o.boot_startup && !o.game_menu && !o.roots.empty()) start_worker("index");
        if (!o.input_script.empty()) {
            std::ifstream file(o.input_script);
            if (!file) throw std::runtime_error("Cannot read input script");
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream record(line);
                Input input{};
                std::string type, key;
                if (!(record >> input.time >> type)) throw std::runtime_error("Invalid preview input script");
                if (type == "key" || type == "key-up") {
                    if (!(record >> key)) throw std::runtime_error("Input key missing");
                    input.event.type = type == "key" ? SDL_KEYDOWN : SDL_KEYUP;
                    input.event.key.keysym.sym = s.GetKeyFromName(key.c_str());
                    if (!input.event.key.keysym.sym) throw std::runtime_error("Unknown input key");
                } else if (type == "text") {
                    input.event.type = SDL_TEXTINPUT;
                    std::string value;
                    std::getline(record >> std::ws, value);
                    if (value.size() > 31) throw std::runtime_error("Input text too long");
                    std::copy(value.begin(), value.end(), input.event.text.text);
                } else if (type == "tap" || type == "click") {
                    input.event.type = SDL_MOUSEBUTTONDOWN;
                    input.event.button.button = SDL_BUTTON_LEFT;
                    if (!(record >> input.event.button.x >> input.event.button.y))
                        throw std::runtime_error("Input coordinates missing");
                    input.event.button.y += 480;
                    input.event.button.windowID = surfaces[0].id;
                } else if (type == "drag") {
                    input.event.type = SDL_MOUSEMOTION;
                    input.event.motion.state = SDL_BUTTON_LMASK;
                    if (!(record >> input.event.motion.x >> input.event.motion.y))
                        throw std::runtime_error("Input coordinates missing");
                    input.event.motion.y += 480;
                    input.event.motion.windowID = surfaces[0].id;
                } else if (type == "release") {
                    input.event.type = SDL_MOUSEBUTTONUP;
                    input.event.button.button = SDL_BUTTON_LEFT;
                } else if (type == "finger-down" || type == "finger-move" || type == "finger-up") {
                    input.event.type = type == "finger-down"   ? SDL_FINGERDOWN
                                       : type == "finger-move" ? SDL_FINGERMOTION
                                                               : SDL_FINGERUP;
                    input.event.tfinger.touchId = 1;
                    input.event.tfinger.fingerId = 1;
                    input.event.tfinger.windowID = surfaces[0].id;
                    if (!(record >> input.event.tfinger.x >> input.event.tfinger.y))
                        throw std::runtime_error("Finger coordinates missing");
                    record >> input.event.tfinger.fingerId;
                } else if (type == "joy-down" || type == "joy-up") {
                    input.event.type = type == "joy-down" ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                    int button = 0;
                    if (!(record >> button) || button < 0 || button > 255)
                        throw std::runtime_error("Joystick button missing");
                    input.event.jbutton.button = Uint8(button);
                    input.event.jbutton.which = 0;
                    input.event.jbutton.state = type == "joy-down" ? SDL_PRESSED : SDL_RELEASED;
                } else if (type == "joy-hat") {
                    int value = 0;
                    if (!(record >> value) || value < 0 || value > 15)
                        throw std::runtime_error("Joystick direction missing");
                    input.event.type = SDL_JOYHATMOTION;
                    input.event.jhat.which = 0;
                    input.event.jhat.hat = 0;
                    input.event.jhat.value = Uint8(value);
                } else if (type == "wheel") {
                    input.event.type = SDL_MOUSEWHEEL;
                    if (!(record >> input.event.wheel.x >> input.event.wheel.y))
                        throw std::runtime_error("Wheel delta missing");
                } else if (type == "focus-lost") {
                    input.event.type = SDL_WINDOWEVENT;
                    input.event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                    input.event.window.windowID = surfaces[0].id;
                } else throw std::runtime_error("Unknown preview input type");
                inputs.push_back(input);
                if (type == "click") {
                    input.time++;
                    input.event.type = SDL_MOUSEBUTTONUP;
                    inputs.push_back(input);
                }
            }
            std::stable_sort(inputs.begin(), inputs.end(),
                             [](const Input& a, const Input& b) { return a.time < b.time; });
            // A key the script never releases is a tap, as a real keyboard would send; a key-up later on holds it (and repeats) until then.
            std::vector<Input> taps;
            for (size_t i = 0; i < inputs.size(); i++)
                if (inputs[i].event.type == SDL_KEYDOWN) {
                    bool released = false;
                    for (size_t j = i + 1; j < inputs.size() && !released; j++)
                        released = inputs[j].event.type == SDL_KEYUP &&
                                   inputs[j].event.key.keysym.sym == inputs[i].event.key.keysym.sym;
                    if (!released) {
                        Input up = inputs[i];
                        up.time++;
                        up.event.type = SDL_KEYUP;
                        taps.push_back(up);
                    }
                }
            inputs.insert(inputs.end(), taps.begin(), taps.end());
            std::stable_sort(inputs.begin(), inputs.end(),
                             [](const Input& a, const Input& b) { return a.time < b.time; });
        }
        while (!quit && !stopping) {
            update_health();
            if (o.game_menu && !o.preview && !sleep_audio_parked &&
                fs::exists(o.data / "session/game-menu.audio-sleep")) {
                sounds.close();
                sleep_audio_parked = true;
                std::ofstream(o.data / "session/game-menu.audio-sleep.ready").put('1');
            }
            if (sleep_audio_parked && !fs::exists(o.data / "session/game-menu.audio-sleep")) {
                sleep_audio_parked = false;
                restore_sleep_audio();
            }
            if (o.game_menu && !o.preview && fs::exists(o.data / "session/game-menu.wake")) {
                fs::remove(o.data / "session/game-menu.wake");
                reset_after_sleep();
                sleep_audio_parked = false;
                restore_sleep_audio();
            }
            // The emulator owns the displays during play; keep only its lifecycle alive.
            if (launch_phase == LaunchPhase::Playing) {
                ui_trace.mark(UiTrace::Playing, rendered_frames, uint32_t(settings.page));
                poll_launch(0);
                if (launch_phase == LaunchPhase::Playing) wait_child_exit(launch_child, 1000);
                continue;
            }
            while (next_input < inputs.size() && s.GetTicks() - began >= inputs[next_input].time)
                s.PushEvent(&inputs[next_input++].event);
            // Apply releases/back actions before held-button repeat or drawing.
            // A slow screenshot/frame must not repeat a key already released.
            ui_trace.mark(UiTrace::Events, rendered_frames, uint32_t(settings.page));
            SDL_Event queued{};
            while (s.PollEvent(&queued)) event(queued);
            ui_trace.mark(UiTrace::Loop, rendered_frames, uint32_t(settings.page));
            if (!o.cache_report.empty()) visited_pages.insert(int(settings.page));
            if (quit || stopping) break;
            auto now = s.GetTicks();
            const bool muted = hardware.volume && hardware.volume_percent == 0;
            sounds.mute_music(muted);
            if (muted && sounds.active()) sounds.stop();
            sounds.poll(now);
            if (!departure_action.empty() && int32_t(now - departure_at) >= 0) {
                auto request = std::move(departure_action);
                departure_action.clear();
                departure_dispatch = true;
                menu_request(request);
                departure_dispatch = false;
                if (quit) break;
            }
            if (settings.page == Page::Controls && control_capture) {
                if (now - capture_started >= 10000) {
                    control_capture = false;
                    capture_logical = -1;
                    controls_notice = "No button detected. Try again when you're ready.";
                    dirty = true;
                } else if (now - capture_clock >= 1000) {
                    capture_clock = now;
                    dirty = true;
                }
            }
            if (!o.probe && (session_initialized || o.card_repair)) check_card(now);
            if (card_repair) {
                if (o.seconds && now - began >= Uint32(o.seconds) * 1000) break;
                if (card_repair == 1 && preferences.animations && now - feedback_frame_at >= 50) {
                    feedback_frame_at = now;
                    dirty = true;
                }
                if (dirty || o.frames) {
                    draw(!o.screenshot.empty());
                    frames++;
                }
                if (o.frames && frames >= o.frames) break;
                SDL_Event wait{};
                if (wait_event(&wait, 50)) {
                    event(wait);
                    while (s.PollEvent(&wait)) event(wait);
                }
                continue;
            }
            int volume_delta = volume_keys.poll();
            if (session_initialized && !launching && volume_delta)
                change_volume(hardware.volume_percent + volume_delta);
            if (volume_toast && now >= volume_toast) {
                volume_toast = 0;
                dirty = true;
            }
            for (auto point : touchscreen.poll(now - began)) raw_touch(point, now);
            if (!session_initialized) {
                if (o.seconds && now - began >= Uint32(o.seconds) * 1000) break;
                const bool motion = boot_animating(now);
                if (motion && now - feedback_frame_at >= 33) {
                    feedback_frame_at = now;
                    dirty = true;
                }
                if (dirty || o.frames) {
                    draw(!o.screenshot.empty());
                    frames++;
                }
                if (o.frames && frames >= o.frames) break;
                if (o.boot_startup && !o.boot_warning) {
                    start_boot_session();
                    continue;
                }
                SDL_Event wait{};
                if (wait_event(&wait, motion ? 16 : input_wake.available() ? 250 : touchscreen.available() ? 8 : 50)) {
                    event(wait);
                    while (s.PollEvent(&wait)) event(wait);
                }
                continue;
            }
            poll_library_scan();
            poll_collection();
            poll_search_preview(now);
            if (facet_index.poll()) dirty = true;
            if (auto result = library_summary.poll()) {
                summary = std::move(*result);
                summary_ready = true;
                dirty = true;
            }
            if (!o.game_menu && library_ready && !setup_checked) {
                setup_checked = true;
                if (!o.preview && !fs::exists(o.data / "setup-complete") && fs::exists(o.data / "setup-required")) {
                    settings.page = Page::RomSetup;
                    settings.focus = 0;
                    setup_first_run = true;
                    dirty = true;
                }
            }
            if (!o.game_menu && o.boot_startup && library_ready && boot_hardware_ready && !return_writes_pending() &&
                !system.volume_pending() && !system.brightness_pending() && !resume_checked &&
                settings.page != Page::BootWarning) {
                resume_checked = true;
                auto ticket = o.data / "session/resume-launch.ini";
                auto saved = fields(ticket);
                std::error_code ec;
                fs::remove(ticket, ec);
                if (!saved["rom"].empty()) {
                    search_query.clear();
                    discovery_filter = 0;
                    discovery_facets = {};
                    library_view(0);
                    for (size_t i = 0; i < model.games.size(); i++)
                        if (model.games[i].path.string() == saved["rom"]) {
                            select_game(i, false);
                            home_menu = false;
                            for (int core = 0; core < core_count; core++)
                                if (saved["core"] == core_ids[core]) resume_core = core;
                            resume_launch = resume_core >= 0;
                            launch();
                            break;
                        }
                }
            }
            poll_return_controls();
            if (!launching) {
                int direction = hold.poll(now);
                if (direction == -2 || direction == 2) {
                    action(direction < 0 ? 8 : 9);
                    last_activity = now;
                } else if (direction && !picker_open && horizontal_hold_page()) {
                    if (settings.page == Page::Library) hold_navigate(direction, hold.held_for(now));
                    else action(direction < 0 ? 0 : 1);
                    last_activity = now;
                }
            }
            poll_brightness();
            poll_volume();
            if (auto refreshed = system.poll_status())
                if (refreshed->success) {
                    const auto previous = hardware;
                    auto brightness = hardware.brightness_percent, volume = hardware.volume_percent;
                    hardware = refreshed->value;
                    if (brightness_unsaved || brightness_drag || system.brightness_pending())
                        hardware.brightness_percent = brightness;
                    if (o.preview || return_writes_pending() || volume_unsaved || volume_drag ||
                        system.volume_pending() || volume_toast > now)
                        hardware.volume_percent = volume;
                    if (!volume_unsaved && !system.volume_pending()) {
                        volume_applied = hardware.volume_percent;
                        if (volume_applied > 0) unmuted_volume = volume_applied;
                    }
                    if (o.boot_startup && !boot_hardware_ready) {
                        boot_hardware_ready = true;
                        if (hardware.volume) {
                            system.request_volume(hardware.volume_percent);
                            volume_unsaved = true;
                        }
                        if (hardware.brightness) {
                            system.request_brightness(system_preferences.brightness);
                            brightness_unsaved = true;
                            brightness_changed = now;
                            hardware.brightness_percent = system_preferences.brightness;
                        }
                        if (hardware.power && system_preferences.power > 0)
                            system.set("power", system_preferences.power);
                        if (hardware.lid && system_preferences.lid) system.set("lid", 1);
                    }
                    if (!same_hardware(previous, hardware)) dirty = true;
                }
            if (!launching || launch_preview) poll_service(now);
            if (!o.preview && !o.game_menu && !launching && worker <= 0 && now - automatic_update_at > 60000) {
                automatic_update_at = now;
                start_worker("device.update-auto");
            }
            poll_microphone(now);
            watch_menu(now);
            if (settings.page == Page::Scraper && !service_loaded && worker <= 0) {
                start_worker("status");
                service_loaded = true;
            }
            if (!o.game_menu && !launching && worker <= 0 && (!device_loaded || now - connection_poll >= 15000))
                refresh_network(now);
            poll_launch(now);
            poll_chat();
            if (media.adopt()) dirty = true;
            if (launch_phase == LaunchPhase::Playing) continue;
            if (touch_tail && now - release_at >= 140) {
                touch_tail = false;
                dirty = true;
            }
            bool launch_transition =
                launching && preferences.animations && (now - launch_at < 110 || launch_phase == LaunchPhase::Exiting);
            if (launching && preferences.animations) {
                int head = int((now - launch_at) / 75) % 8;
                if (head != launch_drawn_head) {
                    launch_drawn_head = head;
                    dirty = true;
                }
            }
            sync_view_motion(now);
            bool view_active = view_motion.active();
            if (view_motion.update(now) && !view_motion.active()) dirty = true;
            bool rail_animating = update_rail_feedback(now);
            bool feedback_active = view_active || rail_animating || (preferences.animations && shutting_down()) ||
                                   (o.game_menu && menu_status["state"] == "busy" && preferences.animations) ||
                                   (preferences.animations && network_busy() &&
                                    (settings.page == Page::Network || settings.page == Page::Wifi)) ||
                                   boot_animating(now) || launch_transition || touch_animating(now) ||
                                   (preferences.animations &&
                                    (now < micro_until || (picker_open && now - picker_at < 170) || picker_closing()));
            // Stop refreshing a stationary held contact after its cue completes.
            if (feedback_active && now - feedback_frame_at >= 16) {
                feedback_frame_at = now;
                dirty = true;
            }
            bool ready = options_hold() && now - pointer_at >= 600;
            if (ready != options_ready) {
                options_ready = ready;
                dirty = true;
            }
            ease_update(now);
            const bool glitter = glitter_active();
            if (glitter && now - glitter_frame_at >= 33) {
                glitter_frame_at = now;
                dirty = true;
            }
            if (banner_shown() && banner_frame(now) != drawn_banner_frame) dirty = true;
            bool list_animated = browser_list.update(now);
            const bool shader_moving = settings.page == Page::ShaderPicker && shader_carousel.update(now);
            bool animated =
                (preferences.animations && (carousel.update(now) || shader_moving || list_animated ||
                                            now - selected_at < 190 || (!view_active && now - page_at < 150))) ||
                (settings.page == Page::ShaderPicker && shader_thumbs_pending);
            if (animated) dirty = true;
            if (hardware.charging && !drawn_status_stamp.empty() && int((now / 420) % 4) != drawn_charge_phase)
                dirty = true;
            if (artwork_busy() && !drawn_status_stamp.empty() && loader_phase(now) != drawn_loader_phase) dirty = true;
            watch_theme(now);
            if (!launching) watch_system(now);
            if (!launching && !animated && !carousel.dragging) watch_library_assets(now);
            if (o.seconds && s.GetTicks() - began >= static_cast<Uint32>(o.seconds) * 1000) break;
            if (now - clock_poll >= 250) {
                clock_poll = now;
                if (!drawn_status_stamp.empty() && status_stamp() != drawn_status_stamp) dirty = true;
                if (home_menu && preferences.home_layout == 1 && settings.page == Page::Library && !chat_open &&
                    !launching && std::time(nullptr) != drawn_second) {
                    drawn_second = std::time(nullptr);
                    dirty = true;
                }
            }
            if (dirty || o.frames) {
                ui_trace.mark(UiTrace::Assets, rendered_frames, uint32_t(settings.page));
                if (!rendered_frames) launch_profile.mark("assets-start");
                prepare_render_assets();
                if (!rendered_frames) launch_profile.mark("assets-ready");
                draw(!o.screenshot.empty());
                frames++;
                if (frames == 1 && o.game_menu && !o.preview) {
                    system.request_status();
                    system_poll = now;
                }
            }
            if (!o.preview && fs::exists(o.data / "session/capture-request")) {
                auto previous = o.screenshot;
                o.screenshot = o.data / "session/ui-snapshot";
                prepare_render_assets();
                draw(true);
                o.screenshot = previous;
                // Acknowledge only after both captures have been saved.
                fs::remove(o.data / "session/capture-request");
            }
            if (o.frames && frames >= o.frames) break;
            prepare_next_cover();
            SDL_Event e{};
            int wait_ms = animated || dirty                     ? 1
                          : feedback_active                     ? 16
                          : launching && preferences.animations ? int(std::min(50u, 75u - (now - launch_at) % 75))
                                                                : idle_wait_ms(now);
            if (o.game_menu && (!menu_pending_token.empty() || menu_status["state"] == "busy"))
                wait_ms = std::min(wait_ms, 20);
            if (chat_open) wait_ms = std::min(wait_ms, 40);
            if (glitter) wait_ms = std::min(wait_ms, 33);
            if (banner_shown() && preferences.animations) wait_ms = std::min(wait_ms, int(banner_wait(now)));
            if (wait_event(&e, wait_ms)) {
                event(e);
                while (s.PollEvent(&e)) event(e);
            }
        }
        flush_brightness();
        flush_volume();
        save_state();
        if (!o.cache_report.empty()) {
            std::ofstream report(o.cache_report);
            if (!report) throw std::runtime_error("Cannot write cache report");
            report << "{\"picker_detail\":" << picker_detail << ",\"detail_lines\":" << detail_lines.size()
                   << ",\"detail_scroll\":" << detail_scroll << ",\"picker_open\":" << picker_open
                   << ",\"picker_focus\":" << picker_focus << ",\"picker_scroll\":" << picker_scroll
                   << ",\"list_scroll\":" << list_scroll << ",\"status_stamp\":" << std::quoted(drawn_status_stamp)
                   << ',';
            report << "\"menu_music\":" << preferences.menu_music << ",\"music_volume\":" << preferences.music_volume
                   << ",\"music_reads\":" << sounds.music_reads << ",\"music_playing\":" << sounds.music_playing()
                   << ",\"music_samples\":" << sounds.music_samples() << ",\"menu_sounds\":" << preferences.menu_sounds
                   << ",\"sound_volume\":" << preferences.sound_volume << ",\"sound_plays\":" << sounds.plays
                   << ",\"sound_opens\":" << sounds.opens << ",\"sound_releases\":" << sounds.releases
                   << ",\"sound_open_now\":" << sounds.open_now() << ",\"sound_reads\":" << sounds.reads
                   << ",\"sound_bytes\":" << sounds.bytes() << ",\"sound_active\":" << sounds.active()
                   << ",\"sound_counts\":[";
            for (size_t i = 0; i < sounds.counts.size(); i++) {
                if (i) report << ',';
                report << sounds.counts[i];
            }
            report << "],";
            report << "\"element_moves\":" << element_moves << ",\"back_transitions\":" << back_transitions
                   << ",\"view_energy\":" << view_motion.energy << ",\"view_variant\":" << view_motion.variant << ',';
            report << "\"view_transition_style\":" << std::quoted(view_style_name(preferences.view_style))
                   << ",\"push_animation_frames\":" << push_animation_frames << ',';
            report << "\"view_animations\":" << preferences.view_animations
                   << ",\"view_transitions\":" << view_transitions
                   << ",\"view_animation_frames\":" << view_animation_frames
                   << ",\"view_animation_active\":" << view_motion.active()
                   << ",\"view_canvas_failed\":" << view_canvas_failed << ',';
            report << "\"font_atlases\":" << rounded.loaded_atlases() << ",\"font_bytes\":" << rounded.loaded_bytes()
                   << ",\"shader_catalog_loaded\":" << !shaders.empty() << ',';
            report << "\"shader_preview_scene\":" << shader_preview_scene
                   << ",\"shader_preview_mode\":" << int(shader_preview_mode)
                   << ",\"shader_preview_split\":" << shader_preview_split
                   << ",\"shader_preview_entries\":" << shader_preview.count()
                   << ",\"shader_preview_bytes\":" << shader_preview.bytes()
                   << ",\"shader_preview_reads\":" << shader_preview.counters.file_reads
                   << ",\"shader_preview_stamp_checks\":" << shader_preview.counters.stamp_checks << ',';
            report << "\"controls_game\":" << controls_per_game << ",\"controls_inherited\":" << controls_inherit
                   << ",\"controls_capturing\":" << control_capture
                   << ",\"controls_preset\":" << std::quoted(edited_controls.preset) << ',';
            report << "\"system_configuration_loaded\":" << system_configuration_loaded
                   << ",\"timezone\":" << std::quoted(system_preferences.timezone) << ',';
            report << "\"drastic_backend\":" << preferences.drastic_backend
                   << ",\"game_drastic_backend\":" << game_preferences.drastic_backend << ",";
            report << "\"systems_menu\":" << systems_menu << ",\"systems_selected\":" << systems_selected << ',';
            report << "\"show_systems\":" << preferences.show_systems
                   << ",\"shown_system\":" << std::quoted(shown_system)
                   << ",\"listed_systems\":" << listed_systems.size() << ",\"launching_core\":" << launching_core
                   << ",\"library_count\":" << model.games.size() << ",\"play_fit\":" << play_fit
                   << ",\"play_second\":" << play_second << ",\"play_frame\":" << std::quoted(play_frame) << ',';
            report << "\"shader_choices\":[";
            for (size_t i = 0; i < shader_choices.size(); ++i) report << (i ? "," : "") << shader_choices[i];
            report << "],";
            report << "\"default_core\":" << preferences.core << ",\"game_core_override\":" << game_preferences.core
                   << ",\"game_threaded\":" << game_preferences.threaded << ",\"game_hires\":" << game_preferences.hires
                   << ",\"game_frameskip\":" << game_preferences.frameskip
                   << ",\"game_scale\":" << game_preferences.rgds_scale
                   << ",\"game_filter2d\":" << game_preferences.rgds_2d
                   << ",\"menu_fast_forward\":" << (menu_status["fast_forward"] == "1")
                   << ",\"cheat_rows\":" << cheat_rows.size() << ",\"cheats_on\":" << cheats_on()
                   << ",\"cheat_view\":" << int(cheat_view) << ",\"cheat_group\":" << cheat_group
                   << ",\"cheats_available\":" << cheats_available() << ",\"settings_notice\":" << std::quoted(status)
                   << ",\"picker_title\":" << std::quoted(picker_title) << ',';
            report << "\"visited_pages\":[";
            bool first_page = true;
            for (int page : visited_pages) {
                if (!first_page) report << ',';
                report << page;
                first_page = false;
            }
            report << "],";
            report << "\"discovery_active\":" << discovery_active()
                   << ",\"discovery_description\":" << std::quoted(discovery_description())
                   << ",\"search_preview_games\":" << search_matches.size()
                   << ",\"search_preview_loading\":" << search_preview_loading << ",\"keyboard_row\":" << key_row
                   << ",\"keyboard_col\":" << key_col << ",\"keyboard_layer\":" << key_layer
                   << ",\"keyboard_caps\":" << key_caps << ",\"edit_cursor\":" << (edit_field == 4 ? edit_cursor : 0)
                   << ',';
            report << "\"home_menu\":" << home_menu << ",\"filtered_games\":" << model.games.size()
                   << ",\"search_query\":" << std::quoted(search_query) << ",\"discovery_filter\":" << discovery_filter
                   << ',';
            report << "\"page\":" << int(settings.page) << ",\"boot_pending\":" << (settings.page == Page::BootWarning)
                   << ",\"library_scan_started\":" << library_scan_started << ",\"library_ready\":" << library_ready
                   << ",\"frames\":" << rendered_frames << ",\"metadata_reads\":" << library_cache.counters.file_reads
                   << ",\"metadata_lookups\":" << library_cache.counters.lookups
                   << ",\"metadata_hits\":" << library_cache.counters.hits
                   << ",\"metadata_stamp_checks\":" << library_cache.counters.stamp_checks
                   << ",\"image_reads\":" << media.image_reads << ",\"async_loads\":" << media.async_loads
                   << ",\"top_skips\":" << top_skips << ",\"banner_reads\":" << media.banner_reads
                   << ",\"media_lookups\":" << media.counters.lookups << ",\"media_hits\":" << media.counters.hits
                   << ",\"media_stamp_checks\":" << media.counters.stamp_checks
                   << ",\"render_file_reads\":" << render_file_reads
                   << ",\"render_stamp_checks\":" << render_stamp_checks
                   << ",\"metadata_entries\":" << library_cache.size() << ",\"image_entries\":" << media.image_count()
                   << ",\"banner_entries\":" << media.banner_count() << ",\"texture_bytes\":" << media.bytes()
                   << ",\"evictions\":" << library_cache.counters.evictions + media.counters.evictions << "}\n";
        }
        // The managed session dispatches a terminal request only with this
        // acknowledgement; ordinary exit or failed cleanup cannot trigger it.
        return system_exit_ack ? 76 : 0;
    }
};

// Out-of-line App members, split by screen.
#include "launcher/settings_screen.hpp"
#include "launcher/settings_picker.hpp"
#include "launcher/input_handling.hpp"
#include "launcher/update_banner.hpp"
#include "launcher/pictochat_screen.hpp"
#include "launcher/panels.hpp"
#include "launcher/home_screen.hpp"
#include "launcher/library_screen.hpp"
#include "launcher/colours_screen.hpp"
#include "launcher/cheats_screen.hpp"
#include "launcher/shader_screen.hpp"
#include "launcher/controls_editor.hpp"
#include "launcher/keyboard_screen.hpp"
#include "launcher/game_info_screen.hpp"
#include "launcher/filter_pages.hpp"

static int self_test() {
    auto root = fs::temp_directory_path() / ("rgds-model-" + std::to_string(getpid()));
    fs::create_directories(root / "sub");
    std::ofstream(root / "B.nds").put('x');
    std::ofstream(root / "sub/A.DSI").put('x');
    std::ofstream(root / "ignore.txt").put('x');
    std::ofstream(root / "C.zip").put('x');
    auto games = scan({root, root / "sub"});
    Model m;
    m.games = games;
    m.move(-1);
    bool ok = games.size() == 3 && games[0].title == "A" && m.selected == 2;
    m.move(1);
    ok = ok && m.selected == 0;
    int offered = 0;
    for (int i = 0; i < core_count; i++) offered += core_offered(i);
    for (int i = 0; i < offered; i++) {
        m.next_core();
        ok = ok && core_offered(int(m.core));
    }
    ok = ok && m.core == 0;
    Model empty;
    empty.move(-99);
    ok = ok && empty.selected == 0;
    { // One IMA ADPCM block of rising nibbles must decode to a rising ramp of the declared length.
        std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0,    0, 'W', 'A', 'V',  'E',  'f',  'm',
                                    't', ' ', 20,  0,   0, 0, 0x11, 0, 1,   0,   0,    0x7d, 0,    0,
                                    0,   0,   0,   0,   8, 0, 4,    0, 2,   0,   9,    0,    'd',  'a',
                                    't', 'a', 8,   0,   0, 0, 0,    0, 0,   0,   0x77, 0x77, 0x77, 0x77};
        auto track = decode_ima_adpcm(wav);
        bool rising = track.samples.size() == 9 && track.rate == 32000;
        for (size_t i = 1; rising && i < track.samples.size(); i++) rising = track.samples[i] > track.samples[i - 1];
        ok = ok && rising && decode_ima_adpcm({'R', 'I', 'F', 'F'}).empty();
    }
    fs::remove_all(root);
    std::cout << (ok ? "Library, deduplication, navigation and engine selection passed." : "Model test failed.")
              << std::endl;
    return ok ? 0 : 1;
}
int main(int argc, char** argv) {
    launch_profile.start();
    try {
        if (argc > 1 && std::string(argv[1]) == "--scan-library") {
            std::vector<fs::path> roots;
            for (int i = 2; i < argc; i++) {
                if (std::string(argv[i]) != "--rom-dir" || ++i >= argc)
                    throw std::runtime_error("Invalid library scan arguments");
                roots.emplace_back(argv[i]);
            }
            for (const auto& game : scan(roots)) {
                auto path = game.path.string();
                std::cout.write(path.data(), path.size());
                std::cout.put('\0');
                std::cout.write(game.title.data(), game.title.size());
                std::cout.put('\0');
            }
            return std::cout ? 0 : 1;
        }
        if (argc > 1 && std::string(argv[1]) == "--restore-controls") {
            if (argc != 8) throw std::runtime_error("Invalid return-control arguments");
            setenv("RGDS_SYSTEM_MANAGED", "1", 1);
            SystemControls controls(false, fs::path(argv[2]), fs::path(argv[3]));
            const std::array<const char*, 4> names = {"volume", "brightness", "power", "lid"};
            const std::array<int, 4> minimum = {0, 5, 0, 0}, maximum = {100, 100, 3, 1};
            int failures = 0;
            for (int i = 0; i < 4; i++) {
                size_t end = 0;
                std::string raw = argv[4 + i];
                int value = std::stoi(raw, &end);
                if (end != raw.size() || (value != -1 && (value < minimum[i] || value > maximum[i])))
                    throw std::runtime_error("Invalid return-control value");
                if (value >= 0 && !controls.set(names[i], value)) {
                    failures |= 1 << i;
                    std::cerr << "Could not restore " << names[i] << std::endl;
                }
            }
            return failures;
        }
        Options o;
        if (const char* top = std::getenv("RGDS_TOP_DISPLAY")) o.top = top;
        if (const char* bottom = std::getenv("RGDS_BOTTOM_DISPLAY")) o.bottom = bottom;
        if (const char* platform = std::getenv("RGDS_SESSION_PLATFORM"))
            o.independent = std::string(platform) == "independent";
        if (const char* profile = std::getenv("RGDS_INPUT_PROFILE"))
            o.mainline = o.independent && std::string(profile) == "mainline-rg-ds";
        o.executable = fs::absolute(argv[0]);
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
                return std::string(argv[i]);
            };
            if (arg == "--self-test") return self_test();
            if (arg == "--preview") o.preview = true;
            else if (arg == "--probe") o.probe = true;
            else if (arg == "--game-menu") o.game_menu = true;
            else if (arg == "--game-switcher") {
                o.game_switcher = true;
                o.game_menu = true;
            } else if (arg == "--game-core") o.game_core = value();
            else if (arg == "--game-rom") o.game_rom = value();
            else if (arg == "--allow-launch") o.allow_launch = true;
            else if (arg == "--watch-assets") o.watch_assets = true;
            else if (arg == "--settings") o.settings = true;
            else if (arg == "--home") o.home = true;
            else if (arg == "--library") o.library = true;
            else if (arg == "--boot-warning") o.boot_warning = true;
            else if (arg == "--boot-startup") o.boot_startup = true;
            else if (arg == "--card-repair") o.card_repair = true;
            else if (arg == "--boot-ready-fd") {
                auto raw = value();
                size_t end = 0;
                o.boot_ready_fd = std::stoi(raw, &end);
                if (end != raw.size() || o.boot_ready_fd < 3)
                    throw std::runtime_error("Invalid boot notification descriptor");
            } else if (arg == "--cache-report") o.cache_report = value();
            else if (arg == "--touch-recording") o.touch_recording = value();
            else if (arg == "--section") o.section = value();
            else if (arg == "--mode") o.mode = value();
            else if (arg == "--font") o.font = value();
            else if (arg == "--preferences") o.preferences = value();
            else if (arg == "--system-preferences") o.system_preferences = value();
            else if (arg == "--themes") o.themes = value();
            else if (arg == "--input-script") o.input_script = value();
            else if (arg == "--frames") o.frames = std::stoi(value());
            else if (arg == "--seconds") o.seconds = std::stoi(value());
            else if (arg == "--theme") o.theme = value();
            else if (arg == "--screenshot") o.screenshot = value();
            else if (arg == "--rom-dir") o.roots.emplace_back(value());
            else if (arg == "--system-dir") {
                const std::string entry = value();
                const auto eq = entry.find('=');
                if (eq == std::string::npos || system_info(entry.substr(0, eq)).id != entry.substr(0, eq) ||
                    entry.substr(0, eq) == "nds")
                    throw std::runtime_error("--system-dir needs gba=, gbc= or gb= and a folder");
                o.system_roots.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            } else if (arg == "--app") o.app = value();
            else if (arg == "--data") o.data = value();
            else if (arg == "--state") o.state = value();
            else if (arg == "--top-display") o.top = value();
            else if (arg == "--bottom-display") o.bottom = value();
            else if (arg == "--help") {
                std::cout
                    << "RGDS: --preview | --probe [--seconds 90] | --allow-launch --app DIR --data DIR\n"
                    << "--rom-dir DIR (repeatable), --theme FILE, --frames N, --screenshot FILE.bmp\n"
                    << "--watch-assets (reload theme on save), --state FILE (remember selection/core)\n"
                    << "--home | --library (category menu or game-list preview)\n"
                    << "--boot-warning (optional direct-boot touch-to-continue screen)\n"
                    << "--boot-startup (direct startup with deferred scan and hardware work)\n"
                    << "--cache-report PATH (development asset-cache counters)\n"
                    << "--touch-recording FILE (preview Linux AArch64 evdev capture)\n"
                    << "--settings [--section theme|brightness|power|library|sound|connectivity|system|controls]\n"
                    << "--game-menu --game-core drastic|dsperate|melonds|rgds-experimental [--game-rom FILE]\n"
                    << "--mode light|dark --font rounded|pixel --preferences FILE --themes DIR\n"
                    << "--system-preferences FILE (power, brightness and menu sleep)\n"
                    << "--input-script FILE (desktop-only timed key/tap events)\n"
                    << "--top-display NAME/INDEX --bottom-display NAME/INDEX\n";
                return 0;
            } else throw std::runtime_error("Unknown option: " + arg);
        }
        if (o.probe && !o.seconds) o.seconds = 90;
        if (!o.mode.empty() && o.mode != "light" && o.mode != "dark")
            throw std::runtime_error("Mode must be light or dark");
        if (!o.font.empty() && o.font != "rounded" && o.font != "pixel")
            throw std::runtime_error("Font must be rounded or pixel");
        if (!o.preview && !o.input_script.empty())
            throw std::runtime_error("Input scripts are only available in desktop preview");
        if (!o.preview && !o.touch_recording.empty())
            throw std::runtime_error("Touch recordings are only available in desktop preview");
        if (o.boot_warning) o.boot_startup = true;
        if (o.boot_ready_fd >= 0) {
            int kind = 0;
            socklen_t length = sizeof(kind);
            if (!o.boot_startup || getsockopt(o.boot_ready_fd, SOL_SOCKET, SO_TYPE, &kind, &length) != 0 ||
                kind != SOCK_STREAM)
                throw std::runtime_error("Boot notification needs a managed startup socket");
            if (fcntl(o.boot_ready_fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(o.boot_ready_fd, F_SETFL, O_NONBLOCK) < 0)
                throw std::runtime_error("Cannot configure boot notification socket");
        }
        if (o.boot_startup && (o.probe || o.game_menu || o.settings))
            throw std::runtime_error("Boot startup is a home startup option");
        if (o.seconds < 0 || o.frames < 0) throw std::runtime_error("Frames and seconds must be nonnegative");
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
        launch_profile.mark("arguments-ready");
        App app(std::move(o));
        int result = app.run();
        app.finish_managed_menu(result);
        return result;
    } catch (const std::exception& e) {
        std::cerr << "RGDS: " << e.what() << std::endl;
        return 1;
    }
}
