#pragma once
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace rgds {
enum class PowerPress { None, Sleep, Off };

// The PMIC's power key: a short press asks for sleep, a two-second hold for a clean power off.
// runtime/lid_sleep.py's PowerKey is the same contract for games and ports. Sleep waits for the release, so a hold
// never sleeps first, and holding the key to boot leaves a release with no press. The press that wakes the handheld
// arrives with the resume, so after_sleep discards it and ignores the key for a moment.
class PowerKey {
    static constexpr long long hold_limit = 2000, wake_quiet = 1000;
    int fd = -1;
    bool armed = false;
    PowerPress wanted = PowerPress::None;
    long long pressed_at = 0, ignore_until = 0;

    void drain() {
#ifdef __linux__
        if (fd < 0) return;
        input_event event{};
        while (::read(fd, &event, sizeof(event)) == sizeof(event)) {}
#endif
    }

public:
    ~PowerKey() { close_device(); }
    void close_device() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        armed = false;
        wanted = PowerPress::None;
    }
    void open_device() {
#ifdef __linux__
        close_device();
        std::error_code error;
        for (auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
            if (entry.path().filename().string().rfind("event", 0) != 0) continue;
            int node = ::open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (node < 0) continue;
            char name[64]{};
            if (ioctl(node, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && std::strcmp(name, "rk805 pwrkey") == 0) {
                fd = node;
                return;
            }
            ::close(node);
        }
#endif
    }
    bool holding() const { return armed; }
    void feed(int value, long long now) {
        if (value == 1) {
            armed = now >= ignore_until;
            pressed_at = now;
        } else if (value == 0) {
            if (armed) wanted = now - pressed_at < hold_limit ? PowerPress::Sleep : PowerPress::Off;
            armed = false;
        }
    }
    void tick(long long now) {
        if (armed && now - pressed_at >= hold_limit) {
            wanted = PowerPress::Off;
            armed = false;
        }
    }
    // A request made while not ready (a save finishing) is kept until it is.
    PowerPress take(bool sleep_ready, bool off_ready) {
        const PowerPress press = wanted;
        if (press == PowerPress::None || !(press == PowerPress::Sleep ? sleep_ready : off_ready))
            return PowerPress::None;
        wanted = PowerPress::None;
        return press;
    }
    PowerPress poll(bool sleep_ready, bool off_ready, long long now) {
#ifdef __linux__
        if (fd >= 0) {
            input_event event{};
            while (::read(fd, &event, sizeof(event)) == sizeof(event)) {
                if (event.type == EV_KEY && event.code == KEY_POWER) feed(event.value, now);
                else if (event.type == EV_SYN && event.code == SYN_DROPPED) armed = false;
            }
        }
#endif
        tick(now);
        return take(sleep_ready, off_ready);
    }
    void after_sleep(long long now) {
        drain();
        armed = false;
        wanted = PowerPress::None;
        ignore_until = now + wake_quiet;
    }
};
}
