#pragma once
#include "launcher/media.hpp"

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
    facts.rating = info["rating"];
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
