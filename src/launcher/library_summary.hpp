#pragma once
#include "launcher/collection_filter.hpp"
#include <array>
#include <ctime>
#include <fstream>
#include <set>

namespace rgds {
// Read historical records once off the UI thread, never from a draw callback.
class LibrarySummary {
public:
    struct Group {
        uint64_t games = 0, played = 0, sessions = 0, seconds = 0;
        std::string last_date, last_title;
        std::optional<Game> last_game;
    };
    struct Entry {
        std::string title;
        uint64_t seconds = 0, sessions = 0;
        Game game;
        int days_ago = -1; // since it was last played, in local days; -1 unknown
    };
    // The session history (sessions.tsv beside each game's stats.ini), by local day.
    struct Activity {
        static constexpr int days = 28;
        std::array<uint64_t, days> seconds{}; // [days - 1] is today
        uint64_t longest = 0, logged = 0;     // the longest session; sessions in the history
        int streak = 0, best_streak = 0;      // days in a row with a game; today may still be open
        int weekday = 0;                      // today's, 0 = Sunday
        uint64_t week(int back) const {       // the 7 days ending `back` weeks ago
            uint64_t sum = 0;
            for (int i = 0; i < 7; i++) sum += seconds[size_t(days - 1 - back * 7 - i)];
            return sum;
        }
    };
    struct Result {
        unsigned generation = 0;
        std::array<Group, 3> groups{};
        std::vector<Entry> most_played;
        std::vector<Game> recent_games;
        Activity activity;
        std::vector<std::pair<std::string, uint64_t>> systems; // time by system id, most first
    };
    // Days since 1970-01-01 of a moment's local date.
    static long local_day(std::time_t moment) {
        std::tm t{};
        if (!localtime_r(&moment, &t)) return 0;
        const int y = t.tm_year + 1900 - (t.tm_mon < 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400,
                  m = t.tm_mon + 1, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
        return long(era) * 146097 + long(yoe) * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    }
    // stats.ini's last_played: a UTC ISO time from the runtime ("2026-10-29T08:15:00+00:00").
    static std::optional<std::time_t> utc_moment(const std::string& text) {
        std::tm t{};
        int fields = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour,
                                 &t.tm_min, &t.tm_sec);
        if (fields < 3) return {};
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        return timegm(&t);
    }

private:
    struct Request {
        fs::path data;
        std::vector<Game> games;
        unsigned generation;
    };
    std::optional<Request> queued;
    std::future<Result> worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
    unsigned generation = 0;

public:
    ~LibrarySummary() { cancel(); }
    void cancel() {
        ++generation;
        if (cancelled) *cancelled = true;
        queued.reset();
    }
    void request(const fs::path& data, const std::vector<Game>& games) {
        cancel();
        queued = Request{data, games, generation};
    }
    std::optional<Result> poll() {
        std::optional<Result> ready;
        if (worker.valid()) {
            if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
            auto result = worker.get();
            if (result.generation == generation) ready = std::move(result);
        }
        if (queued) {
            auto request = std::move(*queued);
            queued.reset();
            cancelled = std::make_shared<std::atomic<bool>>(false);
            worker = std::async(std::launch::async, [request = std::move(request), stop = cancelled] {
                Result result;
                result.generation = request.generation;
                LibraryCache cache(request.data, 1);
                std::vector<std::pair<std::string, Game>> recent;
                std::map<std::string, uint64_t> by_system;
                std::set<long> played_days;
                const std::time_t now = std::time(nullptr);
                const long today = local_day(now);
                {
                    std::tm t{};
                    localtime_r(&now, &t);
                    result.activity.weekday = t.tm_wday;
                }
                auto& activity = result.activity;
                for (const auto& game : request.games) {
                    if (*stop) return result;
                    auto stats = cache.stats(game);
                    bool favourite = cache.options(game).favourite;
                    if (!game.demo && !stats.last_played.empty()) {
                        auto at = std::find_if(recent.begin(), recent.end(),
                                               [&](const auto& item) { return stats.last_played > item.first; });
                        if (at != recent.end() || recent.size() < 8) {
                            recent.insert(at, {stats.last_played, game});
                            if (recent.size() > 8) recent.pop_back();
                        }
                    }
                    if (stats.seconds) by_system[game.system] += stats.seconds;
                    if (stats.sessions) {
                        std::ifstream history(cache.folder(game) / "sessions.tsv");
                        long long began = 0, played = 0;
                        while (history >> began >> played) {
                            if (played <= 0 || began <= 0) continue;
                            activity.logged++;
                            activity.longest = std::max(activity.longest, uint64_t(played));
                            const long day = local_day(std::time_t(began));
                            played_days.insert(day);
                            const long back = today - day;
                            if (back >= 0 && back < Activity::days)
                                activity.seconds[size_t(Activity::days - 1 - back)] += uint64_t(played);
                        }
                    }
                    if (stats.seconds || stats.sessions) {
                        Entry entry{library_title(game.title), stats.seconds, stats.sessions, game};
                        if (auto moment = utc_moment(stats.last_played))
                            entry.days_ago = int(std::max(0L, today - local_day(*moment)));
                        auto before = [](const Entry& a, const Entry& b) {
                            return a.seconds != b.seconds     ? a.seconds > b.seconds
                                   : a.sessions != b.sessions ? a.sessions > b.sessions
                                                              : a.title < b.title;
                        };
                        auto at = std::lower_bound(result.most_played.begin(), result.most_played.end(), entry, before);
                        if (at != result.most_played.end() || result.most_played.size() < 5) {
                            result.most_played.insert(at, std::move(entry));
                            if (result.most_played.size() > 5) result.most_played.pop_back();
                        }
                    }
                    for (size_t i = 0; i < 3; i++) {
                        if ((i == 1 && !favourite) || (i == 2 && stats.last_played.empty())) continue;
                        auto& group = result.groups[i];
                        group.games++;
                        group.seconds += stats.seconds;
                        group.sessions += stats.sessions;
                        if (stats.sessions || stats.seconds || !stats.last_played.empty()) group.played++;
                        if (stats.last_played > group.last_date) {
                            group.last_date = stats.last_played;
                            group.last_game = game;
                            auto& info = cache.metadata(game);
                            group.last_title = info.info["title"];
                            if (group.last_title.empty()) group.last_title = library_title(game.title);
                        }
                    }
                }
                for (auto& entry : recent) result.recent_games.push_back(std::move(entry.second));
                result.systems.assign(by_system.begin(), by_system.end());
                std::stable_sort(result.systems.begin(), result.systems.end(),
                                 [](const auto& a, const auto& b) { return a.second > b.second; });
                // Streaks: runs of consecutive days; the current one may end yesterday, today is not over.
                long run = 0, previous = 0;
                for (long day : played_days) {
                    run = run && day == previous + 1 ? run + 1 : 1;
                    previous = day;
                    activity.best_streak = std::max(activity.best_streak, int(run));
                }
                if (!played_days.empty() && previous >= today - 1) activity.streak = int(run);
                return result;
            });
        }
        return ready;
    }
};
}
