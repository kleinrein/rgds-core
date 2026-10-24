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
#include "launcher/dual_screen.hpp"
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
    void motion_input(int code);
    void move_element(uint32_t id, int& x, int& y, int width, int height, bool focused, int clip_top = 0,
                      int clip_bottom = 480, int clip_left = 0, int clip_right = 640);
    SDL_Texture* view_canvas = nullptr;
    SDL_Texture *view_previous = nullptr, *view_presented = nullptr;
    bool view_snapshot_ready = false, view_previous_ready = false;
    ViewStyle last_view_style = ViewStyle::Spring;
    std::vector<std::pair<int, bool>> view_levels;
    uint64_t push_animation_frames = 0;
    void clear_view_canvases();
    void start_view_motion(Uint32 now, ViewTravel route = ViewTravel::Forward);
    SDL_Renderer* bottom_renderer() { return surfaces.size() > 1 ? surfaces[1].renderer : surfaces[0].renderer; }
    uint64_t view_capture_frame = ~uint64_t(0);
    // The outgoing image for a Push transition, captured once per displayed
    // frame when an input arrives. Idle frames never render offscreen.
    void capture_view_for_input(const SDL_Event& e);
    bool view_canvas_failed = false, view_seen = false;
    std::tuple<int, bool, int, int> last_view;
    uint64_t view_transitions = 0, view_animation_frames = 0;
    void sync_view_motion(Uint32 now);
    struct SoundState {
        Page page;
        bool home, picker;
        int view, layout, focus, pick, row, col, volume, brightness;
        size_t game, text;
        unsigned toggles;
        bool favourite, launch;
        int bar; // Home's bottom-bar button under the D-pad, or -1
    };
    unsigned sound_toggles() const {
        return unsigned(preferences.dark) | (unsigned(preferences.pixel) << 1) |
               (unsigned(preferences.animations) << 2) | (unsigned(preferences.view_animations) << 3) |
               (unsigned(preferences.touch_indicator) << 4) | (unsigned(preferences.auto_load) << 5) |
               (unsigned(preferences.auto_save) << 6) | (unsigned(preferences.menu_sounds) << 7) |
               (unsigned(system_preferences.lid) << 8);
    }
    SoundState sound_state() const;
    unsigned sound_depth = 0;
    int sound_code = 2;
    SoundState sound_before{};
    std::optional<MenuCue> explicit_sound;
    std::string menu_sound_action, departure_action;
    Uint32 departure_at = 0;
    bool departure_dispatch = false, sleep_audio_parked = false;
    void configure_sounds();
    // Every new sink can reset the RK817 DAC, including the boot screen. The
    // menu sink stays paused until the saved level is back; a direct control
    // write avoids a helper process on each reopen. Status may not exist yet.
    void restore_dac_level();
    bool emit_sound(MenuCue cue, int direction = 0);
    void sound_intent(MenuCue cue);
    void finish_sound_action();
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
    void menu_sounds_action(int code);
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
            // The last three rows are always Game info, Save gallery and DS controls.
            if (other_system_selected()) return 8; // the rows every engine has; DS-only ones read Unavailable
            if (experimental_game()) return 9;
            return (game_preferences.core < 0 ? int(preferences.core) : game_preferences.core) == 0
                       ? (superdrastic_game() ? 11 : 10)
                       : 8;
        case Page::GameArtwork: return 3;
        case Page::Maintenance: return 7;
        case Page::Network: return 5;
        case Page::Startup: return 4;
        case Page::SaveGallery: return 9;
        case Page::Discover: return 4;
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
        home_bar_focus = -1;
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
    void load_library(std::vector<Game> games);
    void stop_library_scan();
    void begin_library_scan();
    void poll_library_scan();
    void start_boot_session();
    void continue_boot();
    void snapshot_boot_input();
    std::set<SDL_JoystickID> menu_held_hats;
    // A fresh pause menu would read the game's held controls as new presses:
    // SDL posts every joystick button already down on its first update, and a
    // stylus resting on the panel decodes as a new contact. A held A (accelerate),
    // B, Start, Select or Menu, or a stylus over Resume, would close the menu at
    // once, so each control held at open is ignored until it is released.
    void snapshot_menu_input();
    // The column's top tile: straight back into the last game.
    void open_last_played();
    void open_category();
    // ---- Show all systems (Settings > Library): other systems' games are played by
    // RG DS Play. Games opens the Systems page; Favourites and Recents span every system.
    bool other_system_selected() const;
    void refresh_other_systems();
    std::vector<Game> view_source(int view) const;
    size_t system_games(const std::string& id) const;
    void open_systems();
    void choose_system(size_t index);
    // A system's tile: a cartridge and its badge, drawn in the space a game's icon takes.
    void system_icon(SDL_Renderer* r, size_t index, int x, int y, int size);
    // RG DS Play's view of the running game, as the runtime stores it beside game.ini.
    fs::path play_view_path() { return item_folder(0) / "play.ini"; }
    void load_play_view();
    // What RG DS Play can draw around the game (play/src/ui/frame.hpp), by their play.ini names: its own
    // styles, then the owner's picture frames, userdata/frames/<system>/<name>.png.
    static constexpr std::array<const char*, 9> play_frames = {"off", "indigo",  "glacier",   "black",      "platinum",
                                                               "bar", "sp-onyx", "sp-cobalt", "sp-platinum"};
    static constexpr std::array<const char*, 9> play_frame_names = {
        "Off", "Indigo", "Glacier", "Black", "Platinum", "Black bar", "SP Onyx", "SP Cobalt", "SP Platinum"};
    fs::path play_frames_folder();
    std::vector<std::string> play_frame_choices();
    std::string play_frame_label(const std::string& frame) const;
    void apply_play_view(int fit, int second, const std::string& frame);
    const char* play_fit_name(int fit) const;
    const char* play_second_name(int second) const;
    void play_graphics_action();
    void return_home();
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
    bool return_writes_pending() const;
    void begin_return_controls();
    void poll_return_controls(bool wait = false);
    void flush_return_controls();
    LidSleep lid_sleep;
    bool font_ready = true, font_checked = false;
    void check_fonts(bool first_frame = false);
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
    void gesture_intro();
    Uint32 gesture_flash_at = 0;
    int gesture_flash_kind = 0;
    // The handle at the bottom edge, the Home pill that rises with the finger,
    // the Back disc that slides in from the left, and a ring where one fired.
    void gesture_cues(SDL_Renderer* r, int y);
    bool gestures_available() const;
    // A swipe that started on an edge and travelled far enough: act once, then swallow the rest of the touch.
    bool edge_gesture_move(int x, int y);
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
    void refresh_network(Uint32 now);
    std::string network_id, confirmation;
    std::string startup_mode = "stock";
    bool startup_warning = true;
    Page confirm_return = Page::System;
    Uint32 menu_poll = 0;
    unsigned menu_sequence = 0;
    std::string menu_pending_token;
    Uint32 menu_request_at = 0;
    void confirm(const std::string& operation, Page parent);
    void save_startup();
    void change_startup(int direction, bool toggle);
    void wifi_choose();
    std::string menu_input = "no input";
    void note_menu_input(const SDL_Event& e);
    void menu_request(const std::string& action);
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
    // The filter sheet (filter_sheet.hpp): chips for played state and the facets, over the lower screen.
    struct FilterChip {
        int facet = -1;  // -1: the played state; otherwise a facet_rows() index
        int slider = -1; // 0 players, 1 release year: a range slider instead of a chip
        int section = 0, title_y = 0;
        std::string label, value;
        int count = -1; // games with this value, shown after the label; -1: none
        bool selected = false, more = false;
        SDL_Rect box{};
    };
    struct FilterTitle {
        std::string text;
        int y;
    };
    // A slider's span (the library's own extent) and where its two handles are.
    struct FilterSlider {
        int low_bound = 0, high_bound = 0, low = 0, high = 0;
        const FacetIndex::Histogram* histogram = nullptr;
    };
    struct FilterLayout {
        std::vector<FilterChip> chips;
        std::vector<FilterTitle> titles;
        int height = 0;
        bool loading = false;
    };
    bool filter_sheet_open = false;
    int filter_focus = 0, filter_scroll = 0;
    int filter_thumb = 0;        // which handle of the focused slider the arrow keys move
    int filter_drag = -1;        // the slider a finger is dragging, or -1
    bool filter_touch = false;   // a finger is down on the sheet
    int filter_scroll_start = 0; // the scroll when the finger went down
    bool filter_slider(int slider, FilterSlider& out) const;
    void filter_slider_set(int slider, int low, int high);
    void filter_slider_move(int slider, int thumb, int delta);
    std::string filter_slider_caption(int slider) const;
    bool filter_slider_press(int x, int y);
    void filter_slider_drag(int x);
    void filter_drag_scroll(int y);
    void filter_draw_slider(SDL_Renderer* r, const FilterChip& item, int top, bool focused);
    bool filter_sheet_active() const;
    void open_filter_sheet();
    void reset_filters();
    void facet_picker(int facet);
    FilterLayout filter_layout();
    void filter_scroll_to_focus(const FilterLayout& layout);
    void filter_activate(const FilterChip& chip);
    void filter_sheet_key(int code);
    void filter_sheet_touch(int x, int y);
    void filter_sheet_render(SDL_Renderer* r, int y);
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
    void open_gallery(GalleryMode mode = GalleryMode::Manage);
    void slot_worker(const std::string& action, int slot, const std::string& file = "", bool overwrite = false);
    // Checkpoint slots: the same three verbs on every slot. A does what the page
    // was opened for (save here, load, or export from the library), X deletes,
    // Y holds the rare actions. Anything that replaces progress asks once first,
    // with Cancel selected.
    const char* gallery_title() const;
    SettingsIcon gallery_icon() const;
    bool slot_occupied(int slot) const;
    bool slot_primary_enabled(int slot) const { return gallery_mode == GalleryMode::Save || slot_occupied(slot); }
    const char* slot_primary_name() const;
    const char* slot_hint(bool occupied) const;
    std::string slot_question(const char* verb, int slot) const;
    void slot_primary();
    void slot_delete();
    void slot_import(int slot);
    void slot_more();
    pid_t microphone_worker = -1;
    int microphone_pipe = -1, microphone_level = 0, microphone_peak = 0, microphone_db = -60;
    std::string microphone_bytes, microphone_state;
    Uint32 microphone_update = 0;
    void stop_microphone();
    void open_microphone();
    void poll_microphone(Uint32 now);
    void microphone_screen(SDL_Renderer* r, int y);
    void open_discover() {
        discover_return = settings.page;
        settings.page = Page::Discover;
        settings.focus = 0;
        status.clear();
        dirty = true;
    }
    void launcher_search_shortcut();
    void save_switcher_library();
    void load_switcher();
    bool discovery_active() const { return !search_query.empty() || discovery_filter != 0 || discovery_facets.any(); }
    std::string discovery_filter_name() const;
    // What the list is filtered by, apart from the search words: "Unplayed  ·  Puzzle  ·  2 players".
    std::string discovery_filters_text() const;
    std::string discovery_description() const;
    void apply_discovery();
    void clear_discovery();
    void quick_search();
    static std::string trimmed_query(const std::string& value) {
        auto first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? "" : value.substr(first, last - first + 1);
    }
    void search_changed();
    void poll_search_preview(Uint32 now);
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
    void ds_controls_row(SDL_Renderer* r, int y, int i);
    void ds_controls_action();
    void save_ds_setting(const std::string& key, int value) { save_ds_setting(key, std::to_string(value)); }
    void save_ds_setting(const std::string& key, const std::string& value);
    bool scan_stay = false; // a scan from Add your games keeps that page open
    void rescan_games(bool stay = false);
    // Add your games: opened by itself on a first start (Done then goes to the
    // library) or from Library settings (Done goes back there). A scan from the
    // page stays on it and reports what it found.
    bool setup_first_run = false, setup_scanned = false;
    bool backup_requested = false; // Back up all saves was chosen; its outcome shows on the Library page
    void confirm_reset_state();
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
    void watch_menu(Uint32 now);
    void start_worker(const std::string& operation, const std::string& payload = "");
    void stop_worker();
    Uint32 worker_finished_at = 0;
    void poll_service(Uint32 now);
    void select_game(size_t index, bool animate = true);
    void navigate(int delta);
    // A held Left or Right steps one game at a time; from 2.4 s on its stride
    // doubles every 600 ms, so a card of thousands of games is crossed in a
    // few seconds. The run stops at either end instead of wrapping round.
    static int hold_stride(unsigned held, size_t count) {
        if (held < 2400 || count < 24) return 1;
        const int cap = int(std::clamp<size_t>(count / 40, 1, 128));
        return std::min(cap, 1 << std::min(7u, 1 + (held - 2400) / 600));
    }
    void hold_navigate(int direction, unsigned held);
    void library_view(int view);
    void poll_collection();
    void open_game_options();
    bool apply_game_preferences(const GameOptions& next, const char* message);
    void choose_game_emulator(int core);
    void change_game_option(int delta, bool toggle = false);
    std::string shader_name(int value);
    bool experimental_game() const;
    std::string experimental_resolution_name() const;
    void show_experimental_resolution_picker();
    bool superdrastic_game() const;
    int superdrastic_resolution() const;
    const char* superdrastic_resolution_name() const;
    void show_superdrastic_resolution_picker();
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
    void open_game_artwork(Page back = Page::GameOptions);
    bool save_artwork_match(const std::string& title);
    void review_artwork();
    void game_artwork_action();
    void open_scraper() {
        settings.section = 2;
        settings.page = Page::Scraper;
        settings.focus = 0;
        if (worker <= 0) start_worker("status");
        service_loaded = true;
        dirty = true;
    }
    void begin_edit(int field);
    void finish_edit(bool commit);
    void move_edit_cursor(int direction);
    void insert_text(const std::string& value);
    void erase_character();
    void delete_character();
    void scraper_action();
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
    std::map<fs::path, std::optional<std::string>> read_theme_source();
    // The selected light/dark palette with the optional DS favourite colour.
    void select_theme();
    void reload_themes();
    void remember_appearance();
    // Theme & display uses the shared settings list, four roomy rows per
    // screen: 0 colour mode, 1 accent colour, 2 high contrast, 3 typeface,
    // 4 icon style, 5 screen brightness, 6 animations, 7 touch indicator.
    // The two-choice rows show both options in place (tap a side, Left/Right,
    // or A to swap).
    using SegmentNames = std::array<const char*, 2>;
    static constexpr SegmentNames mode_names{"Light", "Dark"}, typeface_names{"DS rounded", "Pixel classic"};
    SDL_Rect segment_bounds(int index, int option);
    void segment_row(SDL_Renderer* r, int y, int index, const std::string& title, bool second);
    void appearance_row(SDL_Renderer* r, int y, int i);
    static std::string accent_name(int index) {
        return index >= 0 && index < int(ds_colours.size()) ? ds_colours[size_t(index)].name : "Classic";
    }
    void accent_swatch(SDL_Renderer* r, int cx, int cy, int radius, Color fill);
    // Off, an open share that appears in Finder/Explorer like a NAS, or the
    // previous one-time password login. Both shares are advertised by name.
    void show_sharing_picker();
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
    void choose_appearance(int choice);
    void refresh_system();
    void save_preview_volume(int value);
    int confirmed_volume();
    void poll_volume();
    void flush_volume();
    void toggle_mute();
    void change_volume(int value);
    void apply_timezone();
    // UTC offsets per zone, read from tzdata by switching TZ for a moment and
    // putting the chosen zone back. Only rows on screen ask, and the cache
    // empties every quarter hour so a daylight-saving change shows up.
    std::map<std::string, long> zone_offsets;
    long zone_offsets_bucket = -1;
    long zone_offset(const std::string& zone);
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
    std::string zone_time(const std::string& zone);
    std::string timezone_summary();
    std::string timezone_title();
    // The regions L and R (and the d-pad's Left and Right) jump to; empty at either end.
    std::pair<std::string, std::string> timezone_neighbours();
    void load_system_configuration();
    void save_date_format(int value);
    void save_time_format(int value);
    void save_timezone();
    bool apply_control(int focus, const SystemPreferences& value);
    void poll_brightness();
    void flush_brightness();
    void change_system(int focus, int delta, bool cycle = false);
    void reset_after_sleep();
    void restore_sleep_audio() { configure_sounds(); }
    void sleep_now(bool lid = false);
    void watch_system(Uint32 now);
    void watch_theme(Uint32 now);
    void restore_state();
    void save_state();
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
    void sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength = 12);
    void horizontal_sheen(SDL_Renderer* r, int x, int y, int w, int h, const std::string& key, int strength = 12);
    void triangle(SDL_Renderer* r, int x, int y, int half, int height, const std::string& key, bool down = true);
    void battery_icon(SDL_Renderer* r, int y);
    fs::path status_asset(const std::string& name);
    std::string wifi_asset();
    std::string sound_asset() { return hardware.volume && hardware.volume_percent == 0 ? "muted" : "sound"; }
    // Covers are being fetched: a small spinner and the count beside the
    // brand, on every screen, until the artwork worker reports it is done.
    bool artwork_busy() const;
    int loader_phase(Uint32 now) const { return preferences.animations ? int((now / 110) % 8) : 0; }
    void artwork_loader(SDL_Renderer* r, int y);
    int drawn_loader_phase = -1;
    static int pixel_scale(int size) { return std::max(1, (size + 4) / 10); }
    // Classic-text width in 6-dot cells; kana and kanji take two.
    static int pixel_cells(const std::string& value, int limit = 1000) {
        int cells = 0;
        for (auto cp : RoundedFont::codepoints(value, limit)) cells += pixel_wide(cp) ? 2 : 1;
        return cells;
    }
    int label_width(const std::string& source, int size, bool translate = true);
    std::string status_stamp();
    // The label y that puts this text's ink (capitals and digits) on `cy`.
    int centred_label_y(const std::string& value, int size, int cy);
    int status_label_y(const std::string& value, int size, int y) { return centred_label_y(value, size, y + 24); }
    void status_bar(SDL_Renderer* r, int y);
    int point_size(int size) { return size == 1 ? 16 : size == 2 ? 20 : size == 3 ? 28 : 36; }
    // Classic 5x7 text as filled squares: `fill` takes each dot's rectangle and
    // `wide` each kana or kanji cell (the caller draws those from the rounded
    // font). `y` is the line box's top, as for the rounded font.
    template <class Fill, class Wide>
    void pixel_dots(const std::string& value, int x, int y, int size, int max_chars, Fill&& fill, Wide&& wide) {
        y += size * 3; // Match the rounded font's visible top, rather than its line-box top.
        int n = 0;
        for (unsigned c : RoundedFont::codepoints(value)) {
            if (++n > max_chars) break;
            if (pixel_wide(c)) {
                wide(c, x, y);
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
                auto dot = [&](int col, int row) { fill(SDL_Rect{x + col * size, y + row * size, size, size}); };
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
                auto dot = [&](int col, int row) { fill(SDL_Rect{x + col * size, y + row * size, size, size}); };
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
                        fill(SDL_Rect{x + col * size, y + row * size, size, size});
                    }
                }
            x += size * 6;
        }
    }
    void text(SDL_Renderer* r, std::string value, int x, int y, int size, const std::string& key, int max_chars = 80,
              bool translate = true);
    void centered(SDL_Renderer* r, const std::string& source, int y, int size, const std::string& key, int limit = 48);
    void label(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, const std::string& key,
               int width = 560, bool translate = true);
    void raw_label(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, const std::string& key,
                   int width = 560);
    void raw_wrapped(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, int width, int rows = 3);
    // The DSi's START frame around a selected tile: one accent body in one
    // continuous shade, the tile set into it and the word centred in the
    // space below over a soft engraved shadow.
    void start_frame(SDL_Renderer* r, int x, int row, int tile, const std::string& word, int band = 45);
    std::string category_title(size_t index);
    void category_icon(SDL_Renderer* r, size_t index, int x, int y, int size);

    void settings_footer(SDL_Renderer* r, int y);
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
    void ui_icon(SDL_Renderer* r, SettingsIcon symbol, int x, int y, int size, const std::string& key);
    void settings_card(SDL_Renderer* r, int x, int y, int w, int h, bool focused = false);
    void list_row(SDL_Renderer* r, int y, int index, const std::string& title, const std::string& value = "",
                  bool enabled = true, const Color* swatch = nullptr, bool switches = true);
    void draw_toggle(SDL_Renderer* r, int x, int y, const std::string& name, bool value, bool enabled = true);
    void list_scrollbar(SDL_Renderer* r, int y);
    // The vertical rail after the horizontal one: a recessed groove with a
    // pip per page, a glass window under the thumb, and a glossy thumb with
    // an engraved grip that widens while it is held.
    // The vertical rail: a quiet slot with a recessed groove down its middle
    // and a glossy thumb with an engraved grip. Nothing changes on hover;
    // dragging only adds a soft accent glow, so the thumb never jumps.
    void draw_scrollbar(SDL_Renderer* r, int x, int y, int height, int thumb, int offset, bool dragging, bool hover,
                        int width = 36);
    void round_rect(SDL_Renderer* r, int x, int y, int w, int h, int radius, const std::string& key);
    // Explicit colours (DS colour tiles) reuse the themed drawing paths.
    void fill_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill);
    void label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width);
    void raw_label_colour(SDL_Renderer* r, const std::string& value, int x, int y, int pixels, Color ink, int width);
    void check_mark(SDL_Renderer* r, int cx, int cy, Color ink);
    void ring(SDL_Renderer* r, int x, int y, int radius, const std::string& key);
    void icon(SDL_Renderer* r, int kind, int x, int y, const std::string& key);
    void gear(SDL_Renderer* r, int x, int y);
    void choose_browser_layout() {
        show_picker("Browser layout", {"Cartridges", "Covers", "List"}, preferences.layout, [this](int n) {
            preferences.layout = n;
            cancel_navigation();
            remember_appearance();
        });
    }
    void choose_library_view();
    // Three sections of three equal tiles with room between them.
    static SDL_Rect library_view_choice(int index) {
        return {40 + (index % 3) * 191, 120 + (index / 3) * 100, 178, 52};
    }
    void library_choice_icon(SDL_Renderer* r, int index, int x, int y, int size, const std::string& ink);
    static std::string initial_letter(const std::string& title) {
        auto first = title.find_first_not_of(" ");
        if (first == std::string::npos) return "#";
        unsigned char c = title[first];
        if (c >= 'a' && c <= 'z') c -= 32;
        return c >= 'A' && c <= 'Z' ? std::string(1, char(c)) : "#";
    }
    // The displayed title when it is already known; otherwise the file name's.
    // Opening the sheet never reads metadata for every cartridge on the card.
    std::string jump_title(size_t index);
    void jump_to_letter();
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
    void cart(SDL_Renderer* r, int x, int y, int width, int height, bool selected, int number);
    // The clock as the user chose it: 24-hour, or 12-hour with AM/PM.
    std::string time_text(const std::tm& t) const;
    std::string clock();
    fs::path folder_for_game(const Game& game) { return library_cache.folder(game); }
    fs::path item_folder(size_t index) { return folder_for_game(model.games[index]); }
    std::string game_title(size_t index);
    void warm_icon(SDL_Renderer* r, const Game& game);
    void warm_icon(SDL_Renderer* r, size_t index) { warm_icon(r, model.games[index]); }
    // The same icon, read and decoded by the loader thread; it appears on the
    // frame after it arrives, so browsing never waits on the card.
    void request_icon(SDL_Renderer* r, const Game& game);
    void prepare_render_assets();
    // Covers near the selection load in the background, nearest first; cached
    // misses are remembered too, so missing artwork is never asked for twice.
    void prepare_next_cover();
    void watch_library_assets(Uint32 now);
    void pattern(SDL_Renderer* r, int y, int height, bool lower = false);
    void game_icon(SDL_Renderer* r, const Game& game, int x, int y, int size, size_t index = 0);
    void game_icon(SDL_Renderer* r, size_t index, int x, int y, int size);
    bool cover_art(SDL_Renderer* r, const fs::path& path, SDL_Rect box, bool selected);
    void wrapped(SDL_Renderer* r, const std::string& source, int x, int y, int pixels, int width, int rows = 3,
                 bool translate = true);
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
    void shutdown_overlay(SDL_Renderer* r, int y, bool upper);
    void brand(SDL_Renderer* r, int x, int y, int size);
    bool boot_animating(Uint32) const;
    double boot_phase() const { return preferences.animations ? ((s.GetTicks() - boot_scene_at) % 2400) / 2400.0 : 0; }
    void boot_caption(SDL_Renderer* r, const std::string& value, int y, int pixels, const std::string& key);
    void boot_emblem(SDL_Renderer* r, int y);
    void boot_wordmark(SDL_Renderer* r, int y, int);
    void boot_scene(SDL_Renderer* r, int y);
    void boot_top(SDL_Renderer* r, int y);
    void boot_stylus(SDL_Renderer* r, int x, int y);
    void boot_bottom(SDL_Renderer* r, int y);
    // A read-only games card breaks saves, volume and launch at once. Explain
    // the automatic check plainly instead of letting every action fail.
    int card_repair = 0;
    bool card_repair_dismissed = false;
    Uint32 card_check_at = 0, card_repair_at = 0;
    void check_card(Uint32 now, bool force = false);
    void start_card_repair(Uint32 now);
    void card_top(SDL_Renderer* r, int y);
    void card_bottom(SDL_Renderer* r, int y);
    void top(SDL_Renderer* r, int y);
    // Word-wrapped text centred on the screen, at most `rows` lines.
    void centered_wrapped(SDL_Renderer* r, const std::string& value, int y, int pixels, int width, int rows,
                          const std::string& key);
    // PictoChat's page on Home's top screen: what it is and whether Wi-Fi is up.
    void pictochat_top(SDL_Renderer* r, int y);
    // A heart badge for favourites, on the cover stage and on the tiles below.
    bool stage_favourite = false;
    bool game_loved(size_t index);
    void favourite_badge(SDL_Renderer* r, int cx, int cy, int size);
    // A small two-screens mark for games RG DS Play gives a second screen of
    // their own (a map, the team, the HUD; launcher/dual_screen.hpp).
    bool stage_dual = false;
    bool game_dual(size_t index);
    void dual_badge(SDL_Renderer* r, int x, int y);
    // Without a cover: the cartridge's own DS icon, large and crisp on the
    // same card as covers, with its title (the only place it is not repeated).
    void icon_stage(SDL_Renderer* r, int y, const std::string& developer);
    // Translucent rounded fill without overlapping strips (blending stays even).
    void alpha_round(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, int alpha);
    // DSi / GammaOS-style cover stage: the box art alone, large and centred on
    // a soft shadow and a white card edge. The title already sits in the lower
    // screen's speech bubble. A new selection settles in over 190 ms.
    bool cover_stage(SDL_Renderer* r, int y, const fs::path& path);
    // A shoulder button seen from the front, as the DSi draws L and R: a flat
    // tab with small corners, whose outer top corner sweeps down in a deep
    // quarter ellipse. `right` puts the sweep on the right (R). Blended at `alpha`.
    void shoulder_fill(SDL_Renderer* r, int x, int y, int w, int h, bool right, Color fill, int alpha = 255);
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
    int key_badge_width(const std::string& key);
    int key_badge(SDL_Renderer* r, int x, int cy, const std::string& key, bool enabled = true, bool inverse = false);
    // A key group ("A", "Start", "L R"): its badges side by side.
    int key_group(SDL_Renderer* r, int x, int cy, const std::string& keys, bool enabled = true, bool inverse = false);
    int key_hints_width(const std::vector<KeyHint>& hints, int size = 16);
    // A row of tips from x (align -1), centred on x (0) or ending at x (1),
    // centred on cy; long translations step the text down to fit `limit`.
    int key_hints(SDL_Renderer* r, int x, int cy, const std::vector<KeyHint>& hints, int align = -1, int size = 16,
                  int limit = 584, const std::string& ink = "muted", bool inverse = false);
    // The DSi's note on the top screen: a panel card with an accent edge, the
    // page's icon and a title; the tip sits under it (settings_top draws it).
    // `y` places the icon and title; `screen` is the top screen's own top.
    void dsi_note(SDL_Renderer* r, int y, int screen, SettingsIcon symbol, const std::string& title);
    // A shoulder tab in a lower corner of the top screen, flush with its
    // edge: the key badge on the outer end, where the button is, then what it does.
    void shoulder_tab(SDL_Renderer* r, int y, bool right, const std::string& key, const std::string& text);
    std::string system_row_name(int row) const;
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
                      int alpha = 255, int weight = 2);
    bool options_hold() const;
    bool touch_animating(Uint32 now) const;
    void touch_feedback(SDL_Renderer* r, int y);
    double launch_opacity(Uint32 now) const;
    void loading_overlay(SDL_Renderer* r, int y, bool lower);
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
    void glossy_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color base, double press = 0);
    Color shadow_colour() const { return preferences.dark ? Color{0, 0, 0} : Color{24, 40, 60}; }
    // A raised control: soft shadow, then the glass body, sunk 2 px when pressed.
    void raised_pill(SDL_Renderer* r, int x, int y, int w, int h, int radius, Color fill, double press = 0,
                     bool shadow = true);
    // A footer or sheet button: a raised glass pill (accent when primary) with an
    // optional round key badge ("B", "A") and a label that sinks while pressed.
    void footer_button(SDL_Renderer* r, int x, int y, int w, const std::string& text, const std::string& badge,
                       bool primary, bool pressed, Color* tint = nullptr);
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
    double update_fraction();
    // Speed and time left while bytes arrive, e.g. "1.8 MB/s · About 2 min left".
    std::string update_speed();
    // The bar's shown value eases toward the reported one between status polls.
    double update_shown = 0;
    Uint32 update_shown_at = 0, glitter_frame_at = 0;
    int drawn_banner_frame = -1;
    bool glitter_was = false;
    void ease_update(Uint32 now);
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
    void blend_rect(SDL_Renderer* r, int x, int y, int w, int h, Color c, int alpha);
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
                     bool pressed = false, const std::string& key = "Y", bool translate = true);
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
    // Down from the last row walks onto the bottom bar: 0 brightness, 1 the
    // update mark (only while one is waiting), 2 the clock. -1 is the tiles.
    int home_bar_focus = -1;
    bool home_column_move(int code);
    void home_bar_activate(int button);
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
    void paint_bottom(SDL_Renderer* renderer, int y);
    void animated_bottom(SDL_Renderer* renderer, int y);
    void push_bottom(SDL_Renderer* renderer, int y);
    void draw(bool screenshot = false);
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
    void launch(bool from_home = false);
    // ---- PictoChat: our own chat pad. Its rules and state live in launcher/pictochat_app.hpp;
    // this is its drawing and its hooks into the launcher's input. It uses the launcher's one keyboard,
    // and it needs no BIOS, firmware or file from Nintendo.
    struct ChatGlyphs : pictochat::GlyphSource {
        App& app;
        RoundedFont& font;
        ChatGlyphs(App& a, RoundedFont& f) : app(a), font(f) {}
        // Message text follows the Typeface setting: smooth is the rounded font,
        // pixel the launcher's 5x7 dots at 2x (12 px a letter, like 24 px rounded).
        static constexpr int pixel_size = 2;
        // A missing font file must not take the launcher down: the text is simply not drawn.
        void draw(const std::string& t, int x, int y, const std::function<void(int, int, uint8_t)>& put) override {
            if (app.preferences.pixel) {
                app.pixel_dots(
                    t, x, y, pixel_size, 1000,
                    [&](const SDL_Rect& p) {
                        for (int dy = 0; dy < p.h; dy++)
                            for (int dx = 0; dx < p.w; dx++) put(p.x + dx, p.y + dy, 255);
                    },
                    [&](unsigned c, int cell_x, int baseline_y) {
                        try {
                            const auto glyph = utf8_encode(c);
                            const int points = pixel_size * 10;
                            const int w = int(std::lround(font.width(glyph, points)));
                            font.rasterize(glyph, points, cell_x + pixel_size * 6 - w / 2,
                                           baseline_y + pixel_size * 7 - font.ascent(points), put);
                        } catch (const std::exception&) {}
                    });
                return;
            }
            try {
                font.rasterize(t, 24, x, y, put);
            } catch (const std::exception&) {}
        }
        int width(const std::string& t) override {
            if (app.preferences.pixel) return pixel_cells(t) * pixel_size * 6;
            try {
                return int(std::lround(font.width(t, 24)));
            } catch (const std::exception&) {
                return int(RoundedFont::codepoints(t).size()) * 12;
            }
        }
    };
    pictochat::Chat chat;
    ChatGlyphs chat_glyphs{*this, rounded};
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
    void begin_launch_exit(bool handoff, Uint32 now);
    void cancel_launch();
    void finish_launch(bool reopen);
    void start_launch_runner(Uint32 now);
    void poll_launch(Uint32 now);
    void stop_launch();
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
    int run();
};

// Out-of-line App members, split by screen.
#include "launcher/settings_screen.hpp"
#include "launcher/settings_picker.hpp"
#include "launcher/input_handling.hpp"
#include "launcher/status_bar.hpp"
#include "launcher/ui_widgets.hpp"
#include "launcher/library_pickers.hpp"
#include "launcher/render_assets.hpp"
#include "launcher/key_hints_pills.hpp"
#include "launcher/update_progress.hpp"
#include "launcher/frame_draw.hpp"
#include "launcher/game_launch.hpp"
#include "launcher/view_transitions.hpp"
#include "launcher/menu_sounds.hpp"
#include "launcher/library_boot.hpp"
#include "launcher/systems_play.hpp"
#include "launcher/return_gestures.hpp"
#include "launcher/network_menu.hpp"
#include "launcher/gallery_slots.hpp"
#include "launcher/microphone.hpp"
#include "launcher/switcher_search.hpp"
#include "launcher/game_settings.hpp"
#include "launcher/worker_service.hpp"
#include "launcher/library_navigation.hpp"
#include "launcher/game_preferences.hpp"
#include "launcher/artwork_actions.hpp"
#include "launcher/text_edit.hpp"
#include "launcher/appearance.hpp"
#include "launcher/system_settings.hpp"
#include "launcher/state_persistence.hpp"
#include "launcher/boot_screens.hpp"
#include "launcher/run_loop.hpp"
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
#include "launcher/filter_sheet.hpp"

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
