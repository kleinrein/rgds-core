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
    if (!parse_address(base.substr(open + 1, close - open - 1), out.address)) {
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

// An address in hex: 03000BDE, or banked as BANK:ADDRESS (1C:590D, 1:DCD7) for a
// Game Boy's ROM or work RAM bank (core.hpp's banked()).
bool parse_address(const std::string& text, uint32_t& out) {
    std::string s = trim(text);
    auto colon = s.find(':');
    if (colon == std::string::npos) return parse_number(s, out, true);
    uint32_t bank = 0, address = 0;
    if (!parse_number(s.substr(0, colon), bank, true) || !parse_number(s.substr(colon + 1), address, true) ||
        bank > 0xFF || address > 0xFFFF)
        return false;
    out = banked(int(bank), address);
    return true;
}

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
        const uint32_t a = t.ref.address;
        if (a & banked_flag)
            std::snprintf(text, sizeof(text), "%smem%d[%X:%04X]=%X", out.empty() ? "" : " ", t.ref.size * 8,
                          unsigned((a >> 16) & 0xFF), unsigned(a & 0xFFFF), unsigned(t.ref.read(core)));
        else
            std::snprintf(text, sizeof(text), "%smem%d[%08X]=%X", out.empty() ? "" : " ", t.ref.size * 8, unsigned(a),
                          unsigned(t.ref.read(core)));
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
    bool alt = false;         // in a [map.<name>]
    ViewSpec* view = nullptr; // the [view] or [view.<name>] being read
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
            const bool alt_map = section.rfind("map.", 0) == 0 && section.size() > 4;
            const bool alt_view = section.rfind("view.", 0) == 0 && section.size() > 5;
            if (section != "game" && section != "companion" && section != "map" && section != "hud" &&
                section != "party" && section != "view" && !alt_map && !alt_view)
                return fail("unknown section [" + section + "]");
            if (section == "view" || alt_view) {
                // Several views: the first in the file whose `on` holds is shown.
                out.views.emplace_back();
                out.views.back().name = alt_view ? section.substr(5) : "";
                view = &out.views.back();
                section = "view";
                out.has_view = true;
                alt = false;
                continue;
            }
            if (alt_map) {
                out.alt_maps.emplace_back();
                section = "map";
                alt = true;
                continue;
            }
            alt = false;
            if (section == "map") out.has_map = true;
            if (section == "hud") out.has_hud = true;
            if (section == "party") out.has_party = true;
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
            else if (key == "note") out.note = value;
            else return fail("unknown key '" + key + "' in [companion]");
        } else if (section == "map") {
            auto& m = alt ? out.alt_maps.back() : out.map;
            if (key == "when" && alt) ok = parse_condition(value, m.when, why);
            else if (key == "open") ok = parse_steps(value, m.open, why) && !m.open.empty();
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
            } else if (key == "bg_priority_below") {
                ok = parse_int(value, h.bg_priority_below) && h.bg_priority_below >= 1 && h.bg_priority_below <= 4;
            } else if (key == "map_cnt") {
                ok = parse_number(value, h.map_cnt, true) && h.map_cnt <= 0xFFFF && h.map_cnt != 0;
            } else if (key == "window_effects_off") {
                ok = parse_number(value, h.window_effects_off, true) && h.window_effects_off <= 3;
            } else if (key == "sprite_tiles") {
                // lo-hi [priority]
                auto w = words(value);
                Core::ObjRule r;
                auto dash = w.empty() ? std::string::npos : w[0].find('-');
                ok = dash != std::string::npos && w.size() <= 2 && parse_int(w[0].substr(0, dash), r.lo) &&
                     parse_int(w[0].substr(dash + 1), r.hi) && r.lo >= 0 && r.hi <= 1023 && r.lo <= r.hi &&
                     (w.size() == 1 || (parse_int(w[1], r.priority) && r.priority >= 0 && r.priority <= 3));
                if (ok) h.sprite_tiles.push_back(r);
                else why = "expected sprite_tiles = lo-hi [priority]";
            } else if (key == "backdrop") {
                h.backdrop_game = value == "game";
                h.backdrop_theme = value == "theme";
                if (!h.backdrop_game && !h.backdrop_theme) ok = parse_number(value, h.backdrop, true);
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
            } else if (key == "strip") {
                // x y w h, whole rows at the top or bottom of the picture
                ok = parse_rect(value, h.strip) && h.strip.x == 0 && h.strip.y >= 0;
                if (!ok) why = "expected strip = 0 y w h";
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
        } else if (section == "party") {
            auto& p = out.party;
            if (key == "format") {
                p.format = value;
                ok = value == "pokemon1" || value == "pokemon2" || value == "pokemon3" || value == "goldensun" ||
                     value == "saga1" || value == "dwm1";
            } else if (key == "count") ok = parse_address(value, p.count);
            else if (key == "slots") ok = parse_address(value, p.slots);
            else if (key == "icons") ok = parse_address(value, p.icons);
            else if (key == "icon_palette_index") ok = parse_address(value, p.icon_palette_index);
            else if (key == "icon_palettes") ok = parse_address(value, p.icon_palettes);
            else if (key == "names") ok = parse_address(value, p.names);
            else if (key == "dex_order") ok = parse_address(value, p.dex_order);
            else if (key == "icon_kinds") ok = parse_address(value, p.icon_kinds);
            else if (key == "on") ok = parse_condition(value, p.on, why);
            else return fail("unknown key '" + key + "' in [party]");
        } else if (section == "view") {
            if (key == "format") view->format = value;
            else if (key == "on") ok = parse_condition(value, view->on, why);
            else view->values[key] = value; // the format's own keys, checked when the view is made
        } else return fail("key outside a section");
        if (!ok) return fail(why.empty() ? "bad value for '" + key + "'" : why);
    }
    if (out.codes.empty()) return fail("[game] codes is missing");
    if (out.title.empty()) return fail("[game] title is missing");
    if (out.has_hud && out.hud.on.empty()) return fail("[hud] on is missing");
    for (const auto& v : out.views)
        if (v.format.empty()) return fail("[view" + (v.name.empty() ? "" : "." + v.name) + "] format is missing");
    if (!out.views.empty()) out.view = out.views.front();
    if (out.has_party &&
        (out.party.format.empty() || (!out.party.count && out.party.format != "saga1" && out.party.format != "dwm1") ||
         !out.party.slots))
        return fail("[party] needs format, count and slots");
    if (out.has_map) {
        if (out.map.open.empty()) return fail("[map] open is missing");
        if (out.map.ready.empty() && out.map.pixels.empty())
            return fail("[map] needs ready or pixels, or captures could never be checked");
    }
    for (const auto& m : out.alt_maps) {
        if (!out.has_map) return fail("a [map.<name>] needs a [map] to fall back on");
        if (m.when.empty()) return fail("a [map.<name>] needs when");
        if (m.open.empty()) return fail("a [map.<name>] needs open");
        if (m.ready.empty() && m.pixels.empty()) return fail("a [map.<name>] needs ready or pixels");
    }
    return true;
}

const MapSpec& Profile::map_for(Core& live) const {
    for (const auto& m : alt_maps)
        if (m.when.match(live)) return m;
    return map;
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
