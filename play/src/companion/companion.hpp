// SPDX-License-Identifier: GPL-3.0-or-later
// The second screen. It decides when to look at the game's map (on a timer,
// as the player enters a new room, after a fade), and draws what it has:
// the live map edge to edge on the map's own colour, cross-faded as it
// changes, or a title card until there is something to show.
#pragma once
#include "companion/dock.hpp"
#include "companion/map_capture.hpp"
#include "companion/party.hpp"
#include "companion/profile.hpp"
#include "companion/view.hpp"
#include "ui/canvas.hpp"
#include "ui/colour.hpp"
#include "ui/look.hpp"
#include "ui/words.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace play {

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
    void render(Canvas& out, const Look& look, const Font& font, const Words& words) const;
    // A finger on the second screen: the dock first, then the view shown.
    // True when the second screen must be drawn again.
    bool touch(const Touch& t);
    // The second screen's own controls (dock.hpp).
    Dock& dock() { return dock_; }
    // The colours the top screen shows the game in (the menu's Screen colours),
    // so the game's own pictures here (a status-bar band, a map) match it.
    void set_colour(const Colour& colour) { colour_ = colour; }
    // Wall-clock milliseconds, for the dock's holds and fades.
    void set_time(uint64_t ms) { dock_.set_time(ms); }
    // The game picture as the top panel shows it: with the HUD split, the
    // profile's boxes (item box, place) are put back where the player looks;
    // a Game Boy HUD band is cut off unless `whole` (a screen filter is on,
    // which shades only the full-size picture).
    Picture compose_game(const Picture& game, bool whole = false);
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
    // Whether the second screen is shown at all. Off, the game keeps its whole
    // picture: no HUD band is cut off and no HUD is split away from it.
    void set_shown(bool shown) { shown_ = shown; }
    bool shown() const { return shown_; }
    // The profile's first view ([view]), if it has one this build can draw
    // (its explored map is what the session keeps beside the save).
    View* view() const { return views_.empty() ? nullptr : views_.front().get(); }
    // The view on the screen now: the first whose `on` holds (not an alternative).
    View* shown_view() const;
    // [view.x] alternative = yes: a board shown instead of the map or the team
    // when the switch (top right) is tapped, or when there is nothing else.
    View* alternative_view() const;
    bool has_switch() const;
    bool showing_alternative() const;

private:
    void take(MapImage img);
    void update_hud(Core& live);
    bool render_hud(Canvas& out, const Look& look) const;
    // Game Boy HUD band ([hud] strip): cut from the picture, drawn under the rest.
    bool strip_mode() const { return profile_ && profile_->has_hud && !profile_->hud.strip.empty(); }
    void update_strip(Core& live);
    void render_with_strip(Canvas& out, const Look& look, const Font& font) const;
    // What the second screen shows besides a HUD: the map, the team, a view, or the title card.
    void render_content(Canvas& out, const Look& look, const Font& font) const;
    uint64_t content_key() const;
    // What the companion itself shows, without the dock.
    void render_screen(Canvas& out, const Look& look, const Font& font) const;
    uint64_t screen_key() const;

    const Profile* profile_;
    bool shown_ = true;
    std::string title_, system_name_;
    std::unique_ptr<MapCapture> capture_;
    std::unique_ptr<Party> party_;
    bool party_shown_ = false;
    std::vector<std::unique_ptr<View>> views_;
    std::vector<bool> alternative_;
    mutable Colour colour_;
    mutable std::vector<uint32_t> tint_a_, tint_b_;
    const uint32_t* shown(const uint32_t* px, int w, int h, std::vector<uint32_t>& buffer) const;
    uint32_t shown_px(uint32_t v) const;
    bool alt_ = false;
    Box switch_box(int w) const;
    void render_layers(Canvas& out, const Look& look, const Font& font) const;
    uint64_t main_key() const;
    Dock dock_;
    // The companion under the dock while only the dock moves (its circle opening, its button fading).
    mutable Canvas screen_cache_;
    mutable uint64_t screen_cache_key_ = 0;
    uint64_t touch_serial_ = 0; // bumped by a touch that changed a view
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
    unsigned hud_layers_ = 0;
    std::vector<uint32_t> hud_image_, composed_;
    std::vector<Rect> drawn_; // per panel: where the HUD has drawn since it came on
    uint64_t strip_hash_ = 0;
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
