#pragma once
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rgds {
namespace fs = std::filesystem;
struct Game {
    std::string title;
    fs::path path;
    bool demo = false;
    std::string system = "nds"; // "gba", "gbc", "gb": played by RG DS Play
};
inline constexpr const char* core_ids[] = {"drastic",           "dsperate",  "nnddss",    "melonds",
                                           "rgds-experimental", "rgds-play", "portmaster"};
inline constexpr const char* core_names[] = {"DraStic", "DSperate",   "NNDDSS",    "melonDS",
                                             "DSCore",  "RG DS Play", "PortMaster"};
inline constexpr int core_count = sizeof(core_ids) / sizeof(core_ids[0]);
// The DS engines come first; RG DS Play (appended, so stored indexes keep
// their meaning) plays every other system and is never offered for DS games.
inline constexpr int ds_core_count = 5, play_core = 5, dscore_core = 4;
// PortMaster (appended after RG DS Play) runs the Ports system's scripts.
inline constexpr int portmaster_core = 6;
// NNDDSS and melonDS are retired: they are never offered, and a saved choice of
// either falls back to DSCore. Indices stay stable because settings store them.
inline constexpr int retired_core = 2, melonds_core = 3;
inline constexpr bool core_retired(int core) { return core == retired_core || core == melonds_core; }
inline constexpr bool core_offered(int core) { return core >= 0 && core < ds_core_count && !core_retired(core); }
inline constexpr int offered_core(int core) { return core_retired(core) ? dscore_core : core; }
// The other systems RG DS Play opens, by file extension; the order of the Systems page.
struct SystemInfo {
    const char* id;
    const char* name;  // on the Systems page and in the library header
    const char* badge; // on a game without its own artwork
    std::initializer_list<const char*> extensions;
};
inline const std::array<SystemInfo, 12>& systems() {
    static const std::array<SystemInfo, 12> list = {{
        {"nds", "Nintendo DS", "DS", {".nds", ".dsi"}},
        {"gba", "Game Boy Advance", "GBA", {".gba"}},
        {"gbc", "Game Boy Color", "GBC", {".gbc"}},
        {"gb", "Game Boy", "GB", {".gb", ".sgb"}},
        // libretro systems through RG DS Play (runtime/play_options.py names the same extensions and the cores)
        {"nes", "Nintendo Entertainment System", "NES", {".nes", ".fds", ".unf", ".unif"}},
        {"snes", "Super Nintendo", "SNES", {".sfc", ".smc"}},
        {"md", "Mega Drive", "MD", {".md", ".gen", ".smd", ".32x"}},
        {"sms", "Master System", "SMS", {".sms", ".gg"}},
        {"psx", "PlayStation", "PS", {".cue", ".chd", ".pbp", ".m3u"}},
        {"n64", "Nintendo 64", "N64", {".z64", ".n64", ".v64"}},
        {"pico8", "PICO-8", "P8", {".p8"}},
        {"ports", "Ports", "PORT", {".sh"}}, // PortMaster ports: the scripts in its ports folder
    }};
    return list;
}
inline const SystemInfo& system_info(const std::string& id) {
    for (const auto& s : systems())
        if (id == s.id) return s;
    return systems()[0];
}
inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
// The system a game file is for, by its extension ("nds" for anything else, a .zip included).
inline std::string system_for_path(const fs::path& path) {
    const std::string extension = lower(path.extension().string());
    for (const auto& s : systems())
        for (const char* e : s.extensions)
            if (extension == e) return s.id;
    return "nds";
}
inline std::string library_title(std::string name) {
    // Remove recognized dump tags, retaining meaningful parenthetical titles.
    for (size_t at = 0; (at = name.find('(', at)) != std::string::npos;) {
        auto end = name.find(')', at);
        if (end == std::string::npos) break;
        auto tag = lower(name.substr(at + 1, end - at - 1));
        bool language = !tag.empty();
        for (size_t i = 0; i < tag.size(); i++)
            if (i % 3 == 2 ? tag[i] != ',' : tag[i] < 'a' || tag[i] > 'z') language = false;
        language = language && tag.size() % 3 == 2;
        const std::vector<std::string> regions = {
            "usa",     "europe", "japan",       "world",          "australia",   "canada", "france",
            "germany", "italy",  "spain",       "korea",          "china",       "taiwan", "norway",
            "denmark", "sweden", "netherlands", "united kingdom", "scandinavia", "russia", "ndsi enhanced"};
        if (language || tag.rfind("rev ", 0) == 0 || std::find(regions.begin(), regions.end(), tag) != regions.end())
            name.erase(at, end - at + 1);
        else at = end + 1;
    }
    if (name.size() > 5 && std::all_of(name.begin(), name.begin() + 4, [](char c) { return c >= '0' && c <= '9'; })) {
        auto start = name.find_first_not_of(' ', 4);
        if (start != std::string::npos && (name[start] == '-' || name[start] == '.')) name.erase(0, start + 1);
    }
    std::string clean;
    bool space = false;
    for (char c : name) {
        if (c == ' ' || c == '\t') {
            space = !clean.empty();
            continue;
        }
        if (space) clean += ' ';
        clean += c;
        space = false;
    }
    if (clean.size() > 5 && clean.compare(clean.size() - 5, 5, ", The") == 0)
        clean = "The " + clean.substr(0, clean.size() - 5);
    return clean.empty() ? name : clean;
}
inline std::vector<Game> scan(const std::vector<fs::path>& roots) {
    std::map<std::string, Game> unique;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            auto entry = *it;
            if (entry.is_symlink(ec)) {
                it.disable_recursion_pending();
            } else if (entry.is_directory(ec) && it.depth() == 0 &&
                       system_info(lower(entry.path().filename().string())).id != std::string("nds")) {
                // rgds/games/gba and the like belong to RG DS Play, not the DS list.
                it.disable_recursion_pending();
            } else if (entry.is_regular_file(ec)) {
                const auto ext = lower(entry.path().extension().string());
                if (ext == ".nds" || ext == ".dsi" || ext == ".zip") {
                    auto canonical = fs::weakly_canonical(entry.path(), ec);
                    if (!ec) unique[canonical.string()] = {entry.path().stem().string(), canonical, false};
                }
            }
            ec.clear();
            it.increment(ec);
            if (ec) break;
        }
    }
    std::vector<Game> games;
    for (const auto& pair : unique) games.push_back(pair.second);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        auto at = lower(a.title), bt = lower(b.title);
        return at == bt ? a.path.string() < b.path.string() : at < bt;
    });
    return games;
}
// Games of one other system (RG DS Play): its extensions, and .zip archives
// in its own folders. Not recursive into another system's folder.
inline std::vector<Game> scan_system(const std::vector<fs::path>& roots, const std::string& system) {
    const auto& info = system_info(system);
    // A port's own folder holds more scripts and archives: only the top level's scripts are ports.
    const bool ports = std::string(info.id) == "ports";
    std::map<std::string, Game> unique;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            auto entry = *it;
            if (entry.is_symlink(ec) || (ports && entry.is_directory(ec))) it.disable_recursion_pending();
            else if (entry.is_regular_file(ec)) {
                const auto ext = lower(entry.path().extension().string());
                bool wanted = ext == ".zip" && !ports;
                for (const char* e : info.extensions) wanted = wanted || ext == e;
                if (wanted) {
                    auto canonical = fs::weakly_canonical(entry.path(), ec);
                    if (!ec) unique[canonical.string()] = {entry.path().stem().string(), canonical, false, info.id};
                }
            }
            ec.clear();
            it.increment(ec);
            if (ec) break;
        }
    }
    std::vector<Game> games;
    for (const auto& pair : unique) games.push_back(pair.second);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        auto at = lower(a.title), bt = lower(b.title);
        return at == bt ? a.path.string() < b.path.string() : at < bt;
    });
    return games;
}
struct Model {
    std::vector<Game> games;
    size_t selected = 0;
    unsigned core = 0;
    void move(int delta) {
        if (games.empty()) return;
        auto n = static_cast<long>(games.size());
        selected = static_cast<size_t>((static_cast<long>(selected) + delta % n + n) % n);
    }
    void next_core() {
        do core = (core + 1) % core_count;
        while (!core_offered(core));
    }
};
struct Color {
    unsigned char r, g, b;
};
using Theme = std::map<std::string, Color>;
inline Theme default_theme() {
    return {{"background", {242, 245, 247}}, {"panel", {255, 255, 255}},     {"ink", {45, 57, 68}},
            {"muted", {94, 108, 120}},       {"accent", {35, 110, 174}},     {"line", {191, 204, 215}},
            {"slot", {221, 230, 237}},       {"on_accent", {255, 255, 255}}, {"focus", {224, 240, 252}}};
}
inline Theme default_dark_theme() {
    return {{"background", {12, 17, 26}}, {"panel", {23, 32, 46}},    {"ink", {233, 244, 255}},
            {"muted", {145, 167, 190}},   {"accent", {78, 187, 255}}, {"line", {45, 67, 90}},
            {"slot", {17, 26, 39}},       {"on_accent", {7, 21, 36}}, {"focus", {26, 61, 89}}};
}
inline Theme load_theme(const fs::path& path, Theme t = default_theme()) {
    if (path.empty()) return t;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot read theme: " + path.string());
    std::string line;
    bool schema = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto p = line.find('=');
        if (p == std::string::npos) throw std::runtime_error("Theme line lacks =");
        const auto key = line.substr(0, p), value = line.substr(p + 1);
        if (key == "schema") {
            if (value != "1") throw std::runtime_error("Unsupported theme schema");
            schema = true;
            continue;
        }
        if (key == "mode") {
            if (value != "light" && value != "dark") throw std::runtime_error("Invalid colour mode");
            continue;
        }
        if (!t.count(key) || value.size() != 6 ||
            value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Invalid theme field: " + key);
        const auto rgb = std::stoul(value, nullptr, 16);
        t[key] = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8),
                  static_cast<unsigned char>(rgb)};
    }
    if (!schema) throw std::runtime_error("Theme schema is missing");
    return t;
}
}
