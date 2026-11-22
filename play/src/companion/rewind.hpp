// SPDX-License-Identifier: GPL-3.0-or-later
// The Rewind screen (docs/TIME_TRAVEL_PLAN.md), in the manner of Cover Flow and Time Machine: the timeline's moments
// as pictures on a dark stage under the stars, the chosen one facing you, the others turned away in perspective on
// either side, each with its reflection on the floor. Under them an Aqua scrubber, and two gel buttons.
//
// Everything moves on springs: a flick coasts and settles on a picture, the ends give like a rubber band, the strip
// flies in as the screen opens. Drag the strip (or the scrubber) and let go: it settles, then plays from there. A
// tap on a picture glides to it. The top screen shows the picture in the middle (the session loads that moment and
// runs one silent frame). Where a moment has more than one future, the branch pill turns the strip onto the other.
// Back to now returns to the exact moment the screen was opened at.
//
// Pad: Left/Right glide (held: they repeat), L/R ten at a time, Up/Down the branch, A plays from here, B goes back.
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

    // Opened over `timeline` with `now` the moment just taken.
    void open(const MomentTree& moments, uint32_t now);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Wall-clock milliseconds: the springs step to it. Animations off, everything is where it is going at once.
    void set_time(uint64_t ms, bool animations = true);
    // True when what draw() shows changed.
    bool touch(const Touch& t);
    bool pad(uint32_t buttons);
    // What the player asked for, once: Jump to `target`, or Back to now().
    Result take(uint32_t& target);
    // The moment in the middle now (shown on the top screen).
    uint32_t chosen() const;
    uint32_t now() const { return now_; }

    void draw(Canvas& out, const Look& look, const Font& font, const Words& words) const;
    uint64_t key() const;
    // A spring is still moving (the frontend keeps drawing).
    bool moving() const;
    // Nothing moving and no finger down: the moment in the middle is the one to show on the top screen (a finger
    // scrubbing or the strip gliding passes moments too quickly to load each one).
    bool settled() const { return open_ && at_rest(); }

    // Layout (640x480), public for tests.
    static constexpr int flow_y = 206;                 // the middle picture's centre
    static constexpr int cover_w = 208, cover_h = 156; // its largest size
    static constexpr int side_x = 150, stack_x = 44;   // the first picture to a side, and each further one
    static constexpr double drag_per_moment = 120;     // a finger this far moves the strip one moment
    enum class Part { Strip, Scrubber, Branch, Back, Play };
    static Box box(Part part);
    // Where moment i of the line sits now (its centre x).
    double centre_x(size_t i) const;
    const std::vector<uint32_t>& line() const { return line_; }
    size_t selected() const { return selected_; }
    // The chosen moment's futures: which the strip follows (0-based) and how many; 0 when there is only one.
    int branch_count() const;
    int branch_index() const;
    // "-0:42" behind now, "+0:10" on a future left behind, "Now".
    std::string label(uint32_t id, const Words& words) const;

private:
    void aim(double index);
    void step(int by);
    void next_branch(int direction);
    void follow(); // selected_ from where the strip is
    // The moment the strip is heading for (where it rests once the glide ends): what A and Play from here play.
    uint32_t aimed() const;
    // The scrubber: where along its track the thumb's centre is for a position, and back.
    static double scrub_fraction(int x);
    bool at_rest() const;

    const MomentTree* timeline_ = nullptr;
    bool open_ = false, animations_ = true;
    uint32_t now_ = 0, tip_ = 0;
    std::vector<uint32_t> line_;
    size_t selected_ = 0;
    // The strip: where it is (in moments), how fast it moves (moments a ms), where its spring pulls it.
    double pos_ = 0, vel_ = 0, target_ = 0;
    uint64_t ms_ = 0;
    bool commit_ = false; // let go after a drag: play from where it comes to rest
    // A finger on the strip, the scrubber or a button.
    enum class Hold { None, Strip, Scrubber, Button } hold_ = Hold::None;
    bool dragged_ = false;
    int down_x_ = 0;
    double down_pos_ = 0;
    rgds::FingerSpeed speed_;
    int pressing_ = -1; // the button (or the pill) a finger is on
    // The pad: last frame's buttons, and when a held direction repeats next.
    uint32_t held_ = 0;
    uint64_t repeat_at_ = 0;
    Result result_ = Result::None;
    uint32_t target_id_ = 0;
    uint64_t serial_ = 0; // bumped by every change
    // The stage (gradient, stars, glow) is drawn once.
    mutable Canvas stage_;
};

} // namespace play
