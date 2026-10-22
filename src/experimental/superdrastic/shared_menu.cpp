// RGDS menu presentation through SuperDrastic's existing DRM presenter.
// Only the guest thread pauses; the display/event thread remains available.
#include "platform/overlay_frame.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dlfcn.h>
#include <chrono>
#include <string>
#include <vector>
extern "C" {
int dsflip_menu_canvas(int, uint32_t**, int*, int*, int*);
void dsflip_menu_show(int, int);
void rgds_sd_release_inputs(void);
int rgds_sd_menu_presented(int, int);
int dsflip_menu_grab_top(uint32_t*, int, int, int);
void SDL_PauseAudio(int);
void audio_mute(int);
void audio_mic_quiet(int);
int resume_menu_save(const char*, const char*, const char*);
int resume_menu_load(const char*, const char*, const char*, const char*);
int resume_quit_pending(void);
void resume_press(int);
int rgds_sd_idle(void);
void rgds_sd_menu_state(int);
void rgds_sd_exit(const char*);
extern volatile int dsflip_hold;
}
namespace {
rgds::OverlayFrame overlay;
std::string control, blocked, overlay_path, last[2], token, action;
bool enabled = false, initialized = false, speed = false;
std::atomic<bool> in_menu{false};
uint32_t displayed = 0;
long long polled = 0;
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
const char* env(const char* key) {
    auto* p = std::getenv(key);
    return p ? p : "";
}
void write(const std::string& path, const std::string& value) {
    if (path.empty()) return;
    auto temporary = path + ".pending";
    FILE* f = std::fopen(temporary.c_str(), "w");
    if (!f) return;
    bool ok = std::fwrite(value.data(), 1, value.size(), f) == value.size();
    if (std::fclose(f)) ok = false;
    if (ok) std::rename(temporary.c_str(), path.c_str());
    else ::unlink(temporary.c_str());
}
void result(const std::string& t, const char* state) {
    write(control + ".result", "token=" + t + "\nstate=" + state + "\n");
}
void speed_state() { write(control + ".speed-state", std::string("active=") + (speed ? "1\n" : "0\n")); }
void drain() {
    static auto poll = reinterpret_cast<int (*)(void*)>(dlsym(RTLD_NEXT, "SDL_PollEvent"));
    unsigned char event[64];
    if (poll)
        for (int n = 0; n < 256 && poll(event); ++n) {}
}
void background() {
    // The shared launcher consumes the same 24-bit BMP as other RGDS cores.
    std::string path = control.substr(0, control.find_last_of('/')) + "/menu-background.bmp";
    std::vector<uint32_t> pixels(640 * 480);
    if (!dsflip_menu_grab_top(pixels.data(), 640 * 4, 640, 480)) return;
    unsigned char header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    auto put = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put(2, 54 + 640 * 480 * 3);
    put(10, 54);
    put(14, 40);
    put(18, 640);
    put(22, 480);
    header[26] = 1;
    header[28] = 24;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(header, 1, 54, f);
    unsigned char row[640 * 3];
    for (int y = 479; y >= 0; --y) {
        for (int x = 0; x < 640; ++x) {
            uint32_t p = pixels[y * 640 + x];
            row[3 * x] = p;
            row[3 * x + 1] = p >> 8;
            row[3 * x + 2] = p >> 16;
        }
        std::fwrite(row, 1, sizeof row, f);
    }
    std::fclose(f);
}
void present() {
    if (!overlay.open(overlay_path, false) || !overlay.refresh() || overlay.version() == displayed) return;
    int slots[2];
    uint32_t* out[2];
    int pitch[2], w[2], h[2];
    for (int p = 0; p < 2; ++p) {
        slots[p] = dsflip_menu_canvas(p, &out[p], &pitch[p], &w[p], &h[p]);
        if (slots[p] < 0 || w[p] != 640 || h[p] != 480 || pitch[p] < 2560) return;
    }
    for (int p = 0; p < 2; ++p) {
        const auto* src = overlay.panel(p);
        for (int y = 0; y < 480; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(out[p]) + y * pitch[p]);
            for (int x = 0; x < 640; ++x) {
                auto* c = src + (y * 640 + x) * 4;
                row[x] = (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
            }
        }
        dsflip_menu_show(p, slots[p]);
    }
    for (int n = 0; n < 125; ++n) {
        if (rgds_sd_menu_presented(slots[0], slots[1])) {
            displayed = overlay.version();
            overlay.acknowledge();
            break;
        }
        ::usleep(2000);
    }
}
bool read_command(const std::string& path, std::string& t, std::string& a) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char tb[80], ab[80], extra;
    int n = std::fscanf(f, "%79s %79s %c", tb, ab, &extra);
    std::fclose(f);
    if (n != 2) return false;
    for (char* c = tb; *c; ++c)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || *c == '-')) return false;
    t = tb;
    a = ab;
    return true;
}
bool dispatch(const std::string& t, const std::string& a, bool native, bool receipt = true) {
    if (!action.empty() || !rgds_sd_idle()) return false;
    if (a == "menu") {
        write(control + ".menu", "1\n");
    } else if (a == "save" || a == "save-quit" || (a == "quit" && std::strcmp(env("RGDS_SD_SAVE_ON_EXIT"), "0"))) {
        // Write only the session's staging file. Python verifies and promotes it.
        ::unlink(env("RGDS_SD_RESUME"));
        if (resume_menu_save(env("RGDS_SD_RESUME"), "", "") < 0) {
            result(t, "save-failed");
            return true;
        }
        token = t;
        action = a;
        return true;
    } else if (a == "quit") {
        rgds_sd_exit("no-save");
        ::kill(::getpid(), SIGKILL);
    } else if (a == "load") {
        if (resume_menu_load(env("RGDS_SD_CHECKPOINT"), nullptr, "", "") < 0) {
            result(t, "load-failed");
            return true;
        }
        token = t;
        action = a;
        return true;
    } else if (a == "fast-forward" || a == "fast-forward-on" || a == "fast-forward-off" || a == "fast-forward-toggle" ||
               a == "speed-on" || a == "speed-off" || a == "speed-toggle") {
        bool next = (a == "speed-on" || a == "fast-forward-on")     ? true
                    : (a == "speed-off" || a == "fast-forward-off") ? false
                                                                    : !speed;
        if (next != speed) {
            speed = next;
            resume_press(19);
        }
        speed_state();
        if (!native && receipt) result(t, speed ? "fast-forward-on" : "fast-forward-off");
    } else if (a.rfind("shader:", 0) == 0) {
        result(t, "shader-restart-required");
    } else if (a.rfind("ds:", 0) == 0) {
        result(t, "ds-unavailable");
    } else if (a == "fps:0" || a == "fps:1") {
        result(t, "fps-unavailable");
    } else {
        result(t, "load-failed");
    }
    if (native)
        write(env("RGDS_SD_RESULT"), "token=" + t + "\nstate=" +
                                         (a == "menu" ? std::string("ok")
                                          : speed     ? "speed-on"
                                                      : "speed-off") +
                                         "\n");
    return true;
}
void commands() {
    for (int i = 0; i < 2; ++i) {
        std::string t, a;
        if (read_command(i ? env("RGDS_SD_COMMAND") : control, t, a) && t != last[i] && dispatch(t, a, i)) last[i] = t;
    }
    // RGDS's independent fast-forward button uses a field record.
    FILE* f = std::fopen((control + ".speed-request").c_str(), "r");
    if (!f) return;
    char line[160];
    std::string t, a;
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        s.erase(s.find_last_not_of("\r\n") + 1);
        if (s.rfind("token=", 0) == 0) t = s.substr(6);
        if (s.rfind("action=", 0) == 0) a = s.substr(7);
    }
    std::fclose(f);
    static std::string last_speed;
    if (t.empty() || t == last_speed) return;
    if (a != "fast-forward-on" && a != "fast-forward-off" && a != "fast-forward-toggle") return;
    // The independent button must neither consume a pending save receipt nor
    // enable speed while its physical press belongs to the menu.
    if (in_menu && a != "fast-forward-off") {
        last_speed = t;
        return;
    }
    if (dispatch(t, a, false, false)) last_speed = t;
}
}
extern "C" int rgds_shared_enabled() { return std::strcmp(env("RGDS_SD_SHARED_MENU"), "1") == 0; }
extern "C" int rgds_shared_blocked() { return in_menu; }
extern "C" void rgds_shared_job_done(int save, int ok) {
    if (action.empty() || (save != (action != "load"))) return;
    if (ok && (action == "save-quit" || action == "quit")) {
        rgds_sd_exit("saved");
        ::kill(::getpid(), SIGKILL);
        return;
    }
    result(token, save ? (ok ? "native-saved" : "save-failed") : (ok ? "loaded" : "load-failed"));
    action.clear();
}
extern "C" int rgds_shared_poll(void*) {
    if (!initialized) {
        initialized = true;
        enabled = rgds_shared_enabled();
        control = env("RGDS_CONTROL_FILE");
        blocked = control + ".blocked";
        overlay_path = env("RGDS_OVERLAY_FILE");
    }
    if (!enabled || control.empty()) return 0;
    auto now = now_ms();
    if (!in_menu && now - polled < 20) return 0;
    polled = now;
    commands();
    bool block = ::access(blocked.c_str(), F_OK) == 0;
    if (block && !in_menu) {
        in_menu = true;
        rgds_sd_release_inputs();
        displayed = 0;
        background();
        dsflip_hold = 1;
        audio_mute(1);
        audio_mic_quiet(1);
        rgds_sd_menu_state(1);
    }
    while (block && action.empty() && rgds_sd_idle() && !resume_quit_pending()) {
        SDL_PauseAudio(1);
        present();
        drain();
        ::usleep(8000);
        commands();
        block = ::access(blocked.c_str(), F_OK) == 0;
        if (::getppid() == 1) {
            rgds_sd_exit("orphaned");
            ::kill(::getpid(), SIGKILL);
        }
    }
    if (in_menu) {
        present();
        SDL_PauseAudio(0);
        if (!block) {
            drain();
            in_menu = false;
            overlay.close();
            dsflip_hold = 0;
            audio_mute(0);
            audio_mic_quiet(0);
            rgds_sd_menu_state(0);
        } else {
            dsflip_hold = 1;
            audio_mute(1);
        }
    }
    return 0;
}
