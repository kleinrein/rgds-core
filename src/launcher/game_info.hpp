#pragma once
#include "launcher/age_rating.hpp"
#include "launcher/media.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <istream>

namespace rgds {
// What runtime/game_info.py saved for a game, read for the game info page. Everything is optional: a game
// without any of it shows nothing extra, and nothing here changes how the library itself looks.
struct GameFacts {
    std::string title, developer, publisher, genre, release, rating, franchise, players, features, provider;
    std::string synopsis, synopsis_language;
    bool online = false;
    bool any() const {
        return !(title.empty() && developer.empty() && publisher.empty() && genre.empty() && release.empty() &&
                 rating.empty() && franchise.empty() && players.empty() && synopsis.empty());
    }
    // The short facts as label/value pairs, in display order; empty values are left out.
    std::vector<std::pair<std::string, std::string>> rows() const {
        std::vector<std::pair<std::string, std::string>> all = {
            {"Developer", developer}, {"Publisher", publisher}, {"Genre", genre},     {"Released", release},
            {"Rating", rating},       {"Players", players},     {"Series", franchise}};
        if (online) all.emplace_back("Online", features.empty() || features == "online" ? "Yes" : features);
        all.erase(std::remove_if(all.begin(), all.end(), [](const auto& row) { return row.second.empty(); }),
                  all.end());
        return all;
    }
};
// The description languages GameTDB uses, by the launcher's locale id ("nb-NO" -> "NO").
inline std::string game_info_language(const std::string& locale) {
    const auto dash = locale.find('-');
    const std::string base = locale.substr(0, dash);
    if (base == "da") return "DK";
    if (base == "sv") return "SE";
    if (base == "nb" || base == "nn" || base == "no") return "NO";
    std::string code;
    for (char c : base) code += char(std::toupper(static_cast<unsigned char>(c)));
    return code;
}
inline std::string read_text_file(const fs::path& path, size_t limit = 16 * 1024) {
    std::ifstream file(path, std::ios::binary);
    std::string text(limit, '\0');
    file.read(text.data(), std::streamsize(limit));
    text.resize(size_t(file.gcount()));
    // Never cut a UTF-8 sequence in half.
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (file.gcount() == std::streamsize(limit) && !text.empty() && (static_cast<unsigned char>(text.back()) & 0x80))
        text.pop_back();
    return text;
}
inline GameFacts load_game_facts(const fs::path& folder, const std::string& locale) {
    GameFacts facts;
    auto info = fields(folder / "info.ini");
    if (info["info_version"].empty()) return facts;
    facts.title = info["info_title"];
    facts.developer = info["developer"];
    facts.publisher = info["publisher"];
    facts.genre = info["genre"];
    facts.release = info["release"];
    facts.rating = minimum_age(info["rating"]) >= 0 ? info["rating"] : "";
    facts.franchise = info["franchise"];
    facts.players = info["players"];
    facts.features = info["features"];
    facts.online = info["online"] == "1";
    facts.provider = info["info_provider"];
    // Description: the interface language, then English, then whatever the database has.
    std::vector<std::string> languages = {game_info_language(locale), "EN"};
    std::istringstream listed(info["synopsis_languages"]);
    for (std::string language; std::getline(listed, language, ',');)
        if (!language.empty()) languages.push_back(language);
    for (const auto& language : languages) {
        if (language.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) continue;
        auto text = read_text_file(folder / ("synopsis." + language + ".txt"));
        if (text.empty()) continue;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        facts.synopsis = text;
        facts.synopsis_language = language;
        break;
    }
    return facts;
}
// RetroAchievements for one game, as rgds-play last wrote it to <player>/library/<game>/achievements.tsv:
//   #rgds-achievements 1
//   game <id> <title> <unlocked> <total> <points unlocked> <points total> <account> <written>
//   ach <id> <unlocked 0|1> <points> <unlock time or 0> <rarity or empty> <title> <description> <badge> <progress>
// Tab separated; line kinds this build does not know are skipped. total 0: the site knows the game but it has no
// achievements.
struct Achievement {
    std::string id, title, description, rarity, badge, progress;
    bool unlocked = false;
    int points = 0;
    long long unlocked_at = 0;
};
struct GameAchievements {
    bool known = false; // a "game" line was read
    std::string game_id, title, account;
    int unlocked = 0, total = 0, points = 0, points_total = 0;
    long long written = 0;
    std::vector<Achievement> list; // unlocked first (latest first), then locked in the set's order
};
inline std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> parts;
    for (size_t from = 0;;) {
        const auto tab = line.find('\t', from);
        parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos) break;
        from = tab + 1;
    }
    return parts;
}
inline long long achievement_number(const std::string& text) {
    long long value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= 0 ? value : 0;
}
inline GameAchievements parse_achievements(std::istream& in) {
    GameAchievements result;
    std::string line;
    if (!std::getline(in, line) || line.rfind("#rgds-achievements", 0) != 0) return result;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto parts = split_tabs(line);
        if (parts[0] == "game" && parts.size() >= 9) {
            result.known = true;
            result.game_id = parts[1];
            result.title = parts[2];
            result.unlocked = int(achievement_number(parts[3]));
            result.total = int(achievement_number(parts[4]));
            result.points = int(achievement_number(parts[5]));
            result.points_total = int(achievement_number(parts[6]));
            result.account = parts[7];
            result.written = achievement_number(parts[8]);
        } else if (parts[0] == "ach" && parts.size() >= 8 && result.list.size() < 2000) {
            Achievement a;
            a.id = parts[1];
            a.unlocked = parts[2] == "1";
            a.points = int(achievement_number(parts[3]));
            a.unlocked_at = achievement_number(parts[4]);
            a.rarity = parts[5];
            a.title = parts[6];
            a.description = parts[7];
            if (parts.size() > 8) a.badge = parts[8];
            if (parts.size() > 9) a.progress = parts[9];
            result.list.push_back(std::move(a));
        }
    }
    std::stable_sort(result.list.begin(), result.list.end(), [](const Achievement& a, const Achievement& b) {
        if (a.unlocked != b.unlocked) return a.unlocked;
        return a.unlocked && a.unlocked_at > b.unlocked_at;
    });
    return result;
}
inline GameAchievements load_game_achievements(const fs::path& player_folder) {
    std::ifstream file(player_folder / "achievements.tsv", std::ios::binary);
    if (!file) return {};
    return parse_achievements(file);
}
// The pictures kept for a game, in gallery order: cover, title screen, screenshot. Only files that exist.
struct GalleryPicture {
    const char* name; // shown under the picture
    fs::path path;
};
inline std::vector<GalleryPicture> gallery_pictures(const fs::path& folder) {
    std::vector<GalleryPicture> pictures;
    std::error_code ec;
    for (auto [name, file] : {std::pair{"Cover", "box.bmp"}, {"Title screen", "title.bmp"}, {"Screenshot", "snap.bmp"}})
        if (fs::is_regular_file(folder / file, ec)) pictures.push_back({name, folder / file});
    return pictures;
}
} // namespace rgds
