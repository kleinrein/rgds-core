#pragma once
// Touch kinetics: the finger's speed, the coast after a flick, the rubber band past a list's end, the glide of a
// focus change, and the page that follows an edge gesture. Times are SDL ticks (ms), distances panel pixels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rgds {
// The finger's recent positions; its speed at the lift.
struct FingerSpeed {
    static constexpr double hard_flick = 2.2; // px/ms: above this a flick means "far", not just "on a bit"
    struct Sample {
        uint32_t at = 0;
        double value = 0;
    };
    std::array<Sample, 8> samples{};
    size_t count = 0, next = 0;
    void reset() { count = next = 0; }
    void add(uint32_t now, double value) {
        samples[next] = {now, value};
        next = (next + 1) % samples.size();
        count = std::min(count + 1, samples.size());
    }
    // Units per ms over the last ~100 ms. A finger that paused 40 ms or more before lifting gives none.
    double estimate(uint32_t now) const {
        if (count < 2) return 0;
        const Sample* newest = nullptr;
        const Sample* oldest = nullptr;
        size_t recent = 0; // a real flick leaves a trail of samples; one jump is a drag that stopped
        for (size_t i = 0; i < count; i++) {
            const Sample& sample = samples[(next + samples.size() - 1 - i) % samples.size()];
            if (!newest) newest = &sample;
            if (now - sample.at > 120) break;
            oldest = &sample;
            recent++;
        }
        if (!newest || !oldest || recent < 3 || now - newest->at >= 40 || newest->at == oldest->at) return 0;
        return (newest->value - oldest->value) / double(newest->at - oldest->at);
    }
};

// One scrolling list. The launcher keeps reading its clamped integer scroll; `stretch()` is what the drawing adds:
// the rubber band while a finger pulls past an end, the bounce when a coast reaches one, the glide of a focus
// change. Closed in a few hundred ms; the finger always wins.
struct ScrollMotion {
    double position = 0, velocity = 0;
    double carried = 0; // the speed of the coast the finger just caught
    int anchor = 0;
    uint32_t began = 0;
    uint32_t last = 0;
    bool dragging = false, coasting = false, settling = false;
    FingerSpeed finger;
    static constexpr double friction = .0028; // v *= exp(-friction * dt): a firm flick coasts about a second
    static constexpr double extent = 72;      // how far an end gives, at most
    static constexpr double flick_boost = 3;  // each px/ms above a hard flick counts this much more
    static constexpr double top_speed = 32;   // px/ms, with repeated flicks; about 140 rows
    static constexpr double bounce_speed = 3; // an end takes at most this, however fast the coast came in
    static constexpr double tick_speed = 3;   // above this the rows pass too fast for a detent each
    static constexpr double band_k = .0011;   // the band's spring back to the end (critically damped)
    static constexpr double glide_k = .0024;  // a focus change glides there and settles with a small overshoot
    static constexpr double glide_zeta = .62;
    static double band(double excess) {
        const double pull = std::abs(excess);
        return (1 - 1 / (pull * .55 / extent + 1)) * extent * (excess < 0 ? -1 : 1);
    }
    bool active() const { return coasting || settling; }
    double stretch(int scroll) const { return dragging || active() ? position - scroll : 0; }
    bool ticking() const { return coasting && std::abs(velocity) < tick_speed; }
    void stop() {
        carried = coasting ? velocity : 0;
        dragging = coasting = settling = false;
        velocity = 0;
    }
    void begin(uint32_t now, int scroll) {
        const double kept = coasting ? velocity : carried; // pointer-down has usually stopped the coast already
        stop();
        carried = kept;
        began = now;
        dragging = true;
        position = anchor = scroll;
        finger.reset();
        finger.add(now, scroll);
        last = now;
    }
    // While the finger moves: `raw` is where the content would be with the finger glued to it.
    int drag(uint32_t now, double raw, int min, int max) {
        finger.add(now, raw);
        position = raw < min ? min + band(raw - min) : raw > max ? max + band(raw - max) : raw;
        last = now;
        return int(std::lround(std::clamp(raw, double(min), double(max))));
    }
    void release(uint32_t now, int min, int max) {
        if (!dragging) return;
        dragging = false;
        velocity = std::clamp(finger.estimate(now), -6.0, 6.0);
        const double hard = std::abs(velocity) - FingerSpeed::hard_flick;
        if (hard > 0) velocity += std::copysign(hard * flick_boost, velocity);
        // Flicking again while it still runs adds up, as long as the touch was a flick and not a stop and a new drag.
        if (carried * velocity > 0 && std::abs(velocity) > .6 && now - began < 250) velocity += carried;
        velocity = std::clamp(velocity, -top_speed, top_speed);
        carried = 0;
        coasting = std::abs(velocity) > .05 || position < min || position > max;
        if (!coasting) position = std::clamp(position, double(min), double(max));
        last = now;
    }
    // The logical scroll moved by a key: the drawing glides from where it was to `to`.
    void glide(uint32_t now, int from, int to, bool animations) {
        if (dragging || from == to) return;
        if (!active()) position = from;
        anchor = to;
        coasting = false;
        settling = animations;
        if (!settling) position = to;
        last = now;
    }
    // Advances the motion; `scroll` follows a coast. True while a repaint is due.
    bool step(uint32_t now, int& scroll, int min, int max) {
        if (dragging || !active()) return false;
        double remaining = std::min<double>(now - last, 100);
        last = now;
        while (remaining > 0) {
            const double dt = std::min(remaining, 8.0);
            remaining -= dt;
            if (settling) {
                const double x = position - anchor, omega = std::sqrt(glide_k);
                velocity += (-glide_k * x - 2 * glide_zeta * omega * velocity) * dt;
                position += velocity * dt;
                if (std::abs(position - anchor) < .4 && std::abs(velocity) < .01) {
                    position = anchor;
                    velocity = 0;
                    settling = false;
                    break;
                }
            } else if (position < min || position > max) {
                const double bound = position < min ? min : max, x = position - bound;
                velocity = std::clamp(velocity, -bounce_speed, bounce_speed);
                velocity += (-band_k * x - 2 * std::sqrt(band_k) * velocity) * dt;
                position += velocity * dt;
                if (std::abs(position - bound) < .4 && std::abs(velocity) < .01) {
                    position = bound;
                    velocity = 0;
                    coasting = false;
                    break;
                }
            } else {
                velocity *= std::exp(-friction * dt);
                position += velocity * dt;
                if (position < min || position > max) { // a fast coast reaches the end, not far beyond it
                    velocity = std::clamp(velocity, -bounce_speed, bounce_speed);
                    position = std::clamp(position, double(min), double(max)) + velocity * dt;
                    continue;
                }
                if (std::abs(velocity) < .06) { // under a pixel a frame: done, on a whole pixel
                    position = std::round(position);
                    velocity = 0;
                    coasting = false;
                    break;
                }
            }
        }
        if (!settling) scroll = int(std::lround(std::clamp(position, double(min), double(max))));
        return true;
    }
};

// The page under an edge gesture: it follows the finger, flies away when the gesture fires, or springs back.
struct PageDrag {
    enum class Kind { None, Home, Back };
    enum class Phase { Holding, Departing, Returning };
    Kind kind = Kind::None;
    Phase phase = Phase::Holding;
    double pull = 0, dx = 0, dy = 0, scale = 1, alpha = 1;
    double from_dx = 0, from_dy = 0, from_scale = 1;
    uint32_t began = 0;
    static constexpr uint32_t depart_ms = 300, return_ms = 420;
    bool active() const { return kind != Kind::None; }
    bool holding() const { return active() && phase == Phase::Holding; }
    bool departing() const { return active() && phase == Phase::Departing; }
    bool returning() const { return active() && phase == Phase::Returning; }
    void reset() {
        kind = Kind::None;
        phase = Phase::Holding;
        pull = dx = dy = 0;
        scale = alpha = 1;
    }
    static double ease_out(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }
    // The finger's pull along the gesture (px from where it touched the edge).
    void follow(Kind which, double distance) {
        kind = which;
        phase = Phase::Holding;
        pull = std::max(0.0, distance);
        alpha = 1;
        if (kind == Kind::Home) {
            // The page lifts with the finger and shrinks toward the middle; past the trigger it keeps giving a little.
            const double near = std::min(pull, 72.0), beyond = ScrollMotion::band(std::max(0.0, pull - 72));
            dx = 0;
            dy = -(near * .7 + beyond * .5);
            scale = 1 - .14 * ease_out(near / 72) - .04 * (beyond / ScrollMotion::extent);
        } else {
            const double near = std::min(pull, 72.0), beyond = ScrollMotion::band(std::max(0.0, pull - 72));
            dx = near * .9 + beyond * .6;
            dy = 0;
            scale = 1 - .03 * ease_out(near / 72);
        }
    }
    void depart(uint32_t now) {
        if (!active()) return;
        phase = Phase::Departing;
        began = now;
        from_dx = dx;
        from_dy = dy;
        from_scale = scale;
    }
    void settle(uint32_t now) {
        if (!active()) return;
        phase = Phase::Returning;
        began = now;
        from_dx = dx;
        from_dy = dy;
        from_scale = scale;
    }
    // True while a repaint is due.
    bool update(uint32_t now, double pace) {
        if (!active() || phase == Phase::Holding) return false;
        const double span = (phase == Phase::Departing ? depart_ms : return_ms) * pace;
        const double t = std::min(1.0, (now - began) / std::max(1.0, span));
        if (phase == Phase::Departing) {
            const double e = ease_out(t);
            if (kind == Kind::Home) {
                dx = 0;
                dy = from_dy + (-150 - from_dy) * e;
                scale = from_scale * (1 - .45 * e);
                alpha = 1 - e;
            } else {
                dx = from_dx + (660 - from_dx) * e;
                dy = 0;
                scale = from_scale + (1 - from_scale) * e;
                alpha = 1;
            }
        } else {
            // A spring home: a small overshoot through rest, then still.
            const double wave = std::exp(-6 * t) * std::cos(9 * t) * (1 - t);
            dx = from_dx * wave;
            dy = from_dy * wave;
            scale = 1 + (from_scale - 1) * wave;
            alpha = 1;
        }
        if (t >= 1) {
            reset();
            return true;
        }
        return true;
    }
};
}
