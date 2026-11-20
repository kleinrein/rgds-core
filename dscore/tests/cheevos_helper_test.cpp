// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The helper-process transport (make_helper_backend): the line protocol, the
// answer framing, stale answers, status 0, saved files, a helper that dies or
// hangs, and that no child is left behind. The helper here is a small fake
// written in python3 that answers from the URL; nothing leaves the machine.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "check.h"
#include "cheevos/cheevos_client.h"
#include "cheevos/cheevos_http.h"
#include "core/nds.h"

using namespace ds;

namespace {

const char* kFake = R"PY(import os, sys, time
args = sys.argv[1:]
badges = args[args.index("--badges") + 1]
out = sys.stdout.buffer
def answer(ident, status, body):
    out.write(b"%s %d %d\n" % (ident.encode(), status, len(body)) + body)
    out.flush()
for line in iter(sys.stdin.buffer.readline, b""):
    ident, url, post, ctype, save = (line.decode().rstrip("\n").split("\t") + [""] * 5)[:5]
    if url.endswith("/die"):
        sys.exit(3)
    if url.endswith("/die-once"):
        mark = os.path.join(badges, "died")
        if not os.path.exists(mark):
            open(mark, "w").close()
            sys.exit(3)
    if url.endswith("/hang"):
        time.sleep(5)
    if url.endswith("/offline"):
        answer(ident, 0, b"")
        continue
    if url.endswith("/stale"):
        answer("999999", 200, b"not yours\n")
    if url.endswith("/big"):
        answer(ident, 200, bytes(range(256)) * 4096)
        continue
    if save:
        with open(save, "wb") as f:
            f.write(b"PNG" + url.encode())
        answer(ident, 200, b"")
        continue
    body = "%s|%s|%s|%s|%s" % (url, post, ctype, os.environ.get("RGDS_RA_USER_AGENT", ""),
                               os.environ.get("LD_PRELOAD", "-"))
    answer(ident, 200 if not url.endswith("/missing") else 404, body.encode())
)PY";

// A pretend RetroAchievements: a known game with three achievements listed
// out of id order (the list must keep the site's order), one already held.
const char* kServer = R"PY(import json, sys, urllib.parse
out = sys.stdout.buffer
def answer(ident, status, body):
    out.write(b"%s %d %d\n" % (ident.encode(), status, len(body)) + body)
    out.flush()
def ach(i, title, mem, rarity):
    return {"ID": i, "Title": title, "Description": "Desc %d" % i, "Flags": 3, "Points": i // 10,
            "MemAddr": mem, "Author": "A", "BadgeName": "%05d" % i, "Created": 1, "Modified": 1,
            "Rarity": rarity, "RarityHardcore": 1.0,
            "BadgeURL": "https://media.retroachievements.org/Badge/%05d.png" % i,
            "BadgeLockedURL": "https://media.retroachievements.org/Badge/%05d_lock.png" % i}
game = {"Success": True, "GameId": 4321, "Title": "Test\tGame", "ConsoleId": 18,
        "ImageIconUrl": "https://media.retroachievements.org/Images/1.png", "RichPresenceGameId": 4321,
        "RichPresencePatch": "", "Sets": [{"AchievementSetId": 1, "GameId": 4321, "Title": None, "Type": "core",
        "ImageIconUrl": "https://media.retroachievements.org/Images/1.png",
        "Achievements": [ach(30, "Third\tone", "0xH0012=5", 12.5), ach(10, "First", "0xH0010=1", 50.0),
                         ach(20, "Second", "0xH0011=1", 3.25)], "Leaderboards": []}]}
for line in iter(sys.stdin.buffer.readline, b""):
    ident, url, post, ctype, save = (line.decode().rstrip("\n").split("\t") + [""] * 5)[:5]
    if save:
        open(save, "wb").write(b"PNG")
        answer(ident, 200, b"")
        continue
    r = urllib.parse.parse_qs(post or urllib.parse.urlsplit(url).query).get("r", [""])[0]
    if r == "login2":
        body = {"Success": True, "User": "Tester", "Token": "tok", "Score": 0, "Messages": 0}
    elif r == "achievementsets":
        body = game
    elif r == "startsession":
        body = {"Success": True, "Unlocks": [{"ID": 20, "When": 1700000000}], "ServerNow": 1700000100}
    elif r == "awardachievement":
        body = {"Success": True, "Score": 1, "SoftcoreScore": 1, "AchievementID": 10, "AchievementsRemaining": 1}
    else:
        body = {"Success": True}
    answer(ident, 200, json.dumps(body).encode())
)PY";

std::string g_dir;

std::string make_dir() {
  char tmpl[] = "/tmp/dsperate_cheevos_helperXXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  return std::string(d);
}

std::string write_fake() {
  const std::string path = g_dir + "/fake_helper.py";
  std::FILE* f = std::fopen(path.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs(kFake, f);
  std::fclose(f);
  return path;
}

cheevos::HelperOptions options(const std::string& script) {
  cheevos::HelperOptions o;
  o.script = script;
  o.badges = g_dir + "/badges";
  ::mkdir(o.badges.c_str(), 0755);
  o.user_agent = "DSperate/9.9.9 rcheevos/12.4.0";
  o.timeout_ms = 3000;
  return o;
}

cheevos::Request get(const std::string& url) {
  cheevos::Request r;
  r.url = url;
  return r;
}

bool no_children_left() {
  int status = 0;
  return ::waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD;
}

void requests_and_answers(const std::string& script) {
  std::string err;
  auto http = cheevos::make_helper_backend(options(script), err);
  CHECK(http != nullptr);
  CHECK(err.empty());

  // GET: URL through, empty post; the User-Agent from the options; the
  // preloaded bridge kept out of python.
  ::setenv("LD_PRELOAD", "/nonexistent/bridge.so", 1);
  cheevos::Response r = http->perform(get("https://retroachievements.org/a"), "ignored");
  ::unsetenv("LD_PRELOAD");
  CHECK(r.status == 200);
  CHECK(r.body == "https://retroachievements.org/a|||DSperate/9.9.9 rcheevos/12.4.0|-");

  // POST with a content type.
  cheevos::Request p;
  p.url = "https://retroachievements.org/dorequest.php";
  p.body = "r=login2&u=someone&t=abc%20def";
  p.content_type = "application/x-www-form-urlencoded";
  r = http->perform(p, "");
  CHECK(r.status == 200);
  CHECK(r.body ==
        "https://retroachievements.org/dorequest.php|r=login2&u=someone&t=abc%20def|application/x-www-form-urlencoded|"
        "DSperate/9.9.9 rcheevos/12.4.0|-");

  // An HTTP error is a status, not a transport failure.
  r = http->perform(get("https://retroachievements.org/missing"), "");
  CHECK(r.status == 404);

  // Status 0: the site was not reached; rcheevos' retry handling takes it.
  r = http->perform(get("https://retroachievements.org/offline"), "");
  CHECK(r.status == 0);
  CHECK(r.body.empty());
  CHECK(!r.error.empty());

  // An answer for an older request is skipped, with its body.
  r = http->perform(get("https://retroachievements.org/stale"), "");
  CHECK(r.status == 200);
  CHECK(r.body.rfind("https://retroachievements.org/stale|", 0) == 0);

  // A body with newlines and every byte value, larger than one read.
  r = http->perform(get("https://retroachievements.org/big"), "");
  CHECK(r.status == 200);
  CHECK(r.body.size() == 256u * 4096u);
  for (size_t i = 0; i < r.body.size(); ++i) CHECK(static_cast<unsigned char>(r.body[i]) == (i & 255));

  // SAVE-PATH: the helper writes the file, the answer has no body.
  cheevos::Request s = get("https://media.retroachievements.org/Badge/1234.png");
  s.save_path = g_dir + "/badges/1234.png";
  r = http->perform(s, "");
  CHECK(r.status == 200);
  CHECK(r.body.empty());
  std::FILE* f = std::fopen(s.save_path.c_str(), "rb");
  CHECK(f != nullptr);
  char buf[128] = {};
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  CHECK(std::string(buf, n) == "PNGhttps://media.retroachievements.org/Badge/1234.png");
  ::unlink(s.save_path.c_str());

  // A tab would shift the helper's columns: refused before it is sent.
  r = http->perform(get("https://retroachievements.org/a\tb"), "");
  CHECK(r.status == 0);
  CHECK(!r.error.empty());

  http.reset(); // EOF on its stdin: it leaves, and is reaped
  CHECK(no_children_left());
}

void a_helper_that_dies_is_started_again_once(const std::string& script) {
  std::string err;
  auto http = cheevos::make_helper_backend(options(script), err);
  CHECK(http != nullptr);
  ::unlink((g_dir + "/badges/died").c_str());

  // Dies on the first try; the request goes to a fresh helper and is answered.
  cheevos::Response r = http->perform(get("https://retroachievements.org/die-once"), "");
  CHECK(r.status == 200);
  CHECK(no_children_left() == false); // the new helper is running
  r = http->perform(get("https://retroachievements.org/a"), "");
  CHECK(r.status == 200);

  // Dies on both tries: status 0, and no more helpers this session.
  r = http->perform(get("https://retroachievements.org/die"), "");
  CHECK(r.status == 0);
  CHECK(!r.error.empty());
  CHECK(no_children_left());
  r = http->perform(get("https://retroachievements.org/a"), "");
  CHECK(r.status == 0);
  CHECK(no_children_left());
  http.reset();
  CHECK(no_children_left());
  ::unlink((g_dir + "/badges/died").c_str());
}

void a_hung_helper_times_out_and_is_replaced(const std::string& script) {
  std::string err;
  cheevos::HelperOptions o = options(script);
  o.timeout_ms = 300;
  auto http = cheevos::make_helper_backend(o, err);
  CHECK(http != nullptr);
  cheevos::Response r = http->perform(get("https://retroachievements.org/hang"), "");
  CHECK(r.status == 0);
  CHECK(!r.error.empty());
  CHECK(no_children_left()); // killed and reaped
  r = http->perform(get("https://retroachievements.org/a"), "");
  CHECK(r.status == 200);

  // interrupt(): shutdown does not wait out the full timeout.
  o.timeout_ms = 60000;
  auto slow = cheevos::make_helper_backend(o, err);
  CHECK(slow != nullptr);
  slow->interrupt();
  r = slow->perform(get("https://retroachievements.org/a"), "");
  // Two seconds of grace from interrupt(): an answer that quick still lands.
  CHECK(r.status == 200);
  slow.reset();
  http.reset();
  CHECK(no_children_left());
}

void missing_pieces_are_reported() {
  std::string err;
  cheevos::HelperOptions o;
  o.script = g_dir + "/nope.py";
  o.badges = g_dir;
  CHECK(cheevos::make_helper_backend(o, err) == nullptr);
  CHECK(!err.empty());
  o.script = g_dir + "/fake_helper.py";
  o.badges.clear();
  CHECK(cheevos::make_helper_backend(o, err) == nullptr);
  CHECK(!err.empty());
  o.badges = g_dir;
  o.python = "/nonexistent/python3";
  CHECK(cheevos::make_helper_backend(o, err) == nullptr);
  CHECK(!err.empty());
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<std::vector<std::string>> rows(const std::string& text) {
  std::vector<std::vector<std::string>> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> cols;
    size_t at = 0;
    for (;;) {
      const size_t tab = line.find('\t', at);
      cols.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
      if (tab == std::string::npos) break;
      at = tab + 1;
    }
    out.push_back(std::move(cols));
  }
  return out;
}

template <class Done> bool pump(cheevos::Client& c, Done done) {
  for (int i = 0; i < 500; ++i) {
    c.idle();
    if (done()) return true;
    ::usleep(10000);
  }
  return false;
}

// The whole path through the helper: token sign-in, the set, the badges,
// the list file and an unlock reported and listed.
void a_session_writes_the_list() {
  const std::string script = g_dir + "/fake_server.py";
  {
    std::FILE* f = std::fopen(script.c_str(), "w");
    CHECK(f != nullptr);
    std::fputs(kServer, f);
    std::fclose(f);
  }
  const std::string badges = g_dir + "/badges";
  const std::string list = g_dir + "/achievements.tsv";
  auto nds = std::make_unique<NDS>();
  {
    cheevos::Client c;
    c.set_helper(options(script));
    c.set_badges_dir(badges);
    c.set_list_path(list);
    std::string err;
    CHECK(c.start(err));
    c.sign_in_with_token("Tester", "tok");
    CHECK(pump(c, [&] { return c.state() == cheevos::State::SignedIn; }));
    c.load_game(*nds, "0123456789abcdef0123456789abcdef");
    CHECK(pump(c, [&] { return c.state() == cheevos::State::Playing; }));
    // Once the pictures are in, the list names them.
    CHECK(pump(c, [&] { return read_file(list).find(badges + "/00020.png") != std::string::npos; }));
    std::vector<std::vector<std::string>> r = rows(read_file(list));
    CHECK(r.size() == 5);
    CHECK(r[0].size() == 1 && r[0][0] == "#rgds-achievements 1");
    CHECK(r[1].size() == 9);
    CHECK(r[1][0] == "game" && r[1][1] == "4321" && r[1][2] == "Test Game");
    CHECK(r[1][3] == "1" && r[1][4] == "3" && r[1][5] == "2" && r[1][6] == "6" && r[1][7] == "Tester");
    CHECK(std::atoll(r[1][8].c_str()) > 1700000000);
    const char* order[] = {"30", "10", "20"};
    for (int i = 0; i < 3; ++i) {
      const std::vector<std::string>& a = r[2 + i];
      CHECK(a.size() == 10);
      CHECK(a[0] == "ach" && a[1] == order[i]);
      CHECK(a[8] == badges + "/000" + order[i] + ".png");
    }
    CHECK(r[2][2] == "0" && r[2][3] == "3" && r[2][4] == "0" && r[2][5] == "12.50" && r[2][6] == "Third one");
    CHECK(r[2][7] == "Desc 30");
    CHECK(r[4][2] == "1" && r[4][4] == "1700000000" && r[4][5] == "3.25");
    CHECK(r[3][2] == "0");
    for (const char* b : {"00010", "00020", "00030"}) CHECK(read_file(badges + "/" + b + ".png") == "PNG");

    // Earned in play: false first (a trigger never fires on its first look), then true.
    for (int i = 0; i < 5; ++i) c.frame();
    (void)c.take_messages();
    nds->bus.main_ram.get()[0x10] = 1;
    bool unlocked = false;
    for (int i = 0; i < 10 && !unlocked; ++i) {
      c.frame();
      for (const cheevos::Message& m : c.take_messages())
        if (m.kind == cheevos::Message::Kind::Unlock && m.text == "First" && m.points == 1) unlocked = true;
    }
    CHECK(unlocked);
    r = rows(read_file(list));
    CHECK(r.size() == 5 && r[3][1] == "10" && r[3][2] == "1" && std::atoll(r[3][4].c_str()) > 1700000000);
    CHECK(r[1][3] == "2" && r[1][5] == "3");
    CHECK(pump(c, [&] { return c.take_messages().empty(); })); // the award went out (or is on its way)
    c.shutdown();
  }
  CHECK(no_children_left());
  for (const char* b : {"00010", "00020", "00030"}) ::unlink((badges + "/" + b + ".png").c_str());
  ::unlink(list.c_str());
  ::unlink(script.c_str());
}

} // namespace

int main() {
  if (std::system("python3 -c 'import sys' >/dev/null 2>&1") != 0) {
    std::fprintf(stderr, "cheevos_helper: no python3, skipped\n");
    return 0;
  }
  g_dir = make_dir();
  const std::string script = write_fake();
  requests_and_answers(script);
  a_helper_that_dies_is_started_again_once(script);
  a_hung_helper_times_out_and_is_replaced(script);
  missing_pieces_are_reported();
  a_session_writes_the_list();
  ::unlink(script.c_str());
  ::rmdir((g_dir + "/badges").c_str());
  ::rmdir(g_dir.c_str());
  std::puts("cheevos_helper: ok");
  return 0;
}
