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
    std::map<int, long long> previous;

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
    int poll() {
        int delta = 0;
#ifdef __linux__
        for (int fd : descriptors) {
            input_event event{};
            while (::read(fd, &event, sizeof(event)) == sizeof(event))
                if (event.type == EV_KEY && (event.value == 1 || event.value == 2)) {
                    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
                    if (previous.count(event.code) && now - previous[event.code] < 40) continue;
                    previous[event.code] = now;
                    if (event.code == KEY_VOLUMEUP) delta += 5;
                    else if (event.code == KEY_VOLUMEDOWN) delta -= 5;
                }
        }
#endif
        return delta;
    }
};
}
