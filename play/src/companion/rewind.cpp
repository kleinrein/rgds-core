// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/rewind.hpp"

#include "core/core.hpp"
#include "ui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace play {

namespace {

// The stage's own colours: Cover Flow's black, whatever the theme.
const Rgb white{240, 243, 248}, grey{150, 158, 172}, aqua{56, 140, 230}, gel{226, 229, 234}, gel_ink{28, 32, 40},
    graphite{70, 76, 88};

// The springs, in moments and milliseconds.
constexpr double spring_k = .0006;  // stiffness: settles in about a quarter of a second
constexpr double spring_zeta = .8;  // a touch of overshoot, as a picture lands
constexpr double coast_friction = .004; // a flick of v moments/ms coasts v / friction moments
constexpr uint64_t repeat_first_ms = 300, repeat_ms = 90;

// The scrubber's track, and its thumb: the thumb's centre travels the track less half the thumb at each end.
constexpr int track_x = 64, track_w = 512, track_y = 356, scrub_thumb_w = 56;
int scrub_x(double fraction) { return track_x + scrub_thumb_w / 2 + int(std::lround((track_w - scrub_thumb_w) * fraction)); }

struct Place {
    double x, turn, scale; // turn: -1 faces right (left side), 0 faces you, 1 faces left
};

Place place(double d) {
    const double a = std::clamp(d, -1.0, 1.0);
    double x = 320 + a * RewindScreen::side_x;
    if (std::abs(d) > 1) x += (d > 0 ? 1 : -1) * (std::abs(d) - 1) * RewindScreen::stack_x;
    return {x, a, 1 - .14 * std::abs(a)};
}

// The picture's size at `scale`, in its own shape.
void cover_size(const Thumb& t, double scale, double& w, double& h) {
    w = RewindScreen::cover_w * scale;
    h = RewindScreen::cover_h * scale;
    if (!t.empty()) {
        const double fit = std::min(w / t.w, h / t.h);
        w = t.w * fit;
        h = t.h * fit;
    }
}

inline uint32_t blend(uint32_t dst, uint32_t src, int alpha) {
    if (alpha >= 255) return src | 0xFF000000u;
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        const int d = (dst >> s) & 0xFF, c = (src >> s) & 0xFF;
        out |= uint32_t(d + ((c - d) * alpha + 127) / 255) << s;
    }
    return out;
}

inline uint32_t sample(const Thumb& t, double u, double v) {
    if (t.empty()) {
        // No picture (a GPU game): a slate with a soft top.
        const int g = int(46 + 20 * (1 - v));
        return 0xFF000000u | uint32_t(g + 8) << 16 | uint32_t(g + 2) << 8 | uint32_t(g);
    }
    const double x = std::clamp(u * t.w - .5, 0.0, t.w - 1.0), y = std::clamp(v * t.h - .5, 0.0, t.h - 1.0);
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, t.w - 1), y1 = std::min(y0 + 1, t.h - 1);
    const int fx = int((x - x0) * 256), fy = int((y - y0) * 256);
    const uint32_t a = t.px[size_t(y0) * t.w + x0], b = t.px[size_t(y0) * t.w + x1], c = t.px[size_t(y1) * t.w + x0],
                   d = t.px[size_t(y1) * t.w + x1];
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        const int top = int((a >> s) & 0xFF) * (256 - fx) + int((b >> s) & 0xFF) * fx,
                  bottom = int((c >> s) & 0xFF) * (256 - fx) + int((d >> s) & 0xFF) * fx;
        out |= uint32_t((top * (256 - fy) + bottom * fy) >> 16) << s;
    }
    return out;
}

inline uint32_t shade(uint32_t px, int level256) {
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) out |= uint32_t((((px >> s) & 0xFF) * level256) >> 8) << s;
    return out;
}

// One picture turned in perspective about its upright centre line, with its reflection on the floor.
void draw_cover(Canvas& out, const Thumb& t, const Place& p) {
    double w = 0, h = 0;
    cover_size(t, p.scale, w, h);
    const double a = std::abs(p.turn);
    const double shown_w = w * (1 - .58 * a), near_h = h * (1 + .06 * a), far_h = h * (1 - .2 * a);
    const int left = int(std::lround(p.x - shown_w / 2)), cols = std::max(1, int(std::lround(shown_w)));
    const int light = int(256 * (1 - .28 * a));
    uint32_t* px = out.data();
    const int W = out.w(), H = out.h();
    for (int col = 0; col < cols; ++col) {
        const int x = left + col;
        if (x < 0 || x >= W) continue;
        const double c = (col + .5) / cols;
        // From the near edge (outward for a turned picture), perspective-correct across the turn.
        const double cn = p.turn <= 0 ? c : 1 - c;
        const double ch = near_h + (far_h - near_h) * cn;
        const double un = cn * far_h / ((1 - cn) * near_h + cn * far_h);
        const double u = p.turn <= 0 ? un : 1 - un;
        const double top = RewindScreen::flow_y - ch / 2;
        const int y0 = int(std::lround(top)), rows = std::max(1, int(std::lround(ch)));
        for (int r = 0; r < rows; ++r) {
            const int y = y0 + r;
            if (y < 0 || y >= H) continue;
            px[size_t(y) * W + x] = shade(sample(t, u, (r + .5) / rows), light);
        }
        // The floor's reflection: the picture upside down, fading out over a third of its height.
        const int reflect = int(rows * .42);
        for (int r = 0; r < reflect; ++r) {
            const int y = y0 + rows + 1 + r;
            if (y < 0 || y >= H) continue;
            const double f = 1 - double(r) / reflect;
            const int alpha = int(255 * .42 * f * f);
            uint32_t& dst = px[size_t(y) * W + x];
            dst = blend(dst, shade(sample(t, u, 1 - (r + .5) / rows), light), alpha);
        }
    }
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

// A gel button: Aqua's capsule, its shadow and its shine.
void gel_button(Canvas& out, const Box& b, Rgb base, bool down) {
    soft_round(out, b.x, b.y + 3, b.w, b.h, b.h / 2, Rgb{0, 0, 0}, down ? 60 : 110);
    glossy(out, b.x, b.y, b.w, b.h, b.h / 2, base, down ? 1 : 0, true);
}

} // namespace

Box RewindScreen::box(Part part) {
    const int m = kit::margin, g = kit::gap, half = (640 - m * 2 - g) / 2;
    switch (part) {
    case Part::Strip: return {0, 116, 640, 216}; // below the branch pill
    case Part::Scrubber: return {track_x - 24, 338, track_w + 48, 48};
    case Part::Branch: return {200, 64, 240, 48};
    case Part::Back: return {m, 410, half, 50};
    case Part::Play: return {m + half + g, 410, half, 50};
    }
    return {};
}

void RewindScreen::open(const MomentTree& timeline, uint32_t now) {
    timeline_ = &timeline;
    now_ = tip_ = now;
    line_ = timeline.line(tip_);
    const double last = line_.empty() ? 0 : double(line_.size() - 1);
    target_ = last;
    // The strip flies in from the right and lands on now.
    pos_ = animations_ ? last + 2.5 : last;
    vel_ = 0;
    selected_ = size_t(last);
    open_ = true;
    commit_ = dragged_ = false;
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
    if (!open_ || hold_ == Hold::Strip || hold_ == Hold::Scrubber) return;
    if (!animations_) {
        pos_ = target_;
        vel_ = 0;
    } else {
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
    }
    follow();
    if (commit_ && at_rest()) {
        commit_ = false;
        result_ = Result::Jump;
        target_id_ = chosen();
    }
}

double RewindScreen::centre_x(size_t i) const { return place(double(i) - pos_).x; }

double RewindScreen::scrub_fraction(int x) {
    return std::clamp(double(x - track_x - scrub_thumb_w / 2) / (track_w - scrub_thumb_w), 0.0, 1.0);
}

uint32_t RewindScreen::aimed() const {
    if (line_.empty()) return now_;
    return line_[size_t(std::clamp<long>(std::lround(target_), 0, long(line_.size()) - 1))];
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
    // The moment in the middle stays; the pictures after it are the other future's.
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
    const double last = line_.empty() ? 0 : double(line_.size() - 1);
    // Past an end the strip gives a little, harder the further it is pulled.
    auto banded = [&](double p) {
        if (p < 0) return -.45 * (-p) / (1 + -p);
        if (p > last) return last + .45 * (p - last) / (1 + p - last);
        return p;
    };
    auto scrub_to = [&](int x) {
        pos_ = last * scrub_fraction(x);
        target_ = std::round(pos_);
    };
    if (t.kind == Touch::Down) {
        commit_ = false; // a finger stops whatever was about to happen
        dragged_ = false;
        down_x_ = t.x;
        speed_.reset();
        speed_.add(uint32_t(ms_), t.x);
        if (inside(Part::Strip)) {
            hold_ = Hold::Strip;
            vel_ = 0; // catches a coasting strip
            down_pos_ = pos_;
        } else if (inside(Part::Scrubber) && !line_.empty()) {
            hold_ = Hold::Scrubber;
            vel_ = 0;
            scrub_to(t.x);
            follow();
        } else {
            hold_ = Hold::Button;
            pressing_ = inside(Part::Back)                         ? int(Part::Back)
                        : inside(Part::Play)                       ? int(Part::Play)
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
                // The strip follows the finger: dragging right goes back in time.
                pos_ = banded(down_pos_ - double(t.x - down_x_) / drag_per_moment);
                target_ = std::clamp(std::round(pos_), 0.0, last);
                follow();
                ++serial_;
            }
        } else if (hold_ == Hold::Scrubber) {
            if (std::abs(t.x - down_x_) > 4) dragged_ = true;
            scrub_to(t.x);
            follow();
            ++serial_;
        } else if (pressing_ >= 0 && !inside(Part(pressing_))) {
            pressing_ = -1; // slid off: cancelled
            ++serial_;
        }
        return true;
    }
    // Up.
    const Hold was = hold_;
    hold_ = Hold::None;
    if (was == Hold::Strip || was == Hold::Scrubber) {
        if (was == Hold::Scrubber && !dragged_) {
            // A tap on the track: the strip glides to that moment, to look at it (as a tap on a picture does).
            aim(pos_);
        } else if (dragged_) {
            // The flick carries on: it coasts, settles on a picture, and plays from there.
            const double v = was == Hold::Strip ? -speed_.estimate(uint32_t(ms_)) / drag_per_moment : 0;
            vel_ = animations_ ? v : 0;
            aim(pos_ + (animations_ ? v / coast_friction : 0));
            if (at_rest()) {
                result_ = Result::Jump;
                target_id_ = chosen();
            } else commit_ = true;
        } else if (!line_.empty()) {
            // A tap on a picture glides to it: the nearest picture under the finger, the middle one first.
            std::vector<size_t> order(line_.size());
            std::iota(order.begin(), order.end(), size_t(0));
            std::sort(order.begin(), order.end(),
                      [&](size_t a, size_t b) { return std::abs(a - pos_) < std::abs(b - pos_); });
            for (size_t i : order) {
                const Moment* mo = timeline_->find(line_[i]);
                if (!mo) continue;
                const Place p = place(double(i) - pos_);
                double w = 0, h = 0;
                cover_size(mo->thumb, p.scale, w, h);
                const double half = w * (1 - .58 * std::abs(p.turn)) / 2;
                if (std::abs(t.x - p.x) <= std::max(half, 24.0) && std::abs(t.y - flow_y) <= h / 2 + 8) {
                    aim(double(i));
                    break;
                }
            }
        }
        ++serial_;
        return true;
    }
    if (pressing_ >= 0 && inside(Part(pressing_))) {
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
    uint64_t k = serial_ * 0x9E3779B97F4A7C15ull;
    k ^= uint64_t(std::llround(pos_ * 1000) + (1 << 20)) << 20;
    k ^= uint64_t(selected_) << 4 ^ uint64_t(pressing_ + 1) ^ uint64_t(hold_) << 60;
    return k;
}

void RewindScreen::draw(Canvas& out, const Look& look, const Font& font, const Words& words) const {
    const int W = out.w(), H = out.h();
    // The stage: night above the floor, the floor darker, a few stars, a glow behind the middle picture.
    if (stage_.w() != W || stage_.h() != H) {
        stage_.resize(W, H);
        const int floor = flow_y + cover_h / 2;
        for (int y = 0; y < H; ++y) {
            const double f = y < floor ? double(y) / floor : double(y - floor) / std::max(1, H - floor);
            // The floor starts where the sky ends, so the horizon is a soft turn, not a line.
            const Rgb c = y < floor ? mix(Rgb{5, 6, 10}, Rgb{27, 30, 39}, int(256 * f))
                                    : mix(Rgb{27, 30, 39}, Rgb{3, 4, 6}, int(256 * std::sqrt(f)));
            stage_.fill_rect(0, y, W, 1, c);
        }
        uint32_t seed = 0x2007u; // the same sky every time
        auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
        for (int i = 0; i < 110; ++i) {
            const int x = int(next() % W), y = int(next() % uint32_t(floor - 20));
            const int glow = 30 + int(next() % 120) * (floor - y) / floor;
            stage_.fill_rect(x, y, 1, 1, Rgb{230, 236, 255}, glow);
            if (next() % 9 == 0) {
                stage_.fill_rect(x - 1, y, 3, 1, Rgb{200, 215, 255}, glow / 3);
                stage_.fill_rect(x, y - 1, 1, 3, Rgb{200, 215, 255}, glow / 3);
            }
        }
        // A soft blue light behind the middle picture, falling off smoothly to nothing, and its pool on the floor.
        auto glow = [&](int cx, int cy, int rx, int ry, Rgb c, int peak) {
            for (int y = std::max(0, cy - ry); y < std::min(H, cy + ry); ++y)
                for (int x = std::max(0, cx - rx); x < std::min(W, cx + rx); ++x) {
                    const double dx = double(x - cx) / rx, dy = double(y - cy) / ry, r2 = dx * dx + dy * dy;
                    if (r2 >= 1) continue;
                    const double f = (1 - r2) * (1 - r2);
                    uint32_t& d = stage_.data()[size_t(y) * W + x];
                    d = blend(d, c.px(), int(peak * f));
                }
        };
        glow(320, flow_y - 10, 300, 170, Rgb{64, 110, 190}, 46);
        glow(320, floor + 6, 240, 26, Rgb{90, 140, 220}, 30);
    }
    std::copy(stage_.data(), stage_.data() + size_t(W) * H, out.data());
    for (Part p : {Part::Scrubber, Part::Back, Part::Play}) touch_target(box(p));

    // Cover Flow: the far pictures first, so each nearer one lies over them.
    std::vector<size_t> order;
    for (size_t i = 0; i < line_.size(); ++i) {
        const double x = centre_x(i);
        if (x > -cover_w && x < W + cover_w) order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return std::abs(a - pos_) > std::abs(b - pos_); });
    for (size_t i : order)
        if (const Moment* mo = timeline_->find(line_[i])) draw_cover(out, mo->thumb, place(double(i) - pos_));

    // Above: how far back the middle picture is, and what to do (or which future the strip is on).
    const std::string when = label(chosen(), words);
    font.draw(out, (W - font.measure(when, kit::number)) / 2, font.centred(kit::number, 38), when, kit::number, white);
    if (const int n = branch_count()) {
        const Box b = box(Part::Branch);
        touch_target(b);
        const std::string text = words("Branch") + " " + std::to_string(branch_index() + 1) + " / " + std::to_string(n);
        const int tw = font.measure(text, kit::body) + 40;
        const Box pill{(W - tw) / 2, b.y + 6, tw, 36};
        gel_button(out, pill, graphite, pressing_ == int(Part::Branch));
        font.draw(out, pill.x + 20, font.centred(kit::body, pill.y + pill.h / 2), text, kit::body, white);
    } else {
        const std::string hint = line_.size() <= 1 ? words("Nothing to go back to yet")
                                                   : words("Drag to go back, let go to play");
        font.draw(out, (W - font.measure(hint, kit::small)) / 2, font.centred(kit::small, 84), hint, kit::small, grey);
    }

    // The scrubber: a sunken track, where the strip branches marked under it, the Aqua thumb at the middle picture.
    const double last = line_.empty() ? 0 : double(line_.size() - 1);
    out.rounded_rect(track_x - 1, track_y - 1, track_w + 2, 14, 7, Rgb{62, 66, 78});
    out.rounded_rect(track_x, track_y, track_w, 12, 6, Rgb{10, 12, 16});
    out.fill_rect(track_x + 4, track_y + 1, track_w - 8, 1, Rgb{0, 0, 0}, 160);
    if (last > 0) {
        // How many futures each moment has, counted once (children() per moment would be quadratic).
        std::map<uint32_t, int> futures;
        for (uint32_t id : timeline_->ids())
            if (const Moment* m = timeline_->find(id); m && m->parent) ++futures[m->parent];
        for (size_t i = 0; i < line_.size(); ++i)
            if (futures[line_[i]] >= 2)
                disc(out, scrub_x(i / last), track_y + 24, 3, Rgb{90, 170, 255});
    }
    const double f = last > 0 ? std::clamp(pos_ / last, 0.0, 1.0) : 1;
    gel_button(out, {scrub_x(f) - scrub_thumb_w / 2, track_y - 5, scrub_thumb_w, 22}, aqua, hold_ == Hold::Scrubber);

    // Back to now, and Play from here: Time Machine's Cancel and Restore.
    const Box back = box(Part::Back), play = box(Part::Play);
    const bool back_down = pressing_ == int(Part::Back), play_down = pressing_ == int(Part::Play);
    gel_button(out, back, gel, back_down);
    gel_button(out, play, aqua, play_down);
    const std::string back_text = font.fit(words("Back to now"), kit::title, back.w - 32),
                      play_text = font.fit(words("Play from here"), kit::title, play.w - 32);
    font.draw(out, back.x + (back.w - font.measure(back_text, kit::title)) / 2,
              font.centred(kit::title, back.y + back.h / 2 + (back_down ? 2 : 0)), back_text, kit::title, gel_ink);
    font.draw(out, play.x + (play.w - font.measure(play_text, kit::title)) / 2,
              font.centred(kit::title, play.y + play.h / 2 + (play_down ? 2 : 0)), play_text, kit::title, white);
    (void)look;
}

} // namespace play
