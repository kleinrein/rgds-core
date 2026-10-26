// SPDX-License-Identifier: GPL-3.0-or-later
// Shadow-copy map capture. A second, silent copy of the game is loaded with
// a snapshot of the live game, plays the buttons that open the game's own map
// screen, waits until the map is really up and still, and hands back the
// picture (a few frames of it, so the player marker keeps blinking).
//
// The game being played is never paused, slowed or written to: the snapshot
// is a copy, and the shadow has its own memory and no battery file.
#pragma once
#include "companion/profile.hpp"
#include "core/core.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace play {

struct MapImage {
    bool ok = false;
    std::string why; // failure reason
    int w = 0, h = 0;
    std::vector<std::vector<uint32_t>> frames; // w*h each; one when nothing animates
    int frame_step = 1;                        // emulated frames between them
    uint32_t background = 0;                   // pixel value of the map's empty colour
    uint32_t serial = 0;                       // grows with each success
    int frames_to_open = 0;                    // shadow frames it took (cost)
};

// The capture itself, synchronous: used by the worker and by tests.
MapImage capture_map(Core& shadow, const MapSpec& spec, const std::vector<uint8_t>& state);

// Mean brightness (0-255) of a sparse grid of the picture: fades and blank
// transition frames read near 0.
int picture_brightness(const Picture& p);

class MapCapture {
public:
    MapCapture(const Core& live, const Profile& profile);
    ~MapCapture();
    MapCapture(const MapCapture&) = delete;
    MapCapture& operator=(const MapCapture&) = delete;

    bool ready() const { return shadow_ != nullptr; }
    // Snapshots the live game and starts a capture if the worker is idle.
    bool request(Core& live);
    bool busy() const;
    // The newest successful capture newer than `serial`.
    bool poll(uint32_t serial, MapImage& out) const;
    int failures(std::string* why = nullptr) const;
    // Synchronous, on the caller's thread (tools and tests).
    MapImage capture_now(Core& live);

private:
    void worker();

    const Profile& profile_;
    std::unique_ptr<Core> shadow_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::thread thread_;
    std::vector<uint8_t> pending_;
    // The live battery memory for the shadow, taken again whenever the game saves.
    std::vector<uint8_t> pending_battery_;
    const MapSpec* pending_spec_ = nullptr; // which of the profile's maps the job captures
    uint64_t battery_revision_ = 0;
    bool has_job_ = false, working_ = false, quit_ = false;
    MapImage latest_;
    uint32_t serial_ = 0;
    int failures_ = 0;
    std::string last_failure_;
};

} // namespace play
