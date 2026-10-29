#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "touch.hpp"
#include "frame_rate.hpp"
#include "dsperate_state.hpp"
#include <dlfcn.h>
#include <dirent.h>
#include <sys/resource.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <vector>
#include <algorithm>
#include <array>
#include <sstream>
#include <tuple>
#include "platform/emulator_overlay.hpp"
#include "platform/game_shader.hpp"
#include "platform/game_input.hpp"
#include "platform/control_mapping.hpp"
#include "platform/mainline_controls.hpp"
#include "platform/wayland_role.hpp"
#include "platform/alsa_mixer.hpp"
#include "platform/haptics.hpp"
#include <sys/file.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#include <cstdarg>
#endif
#include <atomic>
#include <mutex>
#include <thread>

extern char** environ;

namespace fs = std::filesystem;
void shader_result(bool applied);
namespace {
std::map<SDL_Window*, std::string> panel_roles;
bool independent_wayland() {
    const char* mode = std::getenv("RGDS_SESSION_PLATFORM");
    const char* video = std::getenv("SDL_VIDEODRIVER");
    return mode && std::strcmp(mode, "independent") == 0 && video && std::strcmp(video, "wayland") == 0;
}
using Stamp = std::array<long long, 5>; // mtime, ctime, size, inode, device
std::string core, control, autostate, state_dir, state_slot, token, last_token;
bool initialized = false, autosave = true, saving = false, quitting = false, failed = false, autoload = false;
bool atomic_state_replace = false;
unsigned long long opened = 0, save_at = 0, pulse_at = 0, candidate_at = 0, quit_at = 0;
int pulse = 0;
SDL_JoystickID joystick = 0;
std::map<std::string, Stamp> before;
std::string candidate;
Stamp candidate_stamp{};
std::deque<SDL_Event> events;
std::deque<SDL_Event> releases;
unsigned long long release_at = 0;
bool shared_menu = false, loading = false;
// Read by the present hooks, which DSCore runs on its present thread.
std::atomic<bool> enhanced_game_texture{false};
std::atomic<int> live_shader{0};
// DSCore's HD 2D (rgds_2d=4): the GPU reconstruction runs in the screen draw, under the live shader or alone.
std::atomic<bool> hd_live{false};
// Bridge state changes happen on the thread that polls SDL events. A present
// on another thread reads the atomics above and posts a shader outcome here.
std::atomic<std::thread::id> event_thread{};
std::atomic<int> shader_outcome{0}; // 1 applied, 2 unavailable, from another thread
std::atomic<bool> blocked_shared{false};
bool on_event_thread() {
    auto id = event_thread.load(std::memory_order_relaxed);
    return id == std::thread::id{} || id == std::this_thread::get_id();
}
int previous_shader = 0;
std::atomic<bool> shader_waiting{false};
// The engine presents through rgds_present_canvas (DSCore's direct present):
// no SDL renderer draw carries the frame, so no GL filter pass can replace
// it. A filter chosen now is saved and takes effect on the next launch,
// which the launcher plans with the renderer sink.
std::atomic<bool> direct_present{false};
unsigned long long shader_requested_at = 0;
rgds::EmulatorOverlay overlay_renderer;
rgds::FrameCounterRate game_rate;
rgds::FrameStats frame_stats;
std::atomic<bool> stats_resumed{false};
std::mutex stats_mutex;
void* stats_source = nullptr;
std::string stats_path;
rgds::GameShader game_shader;
rgds::ControlMapping control_mapping;
rgds::ControlShortcut control_shortcut;
std::string controls_global, controls_local;
unsigned long long controls_poll_at = 0;
std::array<long long, 8> controls_stamp{};
std::deque<std::pair<SDL_Event, unsigned long long>> delayed_start_events;
std::map<SDL_JoystickID, std::array<bool, 16>> event_sources;
std::map<SDL_JoystickID, Uint8> event_hats;
std::map<std::pair<SDL_JoystickID, int>, bool> event_thresholds;
std::map<std::tuple<Uint32, SDL_JoystickID, int>, SDL_Event> delivered_controls;
void release_controls();
void graphics_texture_changed(bool enhanced);
void write_cheats(bool force = false);
void release_touch_after_resume();
void remember_control_event(const SDL_Event& event) {
    Uint32 kind = event.type;
    SDL_JoystickID device = 0;
    int code = 0;
    bool down = false;
    if (kind == SDL_JOYBUTTONDOWN || kind == SDL_JOYBUTTONUP) {
        if (event.jbutton.button >= 16 && event.jbutton.button <= 18) return;
        device = event.jbutton.which;
        code = event.jbutton.button;
        down = kind == SDL_JOYBUTTONDOWN;
        kind = SDL_JOYBUTTONDOWN;
    } else if (kind == SDL_CONTROLLERBUTTONDOWN || kind == SDL_CONTROLLERBUTTONUP) {
        device = event.cbutton.which;
        code = event.cbutton.button;
        down = kind == SDL_CONTROLLERBUTTONDOWN;
        kind = SDL_CONTROLLERBUTTONDOWN;
    } else if (kind == SDL_JOYHATMOTION) {
        device = event.jhat.which;
        code = event.jhat.hat;
        down = event.jhat.value != 0;
    } else if (kind == SDL_CONTROLLERAXISMOTION) {
        device = event.caxis.which;
        code = event.caxis.axis;
        down = event.caxis.value != 0;
    } else return;
    auto key = std::make_tuple(kind, device, code);
    if (down) delivered_controls[key] = event;
    else delivered_controls.erase(key);
}
void release_controls() {
    for (auto& held : delivered_controls) {
        auto event = held.second;
        if (event.type == SDL_JOYBUTTONDOWN) {
            event.type = SDL_JOYBUTTONUP;
            event.jbutton.state = SDL_RELEASED;
        } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            event.type = SDL_CONTROLLERBUTTONUP;
            event.cbutton.state = SDL_RELEASED;
        } else if (event.type == SDL_JOYHATMOTION) event.jhat.value = SDL_HAT_CENTERED;
        else if (event.type == SDL_CONTROLLERAXISMOTION) event.caxis.value = 0;
        events.push_back(event);
    }
    delivered_controls.clear();
    event_sources.clear();
    event_hats.clear();
    event_thresholds.clear();
    delayed_start_events.clear();
}
thread_local bool presenting_renderer = false;
thread_local bool drawing_shader = false;
thread_local bool reading_native_event = false, reading_native_buttons = false;
struct NativeInputGuard {
    bool& value;
    bool before;
    explicit NativeInputGuard(bool& field) : value(field), before(field) { value = true; }
    ~NativeInputGuard() { value = before; }
};
bool fast_forward = false, pulse_consumed = false;
unsigned long long ds_lid_reopen = 0, ds_mic_release = 0;
std::string speed_token;
unsigned long long load_at = 0;
unsigned long long command_poll_at = 0, stamp_poll_at = 0, block_poll_at = 0;
bool blocked_cache = false;
unsigned long long volume_poll_at = 0;
int watched_volume = -1;
std::string volume_token;
std::string volume_data, block_path;
// Game rumble (rgds_rumble): the strength the launcher passed (RGDS_RUMBLE,
// 0 off), read again from ui.ini each time the game comes back from the
// in-game menu, where it can be changed. -1 until first read.
std::atomic<int> rumble_strength{-1};
int saved_rumble_strength();
struct FileIdentity {
    dev_t device{};
    ino_t inode{};
    off_t size{};
    timespec modified{}, changed{};
    bool operator==(const FileIdentity& other) const {
        return device == other.device && inode == other.inode && size == other.size &&
               modified.tv_sec == other.modified.tv_sec && modified.tv_nsec == other.modified.tv_nsec &&
               changed.tv_sec == other.changed.tv_sec && changed.tv_nsec == other.changed.tv_nsec;
    }
};
struct SmallFile {
    std::string path, value;
    FileIdentity identity{};
    bool present = false, known = false;
    bool refresh() {
        struct stat info{};
        RGDS_PROFILE(file_stats, 1);
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            bool changed = present;
            present = false;
            known = true;
            value.clear();
            return changed;
        }
#ifdef __APPLE__
        FileIdentity next{info.st_dev, info.st_ino, info.st_size, info.st_mtimespec, info.st_ctimespec};
#else
        FileIdentity next{info.st_dev, info.st_ino, info.st_size, info.st_mtim, info.st_ctim};
#endif
        if (known && present && next == identity) return false;
        // Session commands/status are atomically replaced; only changed files
        // need parsing. Failed reads remain retryable at the next poll.
        if (info.st_size < 0 || info.st_size > 4096) return false;
        std::ifstream stream(path);
        std::array<char, 4096> buffer{};
        RGDS_PROFILE(file_reads, 1);
        stream.read(buffer.data(), buffer.size());
        if (stream.bad() || (!stream.eof() && stream.fail())) return false;
        value.assign(buffer.data(), size_t(stream.gcount()));
        identity = next;
        present = known = true;
        return true;
    }
};
SmallFile command_file, volume_file, hud_file, resume_file, speed_request;
double resume_monotonic = 0, resume_realtime = 0;
struct NativeTexture {
    SDL_Renderer* renderer = nullptr;
    bool game = false;
    int applied = -1;
};
std::map<SDL_Texture*, NativeTexture> native_textures;
std::mutex textures_mutex; // native_textures and renderer_contexts
void graphics_texture_changed(bool enhanced) {
    std::lock_guard<std::mutex> guard(textures_mutex);
    enhanced_game_texture = enhanced;
    native_textures.clear();
}

std::map<SDL_Renderer*, SDL_GLContext> renderer_contexts;
void forget_native_renderer(SDL_Renderer* renderer) {
    std::lock_guard<std::mutex> guard(textures_mutex);
    for (auto item = native_textures.begin(); item != native_textures.end();)
        if (item->second.renderer == renderer) item = native_textures.erase(item);
        else ++item;
}
void update_resume() {
    if (!resume_file.refresh()) return;
    resume_monotonic = resume_realtime = 0;
    std::istringstream fields(resume_file.value);
    std::string line;
    while (std::getline(fields, line)) {
        if (line.rfind("monotonic=", 0) == 0) resume_monotonic = std::atof(line.c_str() + 10);
        else if (line.rfind("realtime=", 0) == 0) resume_realtime = std::atof(line.c_str() + 9);
    }
    if (resume_monotonic > 0 || resume_realtime > 0) {
        // SIGSTOP may precede the next gate poll, so no blocked edge was seen.
        // Rebase on the presenting thread: a stopped process cannot sample pause.
        stats_resumed = true;
        release_controls();
        release_touch_after_resume();
        if (!volume_data.empty()) rumble_strength = saved_rumble_strength();
        control_shortcut.menu_pending = control_shortcut.tap = control_shortcut.start_seen = false;
        control_shortcut.latched = control_shortcut.start || control_shortcut.menu;
    }
}
// runtime/audio.py `sync` without a Python process (~0.5 s each, two or three
// per engine start and one per menu resume): reapply the saved level to the
// DAC under the runtime's volume lock. No saved level yet: the helper decides.
bool sync_saved_volume(const char* data) {
#ifdef __linux__
    int lock = open((std::string(data) + "/session/volume.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) return false;
    while (flock(lock, LOCK_EX) < 0 && errno == EINTR) {}
    int level = -1;
    bool schema = false;
    std::ifstream file(std::string(data) + "/volume.ini");
    std::string line;
    while (std::getline(file, line)) {
        if (line == "schema=1") schema = true;
        else if (line.rfind("volume=", 0) == 0) {
            char* end = nullptr;
            long value = std::strtol(line.c_str() + 7, &end, 10);
            if (end && !*end && value >= 0 && value <= 100) level = int(value);
        }
    }
    bool ok = schema && level >= 0 && rgds::write_saved_level(level);
    flock(lock, LOCK_UN);
    close(lock);
    return ok;
#else
    (void)data;
    return false;
#endif
}
bool sync_volume() {
    const char *helper = std::getenv("RGDS_VOLUME_HELPER"), *data = std::getenv("RGDS_VOLUME_DATA");
    if (!helper || !data) return true;
    static thread_local bool active = false;
    if (active) return true;
    if (sync_saved_volume(data)) return true;
    active = true;
    std::vector<char*> environment;
    for (char** item = environ; *item; item++)
        if (std::strncmp(*item, "LD_PRELOAD=", 11) != 0 && std::strncmp(*item, "LD_LIBRARY_PATH=", 16) != 0)
            environment.push_back(*item);
    environment.push_back(nullptr);
    pid_t child = fork();
    int status = 1;
    if (child == 0) {
        char* arguments[] = {const_cast<char*>("python3"),
                             const_cast<char*>("-B"),
                             const_cast<char*>(helper),
                             const_cast<char*>("sync"),
                             const_cast<char*>("--data"),
                             const_cast<char*>(data),
                             nullptr};
        execve("/usr/bin/python3", arguments, environment.data());
        _exit(127);
    }
    if (child > 0) {
        int result = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &result, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == child && WIFEXITED(result)) status = WEXITSTATUS(result);
    }
    active = false;
    if (status != 0) {
        std::fprintf(stderr, "RGDS: audio held muted; volume synchronization failed\n");
        std::fflush(stderr);
    }
    return status == 0;
}
#ifdef RGDS_BRIDGE_TESTS
unsigned long long test_time = 1;
unsigned long long test_clock_step = 0;
unsigned long long now() {
    test_time += test_clock_step;
    return test_time;
}
#else
unsigned long long now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<unsigned long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
#endif
std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
extern "C" int SDL_Init(Uint32 flags) {
    using Fn = int (*)(Uint32);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_Init"));
    int result = next ? next(flags) : -1;
    if (result == 0 && (flags & SDL_INIT_VIDEO) &&
        (env("RGDS_SESSION_PLATFORM") == "independent" || env("RGDS_DEVICE_MODEL") == "rg-ds-plus") &&
        env("RGDS_CORE") == "dsperate") {
        auto count = rgds::overlay_sdl<decltype(&SDL_GetNumVideoDisplays)>("SDL_GetNumVideoDisplays");
        auto name = rgds::overlay_sdl<decltype(&SDL_GetDisplayName)>("SDL_GetDisplayName");
        int top = -1, bottom = -1;
        if (count && name)
            for (int i = 0; i < count(); i++) {
                const char* label = name(i);
                if (!label) continue;
                if (env("RGDS_TOP_DISPLAY") == label || env("RGDS_TOP_DISPLAY") == std::to_string(i)) top = i;
                if (env("RGDS_BOTTOM_DISPLAY") == label || env("RGDS_BOTTOM_DISPLAY") == std::to_string(i)) bottom = i;
            }
        if (top >= 0 && bottom >= 0 && top != bottom) {
            std::string mapping = "upper=" + std::to_string(top) + ",lower=" + std::to_string(bottom);
            setenv("DS_DUAL_SCREENS", mapping.c_str(), 1);
        }
    }
    return result;
}
// ROCKNIX's joypad driver on RG DS OS (BananaLinux's kernel) reports mainline BTN_* codes with the F key as
// BTN_MODE (MainlineControls::from_joypad). The stock firmware's driver has the same name but its own numbering, so
// only the independent session reads it as mainline.
bool rocknix_joypad(const char* label) {
    return label && std::strcmp(label, "retrogame_joypad") == 0 && env("RGDS_SESSION_PLATFORM") == "independent";
}
extern "C" SDL_bool SDL_IsGameController(int index) {
    using Fn = SDL_bool (*)(int);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_IsGameController"));
    if (env("RGDS_SESSION_PLATFORM") == "independent" || env("RGDS_DEVICE_MODEL") == "rg-ds-plus") {
        auto name = rgds::overlay_sdl<decltype(&SDL_JoystickNameForIndex)>("SDL_JoystickNameForIndex");
        auto guid = rgds::overlay_sdl<decltype(&SDL_JoystickGetDeviceGUID)>("SDL_JoystickGetDeviceGUID");
        auto text = rgds::overlay_sdl<decltype(&SDL_JoystickGetGUIDString)>("SDL_JoystickGetGUIDString");
        auto add = rgds::overlay_sdl<decltype(&SDL_GameControllerAddMapping)>("SDL_GameControllerAddMapping");
        const char* label = name ? name(index) : nullptr;
        if (label && guid && text && add &&
            (std::strstr(label, "retrogame_joypad") || std::strstr(label, "ANBERNIC-rk3568") ||
             std::strcmp(label, "gpio-keys-control") == 0)) {
            static std::map<std::string, bool> configured;
            char identifier[33]{};
            text(guid(index), identifier, sizeof(identifier));
            if (!configured[identifier]) {
                std::string faces = "a:b0,b:b1,x:b3,y:b2";
                std::string mapping =
                    std::string(identifier) + ",RGDS console," + faces +
                    ",back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpleft:h0.8,dpdown:h0.4,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Linux,";
                if (std::strcmp(label, "gpio-keys-control") == 0) {
                    faces = "a:b1,b:b0,x:b2,y:b3";
                    mapping =
                        std::string(identifier) + ",RGDS mainline," + faces +
                        ",back:b8,guide:b16,start:b9,leftstick:b10,rightstick:b11,leftshoulder:b4,rightshoulder:b5,dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,lefttrigger:b6,righttrigger:b7,platform:Linux,";
                } else if (rocknix_joypad(label)) {
                    faces = "a:b1,b:b0,x:b2,y:b3";
                    mapping =
                        std::string(identifier) + ",RGDS mainline joypad," + faces +
                        ",back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,lefttrigger:b6,righttrigger:b7,platform:Linux,";
                }
                configured[identifier] = add(mapping.c_str()) >= 0;
            }
        }
    }
    return next ? next(index) : SDL_FALSE;
}
// Keep the stock DraStic touch adapter, otherwise use the native SDL input API.
// Menu/volume clients are owned by RGDS and never enter another UI controller.
void* input_symbol(const char* name) {
    if (shared_menu) {
        static void* touch = []() {
            auto path = std::getenv("RGDS_TOUCH_LIBRARY");
            return path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
        }();
        if (touch)
            if (auto symbol = dlsym(touch, name)) return symbol;
        static void* vendor = []() {
            auto path = std::getenv("RGDS_SDL_LIBRARY");
            return path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
        }();
        if (vendor)
            if (auto symbol = dlsym(vendor, name)) return symbol;
    }
    return dlsym(RTLD_NEXT, name);
}
const char* joystick_label(SDL_Joystick* stick) {
    static auto name = reinterpret_cast<const char* (*)(SDL_Joystick*)>(dlsym(RTLD_NEXT, "SDL_JoystickName"));
    return name ? name(stick) : nullptr;
}
bool joypad_joystick(SDL_Joystick* stick) { return rocknix_joypad(joystick_label(stick)); }
bool mainline_joystick(SDL_Joystick* stick) {
    const char* label = joystick_label(stick);
    return label && (std::strcmp(label, "gpio-keys-control") == 0 || rocknix_joypad(label));
}
// MainlineControls numbers buttons as gpio-keys does; the joypad's own index for one of them.
int mainline_index(SDL_Joystick* stick, int raw) {
    return raw >= 0 && joypad_joystick(stick) ? rgds::MainlineControls::to_joypad(raw) : raw;
}
rgds::MainlineControls mainline_controls;
bool normalize_joystick_event(SDL_Event& event) {
    if (event.type == SDL_JOYDEVICEREMOVED) {
        mainline_controls.directions.erase(event.jdevice.which);
        mainline_controls.joypads.erase(event.jdevice.which);
        return true;
    }
    if (event.type != SDL_JOYBUTTONDOWN && event.type != SDL_JOYBUTTONUP) return true;
    static auto from_id =
        reinterpret_cast<SDL_Joystick* (*)(SDL_JoystickID)>(dlsym(RTLD_NEXT, "SDL_JoystickFromInstanceID"));
    auto stick = from_id ? from_id(event.jbutton.which) : nullptr;
    if (stick && joypad_joystick(stick)) mainline_controls.joypads.insert(event.jbutton.which);
    return !stick || !mainline_joystick(stick) || mainline_controls.normalize(event);
}
int saved_rumble_strength() {
    int strength = std::clamp(std::atoi(env("RGDS_RUMBLE").c_str()), 0, 100);
    std::ifstream ui(volume_data + "/ui.ini");
    bool on = strength > 0;
    for (std::string line; std::getline(ui, line);)
        if (line == "game_rumble=0" || line == "game_rumble=1") on = line.back() == '1';
        else if (line.rfind("rumble_strength=", 0) == 0) strength = std::clamp(std::atoi(line.c_str() + 16), 10, 100);
    return on ? strength : 0;
}
void initialize() {
    if (initialized) return;
    initialized = true;
    core = env("RGDS_CORE");
    control = env("RGDS_CONTROL_FILE");
    autostate = env("RGDS_AUTOSTATE_FILE");
    state_dir = env("RGDS_STATE_DIR");
    state_slot = env("RGDS_STATE_SLOT");
    autosave = env("RGDS_AUTOSAVE") != "0";
    autoload = env("RGDS_AUTOLOAD") == "1";
    shared_menu = env("RGDS_SHARED_MENU") == "1";
    enhanced_game_texture = env("RGDS_OWN_FF_HUD") == "1" && std::atoi(env("RGDS_ACTIVE_SCALE").c_str()) > 1;
    live_shader = std::clamp(std::atoi(env("RGDS_SHADER").c_str()), 0, rgds::shader_count - 1);
    hd_live = env("RGDS_OWN_FF_HUD") == "1" && (env("RGDS_2D_FILTER") == "4" || env("RGDS_2D_FILTER") == "5");
    game_shader.hd_smooth = env("RGDS_2D_FILTER") == "5";
    if (hd_live)
        std::fprintf(stderr, "RGDS: HD 2D%s on (filter %d)\n", game_shader.hd_smooth ? " smooth" : "",
                     live_shader.load());
    opened = now();
    atomic_state_replace = core == "dsperate" && !state_slot.empty() && env("RGDS_STATE_ATOMIC_REPLACE") == "1";
    frame_stats.enabled = env("RGDS_FPS") == "1";
    stats_path = env("RGDS_FRAME_STATS");
    controls_global = env("RGDS_CONTROLS_GLOBAL");
    controls_local = env("RGDS_CONTROLS_FILE");
    control_mapping.load(controls_global, controls_local);
    control_shortcut.mode = control_mapping.shortcut;
    control_shortcut.action = control_mapping.shortcut_action;
    volume_data = env("RGDS_VOLUME_DATA");
    block_path = control + ".blocked";
    command_file.path = control;
    resume_file.path = control + ".resume";
    volume_file.path = volume_data + "/volume.ini";
    hud_file.path = volume_data + "/session/volume-hud.ini";
}
bool input_blocked() {
    if (!shared_menu || control.empty()) return false;
    if (!on_event_thread()) return blocked_shared.load(std::memory_order_relaxed);
    auto time = now();
    if (time - block_poll_at >= 20 || !block_poll_at) {
        block_poll_at = time;
        RGDS_PROFILE(block_checks, 1);
        bool blocked = access(block_path.c_str(), F_OK) == 0;
        if (blocked && !blocked_cache) {
            release_controls();
            write_cheats(true);
        }
        blocked_cache = blocked;
        blocked_shared = blocked;
        update_resume();
    }
    return blocked_cache;
}
void shortcut_action(rgds::ShortcutAction action) {
    if (action == rgds::ShortcutAction::none || env("RGDS_MENU_EXTERNAL") == "1" || !shared_menu || control.empty())
        return;
    std::ofstream file(control + (action == rgds::ShortcutAction::home ? ".home" : ".menu"));
    file << now() << '\n';
}
void request_game_menu() {
    if (env("RGDS_MENU_EXTERNAL") == "1" || !shared_menu || control.empty()) return;
    std::ofstream file(control + ".game-menu");
    file << now() << '\n';
}
void observe_shortcut(int source, bool down) {
    bool latched = control_shortcut.latched;
    auto action = control_shortcut.feed(source, down, now());
    if ((control_shortcut.latched && !latched) || action != rgds::ShortcutAction::none) release_controls();
    shortcut_action(action);
    if (control_shortcut.latched) delayed_start_events.clear();
}
void refresh_controls() {
    auto time = now();
    if (controls_poll_at && time - controls_poll_at < 250) return;
    controls_poll_at = time;
    std::array<long long, 8> next{};
    int at = 0;
    for (auto& path : {controls_global, controls_local}) {
        struct stat info{};
        if (!path.empty() && stat(path.c_str(), &info) == 0) {
#ifdef __APPLE__
            auto modified = info.st_mtimespec;
            auto changed = info.st_ctimespec;
#else
            auto modified = info.st_mtim;
            auto changed = info.st_ctim;
#endif
            next[at] = info.st_ino;
            next[at + 1] = info.st_size;
            next[at + 2] = modified.tv_sec * 1000000000LL + modified.tv_nsec;
            next[at + 3] = changed.tv_sec * 1000000000LL + changed.tv_nsec;
        }
        at += 4;
    }
    if (next != controls_stamp || !control_mapping.preset.size()) {
        release_controls();
        control_mapping.load(controls_global, controls_local);
        controls_stamp = next;
        control_shortcut.mode = control_mapping.shortcut;
        control_shortcut.action = control_mapping.shortcut_action;
    }
}
int controller_button(int source) {
    constexpr int buttons[]{0, 1, 2, 3, 9, 10, 6, 4, 11, 12, 13, 14, -1, -1, 7, 8};
    if (source < 0 || source >= 16) return -1;
    return buttons[source];
}
int controller_source(int button) {
    for (int source = 0; source < 16; source++)
        if (controller_button(source) == button) return source;
    return button == SDL_CONTROLLER_BUTTON_GUIDE ? rgds::control_menu : -1;
}
int logical_joystick(int button) {
    if (core == "drastic" && button >= 20 && button <= 23) return button - 12;
    return rgds::joystick_source(button);
}
int destination_joystick(int logical) {
    return logical >= 8 && logical <= 11 && core == "drastic" ? logical + 12 : rgds::physical_button(logical);
}
// RGDS_LATENCY_TRACE=FILE (os/banana-probe/game_latency.py): for each button press, CLOCK_MONOTONIC microseconds
// when the engine first read it down and of the next two presents (the frame being shown and the one that read it).
struct LatencyTrace {
    std::FILE* out = nullptr;
    std::mutex mutex;
    uint64_t seen = 0, first = 0;
    void* panel = nullptr; // presents are counted on one panel (each frame presents once per panel)
    bool held[16]{};
    static uint64_t clock_us() {
        timespec time{};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return uint64_t(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
    }
    LatencyTrace() {
        if (const char* path = std::getenv("RGDS_LATENCY_TRACE")) out = std::fopen(path, "a");
    }
    void button(int source, bool down) {
        if (!out || source < 0 || source >= 16) return;
        std::lock_guard<std::mutex> guard(mutex);
        if (down && !held[source] && !seen) seen = clock_us();
        held[source] = down;
    }
    // A button-down event the engine takes from SDL_PollEvent (DSCore reads its controller that way).
    void event(const SDL_Event& event) {
        if (!out) return;
        const bool press = event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_JOYBUTTONDOWN ||
                           (event.type == SDL_JOYHATMOTION && event.jhat.value != 0);
        if (!press) return;
        std::lock_guard<std::mutex> guard(mutex);
        if (!seen) seen = clock_us();
    }
    void present(void* source) {
        if (!out) return;
        std::lock_guard<std::mutex> guard(mutex);
        if (!panel) panel = source;
        if (!seen || source != panel) return;
        if (!first) {
            first = clock_us();
            return;
        }
        std::fprintf(out, "%llu %llu %llu\n", (unsigned long long)seen, (unsigned long long)first,
                     (unsigned long long)clock_us());
        std::fflush(out);
        seen = first = 0;
    }
} latency_trace;
bool joystick_source_down(SDL_Joystick* stick, int source) {
    static auto button = reinterpret_cast<Uint8 (*)(SDL_Joystick*, int)>(input_symbol("SDL_JoystickGetButton"));
    static auto hat = reinterpret_cast<Uint8 (*)(SDL_Joystick*, int)>(input_symbol("SDL_JoystickGetHat"));
    bool down = false;
    {
        NativeInputGuard guard(reading_native_buttons);
        if (mainline_joystick(stick)) {
            int raw = mainline_index(stick, rgds::MainlineControls::raw_button(source));
            down = button && raw >= 0 && button(stick, raw);
        } else
            down = source >= 8 && source <= 11
                       ? hat && (hat(stick, 0) & rgds::hat_bit(source))
                       : button && rgds::physical_button(source) >= 0 && button(stick, rgds::physical_button(source));
    }
    latency_trace.button(source, down);
    if (source == rgds::control_start) {
        observe_shortcut(source, down);
        return control_shortcut.visible_start(now());
    }
    return down;
}
bool controller_source_down(SDL_GameController* pad, int source) {
    static auto button = reinterpret_cast<Uint8 (*)(SDL_GameController*, SDL_GameControllerButton)>(
        input_symbol("SDL_GameControllerGetButton"));
    static auto axis = reinterpret_cast<Sint16 (*)(SDL_GameController*, SDL_GameControllerAxis)>(
        input_symbol("SDL_GameControllerGetAxis"));
    bool down = false;
    {
        NativeInputGuard guard(reading_native_buttons);
        down = source == 12 || source == 13 ? axis && axis(pad, source == 12 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
                                                                             : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000
                                            : button && controller_button(source) >= 0 &&
                                                  button(pad, SDL_GameControllerButton(controller_button(source)));
    }
    latency_trace.button(source, down);
    if (source == rgds::control_start) {
        observe_shortcut(source, down);
        return control_shortcut.visible_start(now());
    }
    return down;
}
bool remap_event(SDL_Event& event) {
    if (event.type == SDL_CONTROLLERAXISMOTION &&
        (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        int source = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 12 : 13,
            logical = control_mapping.logical_for(source);
        if (source == control_mapping.fast_forward_source) return false;
        if (logical < 0) return true;
        auto key = std::make_pair(event.caxis.which, source);
        bool down = event.caxis.value > 16000;
        if (event_thresholds[key] == down) return false;
        event_thresholds[key] = down;
        SDL_Event mapped{};
        mapped.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        mapped.cbutton.which = event.caxis.which;
        mapped.cbutton.timestamp = event.caxis.timestamp;
        mapped.cbutton.button = Uint8(controller_button(logical));
        mapped.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
        event = mapped;
        return true;
    }
    bool joystick_button = event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP;
    bool controller_button_event = event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP;
    if (joystick_button || controller_button_event) {
        int source =
            joystick_button ? rgds::joystick_source(event.jbutton.button) : controller_source(event.cbutton.button);
        if (source == control_mapping.fast_forward_source) return false;
        if (source == rgds::control_menu) return false;
        auto& held = event_sources[joystick_button ? event.jbutton.which : event.cbutton.which];
        if (source >= 0 && source < 16)
            held[source] = joystick_button ? event.jbutton.state != 0 : event.cbutton.state != 0;
        int logical = control_mapping.logical_for(source);
        if (logical < 0) return source < 0 || source >= 12;
        int target = joystick_button ? destination_joystick(logical) : controller_button(logical);
        if (target < 0) {
            if (joystick_button && logical >= 8 && logical <= 11) {
                Uint8 hat = 0;
                for (int direction = 8; direction < 12; direction++) {
                    int assigned = control_mapping.source_for(direction);
                    if (assigned >= 0 && assigned < 16 && held[assigned]) hat |= rgds::hat_bit(direction);
                }
                event.type = SDL_JOYHATMOTION;
                event.jhat.hat = 0;
                event.jhat.value = hat;
                return true;
            }
            return false;
        }
        if (joystick_button) event.jbutton.button = Uint8(target);
        else event.cbutton.button = Uint8(target);
        return true;
    }
    if (event.type == SDL_JOYHATMOTION) {
        auto old = event_hats[event.jhat.which];
        event_hats[event.jhat.which] = event.jhat.value;
        auto& held = event_sources[event.jhat.which];
        for (int source = 8; source < 12; source++) held[source] = (event.jhat.value & rgds::hat_bit(source)) != 0;
        Uint8 mapped = 0;
        for (int logical = 8; logical < 12; logical++) {
            int assigned = control_mapping.source_for(logical);
            if (assigned >= 0 && assigned < 16 && held[assigned]) mapped |= rgds::hat_bit(logical);
        }
        bool native_hat_route = false;
        for (int source = 8; source < 12; source++) {
            int logical = control_mapping.logical_for(source);
            int bit = rgds::hat_bit(source);
            bool down = (event.jhat.value & bit) != 0;
            if (logical >= 8 && logical <= 11) native_hat_route = true;
            if (!(logical >= 8 && logical <= 11 && core != "drastic") && logical >= 0 && bool(old & bit) != down) {
                SDL_Event emitted{};
                emitted.type = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
                emitted.jbutton.which = event.jhat.which;
                emitted.jbutton.button = Uint8(destination_joystick(logical));
                emitted.jbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
                events.push_back(emitted);
            }
        }
        event.jhat.value = mapped;
        return core != "drastic" && (native_hat_route || mapped != 0);
    }
    return true;
}
bool defer_start(const SDL_Event& event) {
    int source = -1;
    bool down = false;
    if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) {
        source = rgds::joystick_source(event.jbutton.button);
        down = event.type == SDL_JOYBUTTONDOWN;
    } else if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
        source = controller_source(event.cbutton.button);
        down = event.type == SDL_CONTROLLERBUTTONDOWN;
    }
    if (source != rgds::control_start) return false;
    observe_shortcut(source, down);
    if (control_shortcut.mode == "anbernic") return false;
    if (!control_shortcut.latched)
        delayed_start_events.emplace_back(event, down ? now() + 150
                                                 : now() < control_shortcut.started + 150
                                                     ? control_shortcut.started + 310
                                                     : now());
    return true;
}
void volume_feedback(unsigned long long time) {
    if (volume_data.empty() || (volume_poll_at && time - volume_poll_at < 80)) return;
    volume_poll_at = time;
    bool hud_changed = hud_file.refresh(), volume_changed = volume_file.refresh();
    if (!hud_changed && !volume_changed) return;
    int level = -1;
    std::string event;
    auto read = [&](const std::string& value, bool hud) {
        std::istringstream file(value);
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("volume=", 0) == 0) {
                char* end = nullptr;
                long parsed = std::strtol(line.c_str() + 7, &end, 10);
                if (end && !*end && parsed >= 0 && parsed <= 100) level = int(parsed);
            } else if (hud && line.rfind("token=", 0) == 0) event = line.substr(6);
        }
    };
    read(hud_file.value, true);
    read(volume_file.value, false);
    bool requested = !event.empty() && event != volume_token;
    if (level >= 0 && (requested || (watched_volume >= 0 && level != watched_volume))) overlay_renderer.volume(level);
    if (level >= 0) watched_volume = level;
    volume_token = event;
}
bool game_input(const SDL_Event& e) {
    return (e.type >= SDL_KEYDOWN && e.type <= SDL_TEXTINPUT) ||
           (e.type >= SDL_MOUSEMOTION && e.type <= SDL_MOUSEWHEEL) ||
           (e.type >= SDL_JOYAXISMOTION && e.type <= SDL_JOYBUTTONUP) ||
           (e.type >= SDL_CONTROLLERAXISMOTION && e.type <= SDL_CONTROLLERBUTTONUP) ||
           (e.type >= SDL_FINGERDOWN && e.type <= SDL_FINGERMOTION);
}
bool menu_event(const SDL_Event& e) {
    bool down = e.type == SDL_KEYDOWN || e.type == SDL_JOYBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONDOWN;
    bool match = ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) &&
                  (e.key.keysym.sym == SDLK_AC_HOME || e.key.keysym.sym == SDLK_HOME ||
                   e.key.keysym.sym == SDLK_AC_BACK || e.key.keysym.sym == SDLK_MENU)) ||
                 ((e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) && e.jbutton.button == 8) ||
                 ((e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) &&
                  e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE);
    if (match) {
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            if (down && !e.key.repeat) request_game_menu();
        } else observe_shortcut(rgds::control_menu, down);
    }
    return match;
}
std::map<std::string, Stamp> stamps() {
    std::map<std::string, Stamp> result;
    std::error_code ec;
    if (!state_slot.empty()) {
        struct stat st{};
        RGDS_PROFILE(state_stats, 1);
        if (stat(state_slot.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
#ifdef __APPLE__
            auto modified = st.st_mtimespec;
            auto changed = st.st_ctimespec;
#else
            auto modified = st.st_mtim;
            auto changed = st.st_ctim;
#endif
            result[state_slot] = {static_cast<long long>(modified.tv_sec) * 1000000000 + modified.tv_nsec,
                                  static_cast<long long>(changed.tv_sec) * 1000000000 + changed.tv_nsec, st.st_size,
                                  static_cast<long long>(st.st_ino), static_cast<long long>(st.st_dev)};
        }
        return result;
    }
    if (state_dir.empty() || !fs::is_directory(state_dir, ec)) return result;
    for (auto& entry : fs::directory_iterator(state_dir, ec)) {
        auto path = entry.path();
        auto ext = path.extension().string();
        struct stat st{};
        RGDS_PROFILE(state_stats, 1);
        if ((ext == ".dss" || ext == ".sta") && stat(path.c_str(), &st) == 0 && st.st_size > 0) {
#ifdef __APPLE__
            auto modified = st.st_mtimespec;
            auto changed = st.st_ctimespec;
#else
            auto modified = st.st_mtim;
            auto changed = st.st_ctim;
#endif
            result[path.string()] = {static_cast<long long>(modified.tv_sec) * 1000000000 + modified.tv_nsec,
                                     static_cast<long long>(changed.tv_sec) * 1000000000 + changed.tv_nsec, st.st_size,
                                     static_cast<long long>(st.st_ino), static_cast<long long>(st.st_dev)};
        }
    }
    return result;
}
void answer(const char* state) {
    if (!control.empty()) {
        std::string path = control + ".result", pending = path + ".tmp";
        std::ofstream file(pending);
        file << "token=" << token << "\nstate=" << state << "\n";
        file.close();
        rename(pending.c_str(), path.c_str());
    }
    std::fprintf(stderr, "RGDS bridge: %s\n", state);
    std::fflush(stderr);
}
void inject(int action) {
    pulse = action;
    pulse_consumed = false;
    pulse_at = now();
    release_at = pulse_at + 160;
    if (core == "drastic") {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
            e.jbutton.which = joystick;
            e.jbutton.button = action == 1 ? 16 : action == 2 ? 17 : action == 4 ? 19 : 18;
            e.jbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
            (action == 4 ? events : down ? events : releases).push_back(e);
        }
    } else {
        SDL_Keycode key = action == 1 ? SDLK_F5 : action == 4 ? SDLK_TAB : SDLK_F7;
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.keysym.sym = key;
            e.key.keysym.scancode = action == 1 ? SDL_SCANCODE_F5 : action == 4 ? SDL_SCANCODE_TAB : SDL_SCANCODE_F7;
            e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            (action == 4 ? events : down ? events : releases).push_back(e);
        }
    }
}
bool set_fast_forward(bool enabled) {
    if (core != "dsperate" && core != "drastic" && core != "melonds" && core != "rgds-play") return false;
    if (enabled != fast_forward) {
        inject(4);
        fast_forward = enabled;
    }
    // DSCore removed its own OSD, and RG DS Play never had one: the badge is ours for both.
    overlay_renderer.fast_forward(fast_forward && (env("RGDS_OWN_FF_HUD") == "1" || core == "rgds-play"));
    if (!control.empty()) {
        auto path = control + ".speed-state", pending = path + ".pending";
        std::ofstream file(pending);
        file << "active=" << fast_forward << '\n';
        file.close();
        rename(pending.c_str(), path.c_str());
    }
    return true;
}
void quit_events(bool checkpoint_saved = false) {
    if (core == "drastic") inject(3);
    SDL_Event e{};
    e.type = SDL_QUIT;
    events.push_back(e);
    quit_at = now();
    answer(checkpoint_saved ? "saved" : "quitting");
}
void start_save(bool exit_after) {
    if (saving) return;
    before = stamps();
    candidate.clear();
    saving = true;
    quitting = exit_after;
    failed = false;
    save_at = now();
    inject(1);
    answer("saving");
}
void request_quit() {
    if (quitting || failed) return;
    if (autosave) start_save(true);
    else {
        quitting = true;
        quit_events();
    }
}
std::error_code copy_state(const std::string& source, const std::string& target) {
    std::error_code error;
    fs::create_directories(fs::path(target).parent_path(), error);
    if (error) return error;
    fs::copy_file(source, target + ".pending", fs::copy_options::overwrite_existing, error);
    if (error) return error;
    int fd = open((target + ".pending").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::error_code(errno, std::generic_category());
    int flushed = fsync(fd), cause = errno;
    close(fd);
    if (flushed < 0) return std::error_code(cause, std::generic_category());
    fs::rename(target + ".pending", target, error);
    if (error) return error;
    // Flush the rename too: a checkpoint must survive a clean power cycle.
    int directory = open(fs::path(target).parent_path().c_str(), O_RDONLY | O_CLOEXEC);
    if (directory < 0) return std::error_code(errno, std::generic_category());
    flushed = fsync(directory);
    cause = errno;
    close(directory);
    return flushed < 0 ? std::error_code(cause, std::generic_category()) : std::error_code{};
}
void start_load() {
    std::error_code error;
    if (autostate.empty() || state_slot.empty() || !fs::is_regular_file(autostate, error)) {
        answer("load-failed");
        return;
    }
    error = copy_state(autostate, state_slot);
    if (error) {
        answer("load-failed");
        return;
    }
    if (core == "melonds") {
        auto load = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "rgds_load_checkpoint"));
        answer(load && load() ? "loaded" : "load-failed");
        return;
    }
    loading = true;
    load_at = now();
    inject(2);
    answer("load-requested");
}
unsigned long long elapsed(unsigned long long time, unsigned long long since) {
    return time >= since ? time - since : 0;
}
bool ds_control(bool blow) {
    if (core == "melonds") {
        auto apply = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_ds_control"));
        return apply && apply(blow ? 0 : 1);
    }
    if (core != "dsperate") return false;
    auto key = blow ? SDLK_m : SDLK_l;
    auto scan = blow ? SDL_SCANCODE_M : SDL_SCANCODE_L;
    for (bool down : {true, false}) {
        SDL_Event e{};
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.keysym.sym = key;
        e.key.keysym.scancode = scan;
        e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        if (blow && !down) {
            ds_mic_release = now() + 2000;
        } else events.push_back(e);
    }
    if (!blow) ds_lid_reopen = now() + 2000;
    return true;
}
std::string pending_graphics_token;
int pending_graphics_scale = 1, pending_graphics_smooth = 1, pending_graphics_skip = 0;
std::string pending_filter2d_token;
int pending_filter2d_mode = 0;
// Cheats (DSCore): the Action Replay codes the engine loaded for this cart,
// written to <control>.cheats for the game menu when the menu opens and after
// each toggle, so the menu lists them without asking the engine itself. The
// slow poll catches the cart (and its codes) arriving after the first ticks.
int cheats_listed = -1;
unsigned long long cheats_poll_at = 0;
std::string cheat_text(const char* value) {
    std::string text = value ? value : "";
    for (char& c : text)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
}
void write_cheats(bool force) {
    if (control.empty()) return;
    static auto count_fn = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "rgds_cheat_count"));
    static auto groups_fn = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "rgds_cheat_group_count"));
    static auto entry_fn =
        reinterpret_cast<int (*)(int, const char**, const char**, int*, int*)>(dlsym(RTLD_DEFAULT, "rgds_cheat_entry"));
    static auto group_fn = reinterpret_cast<int (*)(int, const char**, int*)>(dlsym(RTLD_DEFAULT, "rgds_cheat_group"));
    const int count = count_fn && entry_fn ? std::max(0, count_fn()) : 0;
    if (!force && count == cheats_listed) return;
    cheats_listed = count;
    const int groups = count && groups_fn && group_fn ? std::max(0, groups_fn()) : 0;
    std::string path = control + ".cheats", pending = path + ".pending";
    std::ofstream file(pending);
    file << "count=" << count << "\ngroups=" << groups << '\n';
    for (int g = 0; g < groups; g++) {
        const char* name = nullptr;
        int exclusive = 0;
        if (group_fn(g, &name, &exclusive))
            file << "group_" << g << '=' << cheat_text(name) << "\ngroup_" << g << "_exclusive=" << exclusive << '\n';
    }
    for (int i = 0; i < count; i++) {
        const char* name = nullptr;
        const char* description = nullptr;
        int group = -1, state = 0;
        if (entry_fn(i, &name, &description, &group, &state))
            file << "cheat_" << i << '=' << cheat_text(name) << "\ncheat_" << i
                 << "_description=" << cheat_text(description) << "\ncheat_" << i << "_group=" << group << "\ncheat_"
                 << i << "_state=" << state << '\n';
    }
    file.close();
    rename(pending.c_str(), path.c_str());
}
void tick(bool graphics_safe = false) {
    initialize();
    if (core.empty() || !on_event_thread()) return;
    if (int outcome = shader_outcome.exchange(0)) shader_result(outcome == 1);
    auto time = now();
    if (ds_mic_release && time >= ds_mic_release) {
        SDL_Event e{};
        e.type = SDL_KEYUP;
        e.key.keysym.sym = SDLK_m;
        e.key.keysym.scancode = SDL_SCANCODE_M;
        e.key.state = SDL_RELEASED;
        events.push_back(e);
        ds_mic_release = 0;
    }
    if (ds_lid_reopen && time >= ds_lid_reopen) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.keysym.sym = SDLK_l;
            e.key.keysym.scancode = SDL_SCANCODE_L;
            e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            events.push_back(e);
        }
        ds_lid_reopen = 0;
    }
    refresh_controls();
    input_blocked();
    shortcut_action(control_shortcut.poll(time));
    if (control_shortcut.latched) delayed_start_events.clear();
    while (!delayed_start_events.empty() && time >= delayed_start_events.front().second) {
        auto event = delayed_start_events.front().first;
        delayed_start_events.pop_front();
        if (remap_event(event)) events.push_back(event);
    }
    volume_feedback(time);
    if (!releases.empty() && time >= release_at) {
        events.insert(events.end(), releases.begin(), releases.end());
        releases.clear();
    }
    if (!control.empty() && (time - command_poll_at >= 25 || !command_poll_at)) {
        command_poll_at = time;
        if (speed_request.path.empty()) speed_request.path = control + ".speed-request";
        if (speed_request.refresh()) {
            std::istringstream file(speed_request.value);
            std::string line, id, action;
            while (std::getline(file, line)) {
                if (line.rfind("token=", 0) == 0) id = line.substr(6);
                if (line.rfind("action=", 0) == 0) action = line.substr(7);
            }
            if (!id.empty() && id != speed_token) {
                speed_token = id;
                if (action == "fast-forward-off") set_fast_forward(false);
                else if (!input_blocked() && action == "fast-forward-on") set_fast_forward(true);
                else if (!input_blocked() && action == "fast-forward-toggle") set_fast_forward(!fast_forward);
            }
        }
        if (command_file.refresh()) {
            std::istringstream file(command_file.value);
            std::string id, command;
            // The command is the rest of the line: a picture frame's name may hold spaces.
            if (file >> id && std::getline(file >> std::ws, command))
                command.erase(command.find_last_not_of(" \t\r") + 1);
            if (!id.empty() && !command.empty() && id != last_token) {
                last_token = token = id;
                if (command == "ds:blow" || command == "ds:lid")
                    answer(ds_control(command == "ds:blow") ? "ds-applied" : "ds-unavailable");
                else if (command == "save") start_save(false);
                else if (command == "load") start_load();
                else if (command == "save-quit") start_save(true);
                else if (command == "quit") {
                    failed = false;
                    request_quit();
                } else if (command == "fps:0" || command == "fps:1") {
                    std::lock_guard<std::mutex> guard(stats_mutex);
                    frame_stats.enabled = command == "fps:1";
                    frame_stats.reset();
                    game_rate.reset();
                    stats_source = nullptr;
                    overlay_renderer.fps(frame_stats.enabled ? 0 : -1);
                    answer(frame_stats.enabled ? "fps-on" : "fps-off");
                } else if (command == "threaded:0" || command == "threaded:1") {
                    auto apply = core == "melonds"
                                     ? reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_set_threaded_3d"))
                                     : nullptr;
                    bool enabled = command == "threaded:1";
                    answer(apply && apply(enabled) ? (enabled ? "threaded-on" : "threaded-off") : "threaded-failed");
                } else if (command.rfind("graphics:", 0) == 0) {
                    int scale = 0, smooth = 0, skip = 0;
                    char extra = 0;
                    auto apply =
                        env("RGDS_OWN_FF_HUD") == "1"
                            ? reinterpret_cast<int (*)(int, int, int)>(dlsym(RTLD_DEFAULT, "rgds_set_graphics"))
                            : nullptr;
                    bool valid =
                        std::sscanf(command.c_str(), "graphics:%d:%d:%d%c", &scale, &smooth, &skip, &extra) == 3 &&
                        scale >= 1 && scale <= 5 && smooth >= 0 && smooth <= 1 && skip >= 0 && skip <= 3;
                    if (valid && apply) {
                        pending_graphics_token = id;
                        pending_graphics_scale = scale;
                        pending_graphics_smooth = smooth;
                        pending_graphics_skip = skip;
                    } else answer("graphics-failed");
                } else if (command == "adaptive:0" || command == "adaptive:1") {
                    auto apply = env("RGDS_OWN_FF_HUD") == "1"
                                     ? reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_set_adaptive"))
                                     : nullptr;
                    bool on = command == "adaptive:1";
                    answer(apply && apply(on) ? (on ? "adaptive-on" : "adaptive-off") : "adaptive-failed");
                } else if (command.rfind("play-graphics:", 0) == 0) {
                    // RG DS Play: play-graphics:<fit>:<second>:<frame>, a view change the player takes up
                    // between frames (fit 0 sharp, 1 whole pixels, 2 smooth; second 0 companion, 1 off, 2 swap;
                    // frame a built-in style's name or a picture frame's; the player checks it).
                    int fit = -1, second = -1, used = 0;
                    auto apply = core == "rgds-play" ? reinterpret_cast<int (*)(int, int, const char*)>(
                                                           dlsym(RTLD_DEFAULT, "rgds_play_set_graphics"))
                                                     : nullptr;
                    bool valid =
                        std::sscanf(command.c_str(), "play-graphics:%d:%d:%n", &fit, &second, &used) == 2 && used > 0;
                    const std::string frame = valid ? command.substr(size_t(used)) : std::string();
                    valid = valid && !frame.empty() && frame.size() <= 64 && frame.find('/') == std::string::npos;
                    answer(valid && apply && apply(fit, second, frame.c_str()) ? "play-graphics-applied"
                                                                               : "play-graphics-failed");
                } else if (command.rfind("play-color:", 0) == 0) {
                    // RG DS Play: play-color:<palette>:<correction>, indexes into the player's lists
                    // (ui/colour.hpp); the player checks the range and takes it up between frames.
                    int palette = -1, correction = -1;
                    char extra = 0;
                    auto apply = core == "rgds-play"
                                     ? reinterpret_cast<int (*)(int, int)>(dlsym(RTLD_DEFAULT, "rgds_play_set_color"))
                                     : nullptr;
                    const bool valid =
                        std::sscanf(command.c_str(), "play-color:%d:%d%c", &palette, &correction, &extra) == 2;
                    answer(valid && apply && apply(palette, correction) ? "play-color-applied" : "play-color-failed");
                } else if (command.rfind("filter2d:", 0) == 0) {
                    int mode = -1;
                    char extra = 0;
                    auto apply = env("RGDS_OWN_FF_HUD") == "1"
                                     ? reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_set_filter2d"))
                                     : nullptr;
                    bool valid =
                        std::sscanf(command.c_str(), "filter2d:%d%c", &mode, &extra) == 1 && mode >= 0 && mode <= 5;
                    if (valid && apply) {
                        pending_filter2d_token = id;
                        pending_filter2d_mode = mode;
                    } else answer("filter2d-failed");
                } else if (command.rfind("cheat:", 0) == 0) {
                    // cheat:<index>:<0|1>; applied at once (between frames, on this thread) and the list rewritten before the answer.
                    int index = -1, on = -1;
                    char extra = 0;
                    auto apply = reinterpret_cast<int (*)(int, int)>(dlsym(RTLD_DEFAULT, "rgds_set_cheat"));
                    bool valid = std::sscanf(command.c_str(), "cheat:%d:%d%c", &index, &on, &extra) == 2 &&
                                 index >= 0 && (on == 0 || on == 1);
                    if (valid && apply && apply(index, on)) {
                        write_cheats(true);
                        answer(on ? "cheat-on" : "cheat-off");
                    } else answer("cheat-failed");
                } else if (command == "fast-forward") {
                    if (!set_fast_forward(!fast_forward)) answer("fast-forward-unavailable");
                    else answer(fast_forward ? "fast-forward-on" : "fast-forward-off");
                } else if (command.rfind("shader:", 0) == 0) {
                    char* end = nullptr;
                    long value = std::strtol(command.c_str() + 7, &end, 10);
                    int selected = int(value);
                    if (end == command.c_str() + 7 || !end || *end || value < 0 || value >= rgds::shader_count)
                        answer("shader-failed");
                    else if (selected > 0 && direct_present) answer("shader-restart-required");
                    else if (selected >= 0 && selected < rgds::shader_count) {
                        if (!shader_waiting) previous_shader = live_shader;
                        live_shader = selected;
                        shader_requested_at = time;
                        shader_waiting = selected > 0;
                        answer(shader_waiting ? "shader-applying" : "shader-applied");
                    } else answer("shader-failed");
                }
            }
        }
    }
    if (shader_waiting && elapsed(time, shader_requested_at) >= 1500) {
        shader_waiting = false;
        live_shader = previous_shader;
        answer("shader-failed");
    }
    if (!control.empty() && (time - cheats_poll_at >= 1000 || !cheats_poll_at)) {
        cheats_poll_at = time;
        write_cheats();
    }
    if (autoload && time - opened >= 4000 && core != "dsperate") {
        autoload = false;
        inject(2);
        answer("autoload-requested");
    }
    if (loading && elapsed(time, load_at) >= 600) {
        loading = false;
        answer("load-dispatched");
    }
    if (saving && (time - stamp_poll_at >= 50 || !stamp_poll_at)) {
        stamp_poll_at = time;
        auto current = stamps();
        std::string fresh;
        for (auto& item : current)
            if ((state_slot.empty() || item.first == state_slot) &&
                (!before.count(item.first) || before[item.first] != item.second))
                fresh = item.first;
        if (!fresh.empty()) {
            if (candidate != fresh || candidate_stamp != current[fresh]) {
                candidate = fresh;
                candidate_stamp = current[fresh];
                candidate_at = time;
            }
            // Only the verified DSperate writer promises close + atomic rename.
            // A partial .tmp, an old slot, or an in-place modification cannot
            // satisfy that contract. Other engines retain their stability wait.
            struct stat temporary{};
            const bool replaced =
                !before.count(fresh) || before[fresh][3] != current[fresh][3] || before[fresh][4] != current[fresh][4];
            const bool complete = atomic_state_replace && replaced && lstat((fresh + ".tmp").c_str(), &temporary) < 0 &&
                                  errno == ENOENT && rgds::complete_dsperate_state(fresh);
            if (atomic_state_replace ? complete
                                     : (elapsed(time, candidate_at) >= 350 && elapsed(time, save_at) >= 600)) {
                std::error_code error;
                if (!autostate.empty()) error = copy_state(candidate, autostate);
                saving = false;
                if (error) {
                    failed = true;
                    quitting = false;
                    answer("save-failed");
                } else {
                    if (quitting) quit_events(true);
                    else answer("saved");
                }
            }
        }
        if (saving && elapsed(time, save_at) > 7000) {
            saving = false;
            quitting = false;
            failed = true;
            answer("save-timeout");
        }
    }
    // RenderPresent may be between the top and bottom screen. Only the
    // frontend's event loop can replace resources without invalidating a
    // framebuffer pointer that the current presentation still needs.
    if (graphics_safe && !pending_graphics_token.empty()) {
        auto apply = reinterpret_cast<int (*)(int, int, int)>(dlsym(RTLD_DEFAULT, "rgds_set_graphics"));
        token = pending_graphics_token;
        pending_graphics_token.clear();
        bool ok = apply && apply(pending_graphics_scale, pending_graphics_smooth, pending_graphics_skip);
        if (ok) graphics_texture_changed(pending_graphics_scale > 1);
        answer(ok ? "graphics-applied" : "graphics-failed");
    }
    if (graphics_safe && !pending_filter2d_token.empty()) {
        auto apply = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "rgds_set_filter2d"));
        token = pending_filter2d_token;
        pending_filter2d_token.clear();
        const bool ok = apply && apply(pending_filter2d_mode);
        if (ok) {
            game_shader.hd_smooth = pending_filter2d_mode == 5;
            hd_live = pending_filter2d_mode >= 4;
        }
        answer(ok ? "filter2d-applied" : "filter2d-failed");
    }
}
}
__attribute__((constructor)) static void bridge_loaded() {
    std::fprintf(stderr, "RGDS bridge loaded: %s\n", std::getenv("RGDS_CORE") ? std::getenv("RGDS_CORE") : "unknown");
    std::fflush(stderr);
}
// SDL's playback thread wakes once per device period and must refill the
// shared dmix ring before it drains. With a 2048-frame request the period
// equals the whole 2048-frame ring, so every write waits for an empty ring and
// any scheduling delay underruns (audible crackle in busy 3D scenes). Queue-mode
// engines (no callback) keep their own queue, so a 512-frame period only adds
// headroom. The thread also gets a modest nice boost on the busy aux core.
static void boost_audio_threads() {
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) return;
    while (auto* entry = readdir(tasks)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        char name[32]{};
        FILE* file = std::fopen(path, "r");
        if (!file) continue;
        if (!std::fgets(name, sizeof(name), file)) name[0] = 0;
        std::fclose(file);
        if (!std::strncmp(name, "SDLAudioP", 9)) setpriority(PRIO_PROCESS, id_t(std::atoi(entry->d_name)), -10);
    }
    closedir(tasks);
}
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int capture, const SDL_AudioSpec* desired,
                                                 SDL_AudioSpec* obtained, int changes) {
    using Fn = SDL_AudioDeviceID (*)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_OpenAudioDevice"));
    SDL_AudioSpec adjusted{};
    const SDL_AudioSpec* request = desired;
    const char* shared = std::getenv("RGDS_SHARED_AUDIO");
    if (desired && !capture && !desired->callback && desired->samples > 512 && shared && !std::strcmp(shared, "1") &&
        !std::getenv("RGDS_AUDIO_LARGE_PERIOD")) {
        adjusted = *desired;
        adjusted.samples = 512;
        request = &adjusted;
    }
    auto id = next ? next(device, capture, request, obtained, changes) : 0;
    if (id && !capture) {
        sync_volume();
        boost_audio_threads();
    }
    return id;
}
extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int paused) {
    using Fn = void (*)(SDL_AudioDeviceID, int);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_PauseAudioDevice"));
    if (next && (paused || sync_volume())) next(device, paused);
}
extern "C" int snd_pcm_open(void** pcm, const char* name, int stream, int mode) {
    using Fn = int (*)(void**, const char*, int, int);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "snd_pcm_open"));
    int result = next ? next(pcm, name, stream, mode) : -5;
    if (result == 0 && stream == 0 && !sync_volume()) {
        using Close = int (*)(void*);
        auto close = reinterpret_cast<Close>(dlsym(RTLD_NEXT, "snd_pcm_close"));
        if (close) close(*pcm);
        *pcm = nullptr;
        return -5;
    }
    return result;
}
void shader_result(bool applied) {
    if (shader_waiting) {
        shader_waiting = false;
        if (!applied) live_shader = previous_shader;
        answer(applied ? "shader-applied" : "shader-failed");
    } else if (!applied && live_shader) {
        live_shader = 0;
        std::fprintf(stderr, "RGDS: shader unavailable on this renderer; keeping original pixels\n");
        std::fflush(stderr);
    }
}
// Use a single presentation source; dual panels and SDL's nested GL swap must
// never double the displayed FPS. Timing/file publication runs only when enabled.
void measure_present(void* source) {
    latency_trace.present(source);
    if (!frame_stats.enabled) return;
    std::lock_guard<std::mutex> guard(stats_mutex);
    if (!stats_source) stats_source = source;
    if (stats_source != source) return;
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    const auto us = uint64_t(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
    const bool blocked = input_blocked();
    if (stats_resumed.exchange(false)) {
        frame_stats.pause();
        game_rate.reset();
    }
    static auto native_frames = (core == "melonds" || core == "dsperate")
                                    ? reinterpret_cast<uint32_t (*)()>(dlsym(RTLD_DEFAULT, "rgds_emulated_frames"))
                                    : nullptr;
    const bool game_counter = native_frames != nullptr;
    if (game_counter && game_rate.sample(us, native_frames(), blocked)) overlay_renderer.fps(game_rate.fps, true);
    if (frame_stats.present(us, blocked)) {
        if (!game_counter) overlay_renderer.fps(frame_stats.fps());
        frame_stats.write(stats_path, game_counter ? game_rate.fps : -1);
    }
}
// Flush SDL explicitly before binding a native texture for the owned GL draw.
// The effect replaces a native DS copy, before menus/HUD and before swap.
extern "C" SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
    using Fn = decltype(&SDL_CreateWindow);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_CreateWindow"));
    auto* window = next ? next(title, x, y, w, h, flags) : nullptr;
    if (window && independent_wayland() && title && std::strncmp(title, "DSperate", 8) == 0)
        panel_roles[window] = std::strstr(title, "Bottom") ? "org.rgds.bottom" : "org.rgds.top";
    return window;
}
extern "C" SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags) {
    using Fn = decltype(&SDL_CreateRenderer);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_CreateRenderer"));
    auto* renderer = next ? next(window, index, flags) : nullptr;
    auto role = panel_roles.find(window);
    if (renderer && role != panel_roles.end() && !rgds::wayland_role(window, role->second.c_str(), RTLD_NEXT))
        std::fprintf(stderr, "RGDS: could not assign emulator panel to Wayland output\n");
    return renderer;
}
extern "C" void SDL_DestroyWindow(SDL_Window* window) {
    using Fn = decltype(&SDL_DestroyWindow);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_DestroyWindow"));
    panel_roles.erase(window);
    if (next) next(window);
}
extern "C" void SDL_RenderPresent(SDL_Renderer* renderer) {
    using Fn = void (*)(SDL_Renderer*);
    initialize();
    static auto next = shared_menu ? rgds::overlay_sdl<Fn>("SDL_RenderPresent")
                                   : reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_RenderPresent"));
    tick();
    measure_present(renderer);
    overlay_renderer.capture(renderer);
    overlay_renderer.renderer(renderer, input_blocked());
    overlay_renderer.capture(renderer, true);
    presenting_renderer = true;
    if (next) next(renderer);
    presenting_renderer = false;
}
// DSCore's direct present (its Display::draw_scanout_async, RG DS on a
// scanout tier): no SDL renderer or GL swap is in the frame's path, so the
// engine hands each panel's finished frame here instead, before copying it
// into the buffer it page-flips. The same work as the SDL_RenderPresent hook:
// the presentation telemetry, the launcher's capture, the menu/HUD overlay.
// `display` is the panel's SDL display index (0 the top panel), the pixels
// 0xAARRGGBB rows `pitch` words apart. Runs on the engine's present threads.
extern "C" void rgds_present_canvas(int display, uint32_t* pixels, int w, int h, int pitch) {
    initialize();
    if (!pixels || w <= 0 || h <= 0 || pitch < w) return;
    if (!direct_present.exchange(true) && (live_shader || hd_live)) {
        if (live_shader)
            std::fprintf(stderr, "RGDS: shader %d needs the renderer; the direct present shows original pixels\n",
                         live_shader.load());
        if (hd_live) std::fprintf(stderr, "RGDS: HD 2D needs the renderer; the direct present shows plain pixels\n");
        std::fflush(stderr);
        live_shader = 0;
    }
    tick();
    measure_present(reinterpret_cast<void*>(static_cast<uintptr_t>(display) + 1));
    // Which DS screen this display shows: DSCore's upper window is on display 1 (KMSDRM order).
    const int index = rgds::dscore_screens_swapped() ? (display == 1 ? 1 : 0) : (display == 0 ? 1 : 0);
    overlay_renderer.capture_canvas(pixels, w, h, pitch, index, false);
    overlay_renderer.canvas(pixels, w, h, pitch, index, input_blocked());
    overlay_renderer.capture_canvas(pixels, w, h, pitch, index, true);
}
extern "C" void SDL_GL_SwapWindow(SDL_Window* window) {
    using Fn = void (*)(SDL_Window*);
    initialize();
    static auto next = shared_menu ? rgds::overlay_sdl<Fn>("SDL_GL_SwapWindow")
                                   : reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_GL_SwapWindow"));
    tick();
    if (!presenting_renderer) {
        measure_present(window);
        overlay_renderer.capture_gl(window);
        overlay_renderer.gl(window, input_blocked());
    }
    if (next) next(window);
}
void forget_gl(SDL_GLContext context) {
    overlay_renderer.destroy_gl(context);
    game_shader.destroy(context);
    std::lock_guard<std::mutex> guard(textures_mutex);
    for (auto item = renderer_contexts.begin(); item != renderer_contexts.end();)
        if (item->second == context) item = renderer_contexts.erase(item);
        else ++item;
}
extern "C" void SDL_GL_DeleteContext(SDL_GLContext context) {
    using Fn = void (*)(SDL_GLContext);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_GL_DeleteContext"));
    forget_gl(context);
    {
        std::lock_guard<std::mutex> guard(stats_mutex);
        stats_source = nullptr;
        frame_stats.pause();
    }
    if (next) next(context);
}
extern "C" void SDL_DestroyRenderer(SDL_Renderer* renderer) {
    using Fn = void (*)(SDL_Renderer*);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_DestroyRenderer"));
    {
        std::lock_guard<std::mutex> guard(stats_mutex);
        if (stats_source == renderer) {
            stats_source = nullptr;
            frame_stats.pause();
        }
    }
    overlay_renderer.destroy(renderer);
    forget_native_renderer(renderer);
    SDL_GLContext context = nullptr;
    {
        std::lock_guard<std::mutex> guard(textures_mutex);
        auto found = renderer_contexts.find(renderer);
        if (found != renderer_contexts.end()) context = found->second;
    }
    if (context) forget_gl(context);
    if (next) next(renderer);
}
// DSCore's HD 2D: `rules` (an SDL texture, 256x192) carries the CPU's rules for whatever frame `image` holds, so
// the GPU skips its rules pass; null forgets the pair. Called on the thread that draws, before the draw.
// RG DS Play draws its game texture as it is, with no render target, while a
// screen filter is chosen, so the filter's pass can take that draw.
extern "C" __attribute__((visibility("default"))) int rgds_shader_live() { return live_shader.load(); }
// The engine's rumble, 0-100, from its own thread: the DS Rumble Pak in DSCore,
// rumble cartridges in RG DS Play. Silent while the in-game menu is up.
extern "C" __attribute__((visibility("default"))) void rgds_rumble(int level) {
    static rgds::Haptics motor;
    int strength = rumble_strength.load(std::memory_order_relaxed);
    if (strength < 0) {
        initialize();
        rumble_strength = strength = saved_rumble_strength();
    }
    motor.set_strength(0);
    motor.set_rumble_strength(blocked_shared.load(std::memory_order_relaxed) ? 0 : strength);
    motor.rumble(std::clamp(level, 0, 100) / 100.f);
}
extern "C" __attribute__((visibility("default"))) void rgds_hd_rules(SDL_Texture* image, SDL_Texture* rules) {
    game_shader.pair_rules(image, rules);
}
extern "C" void SDL_DestroyTexture(SDL_Texture* texture) {
    using Fn = void (*)(SDL_Texture*);
    static auto next = rgds::overlay_sdl<Fn>("SDL_DestroyTexture");
    {
        std::lock_guard<std::mutex> guard(textures_mutex);
        native_textures.erase(texture);
    }
    game_shader.pair_rules(texture, nullptr);
    if (next) next(texture);
}
bool sdl_shader(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* source, const SDL_Rect* target,
                double angle = 0, const SDL_Point* center = nullptr, SDL_RendererFlip flip = SDL_FLIP_NONE) {
    const bool hd = hd_live;
    if ((!live_shader && !hd) || drawing_shader) return false;
    static auto render_target = rgds::overlay_sdl<decltype(&SDL_GetRenderTarget)>("SDL_GetRenderTarget");
    if (render_target && render_target(renderer)) return false;
    static auto blend = rgds::overlay_sdl<decltype(&SDL_GetTextureBlendMode)>("SDL_GetTextureBlendMode");
    SDL_BlendMode blend_mode = SDL_BLENDMODE_NONE;
    if (blend && blend(texture, &blend_mode) == 0 && blend_mode != SDL_BLENDMODE_NONE &&
        blend_mode != SDL_BLENDMODE_BLEND)
        return false;
    // Only DS screen textures are shader candidates; menus and RGDS textures
    // keep SDL's own blend/pixel path. Remember dimensions until destruction.
    const bool enhanced = enhanced_game_texture;
    {
        std::lock_guard<std::mutex> guard(textures_mutex);
        auto found = native_textures.find(texture);
        if (found == native_textures.end()) {
            static auto query = rgds::overlay_sdl<decltype(&SDL_QueryTexture)>("SDL_QueryTexture");
            int width = 0, height = 0;
            if (!query || query(texture, nullptr, nullptr, &width, &height) != 0) return false;
            // RG DS Play's second screen (the companion, the info screen) is the player's own
            // interface, drawn in the launcher's look: like the launcher, it is never filtered.
            // Only its game picture is (240x160 or 160x144).
            found = native_textures
                        .emplace(texture, NativeTexture{renderer,
                                                        rgds::GameShader::screen_texture(width, height) ||
                                                            (enhanced && width == 640 && height == 480) ||
                                                            (core == "rgds-play" &&
                                                             rgds::GameShader::handheld_texture(width, height)),
                                                        -1})
                        .first;
        }
        if (!found->second.game) return false;
    }
    drawing_shader = true;
    bool applied = game_shader.sdl(renderer, texture, live_shader, source, target, angle, center, flip, nullptr,
                                   enhanced, hd, core == "rgds-play");
    drawing_shader = false;
    static std::atomic<bool> said_declined{false};
    if (hd && !applied && !said_declined.exchange(true))
        std::fprintf(stderr, "RGDS: HD 2D pass declined for a game texture; showing it plain\n");
    if (applied) {
        static auto context = rgds::overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext");
        if (context) {
            std::lock_guard<std::mutex> guard(textures_mutex);
            renderer_contexts[renderer] = context();
        }
    }
    if (on_event_thread()) shader_result(applied);
    else if (shader_waiting || !applied) shader_outcome = applied ? 1 : 2;
    return applied;
}
using CopyTexture = int (*)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
extern "C" int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* source,
                              const SDL_Rect* target) {
    initialize();
    using Fn = CopyTexture;
    static auto copy = rgds::overlay_sdl<Fn>("SDL_RenderCopy");
    if (shared_menu) {
        if (core == "drastic" && !env("RGDS_TOUCH_LIBRARY").empty()) {
            // The vendor adapter records the lower-screen destination during
            // RenderCopy. Bypassing it leaves its touch rectangle null, and its
            // PollEvent dereferences that pointer on the first finger event.
            // Preserve that draw/state update, then apply the selected filter.
            static auto adapter = reinterpret_cast<Fn>(input_symbol("SDL_RenderCopy"));
            const int result = adapter ? adapter(renderer, texture, source, target) : -1;
            if (result == 0) sdl_shader(renderer, texture, source, target);
            return result;
        }
        if (sdl_shader(renderer, texture, source, target)) return 0;
        return copy ? copy(renderer, texture, source, target) : -1;
    }
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_RenderCopy"));
    return next ? next(renderer, texture, source, target) : -1;
}
extern "C" int SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* source,
                                const SDL_Rect* target, double angle, const SDL_Point* center, SDL_RendererFlip flip) {
    initialize();
    using Fn = int (*)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*, double, const SDL_Point*,
                       SDL_RendererFlip);
    static auto copy = rgds::overlay_sdl<Fn>("SDL_RenderCopyEx");
    if (shared_menu) {
        if (sdl_shader(renderer, texture, source, target, angle, center, flip)) return 0;
        return copy ? copy(renderer, texture, source, target, angle, center, flip) : -1;
    }
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_RenderCopyEx"));
    return next ? next(renderer, texture, source, target, angle, center, flip) : -1;
}
// Weston on RG DS does not reliably deliver the Goodix panel to engine
// windows. Use the same calibrated, non-grabbing reader as the launcher.
// Window 0 deliberately selects DSperate's lower panel, including screen swaps.
rgds::TouchDevice ds_touch;
bool ds_touch_started = false, ds_touch_down = false, ds_touch_suppressed = false;
void route_ds_touch(const std::vector<rgds::TouchPoint>& points, bool blocked) {
    auto emit = [](const rgds::TouchPoint& point) {
        SDL_Event event{};
        event.type = point.kind == 0 ? SDL_FINGERDOWN : point.kind == 1 ? SDL_FINGERMOTION : SDL_FINGERUP;
        event.tfinger.touchId = -2;
        event.tfinger.fingerId = 1;
        event.tfinger.windowID = 0;
        event.tfinger.x = float(point.x) / 640;
        event.tfinger.y = float(point.y) / 480;
        event.tfinger.pressure = point.kind < 2 ? 1 : 0;
        events.push_back(event);
    };
    if (blocked && ds_touch_down) {
        emit({2, 0, 0});
        ds_touch_down = false;
        ds_touch_suppressed = true;
    }
    for (const auto& point : points) {
        if (blocked || ds_touch_suppressed) {
            ds_touch_suppressed = point.kind < 2;
            continue;
        }
        if (point.kind < 2 || ds_touch_down) emit(point);
        ds_touch_down = point.kind < 2;
    }
}
void poll_ds_touch() {
    // RG DS Play takes the lower panel's touches for its second screen (dock, boards).
    if (core != "dsperate" && core != "melonds" && core != "rgds-play") return;
    if (!ds_touch_started) {
        ds_touch_started = true;
        auto path = std::getenv("RGDS_TOUCH_DEV");
        ds_touch.open_device(path ? path : "");
    }
    bool blocked = input_blocked();
    route_ds_touch(ds_touch.poll(), blocked);
}
namespace {
void release_touch_after_resume() {
    if (core != "dsperate" && core != "melonds" && core != "rgds-play") return;
    route_ds_touch({}, true);
    ds_touch.discard_until_release();
    ds_touch_suppressed = false;
}
}
extern "C" int SDL_PollEvent(SDL_Event* event) {
    using Fn = int (*)(SDL_Event*);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_PollEvent"));
    if (event_thread.load(std::memory_order_relaxed) == std::thread::id{}) event_thread = std::this_thread::get_id();
    tick(true);
    poll_ds_touch();
    if (event && !events.empty()) {
        *event = events.front();
        events.pop_front();
        remember_control_event(*event);
        latency_trace.event(*event);
        return 1;
    }
    auto native = [&]() {
        NativeInputGuard guard(reading_native_event);
        return next ? next(event) : 0;
    };
    int result = native();
    while (result && event && shared_menu) {
        if (!normalize_joystick_event(*event)) {
            result = native();
            continue;
        }
        if ((ds_touch.available() && ((event->type >= SDL_FINGERDOWN && event->type <= SDL_FINGERMOTION) ||
                                      ((event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) &&
                                       event->button.which == SDL_TOUCH_MOUSEID) ||
                                      (event->type == SDL_MOUSEMOTION && event->motion.which == SDL_TOUCH_MOUSEID))) ||
            menu_event(*event) || (input_blocked() && game_input(*event)) || defer_start(*event) ||
            !remap_event(*event)) {
            result = native();
            continue;
        }
        break;
    }
    if (!result && event && !events.empty()) {
        *event = events.front();
        events.pop_front();
        remember_control_event(*event);
        latency_trace.event(*event);
        return 1;
    }
    if (result && event && event->type == SDL_JOYBUTTONDOWN) joystick = event->jbutton.which;
    if (result && event && autosave &&
        (event->type == SDL_QUIT ||
         (core == "drastic" && event->type == SDL_JOYBUTTONDOWN && event->jbutton.button == 18)) &&
        !quit_at) {
        request_quit();
        return 0;
    }
    if (result && event) remember_control_event(*event);
    latency_trace.event(*event);
    return result;
}
extern "C" Uint8 SDL_JoystickGetButton(SDL_Joystick* stick, int button) {
    using Fn = Uint8 (*)(SDL_Joystick*, int);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_JoystickGetButton"));
    if (reading_native_buttons) return next ? next(stick, button) : 0;
    tick();
    if (shared_menu && button == 8) return 0;
    int logical = logical_joystick(button);
    auto value = input_blocked() ? 0
                 : shared_menu && logical >= 0 && logical < 12
                     ? Uint8(joystick_source_down(stick, control_mapping.source_for(logical)))
                 : next ? next(stick, button)
                        : 0;
    if (shared_menu && !input_blocked() && mainline_joystick(stick) && !(logical >= 0 && logical < 12)) {
        int raw = mainline_index(stick, rgds::MainlineControls::raw_button(logical));
        value = next && raw >= 0 ? next(stick, raw) : 0;
    }
    if (shared_menu && logical >= 12 && control_mapping.used(logical)) value = 0;
    if (core == "drastic") {
        if (button == 18 && value && !quit_at) {
            request_quit();
            return 0;
        }
        if (!pulse_consumed && now() - pulse_at < 160 &&
            ((button == 16 && pulse == 1) || (button == 17 && pulse == 2) || (button == 18 && pulse == 3) ||
             (button == 19 && pulse == 4))) {
            pulse_consumed = true;
            return 1;
        }
        if (button == 18 && (saving || failed)) return 0;
    }
    return value;
}
extern "C" Uint8 SDL_GameControllerGetButton(SDL_GameController* pad, SDL_GameControllerButton button) {
    using Fn = Uint8 (*)(SDL_GameController*, SDL_GameControllerButton);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_GameControllerGetButton"));
    if (reading_native_buttons) return next ? next(pad, button) : 0;
    tick();
    if ((shared_menu && button == SDL_CONTROLLER_BUTTON_GUIDE) || input_blocked()) return 0;
    int logical = controller_source(button);
    if (shared_menu && logical >= 0 && logical < 12)
        return Uint8(controller_source_down(pad, control_mapping.source_for(logical)));
    if (shared_menu && logical >= 12 && control_mapping.used(logical)) return 0;
    return next ? next(pad, button) : 0;
}
extern "C" const Uint8* SDL_GetKeyboardState(int* count) {
    using Fn = const Uint8* (*)(int*);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_GetKeyboardState"));
    int length = 0;
    auto source = next ? next(&length) : nullptr;
    if (count) *count = length;
    initialize();
    if (!source || !shared_menu) return source;
    bool blocked = input_blocked(), active_pulse = now() - pulse_at < 160 && (pulse == 1 || pulse == 2),
         menu_key = false;
    for (auto key : {SDL_SCANCODE_AC_HOME, SDL_SCANCODE_HOME, SDL_SCANCODE_AC_BACK, SDL_SCANCODE_MENU})
        if (key < length && source[key]) menu_key = true;
    if (!blocked && !active_pulse && !menu_key) return source;
    RGDS_PROFILE(keyboard_copies, 1);
    static thread_local std::vector<Uint8> keys;
    keys.assign(source, source + length);
    if (blocked) std::fill(keys.begin(), keys.end(), 0);
    for (auto key : {SDL_SCANCODE_AC_HOME, SDL_SCANCODE_HOME, SDL_SCANCODE_AC_BACK, SDL_SCANCODE_MENU})
        if (key < length) keys[key] = 0;
    if (active_pulse) {
        auto key = pulse == 1 ? SDL_SCANCODE_F5 : SDL_SCANCODE_F7;
        if (key < length) keys[key] = 1;
    }
    return keys.data();
}
extern "C" int SDL_PeepEvents(SDL_Event* output, int count, SDL_eventaction action, Uint32 low, Uint32 high) {
    using Fn = int (*)(SDL_Event*, int, SDL_eventaction, Uint32, Uint32);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_PeepEvents"));
    if (reading_native_event || !shared_menu || !output || action != SDL_GETEVENT || count <= 0)
        return next ? next(output, count, action, low, high) : 0;
    tick();
    int kept = 0;
    auto drain = [&]() {
        for (auto item = events.begin(); item != events.end() && kept < count;) {
            if (item->type >= low && item->type <= high) {
                output[kept++] = *item;
                item = events.erase(item);
            } else ++item;
        }
    };
    drain();
    int result = 0;
    {
        NativeInputGuard guard(reading_native_event);
        if (next && kept < count) result = next(output + kept, count - kept, action, low, high);
    }
    if (result < 0) return kept ? kept : result;
    int begin = kept;
    for (int i = 0; i < result; i++) {
        auto event = output[begin + i];
        if (normalize_joystick_event(event) && !menu_event(event) && !(input_blocked() && game_input(event)) &&
            !defer_start(event) && remap_event(event))
            output[kept++] = event;
    }
    drain();
    for (int i = 0; i < kept; i++) remember_control_event(output[i]);
    return kept;
}
extern "C" Uint32 SDL_GetMouseState(int* x, int* y) {
    using Fn = Uint32 (*)(int*, int*);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_GetMouseState"));
    auto result = next ? next(x, y) : 0;
    return input_blocked() ? 0 : result;
}
extern "C" Sint16 SDL_JoystickGetAxis(SDL_Joystick* stick, int axis) {
    using Fn = Sint16 (*)(SDL_Joystick*, int);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_JoystickGetAxis"));
    return input_blocked() ? 0 : next ? next(stick, axis) : 0;
}
extern "C" Sint16 SDL_GameControllerGetAxis(SDL_GameController* pad, SDL_GameControllerAxis axis) {
    using Fn = Sint16 (*)(SDL_GameController*, SDL_GameControllerAxis);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_GameControllerGetAxis"));
    if (reading_native_buttons) return next ? next(pad, axis) : 0;
    if (input_blocked() || (shared_menu && ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && control_mapping.used(12)) ||
                                            (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && control_mapping.used(13)))))
        return 0;
    return next ? next(pad, axis) : 0;
}
extern "C" Uint8 SDL_JoystickGetHat(SDL_Joystick* stick, int hat) {
    using Fn = Uint8 (*)(SDL_Joystick*, int);
    initialize();
    static auto next = reinterpret_cast<Fn>(input_symbol("SDL_JoystickGetHat"));
    if (reading_native_buttons) return next ? next(stick, hat) : 0;
    if (input_blocked()) return 0;
    if (!shared_menu || hat != 0) return next ? next(stick, hat) : 0;
    Uint8 value = 0;
    for (int logical = 8; logical < 12; logical++)
        if (joystick_source_down(stick, control_mapping.source_for(logical))) value |= rgds::hat_bit(logical);
    return value;
}
#ifdef __linux__
bool anbernic_input(int descriptor) {
    using Fn = int (*)(int, unsigned long, ...);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "ioctl"));
    char name[128]{};
    // ROCKNIX's joypad is a mainline pad: BTN_TL2 (312) is its L2 and BTN_MODE (316) its F key.
    return next && next(descriptor, EVIOCGNAME(sizeof(name)), name) >= 0 &&
           (std::strstr(name, "ANBERNIC") || (std::strstr(name, "retrogame_joypad") && !rocknix_joypad(name)));
}
extern "C" int ioctl(int descriptor, unsigned long request, ...) noexcept {
    using Fn = int (*)(int, unsigned long, ...);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "ioctl"));
    va_list arguments;
    va_start(arguments, request);
    void* value = va_arg(arguments, void*);
    va_end(arguments);
    // The shared menu and volume watcher need independent evdev clients.
    // Engines otherwise grab touch/keys exclusively.
    if (request == EVIOCGRAB && value && env("RGDS_SHARED_MENU") == "1") return 0;
    int result = next ? next(descriptor, request, value) : -1;
    if (result >= 0 && value && std::getenv("RGDS_SHARED_MENU") && _IOC_TYPE(request) == 'E' &&
        _IOC_NR(request) == 0x18) {
        auto bits = static_cast<unsigned char*>(value);
        auto size = _IOC_SIZE(request);
        if (initialized && input_blocked()) {
            std::memset(bits, 0, size);
            return result;
        }
        bool anbernic = anbernic_input(descriptor);
        // Consume physical Menu before remapping: legacy output code 312 can
        // also represent a legitimate mapped input on a mainline device.
        for (int code : {102, 139, 158, 172, 312, 316, 354})
            if (rgds::game_menu_code(code, anbernic) && static_cast<unsigned>(code / 8) < size)
                bits[code / 8] &= static_cast<unsigned char>(~(1 << (code % 8)));
        // BTN_TL2 (312) is a real trigger on standard/mainline pads.
    }
    return result;
}
extern "C" ssize_t read(int descriptor, void* buffer, size_t size) {
    using Fn = ssize_t (*)(int, void*, size_t);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "read"));
    if (!std::getenv("RGDS_SHARED_MENU")) return next ? next(descriptor, buffer, size) : -1;
    char reference[64], path[256];
    std::snprintf(reference, sizeof(reference), "/proc/self/fd/%d", descriptor);
    auto length = readlink(reference, path, sizeof(path) - 1);
    if (length < 0) return next ? next(descriptor, buffer, size) : -1;
    path[length] = '\0';
    bool input = std::strncmp(path, "/dev/input/event", 16) == 0;
    // Every evdev client reads libc directly; touch framing and calibration
    // stay native.
    Fn reader = next;
    if (input) {
        static void* libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
        static auto raw = libc ? reinterpret_cast<Fn>(dlsym(libc, "read")) : nullptr;
        if (raw) reader = raw;
    }
    auto result = reader ? reader(descriptor, buffer, size) : -1;
    if (result <= 0 || !input || result % sizeof(input_event)) return result;
    initialize();
    if (!shared_menu) return result;
    auto events = static_cast<input_event*>(buffer);
    bool blocked = input_blocked(), anbernic = anbernic_input(descriptor);
    for (ssize_t i = 0; i < result / ssize_t(sizeof(input_event)); i++) {
        auto& event = events[i];
        struct Packet {
            uint64_t micros;
            int type, code, value;
        };
        Packet packet{uint64_t(event.time.tv_sec) * 1000000 + uint64_t(event.time.tv_usec), event.type, event.code,
                      event.value};
        double stamp = double(packet.micros) / 1000000.0;
        bool fresh = stamp <= 0 || stamp >= (stamp > 1000000000 ? resume_realtime : resume_monotonic);
        if (fresh && packet.type == 1 && packet.value >= 0 && packet.value <= 1) {
            int source = rgds::game_menu_code(packet.code, anbernic) ? rgds::control_menu
                         : packet.code == (anbernic ? 311 : 315)     ? rgds::control_start
                                                                     : -1;
            if (source == rgds::control_menu && !rgds::game_switcher_code(packet.code, anbernic)) {
                if (packet.value == 1) request_game_menu();
            } else if (source >= 0) observe_shortcut(source, packet.value == 1);
        }
        rgds::filter_game_input(packet, blocked, anbernic, resume_monotonic, resume_realtime);
        event.type = Uint16(packet.type);
        event.code = Uint16(packet.code);
        event.value = packet.value;
    }
    return result;
}
#endif
extern "C" int SDL_JoystickNumButtons(SDL_Joystick* stick) {
    using Fn = int (*)(SDL_Joystick*);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "SDL_JoystickNumButtons"));
    initialize();
    int value = next ? next(stick) : 0;
    return core == "drastic" && value < 24 ? 24 : value;
}
extern "C" int kill(pid_t pid, int signal) {
    using Fn = int (*)(pid_t, int);
    static auto next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "kill"));
    initialize();
    if (pid == getpid() && signal == SIGTERM && !core.empty() && autosave && !quit_at) {
        request_quit();
        return 0;
    }
    return next ? next(pid, signal) : -1;
}
