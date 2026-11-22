// SPDX-License-Identifier: GPL-3.0-or-later
// Moments of a game's past as a tree (docs/TIME_TRAVEL_PLAN.md), as the Rewind screen reads them: the player's own
// timeline (app/timeline.hpp), or a list an engine handed over through the RG DS bridge (MomentList, below), which
// is how the launcher's game menu shows a DS game's moments with the same screen.
//
// The bridge's files: the list beside the runtime's control file, the pictures where the list says (RAM, /run/rgds,
// on RG DS OS: tens of MB that the card never needs to see; beside the list elsewhere):
//   <control>.rewind   text: now=<id>, rate=<frames a second>, files=<base>, count=<n>, then
//                      moment_<i>=<id> <parent> <position> for i in 0..n-1 (position in frames of play).
//   <base>.thumbs      "RGRW", u32 count, then per moment u32 id, w, h and w*h pixels.
//   <base>.shown       "RGRS", u32 id, screens (1 or 2), w, h, then screens*w*h pixels: the moment the engine holds
//                      now, at full size (a DS: both screens, the top first).
// Without files=, <base> is <control>.rewind.
// Integers little-endian; pixels are play::Picture words (bytes R, G, B, X).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace play {

struct Thumb {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // Picture's byte order
    bool empty() const { return px.empty(); }
};

struct Moment {
    uint32_t id = 0, parent = 0; // ids count up from 1; parent 0: none
    uint64_t position = 0;       // frames of play along its own line since the game started
    Thumb thumb;
};

class MomentTree {
public:
    virtual ~MomentTree() = default;
    virtual const Moment* find(uint32_t id) const = 0;
    // Every moment kept, oldest first (ids count up, and a child is always newer than its parent).
    virtual std::vector<uint32_t> ids() const = 0;
    virtual double frame_rate() const = 0;

    std::vector<uint32_t> children(uint32_t id) const {
        std::vector<uint32_t> out;
        for (uint32_t m : ids())
            if (const Moment* mo = find(m); mo && mo->parent == id && m != id) out.push_back(m);
        return out;
    }
    // From the oldest moment kept down to `tip`.
    std::vector<uint32_t> line(uint32_t tip) const {
        std::vector<uint32_t> out;
        const size_t most = ids().size();
        for (const Moment* m = find(tip); m && out.size() <= most; m = find(m->parent)) out.push_back(m->id);
        std::reverse(out.begin(), out.end());
        return out;
    }
    // The newest moment with no children under `id` (itself when it has none).
    uint32_t newest_leaf(uint32_t id) const {
        uint32_t best = id;
        std::vector<uint32_t> todo{id};
        for (size_t guard = 0, most = ids().size(); !todo.empty() && guard <= most; ++guard) {
            const uint32_t at = todo.back();
            todo.pop_back();
            const auto kids = children(at);
            if (kids.empty() && at > best) best = at;
            todo.insert(todo.end(), kids.begin(), kids.end());
        }
        return best;
    }
};

// The moments an engine listed (the files above).
class MomentList final : public MomentTree {
public:
    uint32_t now = 0;
    double rate = 60;
    std::string files; // <base> above, empty when the list names none
    std::map<uint32_t, Moment> moments;

    const Moment* find(uint32_t id) const override {
        auto it = moments.find(id);
        return it == moments.end() ? nullptr : &it->second;
    }
    std::vector<uint32_t> ids() const override {
        std::vector<uint32_t> out;
        for (const auto& [id, m] : moments) out.push_back(id);
        return out;
    }
    double frame_rate() const override { return rate; }

    // False when the list does not parse or names no moment.
    bool parse(const std::string& text, const std::vector<uint8_t>& thumbs) {
        moments.clear();
        now = 0;
        rate = 60;
        files.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "now") now = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
            else if (key == "rate") rate = std::max(1.0, std::atof(value.c_str()));
            else if (key == "files") files = value;
            else if (key.rfind("moment_", 0) == 0) {
                Moment m;
                unsigned long long position = 0;
                std::istringstream fields(value);
                if (!(fields >> m.id >> m.parent >> position) || !m.id) continue;
                m.position = position;
                moments[m.id] = std::move(m);
            }
        }
        // The pictures: whatever is cut short or names no listed moment is left out.
        if (thumbs.size() >= 8 && std::memcmp(thumbs.data(), "RGRW", 4) == 0) {
            size_t at = 8;
            const uint32_t count = read32(thumbs, 4);
            for (uint32_t i = 0; i < count && at + 12 <= thumbs.size(); ++i) {
                const uint32_t id = read32(thumbs, at), w = read32(thumbs, at + 4), h = read32(thumbs, at + 8);
                at += 12;
                const size_t bytes = size_t(w) * h * 4;
                if (w > 1024 || h > 1024 || at + bytes > thumbs.size()) break;
                if (auto it = moments.find(id); it != moments.end() && w && h) {
                    it->second.thumb.w = int(w);
                    it->second.thumb.h = int(h);
                    it->second.thumb.px.resize(size_t(w) * h);
                    std::memcpy(it->second.thumb.px.data(), thumbs.data() + at, bytes);
                }
                at += bytes;
            }
        }
        return !moments.empty() && find(now);
    }

    static uint32_t read32(const std::vector<uint8_t>& b, size_t at) {
        return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
    }
};

// <control>.rewind.shown: the moment's own picture(s), full size.
struct ShownMoment {
    uint32_t id = 0;
    int screens = 0, w = 0, h = 0;
    std::vector<uint32_t> px; // screens * w * h
    bool parse(const std::vector<uint8_t>& b) {
        if (b.size() < 20 || std::memcmp(b.data(), "RGRS", 4) != 0) return false;
        id = MomentList::read32(b, 4);
        screens = int(MomentList::read32(b, 8));
        w = int(MomentList::read32(b, 12));
        h = int(MomentList::read32(b, 16));
        if (screens < 1 || screens > 2 || w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;
        const size_t n = size_t(screens) * w * h;
        if (b.size() < 20 + n * 4) return false;
        px.resize(n);
        std::memcpy(px.data(), b.data() + 20, n * 4);
        return true;
    }
};

} // namespace play
