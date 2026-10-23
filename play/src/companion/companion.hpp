// SPDX-License-Identifier: GPL-3.0-or-later
// The second screen. It decides when to look at the game's map (on a timer,
// as the player enters a new room, after a fade), and draws what it has:
// the live map edge to edge on the map's own colour, cross-faded as it
// changes, or a title card until there is something to show.
#pragma once
#include "companion/map_capture.hpp"
#include "companion/profile.hpp"
#include "ui/canvas.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace play {

struct Look {
    Rgb background{12, 17, 26}, panel{23, 32, 46}, ink{233, 244, 255}, muted{145, 167, 190}, accent{78, 187, 255};
};

struct CompanionStatus {
    bool has_profile = false, has_map = false, showing_map = false, showing_hud = false, hud_split = false;
    uint32_t serial = 0;
    int requests = 0, failures = 0;
    std::string last_failure;
};

class Companion {
public:
    Companion(Core& live, const Profile* profile, std::string title, std::string system_name);
    ~Companion();
    Companion(const Companion&) = delete;
    Companion& operator=(const Companion&) = delete;
    // After every live frame.
    void after_frame(Core& live);
    void render(Canvas& out, const Look& look, const Font& font) const;
    // The game picture as the top panel shows it: with the HUD split, the
    // profile's boxes (item box, place) are put back where the player looks.
    Picture compose_game(const Picture& game);
    CompanionStatus status() const;
    // The HUD's `on` probes as they read now, for the log when it changes.
    std::string hud_probes(Core& live) const;
    // Changes whenever render() would draw something different, so a frontend
    // redraws and uploads the second screen only then.
    uint64_t visual_key() const;
    // A capture is being made (tools wait on this to stay deterministic).
    bool busy() const { return (capture_ && capture_->busy()) || backdrop_working_; }
    // Tools: capture right now on this thread.
    bool capture_now(Core& live);

private:
    void take(MapImage img);
    void update_hud(Core& live);
    bool render_hud(Canvas& out, const Look& look) const;

    const Profile* profile_;
    std::string title_, system_name_;
    std::unique_ptr<MapCapture> capture_;
    MapImage current_, previous_;
    bool has_current_ = false, has_previous_ = false;
    uint64_t frame_ = 0, due_ = 30, next_periodic_ = 0, shown_at_ = 0;
    uint64_t room_key_ = 0;
    bool in_fade_ = false, menu_ = false, ever_shown_ = false;
    int requests_ = 0;
    // HUD split
    const Core* live_ = nullptr;
    bool hud_on_ = false, hud_shown_ = false, moves_ = false;
    int hud_frames_ = 0, hud_w_ = 0, hud_h_ = 0;
    uint32_t hud_key_ = 0;
    std::vector<uint32_t> hud_image_, composed_;
    std::vector<Rect> drawn_; // per panel: where the HUD has drawn since it came on
    // The scenery behind the HUD, drawn by a shadow copy without the HUD and sprites.
    void make_backdrop(Core& live);
    // Built on its own thread (a state load and a frame, ~10 ms on the A55),
    // so the game's frame never waits for it.
    std::unique_ptr<Core> backdrop_shadow_;
    std::thread backdrop_thread_;
    mutable std::mutex backdrop_lock_;
    std::vector<uint32_t> backdrop_;
    int backdrop_w_ = 0, backdrop_h_ = 0;
    uint32_t backdrop_serial_ = 0;
    std::atomic<bool> backdrop_working_{false};
    uint64_t backdrop_due_ = 0;
    // The backdrop scaled and dimmed to the panel, made again only when it changes.
    mutable Canvas backdrop_panel_;
    mutable uint32_t backdrop_panel_serial_ = 0;
    static constexpr int crossfade_frames = 12;
};

} // namespace play
