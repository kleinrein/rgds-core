// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/cheats.hpp"

#include "app/storage.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace play {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t at = 0;
    while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
    return s.substr(at);
}

// `key = "value"` or `key = value`.
bool split_line(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = trim(line.substr(0, eq));
    value = trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return !key.empty();
}

bool exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace

std::vector<CheatCode> parse_cht(const std::string& text) {
    std::map<int, CheatCode> found;
    std::istringstream in(text);
    std::string line, key, value;
    while (std::getline(in, line)) {
        if (!split_line(line, key, value) || key.rfind("cheat", 0) != 0 || key == "cheats") continue;
        char* end = nullptr;
        const long number = std::strtol(key.c_str() + 5, &end, 10);
        if (end == key.c_str() + 5 || number < 0 || number > 100000 || *end != '_') continue;
        const std::string field = end + 1;
        CheatCode& c = found[int(number)];
        if (field == "desc") c.name = value;
        else if (field == "code") c.code = value;
        else if (field == "enable") c.on = value == "true";
    }
    std::vector<CheatCode> out;
    for (auto& [number, c] : found) {
        if (c.code.empty()) continue;
        if (c.name.empty()) c.name = "Code " + std::to_string(number + 1);
        out.push_back(std::move(c));
    }
    return out;
}

bool CheatBook::load(const std::string& root, const std::string& system, const std::string& rom_path) {
    codes_.clear();
    path_.clear();
    const std::string game = file_stem(rom_path);
    const std::string folder = root + "/cheats/" + system;
    memory_ = folder + "/" + game + ".on";
    for (const std::string& candidate : {folder + "/" + game + ".cht", parent_directory(rom_path) + "/" + game + ".cht",
                                         root + "/cheats/" + game + ".cht"})
        if (exists(candidate)) {
            path_ = candidate;
            break;
        }
    if (path_.empty()) return false;
    std::ifstream f(path_);
    std::stringstream text;
    text << f.rdbuf();
    codes_ = parse_cht(text.str());
    // An enable flag in the file is the starting choice; once the player has chosen, its memory wins.
    std::ifstream remembered(memory_);
    if (remembered.good()) {
        std::set<std::string> names;
        for (std::string name; std::getline(remembered, name);) names.insert(name);
        for (auto& c : codes_) c.on = names.count(c.name) > 0;
    }
    return !codes_.empty();
}

bool CheatBook::set(size_t index, bool on) {
    if (index >= codes_.size()) return false;
    codes_[index].on = on;
    remember();
    return true;
}

void CheatBook::remember() const {
    std::string text;
    for (const auto& c : codes_)
        if (c.on) text += c.name + "\n";
    std::string error;
    make_directories(parent_directory(memory_));
    if (!write_file_atomic(memory_, std::vector<uint8_t>(text.begin(), text.end()), error))
        std::fprintf(stderr, "play: cheat choices not saved: %s\n", error.c_str());
}

} // namespace play
