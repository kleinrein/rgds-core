#pragma once
#include <array>
#include <algorithm>
#include <fstream>
#include <map>
#include <string>

namespace rgds {
inline constexpr std::array<const char*, 12> control_names{"a",     "b",      "x",  "y",    "l",    "r",
                                                           "start", "select", "up", "down", "left", "right"};
inline constexpr std::array<const char*, 17> physical_names{
    "a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right", "l2", "r2", "l3", "r3", "none"};
inline constexpr int control_start = 6, control_menu = 17, control_none = 16;
inline int physical_index(const std::string& name) {
    for (int i = 0; i < int(physical_names.size()); i++)
        if (name == physical_names[i]) return i;
    return -1;
}
inline int joystick_source(int button) {
    switch (button) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 3;
    case 3: return 2;
    case 4: return 4;
    case 5: return 5;
    case 6: return 7;
    case 7: return 6;
    case 8: return control_menu;
    case 9: return 14;
    case 10: return 12;
    case 11: return 13;
    case 12: return 15;
    default: return -1;
    }
}
inline int physical_button(int source) {
    constexpr int buttons[]{0, 1, 3, 2, 4, 5, 7, 6, -1, -1, -1, -1, 10, 11, 9, 12};
    return source >= 0 && source < 16 ? buttons[source] : -1;
}
inline int hat_source(int value) { return value == 1 ? 8 : value == 4 ? 9 : value == 8 ? 10 : value == 2 ? 11 : -1; }
inline int hat_bit(int source) { return source == 8 ? 1 : source == 9 ? 4 : source == 10 ? 8 : source == 11 ? 2 : 0; }
inline int evdev_button(int source, bool mainline = false) {
    constexpr int codes[]{305, 304, 307, 308, 310, 311, 315, 314, 544, 545, 546, 547, 312, 313, 317, 318};
    if (mainline) return source >= 0 && source < 16 ? codes[source] : -1;
    int button = physical_button(source);
    return button >= 0 ? 304 + button : -1;
}
inline int evdev_source(int code, bool mainline = false) {
    if (mainline) {
        for (int source = 0; source < 16; source++)
            if (evdev_button(source, true) == code) return source;
        return -1;
    }
    return code >= 304 && code <= 316 ? joystick_source(code - 304) : -1;
}
inline std::map<std::string, std::string> control_fields(const std::string& path) {
    std::map<std::string, std::string> result;
    if (path.empty()) return result;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        auto split = line.find('=');
        if (split == std::string::npos) continue;
        auto key = line.substr(0, split), value = line.substr(split + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        result[key] = value;
    }
    return result;
}
struct ControlMapping {
    std::array<int, 12> sources{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    std::string preset = "default", shortcut = "start+menu", shortcut_action = "home";
    int fast_forward_source = 13; // R2 fast-forwards unless a preset or the user says otherwise
    std::string fast_forward_mode = "hold";
    void apply(const std::map<std::string, std::string>& fields) {
        auto schema = fields.find("schema");
        if (schema != fields.end() && schema->second != "1") return;
        auto value = [&](const char* key) {
            auto found = fields.find(key);
            return found == fields.end() ? std::string{} : found->second;
        };
        auto name = value("preset");
        if (name == "default" || name == "ab-swap" || name == "left-handed") {
            sources = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
            preset = name;
            if (name == "ab-swap") std::swap(sources[0], sources[1]);
            else if (name == "left-handed") sources = {11, 9, 8, 10, 5, 4, 6, 7, 2, 1, 3, 0};
        } else if (name == "custom") preset = name;
        for (int i = 0; i < 12; i++) {
            auto found = fields.find(std::string("bind_") + control_names[i]);
            if (found != fields.end()) {
                int source = physical_index(found->second);
                if (source >= 0) sources[i] = source;
            }
        }
        auto mode = value("shortcut");
        if (mode == "anbernic" || mode == "start+menu" || mode == "startthenmenu") shortcut = mode;
        auto action = value("shortcut_action");
        if (action == "menu" || action == "home") shortcut_action = action;
        int speed = physical_index(value("fast_forward_button"));
        if (speed >= 0) fast_forward_source = speed;
        auto speed_mode = value("fast_forward_mode");
        if (speed_mode == "hold" || speed_mode == "toggle") fast_forward_mode = speed_mode;
        if ((name == "default" || name == "ab-swap" || name == "left-handed") && !fields.count("fast_forward_button") &&
            logical_for(fast_forward_source) >= 0)
            fast_forward_source = control_none;
    }
    void load(const std::string& global, const std::string& local = {}) {
        *this = ControlMapping{};
        apply(control_fields(global));
        auto fields = control_fields(local);
        if (fields["inherit"] != "1") apply(fields);
    }
    int logical_for(int source) const {
        for (int logical = 0; logical < 12; logical++)
            if (sources[logical] == source) return logical;
        return -1;
    }
    int source_for(int logical) const { return logical >= 0 && logical < 12 ? sources[logical] : logical; }
    bool used(int source) const {
        return logical_for(source) >= 0 || (source != control_none && source == fast_forward_source);
    }
    void remap_key_bits(unsigned char* bits, size_t length, bool start_visible = true, bool mainline = false) const {
        std::array<unsigned char, 128> physical{};
        std::copy(bits, bits + std::min(length, physical.size()), physical.begin());
        auto pressed = [&](int code) {
            return code >= 0 && size_t(code / 8) < physical.size() && (physical[size_t(code) / 8] & (1 << (code % 8)));
        };
        auto set = [&](int code, bool down) {
            if (code < 0 || size_t(code / 8) >= length) return;
            auto bit = static_cast<unsigned char>(1 << (code % 8));
            if (down) bits[code / 8] |= bit;
            else bits[code / 8] &= static_cast<unsigned char>(~bit);
        };
        for (int source = 0; source < 16; source++)
            if (source < 12 || used(source) || mainline) {
                set(evdev_button(source, mainline), false);
                set(evdev_button(source), false);
            }
        for (int logical = 0; logical < 8; logical++) {
            int source = source_for(logical);
            set(evdev_button(logical),
                pressed(evdev_button(source, mainline)) && (source != control_start || start_visible));
        }
    }
    void remap_evdev_key(int& type, int& code, int& value, bool start_visible = true, bool mainline = false,
                         std::array<bool, 16>* held = nullptr) const {
        if (type != 1) return;
        int source = evdev_source(code, mainline), logical = logical_for(source);
        if (source < 0 || source >= 16) return;
        if (held) (*held)[source] = value != 0;
        if (source == control_start && !start_visible) value = 0;
        if (logical >= 0 && logical < 8) code = evdev_button(logical);
        else if (logical >= 8) {
            type = 3;
            code = logical < 10 ? 17 : 16;
            if (held) {
                int negative = source_for(logical < 10 ? 8 : 10), positive = source_for(logical < 10 ? 9 : 11);
                value = (positive < 16 && (*held)[positive] ? 1 : 0) - (negative < 16 && (*held)[negative] ? 1 : 0);
            } else value = value ? (logical == 8 || logical == 10 ? -1 : 1) : 0;
        } else if (mainline || source < 12 || source == fast_forward_source) {
            type = 0;
            code = 0;
            value = 0;
        }
    }
};
enum class ShortcutAction { none, menu, home };
struct ControlShortcut {
    std::string mode = "start+menu", action = "home";
    bool start = false, menu = false, latched = false, tap = false, start_seen = false, menu_pending = false,
         fired = false;
    unsigned long long started = 0, pending_menu = 0, last = 0, tap_end = 0;
    ShortcutAction fire(bool home, unsigned long long time) {
        if (fired && time - last < 250) return ShortcutAction::none;
        last = time;
        fired = true;
        return home ? ShortcutAction::home : ShortcutAction::menu;
    }
    ShortcutAction feed(int source, bool down, unsigned long long time) {
        if (source != control_start && source != control_menu) return ShortcutAction::none;
        bool was_latched = latched;
        bool& held = source == control_start ? start : menu;
        bool changed = held != down;
        held = down;
        if (!start && !menu) latched = false;
        if (!changed) return ShortcutAction::none;
        if (source == control_start && down) {
            started = time;
            start_seen = true;
            tap = false;
        }
        if (source == control_start && !down && !was_latched && time - started < 150) {
            tap = true;
            tap_end = started + 310;
        }
        if (!down || latched) return ShortcutAction::none;
        if (mode == "anbernic" && source == control_menu) return fire(action == "home", time);
        if ((mode == "start+menu" && start && menu) ||
            (mode == "startthenmenu" && source == control_menu && start_seen && time - started <= 650)) {
            latched = true;
            tap = false;
            pending_menu = 0;
            menu_pending = false;
            return fire(action == "home", time);
        }
        if (source == control_menu) {
            pending_menu = time;
            menu_pending = true;
        }
        return ShortcutAction::none;
    }
    ShortcutAction poll(unsigned long long time) {
        if (menu_pending && time - pending_menu >= 150) {
            menu_pending = false;
            return fire(false, time);
        }
        return ShortcutAction::none;
    }
    bool visible_start(unsigned long long time) const {
        return !latched && !menu &&
               ((start && (mode == "anbernic" || time - started >= 150)) ||
                (tap && time >= started + 150 && time < tap_end));
    }
};
}
