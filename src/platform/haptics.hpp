#pragma once
// The RG DS vibration motor: short patterns for the menus and a held level for
// game rumble. The motor sits behind the pad's force-feedback interface
// (ANBERNIC-rk3568-keys, FF_RUMBLE through ff-memless, a PWM on the board):
// the strong magnitude sets the duty as magnitude / 0x8000, saturating there,
// and anything below about 0x3000 (37% duty) leaves the motor off.
//
// A pattern is uploaded as one effect per pulse with its start delay, then all
// of them are started at once, so the kernel's timers play it out. Nothing waits
// on the UI thread, and a pattern still ends on time when the process is
// stopped (the in-game menu SIGSTOPs the game) or exits (the kernel erases a
// handle's effects when it closes).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#ifdef __linux__
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace rgds {
// The menu patterns. The first seventeen follow MenuCue one to one.
enum class Haptic {
    Tick,
    Select,
    Open,
    Back,
    On,
    Off,
    Favourite,
    Unfavourite,
    Launch,
    Resume,
    Save,
    Load,
    Error,
    Key,
    Detent,
    LidClose,
    Unlock,
    Edge, // a move past the end of a list
    Hold, // a long press took effect
    Notify,
    Count
};
inline constexpr std::array<const char*, size_t(Haptic::Count)> haptic_labels = {
    "Browse",           "Select",       "Open a view", "Go back",       "Switch on",     "Switch off",  "Add favourite",
    "Remove favourite", "Start a game", "Resume game", "Save complete", "Load complete", "Try again",   "Keyboard",
    "Adjust",           "Close lid",    "Open lid",    "End of a list", "Long press",    "Notification"};

// One pulse: rest `gap` ms after the previous pulse ends, then drive the motor
// at `drive` (0-1 of its usable range) for `ms`.
struct HapticPulse {
    float drive;
    uint16_t ms, gap;
};
struct HapticPattern {
    std::array<HapticPulse, 4> pulses;
    uint8_t count;
    uint8_t weight; // 0 light ticks, 1 ordinary, 2 events a tick must not cut short
};
// Tuned on the RG DS motor: an ERM needs about 10 ms to be felt at all, so the
// light ticks are short full-ish pulses rather than weak ones.
inline constexpr std::array<HapticPattern, size_t(Haptic::Count)> haptic_patterns = {{
    {{{{.85f, 14, 0}}}, 1, 0},                              // Tick
    {{{{1.f, 22, 0}}}, 1, 1},                               // Select
    {{{{1.f, 18, 0}}}, 1, 1},                               // Open
    {{{{.7f, 14, 0}}}, 1, 1},                               // Back
    {{{{.6f, 10, 0}, {1.f, 20, 45}}}, 2, 1},                // On
    {{{{.7f, 14, 0}}}, 1, 1},                               // Off
    {{{{.8f, 16, 0}, {1.f, 30, 70}}}, 2, 1},                // Favourite
    {{{{.7f, 16, 0}}}, 1, 1},                               // Unfavourite
    {{{{.5f, 30, 0}, {.75f, 40, 0}, {1.f, 90, 0}}}, 3, 2},  // Launch: a short rev-up
    {{{{.9f, 24, 0}, {.9f, 24, 60}}}, 2, 2},                // Resume
    {{{{.8f, 20, 0}, {1.f, 40, 80}}}, 2, 2},                // Save
    {{{{1.f, 40, 0}, {.8f, 20, 80}}}, 2, 2},                // Load
    {{{{1.f, 30, 0}, {1.f, 30, 50}, {1.f, 55, 50}}}, 3, 2}, // Error
    {{{{.75f, 10, 0}}}, 1, 0},                              // Key
    {{{{.6f, 9, 0}}}, 1, 0},                                // Detent
    {{{{1.f, 35, 0}, {.5f, 60, 0}}}, 2, 2},                 // LidClose
    {{{{.7f, 14, 0}, {1.f, 26, 50}}}, 2, 2},                // Unlock
    {{{{.55f, 26, 0}}}, 1, 0},                              // Edge
    {{{{.6f, 12, 0}, {1.f, 34, 30}}}, 2, 1},                // Hold
    {{{{1.f, 40, 0}, {1.f, 40, 110}}}, 2, 2},               // Notify
}};

class Haptics {
public:
    // How many patterns were asked for and played, and per pattern: the menus'
    // own report (and the tests) read these; they count on hosts without a motor.
    int plays = 0, opens = 0, rumbles = 0;
    std::array<int, size_t(Haptic::Count)> counts{};

    ~Haptics() { close(); }
    // 0 turns the menu patterns off; 1-100 scales drive and pulse length.
    void set_strength(int percent) { strength = std::clamp(percent, 0, 100); }
    // 0 turns game rumble off; 1-100 scales the level a game asks for.
    void set_rumble_strength(int percent) {
        rumble_strength = std::clamp(percent, 0, 100);
        if (!rumble_strength) rumble(0);
    }
    int menu_strength() const { return strength; }
    bool available() {
#ifdef __linux__
        return fd >= 0 || !find_device().empty();
#else
        return false;
#endif
    }
    bool open_now() const { return fd >= 0; }
    // A pattern from the table above. False when it was dropped: haptics off,
    // a tick too soon after the last one, or a weightier pattern still playing.
    // A pinned one (a preview) plays out whatever is asked for meanwhile.
    bool play(Haptic which, bool pinned = false) {
        if (!strength || which >= Haptic::Count) return false;
        const auto& p = haptic_patterns[size_t(which)];
        const int64_t now = clock_ms();
        if (p.weight == 0 && now - last_tick < 28) return false;
        if (p.weight < playing_weight && now < playing_until) return false;
        // Every pulse is scaled the same way. The motor is an ERM: how hard a
        // pulse feels depends mostly on how long it spins it up, so strength
        // stretches the pulses (0.76x at 10%, 2.2x at 100%) as well as raising
        // the drive (full for every pulse from about 70%). The table is the 25%
        // feel; never below the two jiffies (8 ms) a pulse needs.
        const float s = strength / 100.f;
        std::array<Pulse, 4> pulses{};
        int at = 0;
        for (int i = 0; i < p.count; i++) {
            const auto& pulse = p.pulses[size_t(i)];
            at += pulse.gap;
            const int ms = std::max(8, int(std::lround(pulse.ms * (.6f + 1.6f * s))));
            pulses[size_t(i)] = {magnitude(pulse.drive * (.4f + .9f * s)), uint16_t(ms), uint16_t(at)};
            at += ms;
        }
        counts[size_t(which)]++;
        plays++;
        if (p.weight == 0) last_tick = now;
        playing_weight = pinned ? 3 : p.weight;
        playing_until = now + at;
        last_active = now;
        send(pulses, p.count);
        return true;
    }
    // A game's motor, held at `level` (0-1) until the next call; 0 stops it.
    // Each call re-arms a short effect instead of an endless one, so a game
    // that stops calling (stopped for its menu, stalled, killed) falls quiet
    // within `hold_ms` on its own.
    void rumble(float level, int hold_ms = 120) {
        const float scaled = rumble_strength ? std::clamp(level, 0.f, 1.f) * rumble_strength / 100.f : 0.f;
        const uint16_t value = scaled > 0 ? magnitude(.3f + .7f * scaled) : 0;
        const int64_t now = clock_ms();
        if (value == rumble_value && (value == 0 || now - rumble_armed < hold_ms / 2)) return;
        if (value && !rumble_value) rumbles++;
        rumble_value = value;
        rumble_armed = now;
        rumble_hold = hold_ms;
        last_active = now;
#ifdef __linux__
        if (!value) {
            if (fd >= 0 && rumble_id >= 0) trigger(rumble_id, 0);
            return;
        }
        if (!ensure_open()) return;
        if (upload(rumble_id, value, uint16_t(std::clamp(hold_ms, 20, 1000)), 0)) trigger(rumble_id, 1);
#endif
    }
    // Stops whatever is playing now.
    void stop() {
        playing_until = 0;
        rumble_value = 0;
#ifdef __linux__
        if (fd < 0) return;
        for (int id : slots)
            if (id >= 0) trigger(id, 0);
        if (rumble_id >= 0) trigger(rumble_id, 0);
#endif
    }
    // Lets go of the device after a few quiet seconds, so effect slots (16 on the
    // whole pad, shared with every process) are not held by an idle menu.
    void poll() {
        const int64_t now = clock_ms();
        if (rumble_value && now - rumble_armed > rumble_hold) rumble_value = 0; // the effect ran out by itself
        if (fd >= 0 && !rumble_value && now - last_active > 4000 && now > playing_until) close();
    }
    void close() {
#ifdef __linux__
        if (fd >= 0) ::close(fd);
#endif
        fd = -1;
        slots.fill(-1);
        rumble_id = -1;
        rumble_value = 0;
    }

private:
    struct Pulse {
        uint16_t value, ms, delay;
    };
    int fd = -1, strength = 60, rumble_strength = 80;
    std::array<int, 4> slots{-1, -1, -1, -1};
    int rumble_id = -1, rumble_hold = 0;
    uint16_t rumble_value = 0;
    int64_t last_tick = -1000, playing_until = 0, last_active = 0, rumble_armed = 0;
    uint8_t playing_weight = 0;
    std::string device;

    static int64_t clock_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    // 0-1 of the usable range onto the driver's magnitude: 0x3000 is the
    // lowest duty that still turns the motor, 0x8000 already full duty.
    static uint16_t magnitude(float drive) {
        if (drive <= 0) return 0;
        return uint16_t(0x3000 + std::lround(std::clamp(drive, 0.f, 1.f) * (0x8000 - 0x3000)));
    }
    void send(const std::array<Pulse, 4>& pulses, int count) {
#ifdef __linux__
        if (!ensure_open()) return;
        // A new pattern replaces the last one; its own slots restart on upload.
        for (size_t i = size_t(count); i < slots.size(); i++)
            if (slots[i] >= 0) trigger(slots[i], 0);
        for (int i = 0; i < count; i++) {
            const auto& p = pulses[size_t(i)];
            if (!upload(slots[size_t(i)], p.value, p.ms, p.delay)) return;
        }
        for (int i = 0; i < count; i++) trigger(slots[size_t(i)], 1);
#else
        (void)pulses;
        (void)count;
#endif
    }
#ifdef __linux__
    static bool has_rumble(int handle) {
        unsigned long bits[(FF_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
        if (ioctl(handle, EVIOCGBIT(EV_FF, sizeof(bits)), bits) < 0) return false;
        return bits[FF_RUMBLE / (8 * sizeof(long))] & (1UL << (FF_RUMBLE % (8 * sizeof(long))));
    }
    // RGDS_HAPTICS_DEVICE names it; otherwise the RG DS pad, else the first
    // device that can rumble.
    std::string find_device() {
        if (!device.empty()) return device;
        if (const char* named = std::getenv("RGDS_HAPTICS_DEVICE")) return device = named;
        std::string first;
        DIR* dir = opendir("/dev/input");
        if (!dir) return "";
        while (dirent* e = readdir(dir)) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            const std::string path = std::string("/dev/input/") + e->d_name;
            int handle = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
            if (handle < 0) continue;
            char name[128] = {};
            ioctl(handle, EVIOCGNAME(sizeof(name) - 1), name);
            const bool rumble = has_rumble(handle);
            ::close(handle);
            if (!rumble) continue;
            if (std::strcmp(name, "ANBERNIC-rk3568-keys") == 0) {
                first = path;
                break;
            }
            if (first.empty()) first = path;
        }
        closedir(dir);
        return device = first;
    }
    bool ensure_open() {
        if (fd >= 0) return true;
        const auto path = find_device();
        if (path.empty() || std::getenv("RGDS_HAPTICS_OFF")) return false;
        fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return false;
        opens++;
        // The gain is the pad's, not this handle's: another program may have
        // left it low. Full gain keeps the strength settings meaning the same.
        input_event gain{};
        gain.type = EV_FF;
        gain.code = FF_GAIN;
        gain.value = 0xFFFF;
        if (write(fd, &gain, sizeof(gain)) < 0) {}
        return true;
    }
    bool upload(int& id, uint16_t value, uint16_t ms, uint16_t delay) {
        ff_effect effect{};
        effect.type = FF_RUMBLE;
        effect.id = int16_t(id);
        effect.replay.length = ms;
        effect.replay.delay = delay;
        effect.u.rumble.strong_magnitude = value;
        effect.u.rumble.weak_magnitude = 0; // the RG DS driver reads only the strong motor
        if (ioctl(fd, EVIOCSFF, &effect) < 0) {
            // A slot taken back by the kernel (another program used them all):
            // ask for a fresh one once before giving up on this pulse.
            if (id < 0) return false;
            effect.id = -1;
            if (ioctl(fd, EVIOCSFF, &effect) < 0) return false;
        }
        id = effect.id;
        return true;
    }
    void trigger(int id, int on) {
        input_event play{};
        play.type = EV_FF;
        play.code = uint16_t(id);
        play.value = on;
        if (write(fd, &play, sizeof(play)) < 0) {}
    }
#endif
};
} // namespace rgds
