// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace play {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}
bool parse_number(std::string s, uint32_t& out, bool hex_default = false) {
    s = trim(s);
    if (s.empty()) return false;
    int base = hex_default ? 16 : 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
        base = 16;
    } else if (s[0] == '#') {
        s = s.substr(1);
        base = 16;
    }
    if (s.empty()) return false;
    for (char c : s)
        if (!(base == 16 ? std::isxdigit(static_cast<unsigned char>(c)) : std::isdigit(static_cast<unsigned char>(c))))
            return false;
    unsigned long long v = std::stoull(s, nullptr, base);
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}
bool parse_int(const std::string& s, int& out) {
    uint32_t v = 0;
    std::string t = trim(s);
    bool negative = !t.empty() && t[0] == '-';
    if (!parse_number(negative ? t.substr(1) : t, v) || v > 1000000) return false;
    out = negative ? -static_cast<int>(v) : static_cast<int>(v);
    return true;
}

// mem8[03000BDE] or mem16[0x02000000] & FF
bool parse_ref(const std::string& text, Ref& out, std::string& error) {
    std::string s = trim(text);
    auto amp = s.find('&');
    std::string base = trim(s.substr(0, amp));
    if (amp != std::string::npos && !parse_number(s.substr(amp + 1), out.mask, true)) {
        error = "bad mask in '" + text + "'";
        return false;
    }
    auto open = base.find('['), close = base.find(']');
    if (open == std::string::npos || close == std::string::npos || close < open || close != base.size() - 1) {
        error = "expected memN[address] in '" + text + "'";
        return false;
    }
    std::string kind = base.substr(0, open);
    if (kind == "mem8") out.size = 1;
    else if (kind == "mem16") out.size = 2;
    else if (kind == "mem32") out.size = 4;
    else {
        error = "unknown value '" + kind + "' (mem8, mem16, mem32)";
        return false;
    }
    if (!parse_number(base.substr(open + 1, close - open - 1), out.address, true)) {
        error = "bad address in '" + text + "'";
        return false;
    }
    return true;
}

bool parse_steps(const std::string& text, std::vector<Step>& out, std::string& error) {
    out.clear();
    for (const auto& w : words(text)) {
        auto colon = w.find(':');
        Step step;
        if (!parse_buttons(w.substr(0, colon), step.buttons)) {
            error = "unknown buttons in step '" + w + "'";
            return false;
        }
        if (colon != std::string::npos && (!parse_int(w.substr(colon + 1), step.frames) || step.frames < 1)) {
            error = "bad frame count in step '" + w + "'";
            return false;
        }
        out.push_back(step);
    }
    return true;
}

bool parse_rect(const std::string& text, Rect& out) {
    auto w = words(text);
    return w.size() == 4 && parse_int(w[0], out.x) && parse_int(w[1], out.y) && parse_int(w[2], out.w) &&
           parse_int(w[3], out.h) && out.w > 0 && out.h > 0;
}

// 0,0=#9C9C9C 239,0=#9C9C9C
bool parse_pixels(const std::string& text, std::vector<PixelProbe>& out) {
    out.clear();
    for (const auto& w : words(text)) {
        auto comma = w.find(','), eq = w.find('=');
        PixelProbe p;
        if (comma == std::string::npos || eq == std::string::npos || eq < comma) return false;
        if (!parse_int(w.substr(0, comma), p.x) || !parse_int(w.substr(comma + 1, eq - comma - 1), p.y) ||
            !parse_number(w.substr(eq + 1), p.rgb, true))
            return false;
        out.push_back(p);
    }
    return !out.empty();
}

} // namespace

bool parse_buttons(const std::string& text, uint32_t& out) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"A", A},       {"B", B}, {"SELECT", Select}, {"START", Start}, {"RIGHT", Right}, {"LEFT", Left}, {"UP", Up},
        {"DOWN", Down}, {"R", R}, {"L", L},           {"X", X},         {"Y", Y},         {"L2", L2},     {"R2", R2},
    };
    out = 0;
    std::string s = upper(trim(text));
    if (s == "-" || s == "NONE") return true;
    std::istringstream in(s);
    for (std::string part; std::getline(in, part, '+');) {
        bool found = false;
        for (const auto& n : names)
            if (part == n.first) {
                out |= n.second;
                found = true;
            }
        if (!found) return false;
    }
    return out != 0;
}

bool parse_condition(const std::string& text, Condition& out, std::string& error) {
    out.terms.clear();
    std::string rest = " " + trim(text) + " ";
    for (;;) {
        auto at = rest.find(" and ");
        std::string part = trim(rest.substr(0, at));
        if (part.empty()) {
            error = "empty condition";
            return false;
        }
        Condition::Term term;
        size_t op_at;
        std::string values;
        if ((op_at = part.find("==")) != std::string::npos) {
            term.op = Condition::Term::Eq;
            values = part.substr(op_at + 2);
        } else if ((op_at = part.find("!=")) != std::string::npos) {
            term.op = Condition::Term::Ne;
            values = part.substr(op_at + 2);
        } else if ((op_at = part.find(" in ")) != std::string::npos) {
            term.op = Condition::Term::In;
            values = part.substr(op_at + 4);
        } else {
            error = "expected ==, != or in: '" + part + "'";
            return false;
        }
        if (!parse_ref(part.substr(0, op_at), term.ref, error)) return false;
        if (term.op == Condition::Term::In) {
            auto dots = values.find("..");
            uint32_t lo, hi;
            if (dots == std::string::npos || !parse_number(values.substr(0, dots), lo, true) ||
                !parse_number(values.substr(dots + 2), hi, true) || hi < lo) {
                error = "expected lo..hi: '" + part + "'";
                return false;
            }
            term.values = {lo, hi};
        } else {
            std::istringstream in(values);
            for (std::string v; std::getline(in, v, '|');) {
                uint32_t n;
                if (!parse_number(v, n, true)) {
                    error = "bad value '" + trim(v) + "'";
                    return false;
                }
                term.values.push_back(n);
            }
            if (term.values.empty()) {
                error = "no value: '" + part + "'";
                return false;
            }
        }
        out.terms.push_back(term);
        if (at == std::string::npos) break;
        rest = rest.substr(at + 4);
        if (trim(rest).empty()) {
            error = "condition ends with 'and'";
            return false;
        }
        rest = " " + rest;
    }
    return true;
}

std::string Condition::describe(Core& core) const {
    std::string out;
    char text[48];
    for (const auto& t : terms) {
        std::snprintf(text, sizeof(text), "%smem%d[%08X]=%X", out.empty() ? "" : " ", t.ref.size * 8,
                      unsigned(t.ref.address), unsigned(t.ref.read(core)));
        out += text;
    }
    return out;
}

bool Condition::match(Core& core) const {
    for (const auto& t : terms) {
        uint32_t v = t.ref.read(core);
        bool ok;
        if (t.op == Term::In) ok = v >= t.values[0] && v <= t.values[1];
        else {
            ok = std::find(t.values.begin(), t.values.end(), v) != t.values.end();
            if (t.op == Term::Ne) ok = !ok;
        }
        if (!ok) return false;
    }
    return true;
}

bool parse_profile(const std::string& text, const std::string& file, Profile& out, std::string& error) {
    out = Profile{};
    out.file = file;
    std::istringstream in(text);
    std::string line, section;
    int number = 0;
    auto fail = [&](const std::string& why) {
        error = file + ":" + std::to_string(number) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        // Elsewhere a comment is a '#' standing alone ("value  # why"), so #RRGGBB colours survive.
        for (size_t i = 1; i < line.size(); ++i)
            if (line[i] == '#' && std::isspace(static_cast<unsigned char>(line[i - 1])) &&
                (i + 1 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 1])))) {
                line = trim(line.substr(0, i));
                break;
            }
        if (line.front() == '[') {
            if (line.back() != ']') return fail("unclosed section");
            section = trim(line.substr(1, line.size() - 2));
            if (section != "game" && section != "companion" && section != "map" && section != "hud")
                return fail("unknown section [" + section + "]");
            if (section == "map") out.has_map = true;
            if (section == "hud") out.has_hud = true;
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        std::string why;
        bool ok = true;
        if (section == "game") {
            if (key == "title") out.title = value;
            else if (key == "codes") out.codes = words(upper(value));
            else if (key == "system") out.system = value;
            else return fail("unknown key '" + key + "' in [game]");
        } else if (section == "companion") {
            if (key == "live") ok = parse_condition(value, out.live, why);
            else if (key == "menu") ok = parse_condition(value, out.menu, why);
            else if (key == "room") {
                out.room.clear();
                for (const auto& w : words(value)) {
                    Ref r;
                    if (!parse_ref(w, r, why)) return fail(why);
                    out.room.push_back(r);
                }
            } else if (key == "interval") ok = parse_int(value, out.interval) && out.interval >= 10;
            else if (key == "dim_when_unavailable") out.dim_when_unavailable = value == "yes" || value == "true";
            else return fail("unknown key '" + key + "' in [companion]");
        } else if (section == "map") {
            auto& m = out.map;
            if (key == "open") ok = parse_steps(value, m.open, why) && !m.open.empty();
            else if (key == "release") ok = parse_int(value, m.release) && m.release >= 0 && m.release <= 60;
            else if (key == "timeout") ok = parse_int(value, m.timeout) && m.timeout > 0;
            else if (key == "stable") ok = parse_int(value, m.stable) && m.stable > 0;
            else if (key == "max_diff") ok = parse_int(value, m.max_diff) && m.max_diff >= 0;
            else if (key == "crop") ok = parse_rect(value, m.crop);
            else if (key == "ready") ok = parse_condition(value, m.ready, why);
            else if (key == "pixels") ok = parse_pixels(value, m.pixels);
            else if (key == "pixels_min") ok = parse_int(value, m.pixels_min) && m.pixels_min >= 0;
            else if (key == "tolerance") ok = parse_int(value, m.tolerance) && m.tolerance >= 0;
            else if (key == "background") ok = m.has_background = parse_number(value, m.background, true);
            else if (key == "min_foreground") ok = parse_int(value, m.min_foreground) && m.min_foreground >= 0;
            else if (key == "anim_frames")
                ok = parse_int(value, m.anim_frames) && m.anim_frames >= 1 && m.anim_frames <= 32;
            else if (key == "anim_step") ok = parse_int(value, m.anim_step) && m.anim_step >= 1;
            else if (key == "layers") {
                uint32_t mask;
                ok = parse_number(value, mask, true) && mask <= 0x1F;
                m.layers = mask;
            } else return fail("unknown key '" + key + "' in [map]");
        } else if (section == "hud") {
            auto& h = out.hud;
            if (key == "on") ok = parse_condition(value, h.on, why);
            else if (key == "keep") ok = parse_condition(value, h.keep, why);
            else if (key == "moves_when") ok = parse_condition(value, h.moves_when, why);
            else if (key == "bg") {
                uint32_t mask;
                ok = parse_number(value, mask, true) && mask <= 0xF;
                h.bg_mask = mask;
            } else if (key == "sprites") {
                if (value == "none") h.sprite_priority = -1;
                else ok = parse_int(value, h.sprite_priority) && h.sprite_priority >= 0 && h.sprite_priority <= 3;
            } else if (key == "backdrop") {
                h.backdrop_game = value == "game";
                if (!h.backdrop_game) ok = parse_number(value, h.backdrop, true);
            } else if (key == "backdrop_layers") {
                uint32_t mask;
                ok = parse_number(value, mask, true) && mask <= 0x1F && mask != 0;
                h.backdrop_layers = mask;
            } else if (key == "backdrop_rows") {
                ok = parse_int(value, h.backdrop_rows) && h.backdrop_rows >= 0 && h.backdrop_rows <= 1024;
            } else if (key == "panel") {
                // x y w h > X Y W H [trim]
                auto gt = value.find('>');
                HudPanel panel;
                auto to = gt == std::string::npos ? std::vector<std::string>{} : words(value.substr(gt + 1));
                if (to.size() == 5 && to[4] == "trim") {
                    panel.trim = true;
                    to.pop_back();
                }
                ok = gt != std::string::npos && parse_rect(value.substr(0, gt), panel.from) && to.size() == 4 &&
                     parse_rect(to[0] + " " + to[1] + " " + to[2] + " " + to[3], panel.to);
                if (ok) h.panels.push_back(panel);
                else why = "expected panel = x y w h > X Y W H [trim]";
            } else if (key == "to_game") {
                // x y w h > X Y
                auto gt = value.find('>');
                HudMove m;
                auto at = gt == std::string::npos ? std::vector<std::string>{} : words(value.substr(gt + 1));
                ok = gt != std::string::npos && parse_rect(value.substr(0, gt), m.from) && at.size() == 2 &&
                     parse_int(at[0], m.x) && parse_int(at[1], m.y);
                if (ok) h.to_game.push_back(m);
                else why = "expected to_game = x y w h > X Y";
            } else return fail("unknown key '" + key + "' in [hud]");
        } else return fail("key outside a section");
        if (!ok) return fail(why.empty() ? "bad value for '" + key + "'" : why);
    }
    if (out.codes.empty()) return fail("[game] codes is missing");
    if (out.title.empty()) return fail("[game] title is missing");
    if (out.has_hud && out.hud.on.empty()) return fail("[hud] on is missing");
    if (out.has_map) {
        if (out.map.open.empty()) return fail("[map] open is missing");
        if (out.map.ready.empty() && out.map.pixels.empty())
            return fail("[map] needs ready or pixels, or captures could never be checked");
    }
    return true;
}

void ProfileLibrary::load_directory(const std::string& directory, std::vector<std::string>& errors) {
    DIR* dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0) names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        std::string path = directory + "/" + name;
        std::ifstream f(path);
        std::stringstream text;
        text << f.rdbuf();
        Profile p;
        std::string error;
        if (!parse_profile(text.str(), path, p, error)) {
            errors.push_back(error);
            continue;
        }
        for (const auto& code : p.codes) profiles_[p.system + "/" + code] = p;
    }
}

const Profile* ProfileLibrary::find(const std::string& system, const std::string& code) const {
    std::string family = system == "gbc" ? "gb" : system;
    for (const auto& key : {system + "/" + upper(code), family + "/" + upper(code)}) {
        auto it = profiles_.find(key);
        if (it != profiles_.end()) return &it->second;
    }
    return nullptr;
}

} // namespace play
