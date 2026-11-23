#pragma once
// The music player's visualisers: the kind a hi-fi and a demo disk had. Each one draws a low, wide picture of big
// pixels from what the engine hears (music/shared.hpp Viz); the screen shows it doubled, as a dark display window
// tinted with the theme's accent colour. Plain arithmetic on a pixel buffer: no SDL, so the pictures can be tested.
#include "launcher/model.hpp"
#include "launcher/pixel_text.hpp"
#include "music/shared.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rgds {
enum class VizMode {
    Bars,
    Scope,
    Fire,
    Plasma,
    Stars,
    Meters,
    Vector,
    Radial,
    Waterfall,
    Tunnel,
    Prism,
    Lissajous,
    Matrix,
    Kaleido,
    Fireworks,
    Lava,
    Aurora,
    Copper,
    Synthwave,
    Ripples,
    Rotozoom,
    Count
};
inline constexpr std::array<const char*, size_t(VizMode::Count)> viz_names = {
    "Spectrum",        "Oscilloscope", "Fire",   "Plasma",      "Starfield", "VU meters",   "Vectorscope",
    "Radial spectrum", "Waterfall",    "Tunnel", "Prism",       "Lissajous", "Matrix rain", "Kaleidoscope",
    "Fireworks",       "Lava lamp",    "Aurora", "Copper bars", "Synthwave", "Ripples",     "Rotozoom"};

// What a visualiser reads: a copy of the engine's numbers, taken once a frame.
struct VizInput {
    std::array<float, music::viz_bands> bands{};
    std::array<std::array<float, music::viz_wave>, 2> wave{};
    float peak[2] = {}, rms[2] = {}, bass = 0, beat = 0;
    void take(const music::Viz& from) {
        std::copy(std::begin(from.bands), std::end(from.bands), bands.begin());
        for (int c = 0; c < 2; c++) {
            std::copy(std::begin(from.wave[c]), std::end(from.wave[c]), wave[size_t(c)].begin());
            peak[c] = from.peak[c];
            rms[c] = from.rms[c];
        }
        bass = from.bass;
        beat = from.beat;
    }
};

class Visualiser {
public:
    // The picture's size in its own big pixels; the screen shows each one as 2 x 2. See `resize`.
    int width = 292, height = 136;

private:
    std::vector<uint32_t> pixels = std::vector<uint32_t>(size_t(width) * size_t(height), 0xff000000u);
    std::vector<float> glow = std::vector<float>(size_t(width) * size_t(height), 0); // scope: phosphor; fire: heat
    std::array<uint32_t, 256> ramp{};
    Color accent{35, 110, 174};
    uint32_t back = 0xff05070c, grid = 0xff101622, dim = 0xff2a3442, bright = 0xffffffff;
    std::array<float, 32> bar{}, cap{}, cap_hold{}, cap_fall{};
    // The waterfall's history: one spectrum a row, the newest first.
    static constexpr int ridges = 30;
    std::array<std::array<float, music::viz_bands>, ridges> ridge{};
    float ridge_clock = 0;
    struct Ring {
        float z = 0, energy = 0, turn = 0;
        std::array<float, 16> shape{};
    };
    std::array<Ring, 24> rings{};
    float ring_clock = 0, ring_spin = 0;
    // An automatic gain for the pictures that draw the wave itself: a quiet recording still fills the screen.
    float wave_gain = 1;
    struct Star {
        float x = 0, y = 0, z = 0;
    };
    std::array<Star, 140> stars{};
    float needle[2] = {}, needle_speed[2] = {}, clip[2] = {};
    float clock = 0, flash = 0, drift = 0;
    // The colourful pictures' own state: fireworks, falling code, ripples, and which Lissajous figure is drawn.
    struct Spark {
        float x = 0, y = 0, vx = 0, vy = 0, life = 0, hue = 0;
        bool rocket = false;
    };
    std::array<Spark, 700> sparks{};
    float launch_wait = 0;
    std::array<float, 96> rain_y{}, rain_speed{};
    bool rain_ready = false;
    struct Drop {
        float x = 0, y = 0, r = 0, life = 0, hue = 0;
    };
    std::array<Drop, 12> drops{};
    float drop_wait = 0, hue_clock = 0, figure_turn = 0;
    int figure = 0;
    uint32_t seed = 0x2545f491;
    VizMode last = VizMode::Count;

    // Follows the loudest recent sample: up at once, back down over a couple of seconds, never more than 8x.
    float follow_gain(const VizInput& in, float dt) {
        float loudest = 0;
        for (int c = 0; c < 2; c++)
            for (float v : in.wave[size_t(c)]) loudest = std::max(loudest, std::fabs(v));
        const float goal = std::clamp(.8f / std::max(loudest, .1f), 1.f, 8.f);
        wave_gain = goal < wave_gain ? goal : wave_gain + (goal - wave_gain) * std::min(1.f, dt * .6f);
        return wave_gain;
    }
    // Phosphor: lights `glow` along a line, softer around it; a fast beam is dimmer where it passes.
    void burn(float x0, float y0, float x1, float y1, float amount) {
        const float dx = x1 - x0, dy = y1 - y0, length = std::max(1e-3f, std::hypot(dx, dy));
        const int steps = std::max(1, int(length * 1.5f));
        for (int s = 0; s <= steps; s++) {
            const float fx = x0 + dx * float(s) / float(steps), fy = y0 + dy * float(s) / float(steps);
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++) {
                    const int px = int(std::lround(fx)) + ox, py = int(std::lround(fy)) + oy;
                    if (px < 0 || px >= width || py < 0 || py >= height) continue;
                    float& g = glow[size_t(py) * width + size_t(px)];
                    g = std::min(1.6f, g + amount * (ox || oy ? .16f : .6f) / (1 + length * .12f));
                }
        }
    }
    // The phosphor onto the screen, over a background `rule` may mark (grid lines and the like).
    template <typename Rule> void show_glow(Rule rule) {
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                uint32_t colour = rule(x, y) ? grid : back;
                const float g = glow[size_t(y) * width + size_t(x)];
                if (g > .004f) colour = add(colour, ramp[size_t(std::min(255.f, 110 + g * 120))], std::min(1.f, g));
                pixels[size_t(y) * width + size_t(x)] = colour;
            }
    }
    // Lets the last picture die away towards the background: trails.
    void fade(float dt, float rate) {
        const float t = std::min(1.f, dt * rate);
        for (uint32_t& p : pixels) p = mix(p, back, t);
    }
    // A band's level read at any point between bands, 0 to 1 across the whole spectrum.
    float band_at(const VizInput& in, float place) const {
        place = std::clamp(place, 0.f, 1.f) * float(music::viz_bands - 1);
        const size_t low = size_t(place), high = std::min<size_t>(music::viz_bands - 1, low + 1);
        return in.bands[low] + (in.bands[high] - in.bands[low]) * (place - float(low));
    }

    float random() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return float(seed & 0xffff) / 65535.f;
    }
    static uint32_t rgb(float r, float g, float b) {
        const auto channel = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + .5f); };
        return 0xff000000u | channel(r) << 16 | channel(g) << 8 | channel(b);
    }
    static uint32_t mix(uint32_t a, uint32_t b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        const auto part = [&](int shift) {
            return float((a >> shift) & 255) * (1 - t) + float((b >> shift) & 255) * t;
        };
        return rgb(part(16), part(8), part(0));
    }
    static uint32_t add(uint32_t a, uint32_t b, float amount) {
        const auto part = [&](int shift) {
            return std::min(255.f, float((a >> shift) & 255) + float((b >> shift) & 255) * amount);
        };
        return rgb(part(16), part(8), part(0));
    }
    void put(int x, int y, uint32_t colour) {
        if (x >= 0 && x < width && y >= 0 && y < height) pixels[size_t(y) * width + size_t(x)] = colour;
    }
    void light(int x, int y, uint32_t colour, float amount) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            uint32_t& p = pixels[size_t(y) * width + size_t(x)];
            p = add(p, colour, amount);
        }
    }
    void fill(int x, int y, int w, int h, uint32_t colour) {
        for (int py = std::max(0, y); py < std::min(height, y + h); py++)
            for (int px = std::max(0, x); px < std::min(width, x + w); px++)
                pixels[size_t(py) * width + size_t(px)] = colour;
    }
    void clear() { std::fill(pixels.begin(), pixels.end(), back); }
    // A line with soft edges: coverage by distance, added as light.
    void beam(float x0, float y0, float x1, float y1, uint32_t colour, float amount, float thickness = 1) {
        const float dx = x1 - x0, dy = y1 - y0, length = std::max(1e-3f, std::hypot(dx, dy)), reach = thickness / 2 + 1;
        const int left = int(std::floor(std::min(x0, x1) - reach)), right = int(std::ceil(std::max(x0, x1) + reach)),
                  top = int(std::floor(std::min(y0, y1) - reach)), bottom = int(std::ceil(std::max(y0, y1) + reach));
        for (int y = std::max(0, top); y <= std::min(height - 1, bottom); y++)
            for (int x = std::max(0, left); x <= std::min(width - 1, right); x++) {
                const float t = std::clamp(((float(x) - x0) * dx + (float(y) - y0) * dy) / (length * length), 0.f, 1.f);
                const float distance = std::hypot(float(x) - (x0 + t * dx), float(y) - (y0 + t * dy));
                const float cover = std::clamp(thickness / 2 + .5f - distance, 0.f, 1.f);
                if (cover > 0) light(x, y, colour, amount * cover);
            }
    }
    void words(const std::string& text, int x, int y, uint32_t colour) {
        struct Dot {
            int x, y, w, h;
        };
        pixel_text_dots<Dot>(
            text, x, y - 3, 1, 40, [&](const Dot& dot) { fill(dot.x, dot.y, dot.w, dot.h, colour); },
            [](unsigned, int, int) {});
    }

    void draw_bars(const VizInput& in, float dt) {
        clear();
        const int base = height - 36, columns = 32, pitch = 9, left = (width - columns * pitch + 2) / 2,
                  cells = (base - 8) / 4;
        flash = std::max(in.beat, flash - dt * 5);
        const uint32_t rule = mix(grid, ramp[150], flash * .35f);
        for (int y = 8; y < base; y += 16)
            for (int x = left; x < left + columns * pitch - 2; x += 4) put(x, y, rule);
        for (int i = 0; i < columns; i++) {
            const float level = std::max(in.bands[size_t(i) * 2], in.bands[size_t(i) * 2 + 1]);
            bar[size_t(i)] = level; // the engine already gives the fall
            if (level >= cap[size_t(i)]) {
                cap[size_t(i)] = level;
                cap_hold[size_t(i)] = .4f;
                cap_fall[size_t(i)] = 0;
            } else if ((cap_hold[size_t(i)] -= dt) < 0) {
                // After the hold the cap drops as a weight would: slowly at first, then faster.
                cap_fall[size_t(i)] += dt * 2.6f;
                cap[size_t(i)] = std::max(0.f, cap[size_t(i)] - cap_fall[size_t(i)] * dt);
            }
            const int lit = int(std::lround(bar[size_t(i)] * cells)), x = left + i * pitch;
            for (int c = 0; c < cells; c++) {
                const int y = base - 4 - c * 4;
                const uint32_t colour = ramp[size_t(96 + c * 159 / (cells - 1))];
                // Unlit cells stay faintly visible, as an LED ladder's do.
                fill(x, y, pitch - 2, 3, c < lit ? colour : mix(back, colour, .09f));
            }
            const int top = int(std::lround(cap[size_t(i)] * cells));
            if (top > 0) fill(x, base - top * 4 - 2, pitch - 2, 2, bright);
            // The floor reflects the ladder, fading out.
            for (int c = 0; c < std::min(lit, 8); c++)
                fill(x, base + 2 + c * 4, pitch - 2, 3,
                     mix(back, ramp[size_t(96 + c * 159 / (cells - 1))], .28f * (1 - float(c) / 8)));
        }
    }

    void draw_scope(const VizInput& in, float dt) {
        // The phosphor keeps what the beam drew and lets it go over about a tenth of a second.
        const float keep = std::exp(-dt * 11);
        for (float& g : glow) g *= keep;
        const float gain = follow_gain(in, dt);
        // Start where the wave climbs through zero, so a steady note stands still.
        int start = 0;
        for (int i = 1; i < music::viz_wave - width; i++) {
            const float a = in.wave[0][size_t(i) - 1] + in.wave[1][size_t(i) - 1],
                        b = in.wave[0][size_t(i)] + in.wave[1][size_t(i)];
            if (a < 0 && b >= 0) {
                start = i;
                break;
            }
        }
        const float middle = height / 2.f, reach = height * .44f;
        float before = 0;
        for (int x = 0; x < width; x++) {
            const size_t at = size_t(std::min(music::viz_wave - 1, start + x));
            const float y = middle - std::clamp((in.wave[0][at] + in.wave[1][at]) * .5f * gain, -1.f, 1.f) * reach;
            if (x) burn(float(x - 1), before, float(x), y, 1 + in.beat * .4f);
            before = y;
        }
        show_glow([&](int x, int y) { return x % 37 == 0 || y % 35 == 0 || (y == height / 2 && x % 3 == 0); });
    }

    void draw_vector(const VizInput& in, float dt) {
        // The studio's goniometer: left against right, turned 45 degrees, so mono stands up as a line and wide
        // stereo spreads out sideways.
        const float keep = std::exp(-dt * 9);
        for (float& g : glow) g *= keep;
        const float gain = follow_gain(in, dt), cx = width / 2.f, cy = height / 2.f, reach = height * .46f;
        float px = cx, py = cy;
        for (int i = 0; i < music::viz_wave; i++) {
            const float l = in.wave[0][size_t(i)] * gain, r = in.wave[1][size_t(i)] * gain;
            const float x = cx + std::clamp((l - r) * .7071f, -1.f, 1.f) * reach * 1.3f,
                        y = cy - std::clamp((l + r) * .7071f, -1.f, 1.f) * reach;
            if (i) burn(px, py, x, y, .26f);
            px = x;
            py = y;
        }
        const int radius = int(reach);
        show_glow([&](int x, int y) {
            const int dx = x - int(cx), dy = y - int(cy);
            if ((dx == 0 || dy == 0) && (x + y) % 3 == 0) return true;             // the axes, dotted
            if (dx == dy || dx == -dy) return std::abs(dx) < radius && x % 2 == 0; // L and R
            const int ring = dx * dx * 10 / 17 + dy * dy; // an ellipse as wide as the trace may go
            return std::abs(ring - radius * radius) < radius && (x % 2 == 0);
        });
    }

    void draw_radial(const VizInput& in, float dt) {
        // The spectrum bent round into a ring that breathes with the kick, low notes at the top, mirrored either side.
        fade(dt, 16);
        flash = std::max(in.beat, flash - dt * 4);
        clock += dt * (.15f + in.bass * .6f);
        const float cx = width / 2.f, cy = height / 2.f, scale = float(height) / 136;
        const float inner = (26 + flash * 5 + in.bass * 4) * scale, reach = 40 * scale;
        const int spokes = 96;
        for (int i = 0; i < spokes; i++) {
            const float half = float(i) / float(spokes), place = half < .5f ? half * 2 : (1 - half) * 2;
            const float level = band_at(in, place), a = half * 6.2831853f + clock * .4f;
            const float s = std::sin(a), c = std::cos(a), length = inner + level * reach;
            const uint32_t colour = ramp[size_t(std::clamp(90 + int(level * 165), 0, 255))];
            // Wider than tall: the window is.
            beam(cx + s * inner * 1.5f, cy - c * inner, cx + s * length * 1.5f, cy - c * length, colour,
                 .55f + level * .45f, 1.4f);
        }
        // The wave itself, round the inside of the ring.
        const float gain = follow_gain(in, dt);
        float bx = 0, by = 0;
        for (int i = 0; i <= 64; i++) {
            const size_t at = size_t((i % 64) * (music::viz_wave / 64));
            const float v = std::clamp((in.wave[0][at] + in.wave[1][at]) * .5f * gain, -1.f, 1.f);
            const float a = float(i) / 64 * 6.2831853f, r = inner * (.62f + v * .28f);
            const float x = cx + std::sin(a) * r * 1.5f, y = cy - std::cos(a) * r;
            if (i) beam(bx, by, x, y, mix(ramp[200], bright, flash * .6f), .7f);
            bx = x;
            by = y;
        }
    }

    void draw_waterfall(const VizInput& in, float dt) {
        // Ridges of past spectra marching away into the distance; the nearest is now. Each hides what is behind it.
        ridge_clock += dt;
        if (ridge_clock >= 1 / 24.f) {
            ridge_clock = std::fmod(ridge_clock, 1 / 24.f);
            for (int k = ridges - 1; k > 0; k--) ridge[size_t(k)] = ridge[size_t(k) - 1];
            std::copy(in.bands.begin(), in.bands.end(), ridge[0].begin());
        }
        clear();
        std::vector<int> horizon(size_t(width), height); // the highest pixel drawn so far in each column
        for (int k = 0; k < ridges; k++) {
            const float depth = float(k) / float(ridges - 1), near = 1 - depth;
            const float span = float(width) * (.96f - depth * .46f), left = (float(width) - span) / 2;
            const float floor = float(height) - 6 - depth * float(height) * .52f,
                        lift = float(height) * (.16f + near * .3f);
            const uint32_t colour = mix(back, ramp[size_t(110 + int(near * 145))], .25f + near * .75f);
            const auto& row = ridge[size_t(k)];
            int before_x = -1, before_y = 0;
            for (int x = int(left); x <= int(left + span); x++) {
                const float place = (float(x) - left) / span;
                // Softer at the two ends, so each ridge starts and ends on its floor.
                const float taper = std::sin(std::clamp(place, 0.f, 1.f) * 3.14159265f);
                const float p = std::clamp(place, 0.f, 1.f) * float(music::viz_bands - 1);
                const size_t lo = std::min<size_t>(size_t(p), music::viz_bands - 2);
                const float level = row[lo] + (row[lo + 1] - row[lo]) * (p - float(lo));
                const int y = int(std::lround(floor - std::pow(level, 1.3f) * lift * (.35f + .65f * taper)));
                if (x >= 0 && x < width) {
                    // Behind a nearer ridge: hidden. Otherwise the line, joined up with its neighbour.
                    const int from = before_x < 0 ? y : std::min(before_y, y),
                              to = before_x < 0 ? y : std::max(before_y, y);
                    for (int py = std::max(0, from); py <= std::min(height - 1, to); py++)
                        if (py < horizon[size_t(x)]) pixels[size_t(py) * width + size_t(x)] = colour;
                    horizon[size_t(x)] = std::min(horizon[size_t(x)], std::max(0, y));
                }
                before_x = x;
                before_y = y;
            }
        }
    }

    void draw_tunnel(const VizInput& in, float dt) {
        // Down the tunnel: rings come out of the dark, shaped by the spectrum when they were born; a kick sends
        // a bright one and speeds the ride.
        fade(dt, 30);
        flash = std::max(in.beat, flash - dt * 4);
        const float speed = .35f + (in.rms[0] + in.rms[1]) * 1.6f + flash * .8f;
        ring_spin += dt * (.2f + in.bass * .9f);
        ring_clock -= dt;
        const bool kick = in.beat > .9f && ring_clock < .12f;
        if (ring_clock <= 0 || kick) {
            ring_clock = .24f;
            Ring* slot = &rings[0];
            for (Ring& r : rings)
                if (r.z <= 0) {
                    slot = &r;
                    break;
                }
            slot->z = 1;
            slot->energy = std::min(1.f, .3f + (in.rms[0] + in.rms[1]) * 1.4f + (kick ? .6f : 0));
            slot->turn = ring_spin;
            for (size_t i = 0; i < slot->shape.size(); i++) slot->shape[i] = band_at(in, float(i) / 15);
        }
        const float cx = width / 2.f + std::sin(ring_spin * .7f) * width * .06f,
                    cy = height / 2.f + std::cos(ring_spin * .5f) * height * .06f;
        for (Ring& r : rings) {
            if (r.z <= 0) continue;
            r.z -= dt * speed * r.z * 1.4f + dt * .02f;
            if (r.z <= .04f) {
                r.z = 0;
                continue;
            }
            const float radius = float(height) * .09f / r.z, near = std::clamp(1.1f - r.z, 0.f, 1.f);
            if (radius > float(width)) {
                r.z = 0;
                continue;
            }
            const uint32_t colour = mix(ramp[size_t(70 + int(r.energy * 130))], bright, near * near * r.energy * .3f);
            float bx = 0, by = 0;
            for (int i = 0; i <= 48; i++) {
                const float a = float(i) / 48 * 6.2831853f + r.turn;
                const float spike = r.shape[size_t((i % 48) / 3)] * .35f;
                const float x = cx + std::sin(a) * radius * (1 + spike) * 1.4f,
                            y = cy - std::cos(a) * radius * (1 + spike);
                if (i) beam(bx, by, x, y, colour, (.25f + near * .6f) * (.5f + r.energy * .5f), near > .7f ? 1.6f : 1);
                bx = x;
                by = y;
            }
        }
    }

    void draw_fire(const VizInput& in, float dt) {
        // The oldest trick on the demo disk: every cell takes the average of the cells under it and cools a little.
        // The music is the fuel along the bottom edge.
        const int steps = std::clamp(int(std::lround(dt * 60)), 1, 3);
        for (int step = 0; step < steps; step++) {
            for (int x = 0; x < width; x++) {
                // Between two bands the fuel is blended, and it comes in sparks: that is what makes tongues.
                const float place = float(x) * float(music::viz_bands - 1) / float(width - 1);
                const size_t low = size_t(place), high = std::min<size_t>(music::viz_bands - 1, low + 1);
                const float band = in.bands[low] + (in.bands[high] - in.bands[low]) * (place - float(low));
                const float fuel = std::min(1.f, std::pow(band, 1.5f) * 1.35f + in.beat * .22f);
                glow[size_t(height - 1) * width + size_t(x)] = random() < .7f ? fuel : fuel * .2f;
                glow[size_t(height - 2) * width + size_t(x)] = random() < .7f ? fuel : fuel * .2f;
            }
            for (int y = 0; y < height - 2; y++) {
                const float* below = &glow[size_t(y + 1) * width];
                const float* under = &glow[size_t(std::min(height - 1, y + 2)) * width];
                float* row = &glow[size_t(y) * width];
                for (int x = 0; x < width; x++) {
                    const float sum =
                        below[std::max(0, x - 1)] + below[x] + below[std::min(width - 1, x + 1)] + under[x];
                    row[x] = std::max(0.f, sum / 4.075f - random() * .004f);
                }
            }
        }
        for (size_t i = 0; i < pixels.size(); i++) {
            const float heat = glow[i];
            pixels[i] = heat <= 0 ? back : ramp[size_t(std::min(255.f, std::pow(heat, .6f) * 300))];
        }
    }

    void draw_plasma(const VizInput& in, float dt) {
        // Four sine fields added up and run through the colour ramp; the bass leans on the clock.
        clock += dt * (.5f + in.bass * 2.2f);
        drift += dt * .13f + in.beat * dt * .9f;
        const float loud = .35f + .65f * std::min(1.f, (in.rms[0] + in.rms[1]) * 2.4f);
        static std::array<float, 1024> sine{};
        if (sine[256] == 0)
            for (size_t i = 0; i < sine.size(); i++) sine[i] = std::sin(float(i) * 6.2831853f / float(sine.size()));
        const auto wave = [&](float turns) { return sine[size_t(int(turns * 1024) & 1023)]; };
        const float cx = width / 2.f + wave(clock * .11f) * 70, cy = height / 2.f + wave(clock * .13f + .25f) * 40;
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                const float distance = std::hypot(float(x) - cx, float(y) - cy);
                const float value = wave(float(x) * .0058f + clock * .21f) + wave(float(y) * .0093f - clock * .17f) +
                                    wave(float(x + y) * .0041f + clock * .12f) + wave(distance * .0125f - clock * .31f);
                // Up the ramp and back down again, so the colours fold over without a seam.
                const float turn = (value + 4) / 8 * 2.4f + drift, fold = std::fabs(2 * (turn - std::floor(turn)) - 1);
                const uint32_t colour = mix(back, ramp[size_t(24 + int(fold * fold * (3 - 2 * fold) * 200))], loud);
                // Blocks of four: plasma was never sharp.
                const size_t at = size_t(y) * width + size_t(x);
                pixels[at] = colour;
                if (x + 1 < width) pixels[at + 1] = colour;
                if (y + 1 < height) {
                    pixels[at + width] = colour;
                    if (x + 1 < width) pixels[at + width + 1] = colour;
                }
            }
    }

    void draw_stars(const VizInput& in, float dt) {
        // Out through the stars: the louder, the faster; a kick lights the whole sky for an instant.
        for (uint32_t& p : pixels) p = mix(p, back, std::min(1.f, dt * 14));
        const float speed = .12f + (in.rms[0] + in.rms[1]) * 1.5f + in.bass * .5f;
        flash = std::max(in.beat * .9f, flash - dt * 4);
        for (Star& s : stars) {
            if (s.z <= 0) {
                s.x = random() * 2 - 1;
                s.y = random() * 2 - 1;
                s.z = .25f + random() * .75f;
            }
            const float before = s.z;
            s.z -= dt * speed * (.5f + s.z);
            const float spread_x = float(width) * .5f, spread_y = float(height) * .54f;
            const float x0 = width / 2.f + s.x / before * spread_x, y0 = height / 2.f + s.y / before * spread_y;
            if (s.z <= .02f || x0 < -20 || x0 > width + 20 || y0 < -20 || y0 > height + 20) {
                s.z = 0;
                continue;
            }
            const float x1 = width / 2.f + s.x / s.z * spread_x, y1 = height / 2.f + s.y / s.z * spread_y;
            const float near = std::clamp(1.2f - s.z, .15f, 1.f);
            beam(x0, y0, x1, y1, mix(ramp[200], bright, near * .6f + flash * .4f), near * (.75f + flash * .25f),
                 near > .75f ? 1.6f : 1);
        }
    }

    void draw_meters(const VizInput& in, float dt) {
        clear();
        // Two moving-coil meters. The needle is a spring with a little mass: it takes about 300 ms to arrive and
        // overshoots by a hair, as the standard asks of a real one.
        const uint32_t face = mix(back, ramp[150], .16f), ink = mix(ramp[200], bright, .55f), hot = rgb(232, 72, 60);
        for (int c = 0; c < 2; c++) {
            // 0 VU sits at 70% of the swing; full scale input reads +3.
            const float db = 20 * std::log10(std::max(1e-4f, in.rms[c] * 1.41f)) + 12;
            const float target = std::clamp((db + 20) / 23 * .78f + .04f, 0.f, 1.f);
            const float force = (target - needle[c]) * 190 - needle_speed[c] * 21;
            needle_speed[c] += force * dt;
            needle[c] = std::clamp(needle[c] + needle_speed[c] * dt, -.02f, 1.04f);
            clip[c] = in.peak[c] > .985f ? 1 : std::max(0.f, clip[c] - dt * 1.4f);
            const int w = 136, h = 120, left = width / 2 - w - 4 + c * (w + 8), top = (height - h) / 2;
            fill(left, top, w, h, face);
            fill(left, top, w, 1, dim);
            fill(left, top + h - 1, w, 1, dim);
            fill(left, top, 1, h, dim);
            fill(left + w - 1, top, 1, h, dim);
            const float px = float(left) + w / 2.f, py = float(top) + 112, radius = 78;
            const auto angle = [](float t) { return (-.74f + t * 1.48f); }; // radians either side of straight up
            // The scale: a black arc to 0 VU, red beyond, ticks at the marked levels.
            for (float t = 0; t <= 1; t += .004f) {
                const float a = angle(t);
                const int x = int(std::lround(px + std::sin(a) * radius)),
                          y = int(std::lround(py - std::cos(a) * radius));
                put(x, y, t > .74f ? hot : ink);
                if (t > .74f) put(x, y + 1, hot), put(x, y + 2, hot);
            }
            static constexpr std::array<std::pair<float, const char*>, 6> marks = {
                {{-20, "20"}, {-10, "10"}, {-5, "5"}, {-3, "3"}, {0, "0"}, {3, "+3"}}};
            for (const auto& [level, name] : marks) {
                const float t = (level + 20) / 23 * .78f + .04f, a = angle(t);
                beam(px + std::sin(a) * radius, py - std::cos(a) * radius, px + std::sin(a) * (radius + 7),
                     py - std::cos(a) * (radius + 7), level >= 0 ? hot : ink, 1);
                const int tx = int(std::lround(px + std::sin(a) * (radius + 15))) - int(std::string(name).size()) * 3,
                          ty = int(std::lround(py - std::cos(a) * (radius + 15))) - 3;
                words(name, tx, ty, level >= 0 ? hot : ink);
            }
            words("VU", left + w / 2 - 6, top + 66, ink);
            words(c ? "R" : "L", left + 8, top + h - 16, ink);
            // The overload lamp.
            fill(left + w - 16, top + h - 16, 8, 8, mix(mix(back, hot, .2f), hot, clip[c]));
            const float a = angle(needle[c]);
            beam(px + std::sin(a) * 8, py - std::cos(a) * 8, px + std::sin(a) * (radius + 4),
                 py - std::cos(a) * (radius + 4), bright, 1, 1.5f);
            // The pivot's cover.
            fill(left + w / 2 - 22, top + h - 14, 44, 13, mix(back, ramp[140], .5f));
            fill(left + w / 2 - 22, top + h - 14, 44, 1, dim);
        }
    }

    // ---- the colourful ones: real hues, not only the theme's accent
    static float fract(float v) { return v - std::floor(v); }
    static float smooth(float a, float b, float v) {
        const float t = std::clamp((v - a) / (b - a), 0.f, 1.f);
        return t * t * (3 - 2 * t);
    }
    // Hue 0 to 1 round the wheel, saturation and value 0 to 1.
    static uint32_t hue(float h, float s = 1, float v = 1) {
        h = fract(h);
        const float k = h * 6, f = k - std::floor(k), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        float r, g, b;
        switch (int(k) % 6) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
        }
        return rgb(r * 255, g * 255, b * 255);
    }
    float loudness(const VizInput& in) const { return std::min(1.f, (in.rms[0] + in.rms[1]) * 2.2f); }

    void draw_prism(const VizInput& in, float dt) {
        // A mirrored rainbow of the spectrum, low notes in the middle; each column is a gradient that glows
        // brightest at its heart, and a column that drops leaves a soft trail.
        fade(dt, 7);
        hue_clock += dt * (.03f + in.bass * .12f);
        flash = std::max(in.beat, flash - dt * 4);
        const float mid = height / 2.f, reach = height * .46f;
        for (int x = 0; x < width; x++) {
            const float place = std::fabs(2.f * float(x) / float(width - 1) - 1);
            const float level = std::pow(band_at(in, place), .85f), half = 3 + level * reach;
            const float h0 = hue_clock + place * .75f;
            for (int y = int(mid - half); y <= int(mid + half); y++) {
                const float d = std::fabs(float(y) - mid) / half;
                const float glowing = (1 - d * d) * (.55f + level * .45f) + flash * .12f;
                uint32_t colour = hue(h0 + d * .09f, .78f + d * .22f, .35f + .65f * glowing);
                colour = add(colour, bright, std::pow(std::max(0.f, 1 - d * 5), 2.f) * .55f);
                put(x, y, mix(pixels[size_t(std::clamp(y, 0, height - 1)) * width + size_t(x)], colour, .85f));
            }
        }
    }

    void draw_lissajous(const VizInput& in, float dt) {
        // A Lissajous knot drawn in a rainbow; each kick turns it into another figure, and the wave itself
        // puffs the line in and out.
        fade(dt, 5);
        hue_clock += dt * .09f;
        figure_turn += dt * (.35f + in.bass * 1.4f);
        flash = std::max(in.beat, flash - dt * 3);
        static constexpr std::array<std::pair<int, int>, 7> figures = {
            {{3, 2}, {5, 4}, {3, 4}, {5, 6}, {7, 4}, {1, 2}, {7, 6}}};
        if (in.beat > .9f && flash > .85f && ring_clock <= 0) {
            figure = (figure + 1) % int(figures.size());
            ring_clock = .6f;
        }
        ring_clock -= dt;
        const auto [a, b] = figures[size_t(figure)];
        const float gain = follow_gain(in, dt), cx = width / 2.f, cy = height / 2.f;
        const float rx = width * .38f * (1 + in.bass * .06f), ry = height * .43f * (1 + in.bass * .06f);
        const int n = 420;
        float px = 0, py = 0;
        for (int i = 0; i <= n; i++) {
            const float t = float(i) / n * 6.2831853f;
            const size_t at = size_t(std::min(music::viz_wave - 1, i * music::viz_wave / n));
            const float puff = 1 + std::clamp((in.wave[0][at] + in.wave[1][at]) * .5f * gain, -1.f, 1.f) * .22f;
            const float x = cx + std::sin(a * t + figure_turn) * rx * puff, y = cy + std::sin(b * t) * ry * puff;
            if (i) beam(px, py, x, y, hue(hue_clock + float(i) / n * .9f), .5f + flash * .2f, 1.5f);
            px = x;
            py = y;
        }
    }

    void draw_matrix(const VizInput& in, float dt) {
        // Falling code. Each column is a stretch of the spectrum: it falls faster the louder that part is, and the
        // colours slide from green through the cold end of the wheel as the pitch goes up.
        const int cell_w = 6, cell_h = 8, columns = std::min(int(rain_y.size()), width / cell_w);
        if (!rain_ready) {
            for (int i = 0; i < columns; i++) {
                rain_y[size_t(i)] = random() * float(height);
                rain_speed[size_t(i)] = .5f + random() * .8f;
            }
            rain_ready = true;
        }
        fade(dt, 3.4f);
        flash = std::max(in.beat, flash - dt * 5);
        clock += dt;
        const int offset = (width - columns * cell_w) / 2;
        for (int i = 0; i < columns; i++) {
            const float place = float(i) / float(columns - 1), level = band_at(in, place);
            float& y = rain_y[size_t(i)];
            const float before = y;
            y += dt * float(cell_h) * (4 + level * 22 + flash * 6) * rain_speed[size_t(i)];
            const int row0 = int(before / cell_h), row1 = int(y / cell_h);
            for (int row = row0 + 1; row <= row1; row++) {
                const int gx = offset + i * cell_w, gy = row * cell_h;
                uint32_t h = uint32_t(i * 7919 + row * 104729) ^ uint32_t(clock * 5) * 2654435761u;
                h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
                const uint32_t colour = hue(.36f + place * .42f + hue_clock, .85f, .75f + level * .25f);
                for (int bit = 0; bit < 15; bit++)
                    if (h >> bit & 1) fill(gx + 1 + (bit % 3) * 1, gy + 1 + (bit / 3), 1, 1, colour);
                // The head is white and wide.
                if (h & 1u << 15) fill(gx + 1, gy + 1, 3, 5, mix(colour, bright, .75f));
            }
            if (y > float(height + cell_h * 2) && random() < dt * 3 + level * .1f) {
                y = -float(cell_h) * random() * 6;
                rain_speed[size_t(i)] = .5f + random() * .8f;
            }
        }
        hue_clock += dt * .01f;
    }

    void draw_kaleido(const VizInput& in, float dt) {
        // Six mirrors, a moving field of sines inside them, hues rolling round the wheel. The bass zooms in.
        clock += dt * (.5f + in.bass * 1.6f);
        hue_clock += dt * .045f;
        flash = std::max(in.beat, flash - dt * 4);
        const float loud = .45f + .55f * loudness(in), cx = width / 2.f, cy = height / 2.f,
                    scale = 1.f / (height * .5f);
        const float segment = 6.2831853f / 6, spin = clock * .12f, zoom = 2.6f + in.bass * .9f + std::sin(clock * .2f);
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                const float dx = (float(x) - cx) * scale, dy = (float(y) - cy) * scale;
                const float r = std::hypot(dx, dy);
                float a = std::fmod(std::atan2(dy, dx) + 3.14159265f + spin + 12.566371f, segment);
                a = std::fabs(a - segment / 2);
                const float u = std::cos(a) * r * zoom, v = std::sin(a) * r * zoom;
                const float value = std::sin(u * 3.f + clock * 1.3f) + std::sin(v * 4.f - clock) +
                                    std::sin((u + v) * 2.5f + clock * .7f) + std::sin(r * 5.f - clock * 1.8f);
                const float h = hue_clock + value * .09f + r * .22f;
                const float light_amount = (.4f + .6f * smooth(-2.4f, 2.4f, value)) * loud + flash * .15f;
                const uint32_t colour = hue(h, .85f - flash * .15f, std::min(1.f, light_amount));
                const size_t at = size_t(y) * width + size_t(x);
                pixels[at] = colour;
                if (x + 1 < width) pixels[at + 1] = colour;
                if (y + 1 < height) {
                    pixels[at + width] = colour;
                    if (x + 1 < width) pixels[at + width + 1] = colour;
                }
            }
    }

    void draw_fireworks(const VizInput& in, float dt) {
        // Over a skyline: a kick sends a rocket up, and at the top it opens into a shower of one colour that
        // falls and fades. The louder the kick, the bigger the burst. Quiet passages still get an occasional one.
        fade(dt, 4.2f);
        flash = std::max(in.beat, flash - dt * 4);
        launch_wait -= dt;
        const auto spare = [&]() -> Spark* {
            for (Spark& s : sparks)
                if (s.life <= 0) return &s;
            return nullptr;
        };
        if ((in.beat > .9f && launch_wait < .1f) || (launch_wait < -1.6f && loudness(in) > .04f)) {
            if (Spark* s = spare()) {
                launch_wait = .32f;
                s->x = width * (.18f + random() * .64f);
                s->y = float(height) - 14;
                s->vx = (random() - .5f) * 16;
                s->vy = -std::sqrt(2 * 260.f * (height * (.42f + random() * .22f)));
                s->hue = random();
                s->life = 3;
                s->rocket = true;
            }
        }
        for (Spark& s : sparks) {
            if (s.life <= 0) continue;
            const float x0 = s.x, y0 = s.y;
            if (s.rocket) {
                s.vy += 260 * dt;
                s.x += s.vx * dt;
                s.y += s.vy * dt;
                beam(x0, y0, s.x, s.y, mix(hue(s.hue, .4f), bright, .5f), .9f, 1.5f);
                if (s.vy > -22 || s.y < 6) {
                    s.life = 0;
                    const int count = 46 + int(in.bass * 36);
                    const float power = 34 + random() * 22 + in.bass * 16, h0 = s.hue, bx = s.x, by = s.y;
                    for (int k = 0; k < count; k++) {
                        Spark* p = spare();
                        if (!p) break;
                        const float a = random() * 6.2831853f, sp = power * std::sqrt(random());
                        p->x = bx, p->y = by;
                        p->vx = std::cos(a) * sp * 1.3f, p->vy = std::sin(a) * sp;
                        p->life = .9f + random() * 1.1f;
                        p->hue = h0 + (random() - .5f) * .12f + (k % 5 == 0 ? .5f : 0);
                        p->rocket = false;
                    }
                    for (int oy = -5; oy <= 5; oy++)
                        for (int ox = -8; ox <= 8; ox++)
                            light(int(bx) + ox, int(by) + oy, hue(h0, .3f),
                                  .5f * std::max(0.f, 1 - std::hypot(ox / 1.6f, float(oy)) / 5));
                }
                continue;
            }
            s.vy += 70 * dt;
            s.vx *= std::exp(-dt * 1.4f);
            s.vy *= std::exp(-dt * 1.1f);
            s.x += s.vx * dt;
            s.y += s.vy * dt;
            s.life -= dt;
            const float f = std::clamp(s.life * 1.3f, 0.f, 1.f);
            beam(x0, y0, s.x, s.y, hue(s.hue + (1 - f) * .06f, .9f - (1 - f) * .3f), f * .9f, f > .5f ? 1.5f : 1);
        }
        // The skyline: dark buildings with lit windows, tinted by the last burst.
        const uint32_t wall = mix(back, rgb(26, 24, 44), 1.f), tintc = hue(hue_clock, .5f, .9f);
        hue_clock += dt * .02f;
        for (int x = 0; x < width; x++) {
            const int h = 9 + int(5 + 4 * std::sin(float(x) * .13f) + 3 * std::sin(float(x) * .41f + 1.f) +
                                  2.5f * std::sin(float(x) * .07f + 3.f));
            for (int y = height - h; y < height; y++) {
                uint32_t colour = wall;
                if ((x / 3 + y / 4) % 2 == 0 && (x % 3) == 1 && (y % 4) < 2 && ((x * 31 + y * 17) % 11) < 5)
                    colour = mix(rgb(255, 200, 90), tintc, .25f);
                put(x, y, colour);
            }
        }
    }

    void draw_lava(const VizInput& in, float dt) {
        // A lava lamp: soft blobs that drift, merge and part again; each swells with its own part of the
        // spectrum and the whole thing shifts through the wheel.
        clock += dt * (.35f + in.bass * .9f);
        hue_clock += dt * .02f;
        flash = std::max(in.beat, flash - dt * 4);
        constexpr int blobs = 7;
        float bx[blobs], by[blobs], br[blobs];
        for (int i = 0; i < blobs; i++) {
            const float f = float(i) + 1;
            bx[i] = width / 2.f + std::sin(clock * (.21f + f * .047f) + f * 1.7f) * width * .36f;
            by[i] = height / 2.f + std::cos(clock * (.17f + f * .039f) + f * 2.3f) * height * .34f;
            const float level = band_at(in, float(i) / (blobs - 1));
            br[i] = height * (.085f + level * .13f + in.bass * .03f + flash * .015f);
            br[i] *= br[i];
        }
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                float field = 0;
                for (int i = 0; i < blobs; i++) {
                    const float dx = (float(x) - bx[i]) * .8f, dy = float(y) - by[i];
                    field += br[i] / (dx * dx + dy * dy + 4);
                }
                const float h = hue_clock + std::min(field, 1.6f) * .16f + float(y) / height * .18f;
                const uint32_t glowing = mix(back, hue(h + .5f, .7f, .55f), std::min(1.f, field) * .35f);
                uint32_t colour = mix(glowing, hue(h, .9f, .95f), smooth(.95f, 1.3f, field));
                colour = add(colour, bright, smooth(2.2f, 4.f, field) * .18f);
                const size_t at = size_t(y) * width + size_t(x);
                pixels[at] = colour;
                if (x + 1 < width) pixels[at + 1] = colour;
                if (y + 1 < height) {
                    pixels[at + width] = colour;
                    if (x + 1 < width) pixels[at + width + 1] = colour;
                }
            }
    }

    void draw_aurora(const VizInput& in, float dt) {
        // Three curtains of northern lights over a field of stars; the spectrum lifts them and the colours drift
        // from green through teal to violet.
        clear();
        clock += dt * (.4f + in.bass * .7f);
        hue_clock += dt * .01f;
        for (int y = 0; y < height * 7 / 10; y++)
            for (int x = (y * 7) % 13; x < width; x += 13) {
                const uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u;
                if (h % 11 == 0)
                    put(x, y, mix(back, bright, .25f + .35f * (.5f + .5f * std::sin(clock * 2 + float(h % 97)))));
            }
        for (int k = 0; k < 3; k++) {
            for (int x = 0; x < width; x++) {
                const float place = float(x) / float(width - 1), level = band_at(in, place * .85f + .05f);
                const float top =
                    height * (.2f + .13f * float(k)) +
                    std::sin(float(x) * .021f + clock * (.5f + .2f * float(k)) + float(k) * 2) * height * .09f +
                    std::sin(float(x) * .047f - clock * .9f) * height * .03f - level * height * .2f;
                const float length = height * .4f + level * height * .25f;
                const float rays = .72f + .28f * std::sin(float(x) * .33f + clock * 2.2f + float(k));
                for (int y = std::max(0, int(top)); y < std::min(height, int(top + length)); y++) {
                    const float d = (float(y) - top) / length;
                    const float a = std::pow(1 - d, 1.7f) * smooth(0, .06f, d) * rays * (.35f + level * .6f);
                    light(x, y, hue(.36f + d * .42f + float(k) * .07f + hue_clock, .75f, 1), a * .7f);
                }
            }
        }
        // A dark ridge along the bottom for the land.
        for (int x = 0; x < width; x++) {
            const int h = 6 + int(3 * std::sin(float(x) * .05f) + 2 * std::sin(float(x) * .19f + 1));
            fill(x, height - h, 1, h, rgb(3, 4, 8));
        }
    }

    void draw_copper(const VizInput& in, float dt) {
        // Raster bars, the Amiga's favourite: full-width gradients that swing on sine paths and pass in front of
        // one another. A bar swells with its part of the spectrum.
        clock += dt * (.8f + in.bass * 1.2f);
        hue_clock += dt * .03f;
        flash = std::max(in.beat, flash - dt * 5);
        for (int y = 0; y < height; y++) {
            // Scan lines in the dark.
            const float stripe = y % 2 ? .5f : 1.f;
            const uint32_t colour = mix(back, hue(hue_clock + float(y) / height * .2f, .6f, .35f), .22f * stripe);
            fill(0, y, width, 1, colour);
        }
        constexpr int count = 9;
        std::array<int, count> order{};
        for (int i = 0; i < count; i++) order[size_t(i)] = i;
        const auto phase = [&](int i) { return clock * 1.15f + float(i) * 1.12f; };
        std::sort(order.begin(), order.end(), [&](int a, int b) { return std::cos(phase(a)) < std::cos(phase(b)); });
        for (int i : order) {
            const float level = band_at(in, float(i) / (count - 1)),
                        centre = height / 2.f + std::sin(phase(i)) * height * .34f * (.65f + level * .5f);
            const int half = 7 + int(level * 7 + flash * 3);
            const float h = hue_clock + float(i) / count * 1.1f + .02f * std::sin(clock + float(i));
            for (int dy = -half; dy <= half; dy++) {
                const float t = std::fabs(float(dy)) / float(half), shade = std::cos(t * 1.5708f);
                uint32_t colour = hue(h, 1 - shade * shade * .35f, .25f + .75f * shade);
                colour = add(colour, bright, std::pow(shade, 8.f) * .5f);
                fill(0, int(centre) + dy, width, 1, colour);
            }
        }
    }

    void draw_synthwave(const VizInput& in, float dt) {
        // The eighties, all of it: a striped sun that pulses with the bass, mountains made of the spectrum, and a
        // neon grid rolling towards you. A kick flashes the grid.
        clock += dt * (.4f + loudness(in) * 1.6f);
        hue_clock += dt * .004f;
        flash = std::max(in.beat, flash - dt * 4);
        const float horizon = height * .56f, cx = width / 2.f,
                    sun_r = height * .3f * (1 + in.bass * .1f + flash * .04f), sun_y = horizon - sun_r * .38f;
        std::vector<int> ridge_top(static_cast<size_t>(width), 0);
        for (int x = 0; x < width; x++) {
            const float place = std::fabs(2.f * float(x) / float(width - 1) - 1);
            const float m = height * .07f + band_at(in, place) * height * .2f * smooth(.04f, .5f, place) +
                            std::fabs(std::sin(float(x) * .05f + 1)) * height * .05f;
            ridge_top[size_t(x)] = int(horizon - m);
        }
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                uint32_t colour;
                if (float(y) < horizon) {
                    const float t = float(y) / horizon;
                    colour = mix(rgb(10, 3, 38), rgb(255, 54, 152), t * t * (.85f + flash * .1f));
                    const uint32_t star = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u;
                    if (t < .55f && star % 233 == 0) colour = mix(colour, bright, .6f);
                    const float d = std::hypot(float(x) - cx, float(y) - sun_y);
                    if (d < sun_r) {
                        const float sy = (float(y) - (sun_y - sun_r)) / (2 * sun_r);
                        const bool gap = sy > .42f && fract((float(y) + clock * 3) / 7.f) < (sy - .38f) * 1.25f;
                        if (!gap) colour = mix(rgb(255, 232, 76), rgb(255, 40, 140), smooth(.1f, .9f, sy));
                    } else {
                        colour = add(colour, rgb(255, 80, 160), std::exp(-(d - sun_r) / (sun_r * .45f)) * .32f);
                    }
                    if (y >= ridge_top[size_t(x)]) {
                        const bool edge = y < ridge_top[size_t(x)] + 2;
                        colour = edge ? hue(.52f + hue_clock, .6f, 1) : rgb(14, 4, 34);
                    }
                } else {
                    const float row = float(y) - horizon + 1.f, t = row / (height - horizon);
                    colour = mix(rgb(22, 4, 44), rgb(8, 2, 22), t);
                    const float z = 70.f / (row + .6f), u = (float(x) - cx) / (row + .6f) * .5f;
                    const float line_z = fract(z - clock * .9f), line_u = std::fabs(fract(u) - .5f);
                    const float thickness = .07f + t * .06f;
                    const float cover =
                        std::max(line_z < thickness ? 1.f : 0.f, line_u > .5f - thickness * 1.5f ? 1.f : 0.f);
                    if (cover > 0)
                        colour = mix(colour, mix(hue(.88f + hue_clock, .85f), hue(.5f + hue_clock, .8f), t),
                                     (.55f + t * .45f + flash * .25f));
                    if (y == int(horizon)) colour = hue(.9f, .5f);
                }
                pixels[size_t(y) * width + size_t(x)] = colour;
            }
    }

    void draw_ripples(const VizInput& in, float dt) {
        // Drops land on a dark pond: every kick drops one, in a colour of its own, and the rings spread, cross and
        // add up where they meet.
        hue_clock += dt * .11f;
        drop_wait -= dt;
        if ((in.beat > .9f && drop_wait < .08f) || (drop_wait < -1.2f && loudness(in) > .03f)) {
            drop_wait = .22f;
            Drop* slot = &drops[0];
            for (Drop& d : drops)
                if (d.life < slot->life) slot = &d;
            slot->x = width * (.1f + random() * .8f);
            slot->y = height * (.15f + random() * .7f);
            slot->r = 0;
            slot->life = .6f + std::min(.4f, in.bass * .5f);
            slot->hue = hue_clock + random() * .3f;
        }
        for (Drop& d : drops)
            if (d.life > 0) d.r += dt * (34 + d.life * 50), d.life -= dt * .38f;
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                float r = 0, g = 0, b = 0;
                for (const Drop& d : drops) {
                    if (d.life <= 0) continue;
                    const float distance = std::hypot((float(x) - d.x) * .85f, float(y) - d.y);
                    const float width_ring = 3.5f + d.r * .05f;
                    const float v = (std::exp(-std::pow((distance - d.r) / width_ring, 2.f)) +
                                     .45f * std::exp(-std::pow((distance - d.r * .72f) / (width_ring * 1.2f), 2.f))) *
                                    d.life;
                    if (v < .01f) continue;
                    const uint32_t c = hue(d.hue, .8f, 1);
                    r += float(c >> 16 & 255) * v, g += float(c >> 8 & 255) * v, b += float(c & 255) * v;
                }
                const float sheen = .5f + .5f * std::sin(float(x) * .02f + float(y) * .03f + clock);
                const uint32_t base = mix(back, hue(hue_clock + .5f, .6f, .4f), .1f + sheen * .06f);
                const uint32_t colour = add(base, rgb(r, g, b), 1.f);
                const size_t at = size_t(y) * width + size_t(x);
                pixels[at] = colour;
                if (x + 1 < width) pixels[at + 1] = colour;
                if (y + 1 < height) {
                    pixels[at + width] = colour;
                    if (x + 1 < width) pixels[at + width + 1] = colour;
                }
            }
        clock += dt;
    }

    void draw_rotozoom(const VizInput& in, float dt) {
        // The first thing every demo scene did: a coloured checkerboard turned and zoomed across the screen. The
        // bass breathes the zoom and the kick makes it jump a little.
        clock += dt * (.5f + in.bass * 1.4f);
        hue_clock += dt * .05f;
        flash = std::max(in.beat, flash - dt * 5);
        const float angle = clock * .35f + std::sin(clock * .21f) * 1.2f, s = std::sin(angle), c = std::cos(angle);
        const float zoom = (.045f + .02f * std::sin(clock * .4f)) * (1 + in.bass * .35f + flash * .12f);
        const float cx = width / 2.f, cy = height / 2.f, loud = .55f + .45f * loudness(in);
        for (int y = 0; y < height; y += 2)
            for (int x = 0; x < width; x += 2) {
                const float dx = float(x) - cx, dy = (float(y) - cy) * 1.15f;
                const float u = (dx * c - dy * s) * zoom + clock * .6f, v = (dx * s + dy * c) * zoom + clock * .35f;
                const bool check = (int(std::floor(u)) + int(std::floor(v))) & 1;
                const float h = hue_clock + u * .03f + v * .02f + (check ? .5f : 0.f);
                const float d = std::hypot(dx, dy) / float(width);
                const uint32_t colour =
                    hue(h, check ? .8f : .6f, (check ? .95f : .5f) * loud * (1 - d * .5f) + flash * .1f);
                const size_t at = size_t(y) * width + size_t(x);
                pixels[at] = colour;
                if (x + 1 < width) pixels[at + 1] = colour;
                if (y + 1 < height) {
                    pixels[at + width] = colour;
                    if (x + 1 < width) pixels[at + width + 1] = colour;
                }
            }
    }

public:
    // The colours follow the theme's accent: near black, the accent, its lighter self, white.
    void tint(Color colour) {
        if (colour.r == accent.r && colour.g == accent.g && colour.b == accent.b && ramp[255]) return;
        accent = colour;
        const float r = colour.r, g = colour.g, b = colour.b;
        back = rgb(4 + r * .022f, 5 + g * .022f, 9 + b * .03f);
        grid = rgb(12 + r * .07f, 14 + g * .07f, 20 + b * .08f);
        dim = rgb(30 + r * .16f, 34 + g * .16f, 42 + b * .16f);
        for (int i = 0; i < 256; i++) {
            const float t = float(i) / 255;
            // 0 to .55: out of the dark into the accent; to .85: lighter; to 1: white hot.
            if (t < .55f) ramp[size_t(i)] = mix(back, rgb(r, g, b), std::pow(t / .55f, 1.25f));
            else if (t < .85f)
                ramp[size_t(i)] = mix(rgb(r, g, b), rgb(r + (255 - r) * .6f, g + (255 - g) * .6f, b + (255 - b) * .6f),
                                      (t - .55f) / .3f);
            else
                ramp[size_t(i)] =
                    mix(rgb(r + (255 - r) * .6f, g + (255 - g) * .6f, b + (255 - b) * .6f), bright, (t - .85f) / .15f);
        }
    }
    void resize(int w, int h) {
        if (w == width && h == height) return;
        width = w;
        height = h;
        pixels.assign(size_t(w) * size_t(h), back);
        glow.assign(size_t(w) * size_t(h), 0);
        last = VizMode::Count;
    }
    // Where bar `index` of the spectrum stands, and how wide: the equaliser page draws its curve over them.
    int bar_left(int index) const { return (width - 32 * 9 + 2) / 2 + index * 9; }
    int bar_floor() const { return height - 36; }
    const std::vector<uint32_t>& draw(VizMode mode, const VizInput& in, float dt) {
        dt = std::clamp(dt, .001f, .1f);
        if (!ramp[255]) tint(accent);
        if (mode != last) {
            // Each picture starts from a dark screen, not from the last one's leftovers.
            std::fill(glow.begin(), glow.end(), 0.f);
            clear();
            sparks = {};
            drops = {};
            rain_ready = false;
            launch_wait = drop_wait = 0;
            last = mode;
        }
        switch (mode) {
        case VizMode::Scope: draw_scope(in, dt); break;
        case VizMode::Fire: draw_fire(in, dt); break;
        case VizMode::Plasma: draw_plasma(in, dt); break;
        case VizMode::Stars: draw_stars(in, dt); break;
        case VizMode::Meters: draw_meters(in, dt); break;
        case VizMode::Vector: draw_vector(in, dt); break;
        case VizMode::Radial: draw_radial(in, dt); break;
        case VizMode::Waterfall: draw_waterfall(in, dt); break;
        case VizMode::Tunnel: draw_tunnel(in, dt); break;
        case VizMode::Prism: draw_prism(in, dt); break;
        case VizMode::Lissajous: draw_lissajous(in, dt); break;
        case VizMode::Matrix: draw_matrix(in, dt); break;
        case VizMode::Kaleido: draw_kaleido(in, dt); break;
        case VizMode::Fireworks: draw_fireworks(in, dt); break;
        case VizMode::Lava: draw_lava(in, dt); break;
        case VizMode::Aurora: draw_aurora(in, dt); break;
        case VizMode::Copper: draw_copper(in, dt); break;
        case VizMode::Synthwave: draw_synthwave(in, dt); break;
        case VizMode::Ripples: draw_ripples(in, dt); break;
        case VizMode::Rotozoom: draw_rotozoom(in, dt); break;
        default: draw_bars(in, dt); break;
        }
        return pixels;
    }
    uint32_t background() const { return back; }
};
} // namespace rgds
