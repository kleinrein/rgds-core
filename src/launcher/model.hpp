#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rgds {
namespace fs = std::filesystem;
struct Game { std::string title; fs::path path; bool demo = false; };
inline constexpr const char* core_ids[] = {"drastic", "dsperate", "nnddss"};
inline constexpr const char* core_names[] = {"DraStic", "DSperate", "NNDDSS"};
inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
inline std::vector<Game> scan(const std::vector<fs::path>& roots) {
    std::map<std::string, Game> unique;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            auto entry = *it;
            if (entry.is_symlink(ec)) { it.disable_recursion_pending(); }
            else if (entry.is_regular_file(ec)) {
                const auto ext = lower(entry.path().extension().string());
                if (ext == ".nds" || ext == ".dsi" || ext == ".zip") {
                    auto canonical = fs::weakly_canonical(entry.path(), ec);
                    if (!ec) unique[canonical.string()] = {entry.path().stem().string(), canonical, false};
                }
            }
            ec.clear(); it.increment(ec);
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
    void next_core() { core = (core + 1) % 3; }
};
struct Color { unsigned char r, g, b; };
using Theme = std::map<std::string, Color>;
inline Theme default_theme() {
    return {{"background", {241,245,251}}, {"panel",{255,255,255}}, {"ink",{32,48,74}},
        {"muted",{95,113,140}}, {"accent",{45,112,227}}, {"line",{219,228,240}}, {"slot",{229,237,248}},
        {"on_accent",{255,255,255}}, {"focus",{231,240,255}}};
}
inline Theme default_dark_theme() {
    return {{"background",{16,26,44}}, {"panel",{26,40,64}}, {"ink",{231,238,251}},
        {"muted",{158,176,204}}, {"accent",{142,184,255}}, {"line",{44,62,89}}, {"slot",{35,52,78}},
        {"on_accent",{16,26,44}}, {"focus",{36,57,88}}};
}
inline Theme load_theme(const fs::path& path,Theme t = default_theme()) {
    if (path.empty()) return t;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot read theme: " + path.string());
    std::string line; bool schema = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto p = line.find('=');
        if (p == std::string::npos) throw std::runtime_error("Theme line lacks =");
        const auto key = line.substr(0,p), value = line.substr(p+1);
        if (key == "schema") { if (value != "1") throw std::runtime_error("Unsupported theme schema"); schema = true; continue; }
        if (key == "mode") { if(value!="light" && value!="dark")throw std::runtime_error("Invalid colour mode");continue; }
        if (!t.count(key) || value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Invalid theme field: " + key);
        const auto rgb = std::stoul(value, nullptr, 16);
        t[key] = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8), static_cast<unsigned char>(rgb)};
    }
    if (!schema) throw std::runtime_error("Theme schema is missing");
    return t;
}
}
