// The play journal (src/launcher/play_journal.hpp): sittings, unlocks and pictures stitched by time, the long-break
// note, the year's recap, and LibrarySummary reading the real files. Built by tests/test_play_journal.py.
#include "launcher/library_summary.hpp"
#include <cassert>
#include <cstdio>
#include <sstream>
#include <thread>

using namespace rgds;

static long long at(int y, int mo, int d, int h, int mi) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return (long long)std::mktime(&t);
}

static void stitching() {
    JournalSource<Game> metroid;
    metroid.game = Game{"Metroid II", "/games/gb/metroid2.gb", false, "gb"};
    metroid.key = "aaaa";
    metroid.title = "Metroid II";
    metroid.complete = true;
    const long long august = at(2026, 8, 3, 20, 0), tuesday = at(2026, 11, 17, 19, 0);
    metroid.sessions = {{tuesday + 1500 + 300, 900}, {august, 3600}, {tuesday, 1500}}; // out of order on purpose
    metroid.unlocks = {{tuesday + 1200, "Varia Suit"}, {august + 100, "Bombs"}, {at(2026, 10, 1, 9, 0), "Elsewhere"}};
    metroid.shots = {{tuesday + 2000, "/album/aaaa/20261117-193320.thumb.bmp"},
                     {tuesday + 2100, "/album/aaaa/20261117-193500.thumb.bmp"}};
    JournalSource<Game> tetris;
    tetris.game = Game{"Tetris", "/games/gb/tetris.gb", false, "gb"};
    tetris.key = "bbbb";
    tetris.title = "Tetris";
    tetris.sessions = {{at(2026, 11, 18, 8, 0), 600}};

    auto journal = journal::build<Game>({metroid, tetris}, 0);
    assert(!journal.recap);
    assert(journal.entries.size() == 3);
    const auto& newest = journal.entries[0];
    assert(newest.title == "Tetris" && !newest.first && newest.since == 0); // incomplete history: no "first time"
    const auto& tuesday_entry = journal.entries[1];
    // Two sessions five minutes apart are one sitting of 40 minutes; the break is not counted.
    assert(tuesday_entry.title == "Metroid II" && tuesday_entry.began == tuesday && tuesday_entry.seconds == 2400);
    assert(tuesday_entry.unlocks.size() == 1 && tuesday_entry.unlocks[0] == "Varia Suit");
    assert(tuesday_entry.photos == 2 && tuesday_entry.photo == "/album/aaaa/20261117-193320.thumb.bmp");
    assert(tuesday_entry.since == august); // three months away
    const auto& first = journal.entries[2];
    assert(first.first && first.unlocks.size() == 1 && first.unlocks[0] == "Bombs" && first.photos == 0);

    auto recap = journal::build<Game>({metroid, tetris}, 2026).recap;
    assert(recap && recap->year == 2026 && recap->sittings == 3 && recap->days == 3 && recap->games == 2);
    assert(recap->seconds == 3600 + 2400 + 600 && recap->longest && recap->longest->seconds == 3600);
    assert(recap->favourite && recap->favourite_title == "Metroid II" && recap->favourite_days == 2);
    assert(recap->unlocks == 3 && recap->photos == 2 && recap->first_photo_at == tuesday + 2000);
    assert(!journal::build<Game>({metroid}, 2019).recap); // nothing played that year

    assert(journal::shot_time("20261117-193320.png") == at(2026, 11, 17, 19, 33) + 20);
    assert(journal::shot_time("20261117-193320-2.png") == at(2026, 11, 17, 19, 33) + 20);
    assert(journal::shot_time("cover.png") == 0 && journal::shot_time("2026111x-193320.png") == 0);
    assert(journal::recap_year(std::time_t(at(2026, 12, 2, 12, 0))) == 2026);
    assert(journal::recap_year(std::time_t(at(2027, 1, 20, 12, 0))) == 2026);
    assert(journal::recap_year(std::time_t(at(2026, 11, 22, 12, 0))) == 0);

    std::istringstream achievements("#rgds-achievements 1\ngame\t1\tM\t1\t2\t5\t10\tme\t0\n"
                                    "ach\t7\t1\t5\t1763400000\t\tVaria Suit\tFind it\t\t\n"
                                    "ach\t8\t0\t5\t0\t\tScrew Attack\tLater\t\t\n");
    auto unlocks = journal::read_unlocks(achievements);
    assert(unlocks.size() == 1 && unlocks[0].title == "Varia Suit" && unlocks[0].at == 1763400000);
}

// LibrarySummary reads sessions.tsv, achievements.tsv and the Album's album.tsv into the journal.
static void summary_reads_files(const fs::path& data) {
    std::vector<Game> games = {{"Metroid II", data / "metroid2.gb", false, "gb"}};
    std::ofstream(games[0].path) << "x";
    LibraryCache cache(data);
    const fs::path folder = cache.player_folder(games[0]);
    fs::create_directories(folder);
    const long long began = at(2026, 11, 17, 19, 0);
    std::ofstream(folder / "stats.ini") << "seconds=1500\nsessions=1\nlast_played=2026-11-17T18:00:00+00:00\n";
    std::ofstream(folder / "sessions.tsv") << began << "\t1500\n";
    std::ofstream(folder / "achievements.tsv")
        << "#rgds-achievements 1\nach\t7\t1\t5\t" << began + 600 << "\t\tVaria Suit\tFind it\t\t\n";
    const fs::path album = folder.parent_path().parent_path() / "screenshots";
    fs::create_directories(album / folder.filename());
    std::ofstream(album / "album.tsv") << (album / folder.filename() / "20261117-191000.png").string() << '\t'
                                       << folder.filename().string() << "\tMetroid II\tgb\t20261117\n";
    LibrarySummary summary;
    summary.request(data, games);
    std::optional<LibrarySummary::Result> result;
    for (int i = 0; i < 3000 && !result; i++) {
        result = summary.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(result && result->journal.entries.size() == 1);
    const auto& entry = result->journal.entries[0];
    assert(entry.first && entry.seconds == 1500 && entry.unlocks.size() == 1 && entry.photos == 1);
    assert(entry.photo == (album / folder.filename() / "20261117-191000.thumb.bmp").string());
}

int main(int argc, char** argv) {
    setenv("TZ", "Europe/Oslo", 1);
    tzset();
    stitching();
    if (argc > 1) summary_reads_files(argv[1]);
    std::puts("journal: ok");
}
