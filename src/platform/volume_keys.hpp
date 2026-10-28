#pragma once
#include <vector>
#include <filesystem>
#include <chrono>
#include <map>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif
namespace rgds {
class VolumeKeys {
    std::vector<int> descriptors;
    struct Held {
        long long began, next;
    };
    // The handheld's volume buttons send no kernel autorepeat, so a held
    // button is repeated here: after a pause, then faster the longer it stays.
    std::map<int, Held> held;

public:
    ~VolumeKeys() { close_devices(); }
    void close_devices() {
        for (int fd : descriptors) ::close(fd);
        descriptors.clear();
    }
    bool available() const { return !descriptors.empty(); }
    void open_devices() {
#ifdef __linux__
        close_devices();
        std::error_code error;
        for (auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
            if (entry.path().filename().string().rfind("event", 0) != 0) continue;
            int fd = ::open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned char bits[96]{};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
                ((bits[KEY_VOLUMEUP / 8] & (1 << (KEY_VOLUMEUP % 8))) ||
                 (bits[KEY_VOLUMEDOWN / 8] & (1 << (KEY_VOLUMEDOWN % 8)))))
                descriptors.push_back(fd);
            else ::close(fd);
        }
#endif
    }
    bool holding() const { return !held.empty(); }
    int poll() {
        int delta = 0;
#ifdef __linux__
        auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        for (int fd : descriptors) {
            input_event event{};
            while (::read(fd, &event, sizeof(event)) == sizeof(event)) {
                if (event.type != EV_KEY || (event.code != KEY_VOLUMEUP && event.code != KEY_VOLUMEDOWN)) continue;
                const int step = event.code == KEY_VOLUMEUP ? 5 : -5;
                if (event.value == 0) held.erase(event.code);
                else if (event.value == 1) {
                    delta += step;
                    held[event.code] = {now, now + 350};
                }
            }
        }
        for (auto it = held.begin(); it != held.end();) {
            // A release lost to a sleep must not leave the volume running away.
            if (now - it->second.began > 15000) {
                it = held.erase(it);
                continue;
            }
            if (now >= it->second.next) {
                delta += it->first == KEY_VOLUMEUP ? 5 : -5;
                long long elapsed = now - it->second.began;
                it->second.next = now + (elapsed > 1500 ? 40 : elapsed > 800 ? 65 : 100);
            }
            ++it;
        }
#endif
        return delta;
    }
};
}
