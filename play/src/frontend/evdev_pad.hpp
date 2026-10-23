// SPDX-License-Identifier: GPL-3.0-or-later
// The RG DS's own buttons, read straight from evdev and grabbed: while the
// player runs, nothing else (the paused launcher, Weston) receives or queues
// them, so a press made in a race can never act on Home afterwards. The
// kernel drops the grab when the descriptor closes, crash included.
#pragma once
#include "core/core.hpp"

#include <cstdint>
#include <string>

#ifdef __linux__
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace play {

class EvdevPad {
public:
    ~EvdevPad() { close(); }
    // The first device named like the RG DS pad. False on other machines.
    bool open(const std::string& name = "ANBERNIC-rk3568-keys") {
#ifdef __linux__
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        while (dirent* e = readdir(dir)) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + e->d_name;
            int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            char got[128] = {};
            ioctl(fd, EVIOCGNAME(sizeof(got) - 1), got);
            if (name == got && ioctl(fd, EVIOCGRAB, 1) == 0) {
                fd_ = fd;
                path_ = path;
                break;
            }
            ::close(fd);
        }
        closedir(dir);
#else
        (void)name;
#endif
        return fd_ >= 0;
    }
    void close() {
#ifdef __linux__
        if (fd_ >= 0) {
            ioctl(fd_, EVIOCGRAB, 0);
            ::close(fd_);
        }
#endif
        fd_ = -1;
    }
    bool active() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    // Reads what is pending; returns the buttons held now. `home` is set when
    // the Home key went down since the last call.
    uint32_t poll(bool& home) {
        home = false;
#ifdef __linux__
        input_event ev[64];
        for (;;) {
            ssize_t n = read(fd_, ev, sizeof(ev));
            if (n <= 0) break;
            for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i) {
                const auto& e = ev[i];
                if (e.type == EV_KEY) {
                    uint32_t bit = key_bit(e.code);
                    if (e.code == BTN_TL2 && e.value == 1) home = true;
                    if (bit) held_ = e.value ? (held_ | bit) : (held_ & ~bit);
                } else if (e.type == EV_ABS && e.code == ABS_HAT0X) {
                    held_ = (held_ & ~(Left | Right)) | (e.value < 0 ? Left : e.value > 0 ? Right : 0);
                } else if (e.type == EV_ABS && e.code == ABS_HAT0Y) {
                    held_ = (held_ & ~(Up | Down)) | (e.value < 0 ? Up : e.value > 0 ? Down : 0);
                }
            }
        }
#endif
        return held_;
    }

private:
    // The RG DS reports its buttons in this order (SDL buttons 0-12 in the
    // runtime's controller mapping): the button labelled A is BTN_SOUTH.
    static uint32_t key_bit(unsigned code) {
#ifdef __linux__
        switch (code) {
        case BTN_SOUTH: return A;
        case BTN_EAST: return B;
        case BTN_C: return Y;
        case BTN_NORTH: return X;
        case BTN_WEST: return L;
        case BTN_Z: return R;
        case BTN_TL: return Select;
        case BTN_TR: return Start;
        case BTN_SELECT: return L2;
        case BTN_START: return R2;
        default: return 0;
        }
#else
        (void)code;
        return 0;
#endif
    }
    int fd_ = -1;
    std::string path_;
    uint32_t held_ = 0;
};

} // namespace play
