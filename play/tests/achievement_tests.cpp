// SPDX-License-Identifier: GPL-3.0-or-later
// RetroAchievements in RG DS Play (app/achievements.hpp) against a fake site: sign-in with the launcher's token, the
// set loaded, an achievement triggered by memory, its unlock sent, the notice and the Game info list.
//
//   play-achievement-tests      (build/play/host, built by tools/build_play.py host)
#include "app/achievements.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace play;

namespace {

int checks = 0, failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            ++failures;                                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                         \
        }                                                                                                              \
    } while (0)

// Answers by the request's "r=" name; remembers what was asked.
struct FakeSite final : AchievementTransport {
    std::map<std::string, std::string> answers;
    std::vector<std::string> asked, saved;
    std::deque<std::pair<uint64_t, std::string>> ready;
    bool offline = false;
    bool send(uint64_t id, const std::string& url, const std::string& post, const std::string&,
              const std::string& save) override {
        if (!save.empty()) {
            saved.push_back(url + " -> " + save);
            ready.push_back({id, ""});
            return true;
        }
        const std::string query = post.empty() ? url.substr(url.find('?') + 1) : post;
        asked.push_back(query);
        const size_t r = query.find("r=");
        const std::string name = r == std::string::npos ? "" : query.substr(r + 2, query.find('&', r) - r - 2);
        ready.push_back({id, answers.count(name) ? answers[name] : ""});
        return true;
    }
    bool receive(uint64_t& id, int& status, std::string& body) override {
        if (ready.empty()) return false;
        id = ready.front().first;
        body = ready.front().second;
        status = offline ? 0 : body.empty() ? 404 : 200;
        ready.pop_front();
        return true;
    }
    bool asked_for(const std::string& name) const {
        for (const auto& q : asked)
            if (q.find("r=" + name) != std::string::npos) return true;
        return false;
    }
};

const char* patch = "{\"Success\":true,\"GameId\":1234,\"Title\":\"Sample Game\",\"ConsoleId\":5,"
                    "\"ImageIconUrl\":\"https://media.retroachievements.org/Images/112233.png\","
                    "\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\",\"Sets\":[{"
                    "\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
                    "\"ImageIconUrl\":\"https://media.retroachievements.org/Images/112233.png\","
                    "\"Achievements\":["
                    "{\"ID\":5501,\"Title\":\"First steps\",\"Description\":\"Reach the\\tcastle\",\"Flags\":3,"
                    "\"Points\":5,\"MemAddr\":\"0xH0001=3_0xH0002=7\",\"Author\":\"User1\",\"BadgeName\":\"00234\","
                    "\"Created\":1367266583,\"Modified\":1376929305},"
                    "{\"ID\":5502,\"Title\":\"Second\",\"Description\":\"Desc2\",\"Flags\":3,\"Points\":2,"
                    "\"MemAddr\":\"0xH0001=2_0x0002=9\",\"Author\":\"User1\",\"BadgeName\":\"00235\","
                    "\"Created\":1376970283,\"Modified\":1376970283}"
                    "],\"Leaderboards\":[]}]}";

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void test_console_and_account(const std::string& dir) {
    CHECK(achievement_console("gba", "a.gba") == 5);
    CHECK(achievement_console("md", "Game.32X") == 10);
    CHECK(achievement_console("md", "Game.md") == 1);
    CHECK(achievement_console("nes", "Zelda.fds") == 81);
    CHECK(achievement_console("psx", "x.cue") == 12);
    CHECK(achievement_console("scummvm", "x") == 0);
    CHECK(achievement_console("pico", "x.md") == 0);
    const std::string account = dir + "/account.json";
    std::ofstream(account) << "{\"user\": \"Tester\\u00e9\", \"token\": \"abc\\\"123\"}";
    std::string user, token;
    CHECK(read_achievement_account(account, user, token));
    CHECK(user == "Tester\xc3\xa9");
    CHECK(token == "abc\"123");
    std::ofstream(account) << "{\"user\": \"Tester\"}";
    CHECK(!read_achievement_account(account, user, token));
}

void test_unlock(const std::string& dir) {
    auto site = std::make_unique<FakeSite>();
    FakeSite* fake = site.get();
    fake->answers["login2"] = "{\"Success\":true,\"User\":\"Tester\",\"Token\":\"abc\",\"Score\":10,"
                              "\"SoftcoreScore\":3,\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
    fake->answers["achievementsets"] = patch;
    fake->answers["startsession"] = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700000000}";
    fake->answers["awardachievement"] =
        "{\"Success\":true,\"Score\":15,\"SoftcoreScore\":8,\"AchievementID\":5501,\"AchievementsRemaining\":1}";

    uint8_t memory[0x10] = {};
    AchievementOptions options;
    options.user = "Tester";
    options.token = "abc";
    options.list_path = dir + "/library/key/achievements.tsv";
    options.badges = dir + "/badges";
    std::vector<uint8_t> cart(0x200, 0x42); // hashed as is: the fake site answers any hash
    Achievements a;
    std::string error;
    const bool started = a.start(
        options, 5, "game.gba", &cart,
        [&](uint32_t address, uint8_t* buffer, uint32_t size) {
            uint32_t n = 0;
            for (; n < size && address + n < sizeof memory; ++n) buffer[n] = memory[address + n];
            return n;
        },
        std::move(site), error);
    CHECK(started);
    for (int i = 0; i < 20 && !a.game_ready(); ++i) a.pump();
    CHECK(a.game_ready());
    CHECK(fake->asked_for("login2"));
    CHECK(fake->asked_for("startsession"));
    CHECK(a.game_title() == "Sample Game");
    AchievementNotice n;
    CHECK(a.take_notice(n) && n.kind == AchievementNotice::GameReady && n.title == "Sample Game" && n.detail == "0 / 2");
    CHECK(fake->saved.size() == 3); // the game's badge and both achievements'
    const auto entries = a.entries();
    CHECK(entries.size() == 2);
    CHECK(!entries.empty() && entries[0].description == "Reach the castle");
    std::string list = read_text(options.list_path);
    CHECK(list.rfind("#rgds-achievements 1\ngame\t1234\tSample Game\t0\t2\t0\t7\tTester\t", 0) == 0);
    CHECK(list.find("ach\t5501\t0\t5\t0\t") != std::string::npos);

    for (int i = 0; i < 3; ++i) a.frame(); // primed: false first
    memory[1] = 3;
    memory[2] = 7;
    a.frame();
    a.frame();
    CHECK(a.take_notice(n) && n.kind == AchievementNotice::Unlocked && n.title == "First steps" && n.points == 5);
    CHECK(n.badge == options.badges + "/00234.png");
    for (int i = 0; i < 5; ++i) a.pump();
    CHECK(fake->asked_for("awardachievement"));
    list = read_text(options.list_path);
    CHECK(list.find("\tSample Game\t1\t2\t5\t7\t") != std::string::npos);
    CHECK(list.find("ach\t5501\t1\t5\t") != std::string::npos);

    // A loaded state starts the set's progress over: no false unlock of what was half way.
    a.state_loaded();
    a.frame();
    CHECK(!a.take_notice(n));
}

void test_sign_in_refused(const std::string& dir) {
    auto site = std::make_unique<FakeSite>();
    site->answers["login2"] = "{\"Success\":false,\"Error\":\"Invalid User/Token\",\"Code\":\"invalid_credentials\"}";
    AchievementOptions options;
    options.user = "Tester";
    options.token = "old";
    options.list_path = dir + "/refused.tsv";
    Achievements a;
    std::string error;
    CHECK(a.start(options, 5, "x.gba", nullptr, nullptr, std::move(site), error));
    for (int i = 0; i < 5; ++i) a.pump();
    AchievementNotice n;
    CHECK(a.take_notice(n) && n.kind == AchievementNotice::Problem);
    CHECK(!a.game_ready());
    CHECK(access(options.list_path.c_str(), F_OK) != 0);
    // Not signed in at all: nothing starts.
    Achievements b;
    options.token.clear();
    CHECK(!b.start(options, 5, "x.gba", nullptr, nullptr, std::make_unique<FakeSite>(), error));
}

// The real pipes, against a stand-in helper that answers with the request's own line.
void test_helper_pipes(const std::string& dir) {
    const std::string stub = dir + "/stub.py";
    std::ofstream(stub) << "import sys\n"
                           "for line in sys.stdin.buffer:\n"
                           "    f = line.rstrip(b'\\n').split(b'\\t')\n"
                           "    body = b'|'.join(f[1:])\n"
                           "    sys.stdout.buffer.write(f[0] + b' 200 %d\\n' % len(body) + body)\n"
                           "    sys.stdout.buffer.flush()\n";
    auto helper = start_achievement_helper({"python3", "-B", stub}, "Test/1");
    CHECK(helper != nullptr);
    if (!helper) return;
    CHECK(helper->send(7, "https://retroachievements.org/dorequest.php", "r=login2&u=a", "x/y", ""));
    CHECK(helper->send(8, "https://x/\ny", "", "", "/tmp/b.png"));
    std::map<uint64_t, std::string> got;
    for (int i = 0; i < 400 && got.size() < 2; ++i) {
        uint64_t id;
        int status;
        std::string body;
        if (helper->receive(id, status, body)) got[id] = std::to_string(status) + ":" + body;
        else usleep(5000);
    }
    CHECK(got[7] == "200:https://retroachievements.org/dorequest.php|r=login2&u=a|x/y|");
    CHECK(got[8] == "200:https://x/ y|||/tmp/b.png"); // a newline inside a field never splits the request
    CHECK(helper->alive());
}

} // namespace

int main() {
    char pattern[] = "/tmp/play-achievements-XXXXXX";
    const char* made = mkdtemp(pattern);
    if (!made) return 2;
    const std::string dir = made;
    test_console_and_account(dir);
    test_unlock(dir);
    test_sign_in_refused(dir);
    test_helper_pipes(dir);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
