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
// DSCore is the only DS engine. DraStic, DSperate, NNDDSS and melonDS are retired:
// never offered, and a saved choice of any falls back to DSCore. Indices stay
// stable because settings store them.
inline constexpr bool core_retired(int core) { return core >= 0 && core < ds_core_count && core != dscore_core; }
inline constexpr bool core_offered(int core) { return core >= 0 && core < ds_core_count && !core_retired(core); }
inline constexpr int offered_core(int core) { return core_retired(core) ? dscore_core : core; }
// The other systems RG DS Play opens, by file extension; the order of the Systems page. An extension another system
// shares (a .bin, a .rom, a disc's .cue) is in folder_extensions: the system's only in a folder named for it, as
// runtime/play_options.py's FOLDER_SYSTEMS and FOLDER_EXTENSIONS have it, so the two always agree.
struct SystemInfo {
    const char* id;
    const char* name;  // on the Systems page and in the library header
    const char* badge; // on a game without its own artwork
    std::initializer_list<const char*> extensions;
    std::initializer_list<const char*> folder_extensions = {};
};
inline const std::array<SystemInfo, 25>& systems() {
    static const std::array<SystemInfo, 25> list = {{
        {"nds", "Nintendo DS", "DS", {".nds", ".dsi"}},
        {"gba", "Game Boy Advance", "GBA", {".gba"}},
        {"gbc", "Game Boy Color", "GBC", {".gbc"}},
        {"gb", "Game Boy", "GB", {".gb", ".sgb"}},
        // libretro systems through RG DS Play (runtime/play_options.py names the same extensions and the cores)
        {"nes", "Nintendo Entertainment System", "NES", {".nes", ".fds", ".unf", ".unif"}},
        {"snes", "Super Nintendo", "SNES", {".sfc", ".smc"}},
        // and the Mega-CD's discs and the Pico's cartridges (play_subsystem)
        {"md", "Mega Drive", "MD", {".md", ".gen", ".smd", ".32x"}, {".bin", ".cue", ".chd", ".m3u", ".iso"}},
        {"sms", "Master System", "SMS", {".sms", ".gg", ".sg", ".sc"}}, // and the Game Gear, SG-1000 and SC-3000
        // and its CD-ROM discs (play_subsystem)
        {"pce", "PC Engine", "PCE", {".pce"}, {".cue", ".chd", ".m3u", ".ccd", ".toc"}},
        {"psx", "PlayStation", "PS", {".cue", ".chd", ".pbp", ".m3u"}},
        {"n64", "Nintendo 64", "N64", {".z64", ".n64", ".v64"}},
        {"zx", "ZX Spectrum", "ZX", {".tzx", ".tap", ".z80", ".sna", ".szx", ".dsk", ".trd", ".scl"}},
        {"c64",
         "Commodore 64",
         "C64",
         {".d64", ".g64", ".d71", ".d81", ".t64", ".crt", ".prg", ".p00", ".x64"},
         {".tap", ".m3u"}},
        {"3ds", "Nintendo 3DS", "3DS", {".3ds", ".cci", ".cxi", ".3dsx"}}, // Tanuki3DS (emulators.tanuki_plan)
        {"psp", "PlayStation Portable", "PSP", {".iso", ".cso"}, {".pbp", ".chd"}},
        {"coleco", "ColecoVision", "COLECO", {".col", ".cv"}, {".rom", ".bin"}},  // Gearcoleco
        {"intellivision", "Intellivision", "INT", {".int"}, {".rom", ".bin"}},    // FreeIntv
        {"msx", "MSX", "MSX", {".mx1", ".mx2", ".cas"}, {".rom", ".dsk", ".m3u"}}, // blueMSX
        {"atari", "Atari 2600", "2600", {".a26"}, {".bin"}},                       // Stella 2014
        {"amiga", "Amiga", "AMIGA", {".adf", ".adz", ".dms", ".ipf", ".hdf", ".hdz", ".lha", ".uae"}, {".m3u"}}, // PUAE
        {"pico8", "PICO-8", "P8", {".p8"}, {".png"}}, // fake-08: .p8 carts, and .p8.png picture carts in its folder
        {"gw", "Game & Watch", "G&W", {".mgw"}}, // and other LCD games: MADrigal's simulators (gw-libretro)
        {"scummvm", "ScummVM", "SCUMM", {}}, // a game is a folder of files: scan_system
        {"ports", "Ports", "PORT", {".sh"}}, // PortMaster ports: the scripts in its ports folder
        // not a system with games of its own: the shelves of free DS, GBA and NES homebrew (homebrew_screen.hpp)
        {"homebrew", "Homebrew", "FREE", {}},
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
inline bool listed(std::initializer_list<const char*> list, const std::string& ext) {
    for (const char* e : list)
        if (ext == e) return true;
    return false;
}
// A game packed in a .zip or .7z: RG DS Play unpacks it for its cores, the runtime a DS one (emulators.resolve_rom).
inline bool is_archive(const fs::path& path) {
    const auto ext = lower(path.extension().string());
    return ext == ".zip" || ext == ".7z";
}
// The systems a games folder's name stands for (lower case), as play_options.FOLDER_SYSTEMS has them: the shelf, and
// the player's own system where it shares one ("" for the shelf's own).
struct GamesFolder {
    const char* name;
    const char* shelf;
    const char* system;
};
inline const GamesFolder* games_folder(const std::string& name) {
    static const GamesFolder folders[] = {
        {"gba", "gba", ""},
        {"gbc", "gbc", ""},
        {"gb", "gb", ""},
        {"nes", "nes", ""},
        {"snes", "snes", ""},
        {"md", "md", ""},
        {"genesis", "md", ""},
        {"megadrive", "md", ""},
        {"mega drive", "md", ""},
        {"segacd", "md", "segacd"},
        {"megacd", "md", "segacd"},
        {"mega cd", "md", "segacd"},
        {"mega-cd", "md", "segacd"},
        {"sega cd", "md", "segacd"},
        {"pico", "md", "pico"},
        {"sega pico", "md", "pico"},
        {"sms", "sms", ""},
        {"pce", "pce", ""},
        {"pc engine", "pce", ""},
        {"pcengine", "pce", ""},
        {"tg16", "pce", ""},
        {"turbografx", "pce", ""},
        {"turbografx-16", "pce", ""},
        {"pcecd", "pce", "pcecd"},
        {"pce-cd", "pce", "pcecd"},
        {"pc engine cd", "pce", "pcecd"},
        {"tg16cd", "pce", "pcecd"},
        {"turbografx-cd", "pce", "pcecd"},
        {"psx", "psx", ""},
        {"n64", "n64", ""},
        {"zx", "zx", ""},
        {"c64", "c64", ""},
        {"commodore", "c64", ""},
        {"commodore 64", "c64", ""},
        {"3ds", "3ds", ""},
        {"psp", "psp", ""},
        {"coleco", "coleco", ""},
        {"colecovision", "coleco", ""},
        {"intellivision", "intellivision", ""},
        {"intv", "intellivision", ""},
        {"msx", "msx", ""},
        {"msx1", "msx", ""},
        {"msx2", "msx", ""},
        {"atari", "atari", ""},
        {"atari2600", "atari", ""},
        {"atari 2600", "atari", ""},
        {"2600", "atari", ""},
        {"amiga", "amiga", ""},
        {"pico8", "pico8", ""},
        {"pico-8", "pico8", ""},
        {"fake08", "pico8", ""},
        {"gw", "gw", ""},
    };
    for (const auto& f : folders)
        if (name == f.name) return &f;
    return nullptr;
}
// The nearest games folder above a game, or null.
inline const GamesFolder* games_folder_of(const fs::path& path) {
    for (auto folder = path.parent_path(); folder.has_relative_path(); folder = folder.parent_path())
        if (const auto* f = games_folder(lower(folder.filename().string()))) return f;
    return nullptr;
}
// The system a game file is for: its games folder's when that folder claims the file (any archive, an extension the
// system shares only there), else by its extension ("nds" for anything else).
inline std::string system_for_path(const fs::path& path) {
    const std::string extension = lower(path.extension().string());
    for (auto folder = path.parent_path(); folder.has_relative_path(); folder = folder.parent_path())
        if (lower(folder.filename().string()) == "scummvm") return "scummvm"; // any file there is a game's
    if (const auto* folder = games_folder_of(path)) {
        const auto& info = system_info(folder->shelf);
        if (is_archive(path) || listed(info.extensions, extension) || listed(info.folder_extensions, extension))
            return folder->shelf;
    }
    for (const auto& s : systems())
        if (listed(s.extensions, extension)) return s.id;
    return "nds";
}
// The player's own system for a game on a shared shelf, as runtime/play_options.py's system_of has it: the Game Gear,
// the SG-1000 and SC-3000 on the Master System's; the Mega-CD (its folder, or a disc among the Mega Drive's) and the
// Pico (its folder) on the Mega Drive's; the CD-ROM (a disc) on the PC Engine's.
inline std::string play_subsystem(const fs::path& path, const std::string& system) {
    const std::string ext = lower(path.extension().string());
    if (system == "sms" && ext == ".gg") return "gg";
    if (system == "sms" && (ext == ".sg" || ext == ".sc")) return "sg";
    const auto* folder = games_folder_of(path);
    const std::string own = folder && folder->shelf == system ? folder->system : "";
    if (system == "md") {
        if (!own.empty()) return own;
        if (ext != ".bin" && listed(system_info("md").folder_extensions, ext)) return "segacd";
    }
    if (system == "pce" && (listed(system_info("pce").folder_extensions, ext) || (own == "pcecd" && is_archive(path))))
        return "pcecd";
    return system;
}
// The shelf a player's system is on.
inline std::string shelf_of(const std::string& system) {
    if (system == "gg" || system == "sg") return "sms";
    if (system == "segacd" || system == "pico") return "md";
    if (system == "pcecd") return "pce";
    return system;
}
// Systems whose controller the player draws on the touch screen (play/src/companion/keypad.cpp).
inline bool has_touch_controller(const std::string& system) {
    return system == "coleco" || system == "intellivision" || system == "pico" || system == "atari";
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
                // not the AppleDouble files macOS writes beside each game on a card it mounts
                if ((ext == ".nds" || ext == ".dsi" || is_archive(entry.path())) &&
                    entry.path().filename().string().rfind("._", 0) != 0) {
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
// Games of one other system (RG DS Play): its extensions, and .zip and .7z archives
// in its own folders. Not recursive into another system's folder.
inline std::vector<Game> scan_system(const std::vector<fs::path>& roots, const std::string& system) {
    const auto& info = system_info(system);
    // A port's own folder holds more scripts and archives: only the top level's scripts are ports.
    const bool ports = std::string(info.id) == "ports";
    // A ScummVM game is a folder in the games folder, named as the game; the core finds the game from any file in it.
    if (std::string(info.id) == "scummvm") {
        std::vector<Game> games;
        for (const auto& root : roots) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;
            for (const auto& folder : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
                if (!folder.is_directory(ec) || folder.path().filename().string().rfind(".", 0) == 0) continue;
                fs::path first;
                int first_depth = 0;
                std::error_code walk;
                fs::recursive_directory_iterator it(folder.path(), fs::directory_options::skip_permission_denied, walk), end;
                for (; !walk && it != end; it.increment(walk)) {
                    const auto name = it->path().filename().string();
                    if (!it->is_regular_file(walk) || name.rfind(".", 0) == 0) continue;
                    // A file at the shallowest level with any (a set may unpack with a folder of its own inside),
                    // the core looks beside it. A hook wins.
                    const bool hook = lower(it->path().extension().string()) == ".scummvm";
                    if (lower(first.extension().string()) == ".scummvm") continue;
                    if (first.empty() || hook || it.depth() < first_depth ||
                        (it.depth() == first_depth && it->path() < first)) {
                        first = it->path();
                        first_depth = it.depth();
                    }
                }
                if (!first.empty()) {
                    auto canonical = fs::weakly_canonical(first, ec);
                    if (!ec) games.push_back({folder.path().filename().string(), canonical, false, info.id});
                }
            }
        }
        std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return lower(a.title) < lower(b.title); });
        return games;
    }
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
                bool wanted = is_archive(entry.path()) && !ports;
                // An extension the system shares counts here when no other system's folder claims the file.
                wanted = wanted || listed(info.extensions, ext) || listed(info.folder_extensions, ext);
                if (wanted && !is_archive(entry.path()) && system_for_path(entry.path()) != info.id) wanted = false;
                // A Mega-CD folder's games are its discs: a track no sheet lists is not a Mega Drive cartridge.
                if (wanted && play_subsystem(entry.path(), info.id) == "segacd" && ext == ".bin") wanted = false;
                // not the AppleDouble files macOS writes beside each game on a drive it mounts
                if (wanted && entry.path().filename().string().rfind("._", 0) != 0) {
                    auto canonical = fs::weakly_canonical(entry.path(), ec);
                    // A PICO-8 picture cart is name.p8.png: its title is the name.
                    std::string title = entry.path().stem().string();
                    if (title.size() > 3 && lower(title.substr(title.size() - 3)) == ".p8") title.resize(title.size() - 3);
                    if (!ec) unique[canonical.string()] = {title, canonical, false, info.id};
                }
            }
            ec.clear();
            it.increment(ec);
            if (ec) break;
        }
    }
    // A game on several discs or disks is its .m3u: the files it lists are not games of their own.
    for (const auto& [path, game] : std::map<std::string, Game>(unique)) {
        if (lower(game.path.extension().string()) != ".m3u") continue;
        std::ifstream list(game.path);
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::error_code ec;
            const auto listed = fs::weakly_canonical(game.path.parent_path() / line.substr(0, line.find('|')), ec);
            if (!ec && listed != game.path) unique.erase(listed.string());
        }
    }
    // A disc is its .cue: its tracks (a Mega-CD's .bin, on the Mega Drive's shelf) are not games of their own.
    for (const auto& [path, game] : std::map<std::string, Game>(unique)) {
        if (lower(game.path.extension().string()) != ".cue") continue;
        std::ifstream sheet(game.path);
        for (std::string line; std::getline(sheet, line);) {
            // FILE "Track 01.bin" BINARY, or unquoted: FILE track01.bin BINARY
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const auto at = line.find("FILE ");
            if (at == std::string::npos) continue;
            std::string name = line.substr(at + 5);
            while (!name.empty() && name.front() == ' ') name.erase(0, 1);
            if (!name.empty() && name.front() == '"') {
                const auto close = name.find('"', 1);
                if (close == std::string::npos) continue;
                name = name.substr(1, close - 1);
            } else if (const auto space = name.rfind(' '); space != std::string::npos) {
                name = name.substr(0, space);
            }
            if (name.empty()) continue;
            std::error_code ec;
            const auto track = fs::weakly_canonical(game.path.parent_path() / name, ec);
            if (!ec && track != game.path) unique.erase(track.string());
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
    unsigned core = dscore_core;
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
