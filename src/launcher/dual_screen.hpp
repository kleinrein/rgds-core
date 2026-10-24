#pragma once
// Which games RG DS Play gives a second screen of their own (a map, the team,
// the HUD): those with a companion profile. The player picks a profile by the
// cartridge's game code, or, for Game Boy carts too old to have one, by its
// header title (play/src/app/session.cpp); this finds the same file the same way.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace rgds {
namespace fs = std::filesystem;

namespace dual_screen_detail {
inline std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The codes in every profile of a system's folder ("codes = AZLE AZLP").
inline const std::set<std::string>& profile_codes(const fs::path& folder) {
    static std::map<fs::path, std::set<std::string>> cache;
    auto found = cache.find(folder);
    if (found != cache.end()) return found->second;
    std::set<std::string>& codes = cache[folder];
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".ini") continue;
        std::ifstream file(entry.path());
        for (std::string line; std::getline(file, line);) {
            auto eq = line.find('=');
            std::string key = line.substr(0, eq == std::string::npos ? 0 : eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            if (key != "codes") continue;
            std::istringstream words(line.substr(eq + 1));
            for (std::string code; words >> code && code[0] != '#';) codes.insert(upper(code));
        }
    }
    return codes;
}

// A Game Boy header title as the player names it: "POKEMON BLUE" -> POKEMON_BLUE.
inline std::string title_code(const unsigned char* header) {
    std::string out;
    for (int i = 0x134; i < 0x144; ++i) {
        unsigned char c = header[i];
        if (c == 0 || (i == 0x143 && (c & 0x80))) break;
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += char(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}
} // namespace dual_screen_detail

// `profiles` is RG DS Play's profiles folder (emulators/rgds-play/profiles in a
// release). Plain ROM files only: an archive would have to be opened to see its
// header, so a zipped game shows no mark. Answers are kept per path.
inline bool dual_screen_game(const fs::path& rom, const std::string& system, const fs::path& profiles) {
    using namespace dual_screen_detail;
    if (system == "nds") return false; // the DS has its own two screens
    static std::map<fs::path, bool> known;
    auto found = known.find(rom);
    if (found != known.end()) return found->second;
    bool& result = known[rom] = false;
    const std::string ext = upper(rom.extension().string());
    if (ext == ".ZIP" || ext == ".7Z") return result;
    unsigned char header[0x150] = {};
    std::ifstream file(rom, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(header), sizeof header)) return result;
    if (system == "gba") {
        const std::string code(reinterpret_cast<const char*>(header) + 0xAC, 4);
        return result = profile_codes(profiles / "gba").count(upper(code)) > 0;
    }
    // Game Boy and Game Boy Color profiles share one folder; a colour cart's code
    // sits at 13F, an older one's title runs into it.
    const auto& codes = profile_codes(profiles / "gb");
    const std::string code(reinterpret_cast<const char*>(header) + 0x13F, 4);
    return result = codes.count(upper(code)) > 0 || codes.count(title_code(header)) > 0;
}

} // namespace rgds
