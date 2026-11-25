#pragma once
// The second card (the RG DS Plus's TF2 slot): a games library beside /data/roms while it is in. RG DS OS mounts it
// read-write at /media/tf2 and says so in /run/rgds/games-card.ini (usr/libexec/rgds/games-card), which the session
// names with --games-card. Its roms/<system> folders (this card's layout) and the stock firmware's Roms/<SYS> ones
// join the library's own roots; nothing is copied. Saves and states are kept by system and name, play time and
// favourites by the game's path (runtime/library.py game_key), all in userdata on the system card: a card taken out
// takes its games out of the library and nothing else, and the same card put back brings them back as they were.
#include "launcher/model.hpp"
#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rgds {

struct GamesCard {
    fs::path point;
    std::string device, fs, id, label;
};

// The state file's text, as the change check compares it ("" with no card).
inline std::string games_card_text(const fs::path& state) {
    if (state.empty()) return {};
    std::ifstream in(state);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The card the state file names, while its folder is there.
inline std::optional<GamesCard> parse_games_card(const std::string& text) {
    GamesCard card;
    std::stringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "point") card.point = value;
        else if (key == "device") card.device = value;
        else if (key == "fs") card.fs = value;
        else if (key == "id") card.id = value;
        else if (key == "label") card.label = value;
    }
    std::error_code ec;
    if (card.point.empty() || !fs::is_directory(card.point, ec)) return std::nullopt;
    return card;
}

// The card's games folders, as library roots: ("nds", path) for the DS ones, (shelf, path) for the rest. Only the
// shelves in `shelves` (the session's own --system-dir systems, so a 3DS folder needs the 3DS engine as /data/roms/3ds
// does), in name order. The roms folder is found whatever its case: "roms" on a card set up like ours, "Roms" on a
// stock one.
inline std::vector<std::pair<std::string, fs::path>> games_card_folders(const fs::path& card,
                                                                        const std::set<std::string>& shelves) {
    std::vector<std::pair<std::string, fs::path>> found;
    std::error_code ec;
    fs::path roms;
    for (fs::directory_iterator it(card, ec), end; !ec && it != end; it.increment(ec))
        if (lower(it->path().filename().string()) == "roms" && it->is_directory(ec)) {
            roms = it->path();
            break;
        }
    if (roms.empty()) return found;
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(roms, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && name[0] != '.' && it->is_directory(ec)) folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        if (lower(folder.filename().string()) == "nds") {
            found.emplace_back("nds", folder);
            continue;
        }
        const auto* known = games_folder_in(folder);
        if (known && shelves.count(known->shelf)) found.emplace_back(known->shelf, folder);
    }
    return found;
}

} // namespace rgds
