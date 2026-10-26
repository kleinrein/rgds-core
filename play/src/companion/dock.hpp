// SPDX-License-Identifier: GPL-3.0-or-later
// The second screen's own controls, low-key and always at hand by touch:
// the game's name and how long this session has run, the clock, the game
// switcher (a tap), Return home (held for a moment, so it never happens by a
// careless tap; the game saves first as from the menu), the screen's
// brightness, and turning the screen off (a touch brings it back). The same
// screen as a port's second screen (src/launcher/port_menu.hpp).
//
// Two ways to show it:
//   - over the companion: a small round button in the corner, faint once left
//     alone; a tap opens the controls out of it as a growing circle, a tap on
//     the button (or nothing for a while) closes them;
//   - the "Info" second screen: the controls are the whole screen, and it
//     fades down when left alone.
// Animations follow the launcher's setting: off, everything is instant.
#pragma once
#include "companion/view.hpp"
#include "ui/canvas.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/words.hpp"

#include <cstdint>
#include <string>

namespace play {

class Dock {
public:
    enum class Action { None, Switcher, Home };

    Dock(std::string title, std::string system);
    // The whole screen (the Info second screen) or a button over the companion.
    void set_full(bool full);
    bool full() const { return full_; }
    // Off the touch screen (the companion swapped to the top panel) there is no button to press.
    void set_reachable(bool reachable) { reachable_ = reachable; }
    // Wall-clock milliseconds (the frontend's ticks; tools pass frame time).
    void set_time(uint64_t ms) { now_ = ms; }
    void set_played(uint64_t seconds) { played_ = seconds; }
    void set_twelve_hour(bool twelve) { twelve_hour_ = twelve; }
    // True when the touch was the dock's (the companion below must not see it).
    bool touch(const Touch& t);
    // Draws the dock over what `out` holds (or, full, the whole screen), then
    // the brightness. An off screen is black.
    void draw(Canvas& out, const Look& look, const Font& font, const Words& words) const;
    // Changes whenever draw() would draw something different (every frame while moving).
    uint64_t key() const;
    // Over the companion with the controls closed and nothing moving: the
    // companion is all there is to draw, apart from the button.
    bool open() const { return full_ || open_; }
    // How far the controls cover the companion, 0..1 (1 when full or wide open).
    double openness() const;
    // Something is changing by itself (an opening, a hold, a fade, a note).
    bool moving() const;

    // What the player asked for; the frontend does it and reports back.
    Action take_action();
    void report(const std::string& message);

    // 10..100 percent, kept by the frontend between sessions.
    int brightness() const { return brightness_; }
    void set_brightness(int percent);
    bool brightness_changed() {
        bool c = brightness_dirty_;
        brightness_dirty_ = false;
        return c;
    }
    bool screen_off() const { return off_; }

    // Layout (in 640x480 units), public for tests.
    enum class Item { None, Button, Switcher, Home, Bright, Off, Count };
    Box box(Item item, int w, int h) const;

private:
    // The resting brightness: the chosen one, or the faded one when left alone (full only).
    int shown_brightness() const;
    void draw_controls(Canvas& out, const Look& look, const Font& font, const Words& words) const;
    // What draw_controls() would draw differently (the opening circle reuses one drawing).
    uint64_t controls_key() const;
    // The controls inside a circle round (cx, cy), over what `out` holds.
    void reveal(Canvas& out, const Canvas& layer, double cx, double cy, double radius) const;
    void draw_button(Canvas& out, const Look& look) const;
    void hold_button(Canvas& out, const Look& look, const Font& font, const Words& words, Item item, Icon glyph,
                     const std::string& name, const std::string& hint) const;
    // Return home is held; the switcher only opens, so a tap does.
    static bool held_item(Item item) { return item == Item::Home; }
    static constexpr uint64_t hold_ms = 1000;
    void wake();

    std::string title_, system_;
    bool full_ = false, open_ = false, off_ = false, twelve_hour_ = false, reachable_ = true;
    uint64_t now_ = 0, played_ = 0, opened_at_ = 0, closed_at_ = 0, touched_at_ = 0;
    int brightness_ = 100;
    bool brightness_dirty_ = false, dragging_ = false;
    Item held_ = Item::None;
    uint64_t held_at_ = 0;
    bool fired_ = false;
    Action action_ = Action::None;
    std::string toast_;
    uint64_t toast_at_ = 0;
    // The battery's charge, read now and then (the handheld's power supply).
    mutable int battery_ = -2;
    mutable uint64_t battery_at_ = 0;
    // The controls drawn once while they open or close over the companion.
    mutable Canvas layer_;
    mutable uint64_t layer_key_ = 0;
};

} // namespace play
