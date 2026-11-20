// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/achievements.hpp"

#include "app/storage.hpp"

#include "rc_client.h"
#include "rc_consoles.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace play {

namespace {

// ---- The helper process: runtime/achievements.py serve on two pipes.
class HelperTransport final : public AchievementTransport {
public:
    ~HelperTransport() override { stop(); }

    bool start(const std::vector<std::string>& argv, const std::string& user_agent) {
        if (argv.empty()) return false;
        int in[2], out[2];
        if (pipe(in) != 0) return false;
        if (pipe(out) != 0) {
            ::close(in[0]), ::close(in[1]);
            return false;
        }
        // A helper gone away must not take the player with it on the next write.
        std::signal(SIGPIPE, SIG_IGN);
        std::vector<std::string> args = argv;
        std::vector<char*> raw;
        for (auto& a : args) raw.push_back(a.data());
        raw.push_back(nullptr);
        if (!user_agent.empty()) setenv("RGDS_RA_USER_AGENT", user_agent.c_str(), 1);
        pid_ = fork();
        if (pid_ == 0) {
            dup2(in[0], STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
            execvp(raw[0], raw.data());
            _exit(127);
        }
        ::close(in[0]);
        ::close(out[1]);
        if (pid_ < 0) {
            ::close(in[1]), ::close(out[0]);
            return false;
        }
        fcntl(in[1], F_SETFD, FD_CLOEXEC);
        fcntl(out[0], F_SETFD, FD_CLOEXEC);
        to_ = in[1];
        from_ = out[0];
        reader_ = std::thread([this] { read_loop(); });
        return true;
    }

    bool send(uint64_t id, const std::string& url, const std::string& post, const std::string& content_type,
              const std::string& save) override {
        if (to_ < 0) return false;
        auto clean = [](std::string s) {
            for (char& c : s)
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            return s;
        };
        const std::string line = std::to_string(id) + "\t" + clean(url) + "\t" + clean(post) + "\t" +
                                 clean(content_type) + "\t" + clean(save) + "\n";
        size_t done = 0;
        while (done < line.size()) {
            const ssize_t n = ::write(to_, line.data() + done, line.size() - done);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            done += size_t(n);
        }
        return true;
    }

    bool receive(uint64_t& id, int& status, std::string& body) override {
        std::lock_guard<std::mutex> guard(lock_);
        if (answers_.empty()) return false;
        id = answers_.front().id;
        status = answers_.front().status;
        body = std::move(answers_.front().body);
        answers_.pop_front();
        return true;
    }

    bool alive() const override { return !dead_; }

private:
    struct Answer {
        uint64_t id;
        int status;
        std::string body;
    };

    bool read_exact(char* out, size_t size) {
        size_t done = 0;
        while (done < size) {
            const ssize_t n = ::read(from_, out + done, size - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += size_t(n);
        }
        return true;
    }

    void read_loop() {
        for (;;) {
            std::string header;
            char c;
            while (true) {
                if (!read_exact(&c, 1)) {
                    dead_ = true;
                    return;
                }
                if (c == '\n') break;
                header += c;
                if (header.size() > 128) {
                    dead_ = true;
                    return;
                }
            }
            unsigned long long id = 0, length = 0;
            int status = 0;
            if (std::sscanf(header.c_str(), "%llu %d %llu", &id, &status, &length) != 3 || length > (32u << 20)) {
                dead_ = true;
                return;
            }
            std::string body(size_t(length), '\0');
            if (length && !read_exact(body.data(), body.size())) {
                dead_ = true;
                return;
            }
            std::lock_guard<std::mutex> guard(lock_);
            answers_.push_back({id, status, std::move(body)});
        }
    }

    void stop() {
        if (to_ >= 0) ::close(to_);
        to_ = -1;
        if (pid_ > 0) {
            // Requests still in flight get a moment (an unlock being submitted), then the helper goes.
            for (int i = 0; i < 100 && waitpid(pid_, nullptr, WNOHANG) == 0; ++i) usleep(20000);
            if (waitpid(pid_, nullptr, WNOHANG) == 0) {
                kill(pid_, SIGTERM);
                waitpid(pid_, nullptr, 0);
            }
            pid_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (from_ >= 0) ::close(from_);
        from_ = -1;
    }

    pid_t pid_ = -1;
    int to_ = -1, from_ = -1;
    std::thread reader_;
    std::mutex lock_;
    std::deque<Answer> answers_;
    std::atomic<bool> dead_{false};
};

Achievements* owner(const rc_client_t* client) { return static_cast<Achievements*>(rc_client_get_userdata(client)); }

uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t bytes, rc_client_t* client) {
    Achievements* a = owner(client);
    return a ? a->read(address, buffer, bytes) : 0;
}
void server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data,
                 rc_client_t* client) {
    if (Achievements* a = owner(client)) a->server_call(request, reinterpret_cast<void*>(callback), data);
}
void event_handler(const rc_client_event_t* event, rc_client_t* client) {
    if (Achievements* a = owner(client)) a->event(event);
}
void log_message(const char* message, const rc_client_t*) { std::fprintf(stderr, "rgds-play: rcheevos: %s\n", message); }
void login_done(int result, const char* error, rc_client_t*, void* userdata) {
    static_cast<Achievements*>(userdata)->signed_in(result, error);
}
void load_done(int result, const char* error, rc_client_t*, void* userdata) {
    static_cast<Achievements*>(userdata)->loaded(result, error);
}

std::string one_line(const char* text) {
    std::string s = text ? text : "";
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string lower_extension(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || name.find('/', dot) != std::string::npos) return "";
    std::string e = name.substr(dot);
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// A JSON string value by key, in the small flat object runtime/achievements.py writes.
bool json_string(const std::string& text, const std::string& key, std::string& out) {
    const std::string quoted = "\"" + key + "\"";
    size_t at = text.find(quoted);
    if (at == std::string::npos) return false;
    at = text.find(':', at + quoted.size());
    if (at == std::string::npos) return false;
    at = text.find('"', at);
    if (at == std::string::npos) return false;
    out.clear();
    for (size_t i = at + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') return true;
        if (c == '\\' && i + 1 < text.size()) {
            const char e = text[++i];
            if (e == 'u' && i + 4 < text.size()) {
                const unsigned v = unsigned(std::strtoul(text.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (v < 0x80) out += char(v);
                else if (v < 0x800) out += char(0xC0 | (v >> 6)), out += char(0x80 | (v & 0x3F));
                else out += char(0xE0 | (v >> 12)), out += char(0x80 | ((v >> 6) & 0x3F)), out += char(0x80 | (v & 0x3F));
            } else out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else out += c;
    }
    return false;
}

} // namespace

std::unique_ptr<AchievementTransport> start_achievement_helper(const std::vector<std::string>& argv,
                                                               const std::string& user_agent) {
    auto helper = std::make_unique<HelperTransport>();
    if (!helper->start(argv, user_agent)) return nullptr;
    return helper;
}

bool read_achievement_account(const std::string& path, std::string& user, std::string& token) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return json_string(text.str(), "user", user) && json_string(text.str(), "token", token) && !user.empty() &&
           !token.empty();
}

uint32_t achievement_console(const std::string& system, const std::string& file_name) {
    const std::string ext = lower_extension(file_name);
    static const std::map<std::string, uint32_t> consoles = {
        {"gba", RC_CONSOLE_GAMEBOY_ADVANCE},
        {"gbc", RC_CONSOLE_GAMEBOY_COLOR},
        {"gb", RC_CONSOLE_GAMEBOY},
        {"nes", RC_CONSOLE_NINTENDO},
        {"snes", RC_CONSOLE_SUPER_NINTENDO},
        {"md", RC_CONSOLE_MEGA_DRIVE},
        {"sms", RC_CONSOLE_MASTER_SYSTEM},
        {"gg", RC_CONSOLE_GAME_GEAR},
        {"sg", RC_CONSOLE_SG1000},
        {"segacd", RC_CONSOLE_SEGA_CD},
        {"coleco", RC_CONSOLE_COLECOVISION},
        {"intellivision", RC_CONSOLE_INTELLIVISION},
        {"msx", RC_CONSOLE_MSX},
        {"cpc", RC_CONSOLE_AMSTRAD_PC},
        {"pce", RC_CONSOLE_PC_ENGINE},
        {"pcecd", RC_CONSOLE_PC_ENGINE_CD},
        {"atari", RC_CONSOLE_ATARI_2600},
        {"atari7800", RC_CONSOLE_ATARI_7800},
        {"lynx", RC_CONSOLE_ATARI_LYNX},
        {"vb", RC_CONSOLE_VIRTUAL_BOY},
        {"dos", RC_CONSOLE_MS_DOS},
        {"psx", RC_CONSOLE_PLAYSTATION},
        {"n64", RC_CONSOLE_NINTENDO_64},
        {"zx", RC_CONSOLE_ZX_SPECTRUM},
        {"c64", RC_CONSOLE_COMMODORE_64},
        {"psp", RC_CONSOLE_PSP},
        {"dc", RC_CONSOLE_DREAMCAST},
        {"nds", RC_CONSOLE_NINTENDO_DS},
    };
    // Not offered: the Pico, the Atari 5200, the Amiga, Game & Watch, PICO-8 and ScummVM (rcheevos cannot identify their games).
    const auto found = consoles.find(system);
    if (found == consoles.end()) return 0;
    if (found->second == RC_CONSOLE_MEGA_DRIVE && ext == ".32x") return RC_CONSOLE_SEGA_32X;
    if (found->second == RC_CONSOLE_NINTENDO && ext == ".fds") return RC_CONSOLE_FAMICOM_DISK_SYSTEM;
    return found->second;
}

Achievements::Achievements() = default;

Achievements::~Achievements() {
    if (client_) {
        auto* client = static_cast<rc_client_t*>(client_);
        rc_client_unload_game(client);
        // Answers still arriving are dropped with the client; the helper finishes what it was sending.
        pending_.clear();
        rc_client_destroy(client);
        client_ = nullptr;
    }
}

bool Achievements::start(const AchievementOptions& options, uint32_t console, const std::string& file,
                         const std::vector<uint8_t>* data, ReadMemory read,
                         std::unique_ptr<AchievementTransport> transport, std::string& error) {
    if (client_) return true;
    if (!console) {
        error = "RetroAchievements has no achievements for this system";
        return false;
    }
    if (options.user.empty() || options.token.empty()) {
        error = "not signed in to RetroAchievements";
        return false;
    }
    options_ = options;
    console_ = console;
    file_ = file;
    if (data) data_ = *data;
    has_data_ = data != nullptr;
    read_ = std::move(read);
    auto* client = rc_client_create(read_memory, ::play::server_call);
    if (!client) {
        error = "rcheevos could not start";
        return false;
    }
    client_ = client;
    rc_client_set_userdata(client, this);
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(client, event_handler);
    rc_client_set_hardcore_enabled(client, 0);
    rc_client_set_unofficial_enabled(client, 0);
    transport_ = std::move(transport);
    if (!transport_) {
        char clause[64] = "";
        rc_client_get_user_agent_clause(client, clause, sizeof clause);
        const std::string agent = options_.client_name + "/" + options_.client_version + " (Linux) " + clause;
        transport_ = start_achievement_helper(options_.helper, agent);
    }
    if (!transport_) {
        error = "the RetroAchievements helper could not start";
        rc_client_destroy(client);
        client_ = nullptr;
        return false;
    }
    rc_client_begin_login_with_token(client, options_.user.c_str(), options_.token.c_str(), login_done, this);
    return true;
}

void Achievements::signed_in(int result, const char* error) {
    auto* client = static_cast<rc_client_t*>(client_);
    if (result != RC_OK) {
        std::fprintf(stderr, "rgds-play: RetroAchievements sign-in failed: %s\n", error ? error : "");
        notice({AchievementNotice::Problem, "RetroAchievements",
                result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED
                    ? "Sign in again in Settings"
                    : "Could not sign in",
                0, ""});
        return;
    }
    std::fprintf(stderr, "rgds-play: RetroAchievements: signed in as %s\n", options_.user.c_str());
    rc_client_begin_identify_and_load_game(client, console_, file_.empty() ? nullptr : file_.c_str(),
                                           has_data_ ? data_.data() : nullptr, has_data_ ? data_.size() : 0, load_done,
                                           this);
    data_.clear();
    data_.shrink_to_fit();
}

void Achievements::loaded(int result, const char* error) {
    auto* client = static_cast<rc_client_t*>(client_);
    if (result == RC_NO_GAME_LOADED) {
        std::fprintf(stderr, "rgds-play: RetroAchievements does not know this game file\n");
        notice({AchievementNotice::NoSet, "RetroAchievements", "This game file is not recognised", 0, ""});
        return;
    }
    if (result != RC_OK) {
        std::fprintf(stderr, "rgds-play: RetroAchievements could not load the game: %s\n", error ? error : "");
        notice({AchievementNotice::Problem, "RetroAchievements", "Could not load this game's achievements", 0, ""});
        return;
    }
    game_ready_ = true;
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    std::fprintf(stderr, "rgds-play: RetroAchievements: %s (game %u), %u of %u unlocked\n",
                 game && game->title ? game->title : "?", game ? game->id : 0, summary.num_unlocked_achievements,
                 summary.num_core_achievements);
    AchievementNotice n{AchievementNotice::GameReady, game && game->title ? game->title : "", "", 0, ""};
    if (!summary.num_core_achievements) n.kind = AchievementNotice::NoSet, n.detail = "No achievements yet";
    else
        n.detail = std::to_string(summary.num_unlocked_achievements) + " / " +
                   std::to_string(summary.num_core_achievements);
    if (game && game->badge_name && *game->badge_name && game->badge_url) {
        const std::string name = std::string("game-") + game->badge_name;
        fetch_badge(name, game->badge_url);
        n.badge = badge_file(name);
    }
    notice(n);
    // Every badge up front, so an unlock can show its own picture; kept between sessions.
    for (const auto& e : entries())
        if (!e.badge.empty()) {
            const rc_client_achievement_t* a = rc_client_get_achievement_info(client, e.id);
            if (a && a->badge_url) fetch_badge(a->badge_name, a->badge_url);
        }
    write_list();
}

std::string Achievements::badge_file(const std::string& name) const {
    if (options_.badges.empty() || name.empty()) return "";
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return "";
    return options_.badges + "/" + name + ".png";
}

void Achievements::fetch_badge(const std::string& name, const std::string& url) {
    const std::string path = badge_file(name);
    if (path.empty() || badges_.count(name)) return;
    badges_[name] = true;
    if (access(path.c_str(), F_OK) == 0) return;
    const uint64_t id = next_id_++;
    pending_[id] = Pending{nullptr, nullptr, name};
    if (!transport_->send(id, url, "", "", path)) pending_.erase(id);
}

void Achievements::server_call(const void* request, void* callback, void* callback_data) {
    const auto* r = static_cast<const rc_api_request_t*>(request);
    const uint64_t id = next_id_++;
    pending_[id] = Pending{callback, callback_data, ""};
    if (!transport_ ||
        !transport_->send(id, r->url ? r->url : "", r->post_data ? r->post_data : "",
                          r->content_type ? r->content_type : "", "")) {
        pending_.erase(id);
        rc_api_server_response_t response{};
        response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        reinterpret_cast<rc_client_server_callback_t>(callback)(&response, callback_data);
    }
}

void Achievements::finish(uint64_t id, int status, const std::string& body) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return;
    const Pending p = found->second;
    pending_.erase(found);
    if (!p.callback) return; // a badge: the helper has written it (or not); the popup looks for the file
    rc_api_server_response_t response{};
    response.body = body.data();
    response.body_length = body.size();
    response.http_status_code = status > 0 ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    reinterpret_cast<rc_client_server_callback_t>(p.callback)(&response, p.data);
}

void Achievements::pump() {
    if (!client_ || !transport_) return;
    uint64_t id;
    int status;
    std::string body;
    for (int i = 0; i < 16 && transport_->receive(id, status, body); ++i) finish(id, status, body);
    // The helper went away: what it still owed is answered "unreachable" (rcheevos keeps unlocks to send again).
    if (!transport_->alive() && !pending_.empty()) {
        if (!helper_lost_) std::fprintf(stderr, "rgds-play: the RetroAchievements helper stopped\n");
        helper_lost_ = true;
        auto owed = std::move(pending_);
        pending_.clear();
        for (auto& [owed_id, p] : owed) {
            pending_[owed_id] = p;
            finish(owed_id, 0, "");
        }
    }
    if (list_dirty_) {
        list_dirty_ = false;
        write_list();
    }
}

void Achievements::frame() {
    if (!client_) return;
    rc_client_do_frame(static_cast<rc_client_t*>(client_));
    ++frames_;
}

void Achievements::idle() {
    if (client_) rc_client_idle(static_cast<rc_client_t*>(client_));
}

void Achievements::state_loaded() {
    if (client_ && game_ready_) rc_client_deserialize_progress(static_cast<rc_client_t*>(client_), nullptr);
}

void Achievements::event(const void* data) {
    const auto* e = static_cast<const rc_client_event_t*>(data);
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        const rc_client_achievement_t* a = e->achievement;
        std::fprintf(stderr, "rgds-play: achievement unlocked: %s (%u points)\n", a->title, a->points);
        AchievementNotice n{AchievementNotice::Unlocked, one_line(a->title), one_line(a->description), a->points, ""};
        if (a->badge_url) fetch_badge(a->badge_name, a->badge_url);
        n.badge = badge_file(a->badge_name);
        notice(n);
        list_dirty_ = true;
        break;
    }
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t* game = rc_client_get_game_info(static_cast<rc_client_t*>(client_));
        AchievementNotice n{AchievementNotice::Mastered, game && game->title ? game->title : "",
                            "Every achievement unlocked", 0, ""};
        if (game && game->badge_name) n.badge = badge_file(std::string("game-") + game->badge_name);
        notice(n);
        list_dirty_ = true;
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        std::fprintf(stderr, "rgds-play: RetroAchievements server error: %s\n",
                     e->server_error && e->server_error->error_message ? e->server_error->error_message : "");
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        notice({AchievementNotice::Problem, "RetroAchievements", "Offline: unlocks are sent when back online", 0, ""});
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        notice({AchievementNotice::Problem, "RetroAchievements", "Back online: unlocks sent", 0, ""});
        break;
    default: break; // leaderboards and challenge indicators: not shown yet
    }
}

void Achievements::notice(AchievementNotice n) {
    if (notices_.size() < 16) notices_.push_back(std::move(n));
}

bool Achievements::take_notice(AchievementNotice& out) {
    if (notices_.empty()) return false;
    out = std::move(notices_.front());
    notices_.pop_front();
    return true;
}

std::string Achievements::game_title() const {
    const rc_client_game_t* game = client_ ? rc_client_get_game_info(static_cast<rc_client_t*>(client_)) : nullptr;
    return game && game->title ? game->title : "";
}

std::vector<AchievementEntry> Achievements::entries() const {
    std::vector<AchievementEntry> out;
    if (!client_ || !game_ready_) return out;
    auto* client = static_cast<rc_client_t*>(client_);
    rc_client_achievement_list_t* list =
        rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                          RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return out;
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const rc_client_achievement_bucket_t& bucket = list->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            AchievementEntry e;
            e.id = a->id;
            e.points = a->points;
            e.unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
            e.unlock_time = e.unlocked ? int64_t(a->unlock_time) : 0;
            e.rarity = a->rarity;
            e.title = one_line(a->title);
            e.description = one_line(a->description);
            e.badge = badge_file(a->badge_name);
            e.progress = one_line(a->measured_progress);
            out.push_back(std::move(e));
        }
    }
    rc_client_destroy_achievement_list(list);
    return out;
}

void Achievements::write_list() {
    if (options_.list_path.empty() || !game_ready_) return;
    auto* client = static_cast<rc_client_t*>(client_);
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    std::ostringstream out;
    out << "#rgds-achievements 1\n";
    out << "game\t" << (game ? game->id : 0) << '\t' << one_line(game ? game->title : "") << '\t'
        << summary.num_unlocked_achievements << '\t' << summary.num_core_achievements << '\t'
        << summary.points_unlocked << '\t' << summary.points_core << '\t' << one_line(options_.user.c_str()) << '\t'
        << int64_t(std::time(nullptr)) << '\n';
    for (const auto& e : entries()) {
        char rarity[16] = "";
        if (e.rarity >= 0) std::snprintf(rarity, sizeof rarity, "%.1f", double(e.rarity));
        out << "ach\t" << e.id << '\t' << (e.unlocked ? 1 : 0) << '\t' << e.points << '\t' << e.unlock_time << '\t'
            << rarity << '\t' << e.title << '\t' << e.description << '\t' << e.badge << '\t' << e.progress << '\n';
    }
    const std::string text = out.str();
    std::string error;
    make_directories(parent_directory(options_.list_path));
    if (!write_file_atomic(options_.list_path, std::vector<uint8_t>(text.begin(), text.end()), error))
        std::fprintf(stderr, "rgds-play: achievement list not written: %s\n", error.c_str());
}

} // namespace play
