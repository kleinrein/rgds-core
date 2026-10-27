#pragma once
#include "launcher/model.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <signal.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace rgds {
struct SystemStatus {
    bool brightness = false, power = false, sleep = false, lid = false, charging = false, external_power = false;
    int brightness_percent = 60, battery = -1;
    bool volume = false;
    int volume_percent = 25;
    bool poweroff = false;
};
struct BrightnessUpdate {
    int value;
    bool success;
    // More recent values may have arrived while this value was applying.
    bool settled;
};
struct VolumeUpdate {
    int value;
    bool success;
    bool settled;
};
struct SystemStatusUpdate {
    SystemStatus value;
    bool success;
};
class SystemControls {
    int lid_device = -1;
    std::optional<unsigned long long> lid_sequence;
    bool preview;
    fs::path app, data;
    using Clock = std::chrono::steady_clock;
    pid_t brightness_child = -1;
    int active_brightness = 60;
    bool last_brightness_ok = true;
    std::optional<int> pending_brightness;
    std::optional<BrightnessUpdate> completed_brightness;
    pid_t volume_child = -1;
    int active_volume = 25;
    bool last_volume_ok = true, prefer_volume = false;
    std::optional<int> pending_volume;
    std::optional<VolumeUpdate> completed_volume;
    Clock::time_point next_control = Clock::time_point::min();
    pid_t status_child = -1;
    int status_pipe = -1;
    bool status_cancelled = false, status_output_failed = false;
    std::string status_output;
    SystemStatus cached_status;
    std::optional<SystemStatusUpdate> completed_status;
    pid_t launch(const std::string& command, std::optional<int> value, int output_fd = -1, int close_fd = -1,
                 bool own_group = false, bool menu_close = false) {
        if (app.empty() || data.empty()) return -1;
        std::vector<std::string> args = {"python3", "-B", (app / "runtime/system_controls.py").string(), command};
        if (value) args.push_back(std::to_string(*value));
        if (command == "sleep" || command == "sleep_lid") args.push_back("--menu-wake");
        if (menu_close) args.push_back("--menu-close");
        args.push_back("--data");
        args.push_back(data.string());
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const auto parent = getpid();
        auto child = fork();
        if (child == 0) {
            if (own_group && setpgid(0, 0) != 0) _exit(127);
#ifdef __linux__
            // Helpers must not outlive a killed launcher and race guardian recovery.
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(127);
#else
            (void)parent;
#endif
            if (close_fd >= 0) close(close_fd);
            if (output_fd >= 0) {
                if (dup2(output_fd, STDOUT_FILENO) < 0) _exit(127);
                close(output_fd);
            }
            execvp(argv[0], argv.data());
            _exit(127);
        }
        if (child > 0 && own_group) setpgid(child, child);
        return child;
    }
    int invoke(const std::string& command, std::optional<int> value, std::string* output = nullptr,
               bool menu_close = false) {
        if (preview) return 0;
        int pipe_fd[2];
        if (pipe(pipe_fd) != 0) return 1;
        auto child = launch(command, value, pipe_fd[1], pipe_fd[0], false, menu_close);
        close(pipe_fd[1]);
        std::string result;
        char buffer[512];
        ssize_t count;
        while ((count = read(pipe_fd[0], buffer, sizeof(buffer))) != 0) {
            if (count > 0) result.append(buffer, count);
            else if (errno != EINTR) break;
        }
        close(pipe_fd[0]);
        if (child < 0) return 1;
        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) return 1;
        }
        if (output) *output = result;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    void collect_brightness(bool wait) {
        if (brightness_child <= 0) return;
        int status = 0;
        pid_t result;
        do {
            result = waitpid(brightness_child, &status, wait ? 0 : WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) return;
        brightness_child = -1;
        last_brightness_ok = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (last_brightness_ok) cached_status.brightness_percent = active_brightness;
        completed_brightness = BrightnessUpdate{active_brightness, last_brightness_ok, false};
    }
    void begin_brightness() {
        if (!pending_brightness || brightness_child > 0 || volume_child > 0) return;
        active_brightness = *pending_brightness;
        pending_brightness.reset();
        // Launch at most once per 80 ms, with no hardware access on the UI thread.
        // Keep the helper: it journals both panels before applying their levels.
        next_control = Clock::now() + std::chrono::milliseconds(80);
        prefer_volume = true;
        if (preview) {
            last_brightness_ok = true;
            cached_status.brightness_percent = active_brightness;
            completed_brightness = BrightnessUpdate{active_brightness, true, false};
            return;
        }
        brightness_child = launch("brightness", active_brightness);
        if (brightness_child < 0) {
            last_brightness_ok = false;
            completed_brightness = BrightnessUpdate{active_brightness, false, false};
        }
    }
    void collect_volume(bool wait) {
        if (volume_child <= 0) return;
        int status = 0;
        pid_t result;
        do {
            result = waitpid(volume_child, &status, wait ? 0 : WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) return;
        volume_child = -1;
        last_volume_ok = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (last_volume_ok) cached_status.volume_percent = active_volume;
        completed_volume = VolumeUpdate{active_volume, last_volume_ok, false};
    }
    void begin_volume() {
        if (!pending_volume || brightness_child > 0 || volume_child > 0) return;
        active_volume = *pending_volume;
        pending_volume.reset();
        next_control = Clock::now() + std::chrono::milliseconds(80);
        prefer_volume = false;
        if (preview) {
            last_volume_ok = true;
            cached_status.volume_percent = active_volume;
            completed_volume = VolumeUpdate{active_volume, true, false};
            return;
        }
        // The existing helper owns the mixer lock and persistent volume.ini.
        volume_child = launch("volume", active_volume);
        if (volume_child < 0) {
            last_volume_ok = false;
            completed_volume = VolumeUpdate{active_volume, false, false};
        }
    }
    void collect_controls(bool wait) {
        collect_brightness(wait);
        collect_volume(wait);
    }
    void begin_next_control(bool force = false) {
        if (brightness_child > 0 || volume_child > 0 || (!force && Clock::now() < next_control)) return;
        if (pending_volume && (prefer_volume || !pending_brightness)) begin_volume();
        else begin_brightness();
    }
    static std::optional<SystemStatus> parse_status(const std::string& output) {
        SystemStatus status;
        std::istringstream stream(output);
        std::string line;
        bool brightness_seen = false, battery_seen = false;
        while (std::getline(stream, line)) {
            auto separator = line.find('=');
            if (separator == std::string::npos) continue;
            const auto key = line.substr(0, separator), number = line.substr(separator + 1);
            char* end = nullptr;
            errno = 0;
            long parsed = std::strtol(number.c_str(), &end, 10);
            if (end == number.c_str() || !end || *end || errno == ERANGE || parsed < std::numeric_limits<int>::min() ||
                parsed > std::numeric_limits<int>::max())
                return std::nullopt;
            int value = int(parsed);
            if (key == "brightness_supported") {
                status.brightness = value == 1;
                brightness_seen = true;
            } else if (key == "power_supported") status.power = value == 1;
            else if (key == "sleep_supported") status.sleep = value == 1;
            else if (key == "lid_supported") status.lid = value == 1;
            else if (key == "brightness") status.brightness_percent = std::clamp(value, 5, 100);
            else if (key == "battery") {
                status.battery = value >= 0 && value <= 100 ? value : -1;
                battery_seen = true;
            } else if (key == "charging") status.charging = value == 1;
            else if (key == "external_power") status.external_power = value == 1;
            else if (key == "volume_supported") status.volume = value == 1;
            else if (key == "volume") status.volume_percent = std::clamp(value, 0, 100);
            else if (key == "poweroff_supported") status.poweroff = value == 1;
        }
        return brightness_seen && battery_seen ? std::optional<SystemStatus>(status) : std::nullopt;
    }
    void close_status_pipe() {
        if (status_pipe >= 0) {
            close(status_pipe);
            status_pipe = -1;
        }
    }
    void cancel_status() {
        completed_status.reset();
        if (status_child <= 0) return;
        // Status is read-only. Kill its group, including an outstanding mixer
        // query, rather than letting slow reads delay an interactive write.
        kill(-status_child, SIGKILL);
        kill(status_child, SIGKILL);
        status_cancelled = true;
        close_status_pipe();
    }
    void drain_status() {
        if (status_pipe < 0) return;
        char buffer[512];
        ssize_t count;
        while ((count = read(status_pipe, buffer, sizeof(buffer))) != 0) {
            if (count > 0) {
                if (status_output.size() + size_t(count) > 8192) {
                    status_output_failed = true;
                    cancel_status();
                    return;
                }
                status_output.append(buffer, size_t(count));
            } else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else {
                status_output_failed = true;
                close_status_pipe();
                return;
            }
        }
        close_status_pipe();
    }

public:
    SystemControls(bool fake, fs::path application, fs::path userdata)
        : preview(fake), app(std::move(application)), data(std::move(userdata)) {}
    SystemControls(const SystemControls&) = delete;
    SystemControls& operator=(const SystemControls&) = delete;
    void finish() {
        if (lid_device >= 0) {
            close(lid_device);
            lid_device = -1;
        }
        cancel_status();
        if (status_child > 0) {
            while (waitpid(status_child, nullptr, 0) < 0 && errno == EINTR) {}
            status_child = -1;
        }
        close_status_pipe();
        flush_controls();
        if (!preview && !std::getenv("RGDS_SYSTEM_MANAGED")) invoke("restore", std::nullopt);
    }
    ~SystemControls() { finish(); }
    SystemStatus status() {
        if (preview) {
            cached_status = {true, true, true, true, false, false, 60, 82, true, 25, true};
            return cached_status;
        }
        cancel_status();
        std::string output;
        if (invoke("status", std::nullopt, &output) == 0)
            if (auto value = parse_status(output)) cached_status = *value;
        return cached_status;
    }
    // Periodic status belongs off the input thread: mixer reads can take seconds.
    bool request_status() {
        if (status_child > 0 || completed_status || brightness_pending() || volume_pending()) return false;
        if (preview) {
            completed_status = SystemStatusUpdate{cached_status, true};
            return true;
        }
        int channel[2];
        if (pipe(channel) != 0) {
            completed_status = SystemStatusUpdate{cached_status, false};
            return false;
        }
        if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
            close(channel[0]);
            close(channel[1]);
            completed_status = SystemStatusUpdate{cached_status, false};
            return false;
        }
        status_output.clear();
        status_cancelled = status_output_failed = false;
        status_child = launch("status", std::nullopt, channel[1], channel[0], true);
        close(channel[1]);
        if (status_child < 0) {
            close(channel[0]);
            completed_status = SystemStatusUpdate{cached_status, false};
            return false;
        }
        status_pipe = channel[0];
        return true;
    }
    std::optional<SystemStatusUpdate> poll_status() {
        if (status_child > 0) {
            drain_status();
            int status = 0;
            pid_t result;
            do {
                result = waitpid(status_child, &status, WNOHANG);
            } while (result < 0 && errno == EINTR);
            if (result != 0) {
                status_child = -1;
                drain_status();
                close_status_pipe();
                auto value = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !status_cancelled &&
                                     !status_output_failed
                                 ? parse_status(status_output)
                                 : std::nullopt;
                if (value) cached_status = *value;
                completed_status = SystemStatusUpdate{cached_status, value.has_value()};
                status_output.clear();
            }
        }
        auto result = completed_status;
        completed_status.reset();
        return result;
    }
    bool status_pending() const { return status_child > 0; }
    void discard_status() { cancel_status(); }
    SystemStatus cached() const { return cached_status; }
    // Paint the requested value immediately; poll to reconcile hardware success.
    // Queue size stays at one even when a touchscreen produces many motions.
    void request_brightness(int value) {
        cancel_status();
        value = std::clamp(value, 5, 100);
        if (brightness_child > 0 && value == active_brightness) pending_brightness.reset();
        else pending_brightness = value;
    }
    std::optional<BrightnessUpdate> poll_brightness() {
        collect_controls(false);
        auto result = completed_brightness;
        completed_brightness.reset();
        begin_next_control();
        if (!result) {
            result = completed_brightness;
            completed_brightness.reset();
        }
        if (result) result->settled = !brightness_pending() && !completed_brightness;
        return result;
    }
    bool brightness_pending() const { return brightness_child > 0 || pending_brightness.has_value(); }
    void request_volume(int value) {
        cancel_status();
        value = std::clamp(value, 0, 100);
        if (volume_child > 0 && value == active_volume) pending_volume.reset();
        else pending_volume = value;
    }
    std::optional<VolumeUpdate> poll_volume() {
        collect_controls(false);
        auto result = completed_volume;
        completed_volume.reset();
        begin_next_control();
        if (!result) {
            result = completed_volume;
            completed_volume.reset();
        }
        if (result) result->settled = !volume_pending() && !completed_volume;
        return result;
    }
    bool volume_pending() const { return volume_child > 0 || pending_volume.has_value(); }
    // Lifecycle boundary only: finish the newest value before suspend, starting
    // another journalled control, handing off to a game, or restoring stock.
    bool flush_controls() {
        cancel_status();
        while (brightness_pending() || volume_pending()) {
            collect_controls(true);
            begin_next_control(true);
        }
        return last_brightness_ok && last_volume_ok;
    }
    bool flush_brightness() {
        flush_controls();
        return last_brightness_ok;
    }
    bool flush_volume() {
        flush_controls();
        return last_volume_ok;
    }
    bool set(const std::string& command, int value) {
        flush_controls();
        bool success = invoke(command, value) == 0;
        if (success) {
            if (command == "brightness") {
                cached_status.brightness_percent = std::clamp(value, 5, 100);
                last_brightness_ok = true;
            } else if (command == "volume") {
                cached_status.volume_percent = std::clamp(value, 0, 100);
                last_volume_ok = true;
            }
        }
        return success;
    }
    bool sync_volume() {
        flush_controls();
        return invoke("volume", std::nullopt) == 0;
    }
    std::optional<unsigned long long> lid_generation() const { return lid_sequence; }
    bool sleep(bool lid = false, bool menu_close = false) {
        flush_controls();
        // A closed lid: the panels go dark here, before the Python helper even starts (its start-up is most of the
        // second between the close and a dark screen). The helper gets the levels to put back (RGDS_SLEEP_BRIGHTNESS)
        // instead of reading the zeros it would otherwise restore.
        std::string levels;
        if (lid) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator("/sys/class/backlight", ec)) {
                const fs::path node = entry.path() / "brightness";
                std::ifstream in(node);
                std::string value;
                if (!(in >> value)) continue;
                std::ofstream out(node);
                if (!(out << "0")) continue;
                levels += (levels.empty() ? "" : ";") + node.string() + "=" + value;
            }
        }
        if (!levels.empty()) setenv("RGDS_SLEEP_BRIGHTNESS", levels.c_str(), 1);
        const bool ok = invoke(lid ? "sleep_lid" : "sleep", std::nullopt, nullptr, menu_close) == 0;
        if (!levels.empty()) {
            unsetenv("RGDS_SLEEP_BRIGHTNESS");
            // Whatever the helper managed: the levels come back (a cancelled sleep included).
            for (size_t at = 0; at < levels.size();) {
                const size_t end = levels.find(';', at), eq = levels.find('=', at);
                if (eq == std::string::npos || eq > end) break;
                std::ofstream out(levels.substr(at, eq - at));
                out << levels.substr(eq + 1, (end == std::string::npos ? levels.size() : end) - eq - 1);
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        return ok;
    }
    static std::optional<bool> read_lid_state(const fs::path& path,
                                              std::optional<unsigned long long>* generation = nullptr) {
        if (generation) generation->reset();
        std::ifstream file(path);
        double stamp;
        int value;
        double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        if (file >> stamp >> value && now >= stamp && now - stamp <= 3 && (value == 0 || value == 1)) {
            file >> std::ws;
            if (file.eof()) return value == 1;
            unsigned long long sequence;
            if (file.peek() < '0' || file.peek() > '9') return std::nullopt;
            if (file >> sequence) {
                file >> std::ws;
                if (file.eof()) {
                    if (generation) *generation = sequence;
                    return value == 1;
                }
            }
        }
        return std::nullopt;
    }
    std::optional<bool> lid_closed() {
        lid_sequence.reset();
#ifdef __linux__
        if (!preview) {
            const char* platform = std::getenv("RGDS_SESSION_PLATFORM");
            if (platform && std::string(platform) == "independent") {
                const char* path = std::getenv("RGDS_LID_DEV");
                if (!path) return std::nullopt;
                if (lid_device < 0) lid_device = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (lid_device < 0) return std::nullopt;
                unsigned char bits[8]{};
                if (ioctl(lid_device, EVIOCGSW(sizeof(bits)), bits) < 0) {
                    close(lid_device);
                    lid_device = -1;
                    return std::nullopt;
                }
                return (bits[SW_LID / 8] & (1u << (SW_LID % 8))) != 0;
            }
        }
#endif
        const char* state = std::getenv("RGDS_LID_STATE");
        return preview || !state ? std::nullopt : read_lid_state(state, &lid_sequence);
    }
};
}
