// SPDX-License-Identifier: GPL-3.0-or-later
// Where the sound goes and the equaliser curve for it: the music player's own settings (music folder's eq.ini, one
// curve each for the speakers, a Bluetooth headset and headphones), so a film and a song sound the same on the same
// output. The route is read as the music engine reads it: the Bluetooth sink file, then the headphone jack.
//
// A change made here goes to the running music engine as its own `eq` command (it keeps the file), or into the file
// when no engine runs, so neither overwrites the other.
#pragma once
#include "music/eq.hpp"
#include "music/shared.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace media {

struct SoundOut {
    struct Setting {
        bool on = false;
        int preset = 0;
        std::array<float, rgds::music::eq_bands> gain{};
    };
    std::string eq_file, music_run, bt_sink = "/run/rgds/bt-sink";
    int route = 0; // rgds::music::Route: speaker, bluetooth, headphones
    Setting eq[3];
    uint32_t checked = 0;
    int64_t file_time = -1;

    static constexpr float speaker_boost_db = 6; // as the music engine gives the speakers
    float boost_db() const { return route == 0 ? speaker_boost_db : 0; }
    const Setting& now() const { return eq[route]; }

    static const char* route_name(int r) { return r == 1 ? "Bluetooth" : r == 2 ? "Headphones" : "Speakers"; }

    // True when the route or the curve changed since the last call. Cheap: looks every 2 s.
    bool poll(uint32_t at, bool force = false) {
        if (!force && checked && at - checked < 2000) return false;
        checked = at ? at : 1;
        bool changed = false;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(eq_file, ec);
        int64_t stamp = ec ? 0 : int64_t(t.time_since_epoch().count());
        if (stamp != file_time) {
            file_time = stamp;
            load();
            changed = true;
        }
        std::string address;
        {
            std::ifstream in(bt_sink);
            std::getline(in, address);
        }
        int r = !address.empty() ? 1 : headphones() ? 2 : 0;
        if (r != route) route = r, changed = true;
        return changed;
    }

    void load() {
        for (auto& e : eq) e = Setting{};
        std::ifstream in(eq_file);
        std::string line;
        static const char* names[3] = {"speaker", "bluetooth", "headphones"};
        while (std::getline(in, line)) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            for (int r = 0; r < 3; ++r) {
                if (line.compare(0, equals, names[r]) != 0) continue;
                std::istringstream cells(line.substr(equals + 1));
                int on = 0;
                Setting e;
                cells >> on >> e.preset;
                for (float& g : e.gain) cells >> g;
                if (!cells.fail()) {
                    e.on = on != 0;
                    eq[r] = e;
                }
            }
        }
    }

    // A preset for the output in use now (-1: off).
    void choose(int preset) {
        Setting e = eq[route];
        if (preset < 0) e.on = false;
        else {
            e.on = true;
            e.preset = preset;
            e.gain = rgds::music::eq_presets[size_t(preset)].gain;
        }
        eq[route] = e;
        std::ostringstream cmd;
        cmd << "eq " << int(e.on) << ' ' << e.preset;
        for (float g : e.gain) cmd << ' ' << g;
        if (send(cmd.str())) return; // the engine keeps the file
        save();
    }

    void save() {
        static const char* names[3] = {"speaker", "bluetooth", "headphones"};
        const std::string tmp = eq_file + ".part";
        {
            std::ofstream out(tmp, std::ios::trunc);
            for (int r = 0; r < 3; ++r) {
                out << names[r] << '=' << int(eq[r].on) << ' ' << eq[r].preset;
                for (float g : eq[r].gain) out << ' ' << g;
                out << '\n';
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, eq_file, ec);
    }

    // One datagram to the music engine's control socket; false when none listens. The engine applies a curve to
    // the route it sees, which is the one seen here.
    bool send(const std::string& text) const {
        if (music_run.empty()) return false;
        const std::string path = music_run + "/" + rgds::music::control_socket;
        sockaddr_un name{};
        name.sun_family = AF_UNIX;
        if (path.size() >= sizeof name.sun_path) return false;
        std::memcpy(name.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        bool ok = sendto(fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&name), sizeof name) ==
                  ssize_t(text.size());
        ::close(fd);
        return ok;
    }

    // The headphone jack, from any input device that reports SW_HEADPHONE_INSERT.
    static bool headphones() {
#ifdef __linux__
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        bool in = false;
        while (dirent* e = readdir(dir)) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            int fd = ::open((std::string("/dev/input/") + e->d_name).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned long caps[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            unsigned long state[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            const size_t bits = 8 * sizeof(long);
            if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof caps), caps) >= 0 &&
                (caps[SW_HEADPHONE_INSERT / bits] >> (SW_HEADPHONE_INSERT % bits) & 1) &&
                ioctl(fd, EVIOCGSW(sizeof state), state) >= 0)
                in = in || (state[SW_HEADPHONE_INSERT / bits] >> (SW_HEADPHONE_INSERT % bits) & 1);
            ::close(fd);
        }
        closedir(dir);
        return in;
#else
        return false;
#endif
    }
};

} // namespace media
