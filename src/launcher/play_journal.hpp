#pragma once
// The play journal: what the device already records about a player's games, stitched into a story. Sessions come
// from sessions.tsv (runtime/library.py), unlocks from achievements.tsv (rgds-play and DSCore), pictures from the
// Album's file names (runtime/screenshots.py). Nothing new is recorded for it. Built off the UI thread by
// LibrarySummary; the Journal page (journal_screen.hpp) and the launch card only read the result.
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <istream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rgds {
// Days since 1970-01-01 of a moment's local date.
inline long journal_local_day(std::time_t moment) {
    std::tm t{};
    if (!localtime_r(&moment, &t)) return 0;
    const int y = t.tm_year + 1900 - (t.tm_mon < 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400,
              m = t.tm_mon + 1, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
    return long(era) * 146097 + long(yoe) * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
inline int journal_local_year(std::time_t moment) {
    std::tm t{};
    return localtime_r(&moment, &t) ? t.tm_year + 1900 : 0;
}

struct JournalSession {
    long long began = 0, seconds = 0;
};
struct JournalUnlock {
    long long at = 0;
    std::string title;
};
struct JournalShot {
    long long at = 0;
    std::string thumb; // the Album's <name>.thumb.bmp beside the PNG
};
// One game's records, as read from its files.
template <class GameT> struct JournalSource {
    GameT game;
    std::string key, title; // key: the game's library folder name
    std::vector<JournalSession> sessions;
    std::vector<JournalUnlock> unlocks;
    std::vector<JournalShot> shots;
    bool complete = false; // the history holds every session stats.ini counts, so its first row is the first time
};
// One sitting with a game: sessions a short break apart (Return home and back) count as one.
template <class GameT> struct JournalEntryT {
    GameT game;
    std::string key, title;
    long long began = 0, seconds = 0;
    long day = 0;
    std::vector<std::string> unlocks; // achievement titles unlocked during it, first first
    std::string photo;                // the first screenshot taken during it, as a thumbnail
    int photos = 0;
    long long since = 0; // when the game was last played before this, if that was three weeks or more earlier
    bool first = false;  // the first time the game was ever played here
};
template <class GameT> struct JournalRecapT {
    int year = 0;
    long long seconds = 0;
    int sittings = 0, days = 0, games = 0, unlocks = 0, photos = 0;
    std::optional<JournalEntryT<GameT>> longest;
    std::optional<GameT> favourite; // the game played on the most days
    std::string favourite_title;
    int favourite_days = 0;
    std::string first_photo, first_photo_title; // the year's first screenshot
    long long first_photo_at = 0;
};
template <class GameT> struct JournalT {
    std::vector<JournalEntryT<GameT>> entries; // newest first
    std::optional<JournalRecapT<GameT>> recap;
};

namespace journal {
constexpr long long merge_gap = 20 * 60;                   // a break shorter than this keeps one sitting
constexpr long long long_break = 21 * 24 * 3600;           // "first time since ..." after this long away
constexpr long long before_slack = 120, after_slack = 600; // a picture or unlock just around a sitting is its own
constexpr size_t entry_limit = 400;

// sessions.tsv rows: "start<TAB>seconds".
inline std::vector<JournalSession> read_sessions(std::istream& in) {
    std::vector<JournalSession> rows;
    long long began = 0, played = 0;
    while (in >> began >> played)
        if (began > 0 && played > 0) rows.push_back({began, played});
    return rows;
}
// The unlocked "ach" lines of achievements.tsv (format in game_info.hpp): unlock time and title.
inline std::vector<JournalUnlock> read_unlocks(std::istream& in) {
    std::vector<JournalUnlock> unlocks;
    std::string line;
    if (!std::getline(in, line) || line.rfind("#rgds-achievements", 0) != 0) return unlocks;
    while (std::getline(in, line) && unlocks.size() < 2000) {
        if (line.rfind("ach\t", 0) != 0) continue;
        std::vector<std::string> cell;
        for (size_t from = 0;;) {
            const auto tab = line.find('\t', from);
            cell.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos || cell.size() > 8) break;
            from = tab + 1;
        }
        if (cell.size() < 8 || cell[2] != "1") continue;
        const long long at = std::atoll(cell[4].c_str());
        if (at > 0 && !cell[6].empty()) unlocks.push_back({at, cell[6]});
    }
    return unlocks;
}
// An Album picture's local time from its name, <YYYYMMDD-HHMMSS>[-n].png; 0 when the name is not one.
inline long long shot_time(const std::string& name) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char dash = 0;
    if (name.size() < 15 || std::sscanf(name.c_str(), "%4d%2d%2d%c%2d%2d%2d", &y, &mo, &d, &dash, &h, &mi, &s) != 7 ||
        dash != '-' || mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    const std::time_t at = std::mktime(&t);
    return at > 0 ? (long long)at : 0;
}
// The year a recap is shown for on `now`: December shows the year ending, January the one just ended; 0 otherwise.
inline int recap_year(std::time_t now) {
    std::tm t{};
    if (!localtime_r(&now, &t)) return 0;
    return t.tm_mon == 11 ? t.tm_year + 1900 : t.tm_mon == 0 ? t.tm_year + 1899 : 0;
}

template <class GameT> JournalT<GameT> build(std::vector<JournalSource<GameT>> sources, int recap_for) {
    JournalT<GameT> result;
    std::vector<JournalEntryT<GameT>> all;
    for (auto& source : sources) {
        auto& rows = source.sessions;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.began < b.began; });
        std::vector<JournalEntryT<GameT>> sittings;
        for (const auto& row : rows) {
            if (!sittings.empty()) {
                auto& last = sittings.back();
                const long long end = last.began + last.seconds;
                // A checkpoint row of the same session, or a short break: one sitting.
                if (row.began <= end + merge_gap && journal_local_day(std::time_t(row.began)) == last.day) {
                    last.seconds += row.seconds; // the break itself is not play
                    continue;
                }
            }
            JournalEntryT<GameT> entry;
            entry.game = source.game;
            entry.key = source.key;
            entry.title = source.title;
            entry.began = row.began;
            entry.seconds = row.seconds;
            entry.day = journal_local_day(std::time_t(row.began));
            sittings.push_back(std::move(entry));
        }
        std::sort(source.unlocks.begin(), source.unlocks.end(),
                  [](const auto& a, const auto& b) { return a.at < b.at; });
        std::sort(source.shots.begin(), source.shots.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
        for (size_t i = 0; i < sittings.size(); i++) {
            auto& entry = sittings[i];
            const long long from = entry.began - before_slack, to = entry.began + entry.seconds + after_slack;
            // The window stops where the next sitting starts, so one unlock or picture is never told twice.
            const long long until = i + 1 < sittings.size() ? std::min(to, sittings[i + 1].began) : to;
            for (const auto& unlock : source.unlocks)
                if (unlock.at >= from && unlock.at < until) entry.unlocks.push_back(unlock.title);
            for (const auto& shot : source.shots)
                if (shot.at >= from && shot.at < until) {
                    if (entry.photo.empty()) entry.photo = shot.thumb;
                    entry.photos++;
                }
            if (i == 0) entry.first = source.complete;
            else {
                const auto& before = sittings[i - 1];
                if (entry.began - (before.began + before.seconds) >= long_break) entry.since = before.began;
            }
        }
        for (auto& entry : sittings) all.push_back(std::move(entry));
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.began > b.began; });

    if (recap_for > 0) {
        JournalRecapT<GameT> recap;
        recap.year = recap_for;
        std::set<long> days;
        std::map<std::string, std::pair<std::set<long>, size_t>> by_game; // key -> (days, its newest entry)
        for (size_t i = 0; i < all.size(); i++) {
            const auto& entry = all[i];
            if (journal_local_year(std::time_t(entry.began)) != recap_for) continue;
            recap.seconds += entry.seconds;
            recap.sittings++;
            days.insert(entry.day);
            auto [game, fresh] = by_game.try_emplace(entry.key, std::set<long>{}, i);
            (void)fresh;
            game->second.first.insert(entry.day);
            if (!recap.longest || entry.seconds > recap.longest->seconds) recap.longest = entry;
        }
        recap.days = int(days.size());
        recap.games = int(by_game.size());
        size_t favourite = all.size();
        for (const auto& [key, game] : by_game) {
            // The most days wins; on a tie, the game played most recently (entries run newest first).
            const int played = int(game.first.size());
            if (played > recap.favourite_days || (played == recap.favourite_days && game.second < favourite)) {
                recap.favourite_days = played;
                favourite = game.second;
            }
        }
        if (favourite < all.size()) {
            recap.favourite = all[favourite].game;
            recap.favourite_title = all[favourite].title;
        }
        for (const auto& source : sources) {
            for (const auto& unlock : source.unlocks)
                if (journal_local_year(std::time_t(unlock.at)) == recap_for) recap.unlocks++;
            for (const auto& shot : source.shots)
                if (journal_local_year(std::time_t(shot.at)) == recap_for) {
                    recap.photos++;
                    if (!recap.first_photo_at || shot.at < recap.first_photo_at) {
                        recap.first_photo_at = shot.at;
                        recap.first_photo = shot.thumb;
                        recap.first_photo_title = source.title;
                    }
                }
        }
        if (recap.sittings) result.recap = std::move(recap);
    }
    if (all.size() > entry_limit) all.resize(entry_limit);
    result.entries = std::move(all);
    return result;
}
} // namespace journal
} // namespace rgds
