// SPDX-License-Identifier: GPL-3.0-or-later
// A panel-sized picture drawn on the CPU (the companion screen), and the
// launcher's own rounded font so the player reads like the rest of RG DS.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace play {

// Pixels as in core.hpp: bytes R, G, B, X.
struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    uint32_t px() const { return r | (g << 8) | (b << 16) | 0xFF000000u; }
    static Rgb from_px(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)}; }
    static Rgb hex(uint32_t rgb) { return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)}; }
};
Rgb mix(Rgb a, Rgb b, int t256); // t256 = 0: a, 256: b

class Canvas {
public:
    Canvas(int w = 0, int h = 0) { resize(w, h); }
    void resize(int w, int h) {
        w_ = w;
        h_ = h;
        px_.assign(static_cast<size_t>(w) * h, 0xFF000000u);
    }
    int w() const { return w_; }
    int h() const { return h_; }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }
    uint32_t at(int x, int y) const { return px_[static_cast<size_t>(y) * w_ + x]; }

    void fill(Rgb c);
    void fill_rect(int x, int y, int w, int h, Rgb c, int alpha = 255);
    void rounded_rect(int x, int y, int w, int h, int radius, Rgb c, int alpha = 255);
    void vertical_gradient(Rgb top, Rgb bottom);
    // Nearest-neighbour blit at an integer scale; alpha blends over what is there.
    void blit(const uint32_t* src, int sw, int sh, int dx, int dy, int scale, int alpha = 255);
    // The same, leaving out source pixels whose low 24 bits equal `key`.
    void blit_keyed(const uint32_t* src, int sw, int sh, int dx, int dy, int scale, uint32_t key);
    // Fill the whole canvas with the picture, keeping its shape (cropping the
    // overflow), each pixel at level/256 brightness.
    void cover(const uint32_t* src, int sw, int sh, int stride, int level);
    // Multiply everything by level/256 (dimming).
    void dim(int level);

private:
    void blend(uint32_t& dst, Rgb c, int alpha);
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
};

// RGF1 atlases from assets/fonts (tools/bake_font.py), drawn in software.
class Font {
public:
    bool load(const std::string& directory);
    bool loaded() const { return !atlases_.empty(); }
    int measure(const std::string& text, int size) const;
    // Baseline at y; returns the advance.
    int draw(Canvas& c, int x, int y, const std::string& text, int size, Rgb color, int alpha = 255) const;
    int ascent(int size) const;
    // Shortens with an ellipsis to fit max_width.
    std::string fit(const std::string& text, int size, int max_width) const;

private:
    struct Glyph {
        int x, y, w, h, left, top, advance;
    };
    struct Atlas {
        int width = 0, height = 0, ascent = 0, size = 0;
        std::map<unsigned, Glyph> glyphs;
        std::vector<uint8_t> alpha;
    };
    const Atlas* pick(int size) const;
    std::map<int, Atlas> atlases_;
};

std::vector<unsigned> utf8_codepoints(const std::string& s);

} // namespace play
