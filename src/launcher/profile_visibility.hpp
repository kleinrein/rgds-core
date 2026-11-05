#pragma once
#include "launcher/age_rating.hpp"
#include "launcher/library_cache.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <optional>

namespace rgds {
// Which games a player sees (docs/PROFILES_PLAN.md, section 5). Whose rules they are is the store's business; this
// only knows the rules. In order: forced hidden, forced shown, no age limit, rated, unrated.
inline constexpr int no_age_limit = -1, only_picked_games = -2;

struct VisibilityRules {
    int age = no_age_limit;
    bool unrated_hidden = false, restricted = false, allow_ports = false, allow_homebrew = false;
    std::map<std::string, std::string> overrides; // game key -> hide or show (visibility.ini in the player's folder)
    // Rules that need each game's rating, which is a read per game: done on a worker.
    bool needs_ratings() const { return age != no_age_limit; }
    // Nothing to do at all: every game shows, at no cost. Every existing user.
    bool open() const { return !restricted && !needs_ratings() && overrides.empty(); }
};

inline bool game_visible(const VisibilityRules& rules, const std::string& key, const std::string& system,
                         const std::string& rating) {
    // Ports go by permission, never by rating; their saves are shared too.
    if (rules.restricted && system == "ports" && !rules.allow_ports) return false;
    const auto forced = rules.overrides.find(key);
    if (forced != rules.overrides.end()) {
        if (forced->second == "hide") return false;
        if (forced->second == "show") return true;
    }
    if (rules.age == no_age_limit) return true;
    if (rules.age == only_picked_games) return false;
    const int age = minimum_age(rating);
    return age >= 0 ? age <= rules.age : !rules.unrated_hidden;
}

inline std::map<std::string, std::string> load_visibility(const fs::path& player) {
    std::map<std::string, std::string> overrides;
    for (const auto& [key, value] : fields(player / "visibility.ini"))
        if (value == "hide" || value == "show") overrides[key] = value;
    return overrides;
}

// Every game with its rating, from a worker: a card can hold thousands of small files. Asked to look through the games
// folders too, for a page that shows what another profile could see, whatever this one has switched on.
class CatalogReader {
public:
    struct Entry {
        Game game;
        std::string key, rating;
        int age = -1;
    };
    using Roots = std::vector<std::pair<std::string, fs::path>>;

private:
    std::future<std::vector<Entry>> worker;
    std::shared_ptr<std::atomic<bool>> stop;

public:
    ~CatalogReader() { cancel(); }
    void cancel() {
        if (stop) *stop = true;
        if (worker.valid()) worker.wait();
        worker = {};
        stop.reset();
    }
    bool pending() const { return worker.valid(); }
    void request(const fs::path& data, std::vector<Game> games, Roots roots = {}) {
        cancel();
        stop = std::make_shared<std::atomic<bool>>(false);
        worker = std::async(std::launch::async,
                            [data, games = std::move(games), roots = std::move(roots), stop = stop]() mutable {
                                for (const auto& info : systems()) {
                                    const std::string id = info.id;
                                    if (id == "nds" || id == "homebrew" || roots.empty()) continue;
                                    std::vector<fs::path> folders;
                                    for (const auto& [system, path] : roots)
                                        if (system == id) folders.push_back(path);
                                    auto found = scan_system(folders, id);
                                    games.insert(games.end(), found.begin(), found.end());
                                }
                                std::vector<Entry> entries;
                                entries.reserve(games.size());
                                for (auto& game : games) {
                                    if (*stop) return std::vector<Entry>{};
                                    Entry entry;
                                    entry.key = game_key(game.path);
                                    entry.rating = fields(data / "library" / entry.key / "info.ini")["rating"];
                                    entry.age = minimum_age(entry.rating);
                                    entry.game = std::move(game);
                                    entries.push_back(std::move(entry));
                                }
                                return entries;
                            });
    }
    std::optional<std::vector<Entry>> poll() {
        if (!worker.valid() || worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
        auto entries = worker.get();
        stop.reset();
        return entries;
    }
};
}
