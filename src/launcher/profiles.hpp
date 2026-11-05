#pragma once
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace rgds {
namespace fs = std::filesystem;
// Whose files: the device root (userdata) and the active profile's player root, as in runtime/profiles.py, which says
// why. A player's look, saves and per-game settings are under the player root; artwork, Wi-Fi and session files under
// the device root. For the owner both are the same folder, so every path is what it was before profiles.
inline bool profile_id(const std::string& id) {
    if (id.size() < 2 || id[0] != 'p') return false;
    for (size_t i = 1; i < id.size(); i++)
        if (id[i] < '0' || id[i] > '9') return false;
    return true;
}
// Sets unclear when profiles exist but the active one cannot be found: the look then comes from the device root, and
// the runtime refuses to start a game, so nothing is written to the wrong saves.
inline fs::path player_root(const fs::path& data, bool* unclear = nullptr) {
    if (unclear) *unclear = false;
    if (data.empty()) return data;
    const auto folder = data / "profiles";
    std::error_code ec;
    std::ifstream file(folder / "active");
    std::string active;
    if (!file) {
        if (fs::exists(folder / "index.ini", ec) && unclear) *unclear = true;
        return data;
    }
    std::getline(file, active);
    while (!active.empty() && (active.back() == '\r' || active.back() == ' ')) active.pop_back();
    if (active == "owner") return data;
    if (profile_id(active) && fs::is_directory(folder / active, ec)) return folder / active;
    std::cerr << "Profiles: the active profile is missing" << std::endl;
    if (unclear) *unclear = true;
    return data;
}
}
