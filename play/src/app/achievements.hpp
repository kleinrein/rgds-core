// SPDX-License-Identifier: GPL-3.0-or-later
// RetroAchievements (rcheevos's rc_client) for whatever game is being played.
//
// Casual mode (softcore) only: the shared in-game menu's states, cheats and fast-forward stay as they are.
// Signing in happens once, in the launcher (runtime/achievements.py keeps the token); here the player signs in
// with that token, identifies the game by its hash, evaluates the set after every frame and submits unlocks.
//
// The player links no TLS library: rcheevos's server calls go through a helper process (runtime/achievements.py
// serve) on pipes, answered on the game thread by pump(), so every rc_client callback runs on that thread.
//
// What the owner sees: notices (an unlock, the game's set loaded, a mastery, sign-in trouble) for the lower
// screen, and a list of the game's achievements written for the launcher's Game info page (list_path).
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace play {

// The way to the internet: requests out, answers in (a helper process; a fake in tests).
class AchievementTransport {
public:
    virtual ~AchievementTransport() = default;
    // save: write the answer's body to this file (a badge picture) instead of returning it.
    virtual bool send(uint64_t id, const std::string& url, const std::string& post, const std::string& content_type,
                      const std::string& save) = 0;
    // One finished answer, without waiting; false when none is ready. status 0: the site could not be reached.
    virtual bool receive(uint64_t& id, int& status, std::string& body) = 0;
    // False once it can answer nothing more (the helper has gone).
    virtual bool alive() const { return true; }
};
// runtime/achievements.py serve, started with argv (python3 -B script serve --badges DIR); null if it can't start.
std::unique_ptr<AchievementTransport> start_achievement_helper(const std::vector<std::string>& argv,
                                                               const std::string& user_agent);

struct AchievementOptions {
    std::string user, token;          // the launcher's sign-in (read_achievement_account)
    std::string list_path;            // achievements.tsv for Game info ("" for none)
    std::string badges;               // folder for badge pictures ("" for none)
    std::vector<std::string> helper;  // the helper's argv
    std::string client_name = "RGDS-Play";
    std::string client_version = "0";
};
// {"user": ..., "token": ...} (runtime/achievements.py's account file); false when unreadable or incomplete.
bool read_achievement_account(const std::string& path, std::string& user, std::string& token);

// RetroAchievements' console number for a system id the launcher knows (gba, nes, md, ...), from the game's file name
// where that decides it (a .32x on the Mega Drive, a .fds on the NES); 0 when it has no achievements to offer.
uint32_t achievement_console(const std::string& system, const std::string& file_name);

struct AchievementNotice {
    enum Kind { Unlocked, GameReady, Mastered, NoSet, Problem } kind = Unlocked;
    std::string title, detail; // detail: the achievement's description, or the line under the title
    uint32_t points = 0;
    std::string badge; // where its badge picture (PNG) is or will be once downloaded, else ""
};

// One achievement as the list and tests see it.
struct AchievementEntry {
    uint32_t id = 0, points = 0;
    bool unlocked = false;
    int64_t unlock_time = 0;
    float rarity = -1;
    std::string title, description, badge, progress;
};

class Achievements {
public:
    // address, buffer, bytes -> bytes read (rcheevos's address space for the console)
    using ReadMemory = std::function<uint32_t(uint32_t, uint8_t*, uint32_t)>;
    Achievements();
    ~Achievements();
    Achievements(const Achievements&) = delete;
    Achievements& operator=(const Achievements&) = delete;

    // Signs in and loads the game's set: `file` is hashed (or `data` when given: a cartridge already in memory).
    // transport: null to start the helper from options.helper. False with `error` when nothing can start.
    bool start(const AchievementOptions& options, uint32_t console, const std::string& file,
               const std::vector<uint8_t>* data, ReadMemory read, std::unique_ptr<AchievementTransport> transport,
               std::string& error);
    bool active() const { return client_ != nullptr; }

    void frame(); // after each emulated frame
    void idle();  // instead of frame() while the game is held (the menu is up), at least once a second
    void pump();  // answers that came in: every loop of the player, whatever else it does

    // A state was loaded: the set's progress starts over (a state from another moment would unlock falsely).
    void state_loaded();

    bool take_notice(AchievementNotice& out);
    std::vector<AchievementEntry> entries() const;
    bool game_ready() const { return game_ready_; }
    std::string game_title() const;

    // Called by the rcheevos callbacks (C), public for that reason only.
    void server_call(const void* request, void* callback, void* callback_data);
    void event(const void* event);
    uint32_t read(uint32_t address, uint8_t* buffer, uint32_t bytes) { return read_ ? read_(address, buffer, bytes) : 0; }
    void signed_in(int result, const char* error);
    void loaded(int result, const char* error);

private:
    void notice(AchievementNotice n);
    void write_list();
    std::string badge_file(const std::string& name) const;
    void fetch_badge(const std::string& name, const std::string& url);
    void finish(uint64_t id, int status, const std::string& body);

    struct Pending {
        void* callback = nullptr, *data = nullptr;
        std::string badge; // a badge download: its picture's name
    };
    void* client_ = nullptr; // rc_client_t*
    AchievementOptions options_;
    uint32_t console_ = 0;
    std::string file_;
    std::vector<uint8_t> data_;
    bool has_data_ = false;
    ReadMemory read_;
    std::unique_ptr<AchievementTransport> transport_;
    std::map<uint64_t, Pending> pending_;
    uint64_t next_id_ = 1;
    std::deque<AchievementNotice> notices_;
    std::map<std::string, bool> badges_; // name -> asked for
    bool game_ready_ = false, list_dirty_ = false, helper_lost_ = false;
    uint64_t frames_ = 0;
};

} // namespace play
