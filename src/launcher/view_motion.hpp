#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace rgds {
// Fade: a layout or collection change in place (no travel, a short crossfade).
enum class ViewTravel { Forward, Back, Left, Right, Fade };
// Spring wobbles the live view; Push and Zoom composite the captured
// outgoing view with the incoming one (slide, or a DSi-style zoom and fade).
enum class ViewStyle { Spring, Push, Zoom };
inline bool layered(ViewStyle style) { return style != ViewStyle::Spring; }
inline const char* view_style_name(ViewStyle style) {
    return style == ViewStyle::Push ? "push" : style == ViewStyle::Zoom ? "zoom" : "spring";
}
// Snappy, Normal, Relaxed.
inline double view_pace(int speed) { return speed == 0 ? .55 : speed == 2 ? 1.7 : 1.0; }
struct MotionRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
// Only the last painted controls participate in hit testing. Fixed storage and
// inverse translation keep moving controls tappable without a widget tree.
struct MotionHits {
    struct Hit {
        MotionRect shown;
        int dx, dy;
    };
    std::array<Hit, 64> hits{};
    size_t count = 0;
    void clear() { count = 0; }
    void add(MotionRect base, int dx, int dy, int clip_top = 0, int clip_bottom = 480, int clip_left = 0,
             int clip_right = 640) {
        MotionRect shown{base.x + dx, base.y + dy, base.w, base.h};
        int bottom = std::min(clip_bottom, shown.y + shown.h);
        shown.y = std::max(clip_top, shown.y);
        shown.h = bottom - shown.y;
        int right = std::min(clip_right, shown.x + shown.w);
        shown.x = std::max(clip_left, shown.x);
        shown.w = right - shown.x;
        if (shown.w > 0 && shown.h > 0 && count < hits.size()) hits[count++] = {shown, dx, dy};
    }
    std::pair<int, int> unproject(int x, int y) const {
        for (size_t i = count; i > 0; i--) {
            auto& hit = hits[i - 1];
            if (hit.shown.contains(x, y)) return {x - hit.dx, y - hit.dy};
        }
        return {x, y};
    }
};
// Closed-form springs settle exactly; randomness is sampled per interaction,
// never per frame. Input cadence adds bounded energy, not queued animations.
struct ViewMotion {
    // Short and decisive: a transition must never feel like waiting.
    static constexpr uint32_t travel_duration = 300, duration = 340, reaction_duration = 160;
    uint32_t started = 0, reacted = 0, last_input = 0, seed = 0x6d2b79f5u, variant = 0;
    bool running = false, reacting = false, had_input = false;
    ViewTravel travel = ViewTravel::Forward;
    ViewStyle style = ViewStyle::Spring;
    double origin_x = 320, origin_y = 240, energy = 1, flavour = 1;
    // Transition speed: every duration scales together (Snappy .7, Relaxed 1.4).
    double pace = 1;
    uint32_t span(uint32_t base) const { return uint32_t(std::lround(base * pace)); }
    struct Pose {
        double x = 0, y = 0, sx = 1, sy = 1, tilt = 0, trail = 0;
    };
    struct Offset {
        int x = 0, y = 0;
    };
    static uint32_t hash(uint32_t n) {
        n ^= n >> 16;
        n *= 0x7feb352du;
        n ^= n >> 15;
        n *= 0x846ca68bu;
        return n ^ (n >> 16);
    }
    void interact(uint32_t now, double x = 320, double y = 240) {
        if (had_input && now - last_input < 8) return; // One physical input may call several helpers.
        energy = had_input ? 1 + .05 * std::max(0.0, 1 - double(now - last_input) / 300) : 1;
        last_input = now;
        had_input = true;
        origin_x = std::clamp(x, 0.0, 640.0);
        origin_y = std::clamp(y, 0.0, 480.0);
        reacted = now;
        reacting = true;
    }
    void start(uint32_t now, ViewTravel route = ViewTravel::Forward, ViewStyle effect = ViewStyle::Spring) {
        started = now;
        travel = route;
        style = effect;
        running = true;
        reacting = false;
        seed = hash(seed + 0x9e3779b9u);
        variant = seed;
        flavour = .97 + (variant % 61) / 1000.0;
    }
    void stop() { running = reacting = false; }
    bool active() const { return running || reacting; }
    bool update(uint32_t now) {
        bool repaint = active();
        if (now - started >= span(layered(style) || travel == ViewTravel::Fade ? push_duration : duration))
            running = false;
        if (now - reacted >= span(reaction_duration)) reacting = false;
        return repaint;
    }
    static double wave(double t, double frequency) {
        return std::exp(-6 * t) * (std::cos(frequency * t) + 6 / frequency * std::sin(frequency * t)) *
               (1 - t * t * t * t);
    }
    static constexpr uint32_t push_duration = 220;
    struct PushLayers {
        double incoming = 0, outgoing = 0, shade = 0;
        bool incoming_above = true;
        double alpha = 1;
    };
    PushLayers push(uint32_t now) const {
        if (!running || now - started >= span(push_duration)) return {};
        double t = double(now - started) / span(push_duration), remaining = (1 - t) * (1 - t) * (1 - t),
               progress = 1 - remaining;
        if (travel == ViewTravel::Fade) return {0, 0, 0, true, progress};
        if (travel == ViewTravel::Back) return {-160 * remaining, 640 * progress, .12 * remaining, false};
        if (travel == ViewTravel::Left) return {-640 * remaining, 640 * progress, 0, true};
        if (travel == ViewTravel::Right) return {640 * remaining, -640 * progress, 0, true};
        return {640 * remaining, -160 * progress, .12 * progress, true};
    }
    // Zoom: the incoming view grows into place as it fades in while the
    // outgoing one swells and fades (Back reverses it; pages drift sideways).
    struct ZoomLayers {
        double in_scale = 1, in_alpha = 1, in_dx = 0, out_scale = 1, out_alpha = 0, out_dx = 0;
    };
    ZoomLayers zoom(uint32_t now) const {
        if (!running || now - started >= span(push_duration)) return {};
        double t = double(now - started) / span(push_duration), p = 1 - (1 - t) * (1 - t) * (1 - t);
        // Ease the fade faster than the scale, so the new view reads at once.
        double in = std::min(1.0, 1 - (1 - t) * (1 - t) * (1 - t) * (1 - t) * (1 - t) * 1.0),
               out = 1 - std::min(1.0, t * 1.6);
        switch (travel) {
        case ViewTravel::Back: return {1.06 - .06 * p, in, 0, 1 - .06 * p, out, 0};
        case ViewTravel::Left: return {1, in, -40 * (1 - p), 1, out, 40 * p};
        case ViewTravel::Right: return {1, in, 40 * (1 - p), 1, out, -40 * p};
        case ViewTravel::Fade: return {1, in, 0, 1, out, 0};
        default: return {.94 + .06 * p, in, 0, 1 + .05 * p, out, 0};
        }
    }
    Pose pose(uint32_t now) const {
        if (style == ViewStyle::Push) return {push(now).incoming, 0, 1, 1, 0, 0};
        if (style == ViewStyle::Zoom) return {};
        if (!running || now - started >= span(travel_duration) || travel == ViewTravel::Fade) return {};
        double t = double(now - started) / span(travel_duration);
        double envelope = std::exp(-5 * t) * (1 - t) * (1 - t), spring = wave(t, 8),
               wobble = envelope * std::sin(8 * t);
        bool back = travel == ViewTravel::Back, lateral = travel == ViewTravel::Left || travel == ViewTravel::Right;
        double sign = (back || travel == ViewTravel::Left) ? -1 : 1, force = energy * flavour;
        return {sign * (back ? 30 : 26) * spring * force,
                (2 * wobble + (origin_y - 240) * .008 * spring) * force,
                1 + (back      ? .018
                     : lateral ? -.012
                               : -.025) *
                        spring,
                1 + (back ? .018 : -.025) * spring,
                0,
                envelope};
    }
    Offset element(uint32_t id, MotionRect box, bool focused, uint32_t now) const {
        if (layered(style)) return {};
        double dx = 0, dy = 0;
        uint32_t noise = hash(variant ^ id);
        double character = .95 + (noise % 101) / 1000.0;
        if (running && now - started < span(duration) && travel != ViewTravel::Fade) {
            double distance = std::hypot((box.x + box.w * .5 - origin_x) / 640, (box.y + box.h * .5 - origin_y) / 480);
            double delay = std::min(40.0, distance * 40) + (noise % 9);
            double elapsed = std::max(0.0, double(now - started) - delay),
                   t = std::min(1.0, elapsed / span(travel_duration));
            double spring = wave(t, 8);
            double sign = (travel == ViewTravel::Back || travel == ViewTravel::Left) ? -1 : 1;
            dx = sign * 7 * spring * energy * character;
            dy = ((box.y + box.h * .5 - origin_y) * .015 + (travel == ViewTravel::Back ? -3 : 4)) * spring * character;
        }
        if (reacting && now - reacted < span(reaction_duration)) {
            double t = double(now - reacted) / span(reaction_duration);
            double kick = std::sin(t * 3.141592653589793) * std::exp(-3 * t) * (1 - t) * (1 - t) * energy;
            dx += 0 * kick;
            dy -= (focused ? 5 : 0) * kick;
        }
        return {int(std::round(dx)), int(std::round(dy))};
    }
    double accent(uint32_t id, uint32_t now) const {
        if (layered(style)) return 0;
        double elapsed = running    ? double(now - started) - hash(variant ^ id) % 25
                         : reacting ? double(now - reacted)
                                    : 600;
        double t = std::clamp(elapsed / 300, 0.0, 1.0);
        return 0.35 * std::sin(t * 3.141592653589793) * std::exp(-3 * t) * (1 - t) * (1 - t) * energy;
    }
    std::pair<int, int> unproject(int x, int y, uint32_t now) const {
        auto p = pose(now);
        double angle = p.tilt * 3.141592653589793 / 180;
        double dx = x - 320 - p.x, dy = y - 240 - p.y;
        return {int(std::round(320 + (dx * std::cos(angle) + dy * std::sin(angle)) / p.sx)),
                int(std::round(240 + (-dx * std::sin(angle) + dy * std::cos(angle)) / p.sy))};
    }
};
}
