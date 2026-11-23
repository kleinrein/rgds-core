// SPDX-License-Identifier: GPL-3.0-or-later
// The Rewind screen (docs/TIME_TRAVEL_PLAN.md): the timeline's moments as a row of cards, the newest on the right,
// laid out and drawn as the launcher's own pages are (a header with the back arrow, settings cards, the footer's
// Back and A buttons), so it is at home beside the game menu, the Save gallery and the Album.
//
// The row moves on a spring: a flick coasts and settles on a card, the ends give a little. Dragging, flicking and
// tapping a card only look; A or Play from here plays. Where a moment has more than one future, the
// branch row turns the strip onto the other one. Back returns to the exact moment the screen was opened at.
//
// RG DS Play draws it on the second screen with its kit (draw()); the launcher's game menu draws the same layout with
// its own cards and textures (src/launcher/rewind_menu.hpp) and uses this class for the motion, touch and pad.
//
// Pad: Left/Right (held: they repeat), L/R ten at a time, Up/Down the branch, A plays from here, B goes back.
#pragma once
#include "app/moments.hpp"
#include "companion/touch.hpp"
#include "launcher/kinetics.hpp"
#include "ui/canvas.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/words.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace play {

class RewindScreen {
public:
    enum class Result { None, Jump, Back };

    // Opened over `moments` with `now` the moment just taken.
    void open(const MomentTree& moments, uint32_t now);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Wall-clock milliseconds: the spring steps to it. Animations off, the row is where it is going at once.
    void set_time(uint64_t ms, bool animations = true);
    // True when what draw() shows changed.
    bool touch(const Touch& t);
    bool pad(uint32_t buttons);
    // What the player asked for, once: Jump to `target`, or Back to now().
    Result take(uint32_t& target);
    // The moment in the middle now.
    uint32_t chosen() const;
    uint32_t now() const { return now_; }

    void draw(Canvas& out, const Look& look, const Font& font, const Words& words) const;
    uint64_t key() const;
    // The spring is still moving (the frontend keeps drawing).
    bool moving() const;
    // Nothing moving and no finger down: the moment in the middle is the one to show at full size.
    bool settled() const { return open_ && at_rest(); }

    // Layout, in the launcher's 640x480 lower screen (shared with rewind_menu.hpp).
    static constexpr int row_y = 206;                // the cards' centre line
    static constexpr int big_w = 224, big_h = 168;   // the card in the middle (its picture's box)
    static constexpr int small_w = 144, small_h = 108;
    static constexpr int near_step = 200, far_step = 160; // centre to centre: next to the middle, then further out
    static constexpr double drag_per_moment = 120;   // a finger this far moves the row one moment
    enum class Part { Strip, Branch, Back, Play };
    static Box box(Part part);
    // Where moment i of the line is now: its picture's box (the card is drawn kit::gap/2 round it).
    Box picture_box(size_t i) const;
    double centre_x(size_t i) const;
    const std::vector<uint32_t>& line() const { return line_; }
    size_t selected() const { return selected_; }
    // The chosen moment's futures: which the row follows (0-based) and how many; 0 when there is only one.
    int branch_count() const;
    int branch_index() const;
    // "-0:42" behind now, "+0:10" on a future left behind, "Now".
    std::string label(uint32_t id, const Words& words) const;
    // The picture's box inside `box`, in the thumb's own shape (a DS moment is both screens, upright).
    static Box fit(const Thumb& thumb, const Box& box);

private:
    void aim(double index);
    void step(int by);
    void next_branch(int direction);
    void follow(); // selected_ from where the row is
    bool at_rest() const;
    // The moment the row is heading for (where it rests once the glide ends): what A and Play from here play.
    uint32_t aimed() const;

    const MomentTree* timeline_ = nullptr;
    bool open_ = false, animations_ = true;
    uint32_t now_ = 0, tip_ = 0;
    std::vector<uint32_t> line_;
    size_t selected_ = 0;
    // The row: where it is (in moments), how fast it moves (moments a ms), where its spring pulls it.
    double pos_ = 0, vel_ = 0, target_ = 0;
    uint64_t ms_ = 0;
    enum class Hold { None, Strip, Button } hold_ = Hold::None;
    bool dragged_ = false;
    int down_x_ = 0;
    double down_pos_ = 0;
    rgds::FingerSpeed speed_;
    int pressing_ = -1; // the button (or the branch row) a finger is on
    uint32_t held_ = 0; // the pad: last frame's buttons
    uint64_t repeat_at_ = 0;
    Result result_ = Result::None;
    uint32_t target_id_ = 0;
    uint64_t serial_ = 0; // bumped by every change
};

} // namespace play
