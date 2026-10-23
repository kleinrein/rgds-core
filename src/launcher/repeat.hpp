#pragma once
#include <algorithm>
#include <map>
namespace rgds {
class HoldRepeat {
    struct Press {
        int direction;
        unsigned began, next;
    };
    std::map<int, Press> keys;

public:
    void press(int source, int direction, unsigned now) {
        if (!keys.count(source) || keys[source].direction != direction) keys[source] = {direction, now, now + 320};
    }
    void release(int source) { keys.erase(source); }
    // How long the newest press has been down, for callers that cover more
    // ground the longer a key stays held.
    unsigned held_for(unsigned now) const { return keys.empty() ? 0 : now - keys.rbegin()->second.began; }
    void clear() { keys.clear(); }
    bool active() const { return !keys.empty(); }
    int poll(unsigned now) {
        if (keys.empty()) return 0;
        auto& held = keys.rbegin()->second;
        if (now < held.next) return 0;
        unsigned elapsed = now - held.began;
        held.next = now + (elapsed > 1400 ? 45 : elapsed > 700 ? 75 : 130);
        return held.direction;
    }
};
}
