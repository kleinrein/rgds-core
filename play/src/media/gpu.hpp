// SPDX-License-Identifier: GPL-3.0-or-later
// What the media screens are drawn with. The rule that keeps them fast: pictures that only move (a poster sliding,
// a page turning, a ripple growing, the focus gliding) are GPU textures drawn at a new place each frame; the CPU
// canvas (play/src/ui) is used only to make a texture, when what it shows changes.
#pragma once
#include "ui/canvas.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"

#include <SDL.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace media {

using play::Box;
using play::Canvas;
using play::Rgb;

// A texture and its size; owns it.
struct Tex {
    SDL_Texture* t = nullptr;
    int w = 0, h = 0;
    Tex() = default;
    Tex(const Tex&) = delete;
    Tex& operator=(const Tex&) = delete;
    Tex(Tex&& o) noexcept : t(o.t), w(o.w), h(o.h) { o.t = nullptr; }
    Tex& operator=(Tex&& o) noexcept {
        if (this != &o) {
            reset();
            t = o.t, w = o.w, h = o.h;
            o.t = nullptr;
        }
        return *this;
    }
    ~Tex() { reset(); }
    void reset() {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
        w = h = 0;
    }
    explicit operator bool() const { return t != nullptr; }
};

// Pixels (bytes R, G, B, A) into a texture, made or remade to fit. `linear`: smooth when scaled.
bool upload(SDL_Renderer* r, Tex& tex, const uint32_t* px, int w, int h, bool blend, bool linear);
inline bool upload(SDL_Renderer* r, Tex& tex, const Canvas& c, bool blend = false, bool linear = false) {
    return upload(r, tex, c.data(), c.w(), c.h(), blend, linear);
}
// Draws a texture into a rect (float, so slides are smooth), at an alpha.
void draw(SDL_Renderer* r, const Tex& tex, double x, double y, double w, double h, int alpha = 255);
inline void draw(SDL_Renderer* r, const Tex& tex, double x, double y, int alpha = 255) {
    draw(r, tex, x, y, tex.w, tex.h, alpha);
}
// The same, turned by `angle` degrees clockwise around the rect's centre.
void draw_turned(SDL_Renderer* r, const Tex& tex, double cx, double cy, double w, double h, double angle,
                 int alpha = 255);
// Part of a texture.
void draw_part(SDL_Renderer* r, const Tex& tex, const SDL_Rect& src, double x, double y, double w, double h,
               int alpha = 255);
void fill(SDL_Renderer* r, double x, double y, double w, double h, Rgb c, int alpha = 255);

// Shapes the canvas lacks, anti-aliased by coverage: a filled polygon (transport glyphs).
void polygon(Canvas& c, const std::vector<std::pair<double, double>>& points, Rgb ink, int alpha = 255);

enum class Glyph {
    Play,
    Pause,
    Back10,
    Forward10,
    Previous,
    Next,
    Subtitles,
    Audio,
    Speed,
    Picture,
    Moon,
    Book,
    Pages,
    Contents,
    Paper,
    TextSize,
    Direction,
    Film,
    Folder,
    Close,
    Lock
};
// A glyph centred on cx, cy, `size` px tall; `ground` is what shows through its cut-outs.
void glyph(Canvas& c, Glyph g, int cx, int cy, int size, Rgb ink, Rgb ground, int alpha = 255);

// Easing and springs for everything that moves.
double ease_out(double t); // cubic
double ease_in_out(double t);
struct Spring { // critically damped toward `target`; feels like the launcher's glide
    double value = 0, velocity = 0, target = 0;
    bool moving() const { return std::abs(value - target) > .01 || std::abs(velocity) > .001; }
    void snap(double v) { value = target = v, velocity = 0; }
    bool step(double dt_ms, double k = .0024, double zeta = .72) {
        if (!moving()) {
            value = target, velocity = 0;
            return false;
        }
        double remaining = std::min(dt_ms, 100.0);
        while (remaining > 0) {
            double dt = std::min(remaining, 8.0);
            remaining -= dt;
            velocity += (-k * (value - target) - 2 * zeta * std::sqrt(k) * velocity) * dt;
            value += velocity * dt;
        }
        return true;
    }
};

// The touch answer: a soft disc that grows from the finger and fades, drawn from one texture.
struct Ripples {
    struct One {
        double x, y, radius;
        uint32_t at;
        bool held = true;
        uint32_t released = 0;
    };
    std::vector<One> live;
    Tex disc;
    void start(double x, double y, double radius, uint32_t now) { live.push_back({x, y, radius, now, true, 0}); }
    void release(uint32_t now) {
        for (auto& o : live)
            if (o.held) o.held = false, o.released = now;
    }
    bool active() const { return !live.empty(); }
    // Draws and ages them; true while any is left.
    bool draw(SDL_Renderer* r, uint32_t now, Rgb tint, bool animations);
};

// The battery as the system reports it (-1: unknown), and whether it charges.
struct Power {
    int percent = -1;
    bool charging = false;
    uint32_t read_at = 0;
    void read(uint32_t now);
};

// The launcher's status bar (Home's top line): the date and clock in the middle-right, the battery at the right,
// on a strip of the panel colour. Drawn into a canvas of width x 36.
void status_bar(Canvas& c, const play::Look& look, const play::Font& font, const Power& power,
                const std::string& left_title);

// The launcher's focus corners: four accent brackets around a box.
void focus_corners(SDL_Renderer* r, double x, double y, double w, double h, Rgb accent, int alpha = 255);

// A canvas written as a PNG (screenshots and tests).
bool write_png(const std::string& path, const uint32_t* px, int w, int h);

} // namespace media
