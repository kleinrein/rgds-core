// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/rewind.hpp"

#include "core/core.hpp"
#include "ui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace play {

namespace {

// The spring, in moments and milliseconds (the launcher's glide: kinetics.hpp's glide_k).
constexpr double spring_k = rgds::ScrollMotion::glide_k, spring_zeta = .8;
constexpr double coast_friction = .004; // a flick of v moments/ms coasts v / friction moments
constexpr uint64_t repeat_first_ms = 300, repeat_ms = 90;

// How big a card is `d` moments from the middle (1 in the middle, 0 from one step out), and where its centre is.
double nearness(double d) { return std::clamp(1 - std::abs(d), 0.0, 1.0); }
double place_x(double d) {
    const double a = std::clamp(d, -1.0, 1.0);
    double x = 320 + a * RewindScreen::near_step;
    if (std::abs(d) > 1) x += (d > 0 ? 1 : -1) * (std::abs(d) - 1) * RewindScreen::far_step;
    return x;
}

std::string clock_text(uint64_t frames, double rate) {
    const uint64_t seconds = uint64_t(std::llround(frames / rate));
    char text[32];
    if (seconds >= 3600)
        std::snprintf(text, sizeof text, "%llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
                      (unsigned long long)(seconds / 60 % 60), (unsigned long long)(seconds % 60));
    else
        std::snprintf(text, sizeof text, "%llu:%02llu", (unsigned long long)(seconds / 60),
                      (unsigned long long)(seconds % 60));
    return text;
}

// The thumb nearest-scaled into b (whole rows and columns: cheap, and sharp for a game's pixels).
void blit_scaled(Canvas& out, const Thumb& t, const Box& b) {
    if (t.empty() || b.w <= 0 || b.h <= 0) return;
    // At the canvas's own pixels (a bigger panel's box is bigger, the thumb scaled into it).
    const int bx = out.to_px(b.x), by = out.to_px(b.y), bw = out.to_px(b.x + b.w) - bx, bh = out.to_px(b.y + b.h) - by;
    if (bw <= 0 || bh <= 0) return;
    uint32_t* px = out.data();
    for (int y = std::max(0, by); y < std::min(out.pixel_h(), by + bh); ++y) {
        const uint32_t* row = t.px.data() + size_t(std::min(t.h - 1, (y - by) * t.h / bh)) * t.w;
        uint32_t* dst = px + size_t(y) * out.pixel_w();
        for (int x = std::max(0, bx); x < std::min(out.pixel_w(), bx + bw); ++x)
            dst[x] = row[std::min(t.w - 1, (x - bx) * t.w / bw)] | 0xFF000000u;
    }
}

} // namespace

Box RewindScreen::box(Part part) {
    switch (part) {
    case Part::Strip: return {0, 88, 640, 252};
    case Part::Branch: return {170, 344, 300, 48};
    case Part::Back: return {12, 432, 160, 40}; // the footer's Back, as on every launcher page
    case Part::Play: return {468, 432, 160, 40};
    }
    return {};
}

Box RewindScreen::fit(const Thumb& t, const Box& b) {
    if (t.empty()) return b;
    const double scale = std::min(double(b.w) / t.w, double(b.h) / t.h);
    const int w = std::max(1, int(t.w * scale)), h = std::max(1, int(t.h * scale));
    return {b.x + (b.w - w) / 2, b.y + (b.h - h) / 2, w, h};
}

Box RewindScreen::picture_box(size_t i) const {
    const double d = double(i) - pos_, n = nearness(d);
    const int w = int(std::lround(small_w + (big_w - small_w) * n)), h = int(std::lround(small_h + (big_h - small_h) * n));
    return {int(std::lround(place_x(d))) - w / 2, row_y - h / 2, w, h};
}

double RewindScreen::centre_x(size_t i) const { return place_x(double(i) - pos_); }

void RewindScreen::open(const MomentTree& moments, uint32_t now) {
    timeline_ = &moments;
    now_ = tip_ = now;
    line_ = moments.line(tip_);
    const double last = line_.empty() ? 0 : double(line_.size() - 1);
    pos_ = target_ = last;
    vel_ = 0;
    selected_ = size_t(last);
    open_ = true;
    dragged_ = false;
    hold_ = Hold::None;
    pressing_ = -1;
    held_ = ~0u; // what is held as the screen opens (the button that opened it) does nothing until let go
    result_ = Result::None;
    ++serial_;
}

void RewindScreen::follow() {
    if (line_.empty()) return;
    const size_t i = size_t(std::clamp<long>(std::lround(pos_), 0, long(line_.size()) - 1));
    if (i != selected_) {
        selected_ = i;
        ++serial_;
    }
}

bool RewindScreen::at_rest() const { return hold_ == Hold::None && vel_ == 0 && pos_ == target_; }

bool RewindScreen::moving() const { return open_ && hold_ == Hold::None && !at_rest(); }

uint32_t RewindScreen::aimed() const {
    if (line_.empty()) return now_;
    return line_[size_t(std::clamp<long>(std::lround(target_), 0, long(line_.size()) - 1))];
}

void RewindScreen::aim(double index) {
    if (line_.empty()) return;
    target_ = std::clamp(std::round(index), 0.0, double(line_.size() - 1));
    if (!animations_) {
        pos_ = target_;
        vel_ = 0;
    }
    follow();
    ++serial_;
}

void RewindScreen::set_time(uint64_t ms, bool animations) {
    animations_ = animations;
    const uint64_t dt = ms_ && ms > ms_ ? std::min<uint64_t>(64, ms - ms_) : 0;
    ms_ = ms;
    if (!open_ || hold_ == Hold::Strip) return;
    if (!animations_) {
        pos_ = target_;
        vel_ = 0;
    } else if (pos_ != target_ || vel_ != 0) {
        // A damped spring to the target, in 4 ms steps.
        const double damping = 2 * spring_zeta * std::sqrt(spring_k);
        for (uint64_t t = 0; t < dt; t += 4) {
            const double h = double(std::min<uint64_t>(4, dt - t));
            vel_ += (spring_k * (target_ - pos_) - damping * vel_) * h;
            pos_ += vel_ * h;
        }
        if (std::abs(target_ - pos_) < .002 && std::abs(vel_) < .00005) {
            pos_ = target_;
            vel_ = 0;
        }
        ++serial_;
    }
    follow();
}

uint32_t RewindScreen::chosen() const { return line_.empty() ? now_ : line_[std::min(selected_, line_.size() - 1)]; }

void RewindScreen::step(int by) { aim(std::round(target_) + by); }

int RewindScreen::branch_count() const {
    if (!timeline_ || line_.empty()) return 0;
    const size_t n = timeline_->children(chosen()).size();
    return n >= 2 ? int(n) : 0;
}

int RewindScreen::branch_index() const {
    if (!branch_count() || selected_ + 1 >= line_.size()) return 0;
    const auto kids = timeline_->children(chosen());
    const auto it = std::find(kids.begin(), kids.end(), line_[selected_ + 1]);
    return it == kids.end() ? 0 : int(it - kids.begin());
}

void RewindScreen::next_branch(int direction) {
    const int n = branch_count();
    if (!n) return;
    const auto kids = timeline_->children(chosen());
    const int k = (branch_index() + direction + n) % n;
    const size_t keep = selected_;
    tip_ = timeline_->newest_leaf(kids[size_t(k)]);
    line_ = timeline_->line(tip_);
    // The moment in the middle stays; the cards after it are the other future's.
    selected_ = std::min(keep, line_.size() - 1);
    pos_ = target_ = double(selected_);
    vel_ = 0;
    ++serial_;
}

std::string RewindScreen::label(uint32_t id, const Words& words) const {
    if (id == now_) return words("Now");
    const Moment* m = timeline_ ? timeline_->find(id) : nullptr;
    const Moment* now = timeline_ ? timeline_->find(now_) : nullptr;
    if (!m || !now) return "";
    const double rate = timeline_->frame_rate();
    return m->position <= now->position ? "-" + clock_text(now->position - m->position, rate)
                                        : "+" + clock_text(m->position - now->position, rate);
}

bool RewindScreen::touch(const Touch& t) {
    if (!open_) return false;
    auto inside = [&](Part p) { return box(p).contains(t.x, t.y); };
    const bool header_back = t.x < 96 && t.y < 80; // the header's back arrow
    auto on = [&](int part) { return inside(Part(part)) || (part == int(Part::Back) && header_back); };
    const double last = line_.empty() ? 0 : double(line_.size() - 1);
    // Past an end the row gives a little, harder the further it is pulled.
    auto banded = [&](double p) {
        if (p < 0) return -.45 * (-p) / (1 + -p);
        if (p > last) return last + .45 * (p - last) / (1 + p - last);
        return p;
    };
    if (t.kind == Touch::Down) {
        dragged_ = false;
        down_x_ = t.x;
        speed_.reset();
        speed_.add(uint32_t(ms_), t.x);
        if (inside(Part::Strip)) {
            hold_ = Hold::Strip;
            vel_ = 0; // catches a coasting row
            down_pos_ = pos_;
        } else {
            hold_ = Hold::Button;
            pressing_ = on(int(Part::Back))                         ? int(Part::Back)
                        : inside(Part::Play)                         ? int(Part::Play)
                        : inside(Part::Branch) && branch_count() ? int(Part::Branch)
                                                                     : -1;
        }
        ++serial_;
        return true;
    }
    if (t.kind == Touch::Move) {
        speed_.add(uint32_t(ms_), t.x);
        if (hold_ == Hold::Strip) {
            if (std::abs(t.x - down_x_) > 8) dragged_ = true;
            if (dragged_) {
                // The row follows the finger: dragging right goes back in time.
                pos_ = banded(down_pos_ - double(t.x - down_x_) / drag_per_moment);
                target_ = std::clamp(std::round(pos_), 0.0, last);
                follow();
                ++serial_;
            }
        } else if (pressing_ >= 0 && !on(pressing_)) {
            pressing_ = -1; // slid off: cancelled
            ++serial_;
        }
        return true;
    }
    // Up.
    const Hold was = hold_;
    hold_ = Hold::None;
    if (was == Hold::Strip) {
        if (dragged_) {
            // The flick carries on as far as it would coast and settles on a card there, to look: only A or Play from
            // here plays. Without the springs (the game menu's software canvas) it lands there at once.
            const double v = -speed_.estimate(uint32_t(ms_)) / drag_per_moment;
            vel_ = animations_ ? v : 0;
            aim(pos_ + v / coast_friction);
        } else {
            // A tap on a card goes to it, to look: the middle card first, then outwards.
            for (size_t step = 0; step < line_.size() * 2 + 1; ++step) {
                const long i = long(std::lround(pos_)) + (step % 2 ? -1 : 1) * long((step + 1) / 2);
                if (i < 0 || i >= long(line_.size())) continue;
                if (picture_box(size_t(i)).contains(t.x, t.y)) {
                    aim(double(i));
                    break;
                }
            }
        }
        ++serial_;
        return true;
    }
    if (pressing_ >= 0 && on(pressing_)) {
        if (Part(pressing_) == Part::Back) result_ = Result::Back;
        else if (Part(pressing_) == Part::Play) {
            result_ = Result::Jump;
            target_id_ = aimed();
        } else next_branch(1);
    }
    pressing_ = -1;
    ++serial_;
    return true;
}

bool RewindScreen::pad(uint32_t buttons) {
    if (!open_) return false;
    if (held_ == ~0u) {
        // Waiting for the hands to let go of what opened the screen.
        if (buttons) return false;
        held_ = 0;
    }
    const uint32_t pressed = buttons & ~held_;
    held_ = buttons;
    const uint64_t before = serial_;
    if (pressed & A) {
        result_ = Result::Jump;
        target_id_ = aimed();
    } else if (pressed & B) result_ = Result::Back;
    if (pressed & L) step(-10);
    if (pressed & R) step(10);
    if (pressed & Up) next_branch(-1);
    if (pressed & Down) next_branch(1);
    const int dir = (buttons & Left) ? -1 : (buttons & Right) ? 1 : 0;
    if (pressed & (Left | Right)) {
        step(dir);
        repeat_at_ = ms_ + repeat_first_ms;
    } else if (dir && ms_ >= repeat_at_) {
        step(dir);
        repeat_at_ = ms_ + repeat_ms;
    }
    return serial_ != before || result_ != Result::None;
}

RewindScreen::Result RewindScreen::take(uint32_t& target) {
    const Result r = result_;
    result_ = Result::None;
    target = target_id_;
    return r;
}

uint64_t RewindScreen::key() const {
    if (!open_) return 0;
    return serial_ * 0x9E3779B97F4A7C15ull ^ uint64_t(pressing_ + 1) << 56;
}

// RG DS Play's second screen: the launcher's page, drawn with the kit (ui/kit.hpp) in the player's look.
void RewindScreen::draw(Canvas& out, const Look& look, const Font& font, const Words& words) const {
    const int W = out.w();
    backdrop(out, look);
    // The header: back arrow, title, and how far back the middle card is.
    touch_target({0, 0, 96, 80});
    icon(out, look, Icon::Back, 48, 44, 28, look.accent);
    font.draw(out, 88, font.centred(kit::large, 42), font.fit(words("Rewind"), kit::large, 360), kit::large, look.ink);
    const std::string when = label(chosen(), words);
    font.draw(out, W - 28 - font.measure(when, kit::title), font.centred(kit::title, 42), when, kit::title, look.muted);
    out.fill_rect(24, 77, 592, 1, look.line);
    out.fill_rect(24, 76, 44, 2, look.accent);

    // The row of cards, the far ones first so the nearer ones lie over them.
    std::vector<size_t> order;
    for (size_t i = 0; i < line_.size(); ++i) {
        const Box b = picture_box(i);
        if (b.x + b.w > -16 && b.x < W + 16) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return std::abs(double(a) - pos_) > std::abs(double(b) - pos_);
    });
    const int pad = kit::gap / 2;
    for (size_t i : order) {
        const Moment* m = timeline_->find(line_[i]);
        if (!m) continue;
        const Box b = picture_box(i), pic = fit(m->thumb, b);
        const bool focused = i == selected_;
        card(out, look, {pic.x - pad, pic.y - pad, pic.w + pad * 2, pic.h + pad * 2}, focused, 10);
        if (m->thumb.empty()) out.fill_rect(pic.x, pic.y, pic.w, pic.h, look.slot);
        else blit_scaled(out, m->thumb, pic);
        const std::string text = label(line_[i], words);
        font.draw(out, b.x + (b.w - font.measure(text, kit::small)) / 2, font.centred(kit::small, row_y + big_h / 2 + 22),
                  text, kit::small, focused ? look.ink : look.muted);
    }

    // Where the middle moment has more than one future: which one the row follows, tap for the next.
    if (const int n = branch_count()) {
        const Box b = box(Part::Branch);
        touch_target(b);
        card(out, look, b, pressing_ == int(Part::Branch));
        const std::string text =
            words("Future") + " " + std::to_string(branch_index() + 1) + "/" + std::to_string(n);
        font.draw(out, b.x + (b.w - font.measure(text, kit::body)) / 2, font.centred(kit::body, b.y + b.h / 2), text,
                  kit::body, look.ink);
    } else if (line_.size() <= 1) {
        const std::string text = words("Play on for a few seconds, then come back");
        font.draw(out, (W - font.measure(text, kit::small)) / 2, font.centred(kit::small, 368), text, kit::small,
                  look.muted);
    }

    // The footer, as the launcher's: Back on the left, the A action on the right.
    out.fill_rect(24, 424, 592, 1, look.line);
    const Box back = box(Part::Back), play = box(Part::Play);
    touch_target(back);
    touch_target(play);
    const bool back_down = pressing_ == int(Part::Back), play_down = pressing_ == int(Part::Play);
    raised(out, look, back.x, back.y, back.w, back.h, back.h / 2, back_down ? look.focus : look.panel,
           back_down ? 1 : 0, true, false);
    raised(out, look, play.x, play.y, play.w, play.h, play.h / 2, look.accent, play_down ? 1 : 0, true, false);
    const std::string back_text = font.fit(words("Back"), kit::body, back.w - 24),
                      play_text = font.fit(words("Play from here"), kit::body, play.w - 24);
    font.draw(out, back.x + (back.w - font.measure(back_text, kit::body)) / 2,
              font.centred(kit::body, back.y + back.h / 2 + (back_down ? 2 : 0)), back_text, kit::body, look.ink);
    font.draw(out, play.x + (play.w - font.measure(play_text, kit::body)) / 2,
              font.centred(kit::body, play.y + play.h / 2 + (play_down ? 2 : 0)), play_text, kit::body,
              look.on_accent);
}

} // namespace play
