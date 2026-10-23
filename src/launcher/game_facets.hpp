#pragma once
#include "launcher/library_cache.hpp"
#include <array>
#include <atomic>
#include <future>
#include <map>
#include <memory>

namespace rgds {
// Library filters built from the game information runtime/game_info.py saved (genre, publisher, ...). Every facet
// is optional: an empty value means "any", and a game without information simply has empty traits.
struct Facets {
    std::string genre, publisher, developer, players, rating, decade, online, info;
    int active() const {
        int count = 0;
        for (const auto* value : {&genre, &publisher, &developer, &players, &rating, &decade, &online, &info})
            count += !value->empty();
        return count;
    }
    bool any() const { return active() > 0; }
    // "Puzzle  ·  2 players", for the line that says what the list is filtered by.
    std::string summary() const {
        std::string text;
        for (const auto* value : {&genre, &publisher, &developer, &players, &rating, &decade, &online, &info})
            if (!value->empty()) text += (text.empty() ? "" : "  ·  ") + *value;
        return text;
    }
};
inline constexpr const char* facet_online = "Online play";
inline constexpr const char* facet_with_info = "With info";
inline constexpr const char* facet_without_info = "Without info";

// The rows of the More filters page, in order.
struct FacetRow {
    const char* title;
    const char* name; // the key in FacetIndex::Counts
    std::string Facets::*field;
    bool by_count; // most common first; otherwise in natural order
};
inline const std::array<FacetRow, 8>& facet_rows() {
    static const std::array<FacetRow, 8> rows = {{{"Genre", "genre", &Facets::genre, true},
                                                  {"Publisher", "publisher", &Facets::publisher, true},
                                                  {"Developer", "developer", &Facets::developer, true},
                                                  {"Players", "players", &Facets::players, false},
                                                  {"Rating", "rating", &Facets::rating, true},
                                                  {"Released", "decade", &Facets::decade, false},
                                                  {"Online", "online", &Facets::online, false},
                                                  {"Game info", "info", &Facets::info, false}}};
    return rows;
}

// What one game is, as far as the facets are concerned.
using FacetValues = Facets;
inline std::string facet_field(const std::map<std::string, std::string>& info, const char* key) {
    auto found = info.find(key);
    return found == info.end() ? "" : found->second;
}
inline FacetValues facet_values(const std::map<std::string, std::string>& info) {
    FacetValues values;
    // GameTDB lists the main genre first ("simulation,life simulation"): that is the one to filter by.
    auto genre = facet_field(info, "genre");
    genre = genre.substr(0, genre.find(','));
    values.genre = genre;
    values.publisher = facet_field(info, "publisher");
    values.developer = facet_field(info, "developer");
    values.rating = facet_field(info, "rating");
    const auto players = facet_field(info, "players");
    if (!players.empty() && players.find_first_not_of("0123456789") == std::string::npos) {
        const int count = std::atoi(players.c_str());
        values.players = count >= 3 ? "3+ players" : count == 2 ? "2 players" : count == 1 ? "1 player" : "";
    }
    const auto release = facet_field(info, "release").substr(0, 4);
    if (release.size() == 4 && release.find_first_not_of("0123456789") == std::string::npos)
        values.decade = std::to_string(std::atoi(release.c_str()) / 10 * 10) + "s";
    values.online = facet_field(info, "online") == "1" ? facet_online : "";
    values.info = facet_field(info, "info_version").empty() ? facet_without_info : facet_with_info;
    return values;
}
inline bool facets_match(const Facets& wanted, const FacetValues& have) {
    auto same = [](const std::string& want, const std::string& value) { return want.empty() || want == value; };
    return same(wanted.genre, have.genre) && same(wanted.publisher, have.publisher) &&
           same(wanted.developer, have.developer) && same(wanted.players, have.players) &&
           same(wanted.rating, have.rating) && same(wanted.decade, have.decade) && same(wanted.online, have.online) &&
           same(wanted.info, have.info);
}

// How many games have each value, read off the disk on a worker so a big library never stalls the menu.
class FacetIndex {
public:
    using Counts = std::map<std::string, std::map<std::string, int>>; // facet name -> value -> games

private:
    struct Result {
        unsigned generation;
        Counts counts;
        int games = 0;
    };
    std::future<Result> worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
    unsigned generation = 0;
    Counts counts_;
    int games_ = 0;
    bool loaded = false;

public:
    ~FacetIndex() { cancel(); }
    void cancel() {
        ++generation;
        if (cancelled) *cancelled = true;
    }
    void request(const fs::path& data, const std::vector<Game>& games) {
        cancel();
        cancelled = std::make_shared<std::atomic<bool>>(false);
        worker = std::async(std::launch::async, [data, games, generation = generation, stop = cancelled]() {
            Result result{generation, {}, 0};
            for (const auto& game : games) {
                if (*stop) return result;
                if (game.demo) continue;
                const auto info = fields(data / "library" / game_key(game.path) / "info.ini");
                const auto values = facet_values(info);
                result.games++;
                const std::pair<const char*, const std::string*> traits[] = {
                    {"genre", &values.genre},       {"publisher", &values.publisher}, {"developer", &values.developer},
                    {"players", &values.players},   {"rating", &values.rating},       {"decade", &values.decade},
                    {"online", &values.online},     {"info", &values.info}};
                for (const auto& [name, value] : traits)
                    if (!value->empty()) result.counts[name][*value]++;
            }
            return result;
        });
    }
    // True once, when a fresh count arrives.
    bool poll() {
        if (!worker.valid() || worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto result = worker.get();
        if (result.generation != generation) return false;
        counts_ = std::move(result.counts);
        games_ = result.games;
        loaded = true;
        return true;
    }
    bool ready() const { return loaded; }
    bool loading() const { return worker.valid(); }
    int games() const { return games_; }
    // The values of one facet, most common first (or in natural order when `by_count` is false).
    std::vector<std::pair<std::string, int>> values(const std::string& name, bool by_count) const {
        std::vector<std::pair<std::string, int>> list;
        auto found = counts_.find(name);
        if (found == counts_.end()) return list;
        list.assign(found->second.begin(), found->second.end());
        if (by_count)
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return list;
    }
    int count(const std::string& name, const std::string& value) const {
        auto found = counts_.find(name);
        if (found == counts_.end()) return 0;
        auto entry = found->second.find(value);
        return entry == found->second.end() ? 0 : entry->second;
    }
};
} // namespace rgds
