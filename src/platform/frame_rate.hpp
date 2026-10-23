#pragma once
#include <cstdint>
namespace rgds {
// Frames per second from a game core's own emulated-frame counter.
class FrameCounterRate {
    uint64_t since = 0;
    uint32_t previous = 0;

public:
    double fps = 0;
    void reset() {
        since = 0;
        previous = 0;
        fps = 0;
    }
    bool sample(uint64_t us, uint32_t frames, bool blocked) {
        if (blocked) {
            since = 0;
            return false;
        }
        if (!since) {
            since = us;
            previous = frames;
            return false;
        }
        if (us - since < 1000000) return false;
        const uint32_t delta = frames - previous;
        fps = delta * 1000000.0 / (us - since);
        since = us;
        previous = frames;
        return true;
    }
};
}
