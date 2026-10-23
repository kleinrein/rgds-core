#pragma once
#include "launcher/library_cache.hpp"
#include <array>
#include <atomic>
#include <future>
#include <map>
#include <memory>

namespace rgds {
// Library filters built from the game information runtime/game_info.py saved (genre, publisher, players, year, ...).
// Every facet is optional: an empty value (or an unset range) means "any", and a game without information simply
// has empty traits. Players and release year are ranges, not lists: they are narrowed with sliders.
inline constexpr int players_cap = 4; // "4 or more" is one stop

struct Facets {
    std::string genre, publisher, developer, rating, online, info;
    // Inclusive ranges; 0 means the slider is at its full extent (not narrowed).
    int players_low = 0, players_high = 0, year_low = 0, year_high = 0;
    int active() const {
        int count = 0;
        for (const auto* value : {&genre, &publisher, &developer, &rating, &online, &info}) count += !value->empty();
        return count + (players_low > 0) + (year_low > 0);
    }
    bool any() const { return active() > 0; }
    // "Puzzle  ·  2–4 players  ·  1995–2005", for the line that says what the list is filtered by.
    std::string summary() const {
        std::string text;
        auto add = [&](const std::string& part) {
            if (!part.empty()) text += (text.empty() ? "" : "  ·  ") + part;
        };
        add(genre);
        add(players_text());
        add(year_text());
        add(rating);
        add(online);
        add(info);
        add(publisher);
        add(developer);
        return text;
    }
    std::string players_text() const {
        if (players_low <= 0) return "";
        auto stop = [](int n) { return std::to_string(n) + (n >= players_cap ? "+" : ""); };
        if (players_low == players_high) return stop(players_low) + (players_low == 1 ? " player" : " players");
        return (players_high >= players_cap ? stop(players_low)
                                            : std::to_string(players_low) + "–" + stop(players_high)) +
               " players";
    }
    std::string year_text() const {
        if (year_low <= 0) return "";
        return year_low == year_high ? std::to_string(year_low)
                                     : std::to_string(year_low) + "–" + std::to_string(year_high);
    }
};
inline constexpr const char* facet_online = "Online play";
inline constexpr const char* facet_with_info = "With info";
inline constexpr const char* facet_without_info = "Without info";

// The list facets (chips), in the order the filter sheet shows them.
struct FacetRow {
    const char* title;
    const char* name; // the key in FacetIndex::Counts
    std::string Facets::* field;
    bool by_count; // most common first; otherwise in natural order
};
inline const std::array<FacetRow, 6>& facet_rows() {
    static const std::array<FacetRow, 6> rows = {{{"Genre", "genre", &Facets::genre, true},
                                                  {"Publisher", "publisher", &Facets::publisher, true},
                                                  {"Developer", "developer", &Facets::developer, true},
                                                  {"Rating", "rating", &Facets::rating, true},
                                                  {"Online", "online", &Facets::online, false},
                                                  {"Game info", "info", &Facets::info, false}}};
    return rows;
}

// What one game is, as far as the facets are concerned.
struct FacetValues {
    std::string genre, publisher, developer, rating, online, info;
    int players = 0, year = 0; // 0: unknown
};
inline std::string facet_field(const std::map<std::string, std::string>& info, const char* key) {
    auto found = info.find(key);
    return found == info.end() ? "" : found->second;
}
inline int facet_number(const std::string& text, size_t digits = 9) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return 0;
    return std::atoi(text.substr(0, digits).c_str());
}
inline FacetValues facet_values(const std::map<std::string, std::string>& info) {
    FacetValues values;
    // GameTDB lists the main genre first ("simulation,life simulation"): that is the one to filter by.
    auto genre = facet_field(info, "genre");
    values.genre = genre.substr(0, genre.find(','));
    values.publisher = facet_field(info, "publisher");
    values.developer = facet_field(info, "developer");
    values.rating = facet_field(info, "rating");
    values.players = std::min(facet_number(facet_field(info, "players"), 3), players_cap);
    const auto release = facet_field(info, "release");
    values.year = release.size() >= 4 ? facet_number(release.substr(0, 4)) : 0;
    values.online = facet_field(info, "online") == "1" ? facet_online : "";
    values.info = facet_field(info, "info_version").empty() ? facet_without_info : facet_with_info;
    return values;
}
inline bool facets_match(const Facets& wanted, const FacetValues& have) {
    auto same = [](const std::string& want, const std::string& value) { return want.empty() || want == value; };
    // A game whose players or year is unknown never matches a narrowed range.
    if (wanted.players_low > 0 && (have.players < wanted.players_low || have.players > wanted.players_high))
        return false;
    if (wanted.year_low > 0 && (have.year < wanted.year_low || have.year > wanted.year_high)) return false;
    return same(wanted.genre, have.genre) && same(wanted.publisher, have.publisher) &&
           same(wanted.developer, have.developer) && same(wanted.rating, have.rating) &&
           same(wanted.online, have.online) && same(wanted.info, have.info);
}

// How many games have each value, read off the disk on a worker so a big library never stalls the menu.
class FacetIndex {
public:
    using Counts = std::map<std::string, std::map<std::string, int>>; // facet name -> value -> games
    using Histogram = std::map<int, int>;                             // players or year -> games

private:
    struct Result {
        unsigned generation;
        Counts counts;
        Histogram players, years;
        int games = 0;
    };
    std::future<Result> worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
    unsigned generation = 0;
    Counts counts_;
    Histogram players_, years_;
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
            Result result{generation, {}, {}, {}, 0};
            for (const auto& game : games) {
                if (*stop) return result;
                if (game.demo) continue;
                const auto info = fields(data / "library" / game_key(game.path) / "info.ini");
                const auto values = facet_values(info);
                result.games++;
                const std::pair<const char*, const std::string*> traits[] = {
                    {"genre", &values.genre},   {"publisher", &values.publisher}, {"developer", &values.developer},
                    {"rating", &values.rating}, {"online", &values.online},       {"info", &values.info}};
                for (const auto& [name, value] : traits)
                    if (!value->empty()) result.counts[name][*value]++;
                if (values.players > 0) result.players[values.players]++;
                if (values.year > 0) result.years[values.year]++;
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
        players_ = std::move(result.players);
        years_ = std::move(result.years);
        games_ = result.games;
        loaded = true;
        return true;
    }
    bool ready() const { return loaded; }
    bool loading() const { return worker.valid(); }
    int games() const { return games_; }
    const Histogram& players() const { return players_; }
    const Histogram& years() const { return years_; }
    // The values of one facet, most common first (or in natural order when `by_count` is false).
    std::vector<std::pair<std::string, int>> values(const std::string& name, bool by_count) const {
        std::vector<std::pair<std::string, int>> list;
        auto found = counts_.find(name);
        if (found == counts_.end()) return list;
        list.assign(found->second.begin(), found->second.end());
        if (by_count)
            std::stable_sort(list.begin(), list.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
        return list;
    }
};
} // namespace rgds
