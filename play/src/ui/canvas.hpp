// SPDX-License-Identifier: GPL-3.0-or-later
// A panel-sized picture drawn on the CPU (the companion screen), and the
// launcher's own rounded font so the player reads like the rest of RG DS.
#pragma once
#include <cmath>
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

// Drawn in layout units, stored in panel pixels: `scale` panel pixels a unit (1 on the RG DS; 1.6 on the RG DS
// Plus's 1024x768, whose second screen is laid out at the RG DS's 640x480 as the launcher's is). Every drawing call
// takes layout units and fills the panel's own pixels, so edges, corners and text are as sharp as the panel; w()
// and h() are the layout's size. data() and at() are the stored pixels (pixel_w() x pixel_h()), for code that
// writes pixels itself: it works in panel pixels (to_px turns a layout length into them).
class Canvas {
public:
    Canvas(int w = 0, int h = 0) { resize(w, h); }
    Canvas(int w, int h, float scale) { resize(w, h, scale); }
    void resize(int w, int h, float scale = 1) {
        lw_ = w;
        lh_ = h;
        k_ = scale > 0 ? scale : 1;
        w_ = k_ == 1 ? w : int(std::lround(w * k_));
        h_ = k_ == 1 ? h : int(std::lround(h * k_));
        px_.assign(static_cast<size_t>(w_) * h_, 0xFF000000u);
    }
    // The same layout size and scale as `other`.
    void resize_like(const Canvas& other, int w, int h) { resize(w, h, other.k_); }
    int w() const { return lw_; }
    int h() const { return lh_; }
    float scale() const { return k_; }
    int pixel_w() const { return w_; }
    int pixel_h() const { return h_; }
    // A layout position or length in panel pixels.
    int to_px(double v) const { return k_ == 1 ? int(v) : int(std::lround(v * k_)); }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }
    uint32_t at(int x, int y) const { return px_[static_cast<size_t>(y) * w_ + x]; }
    // One panel pixel blended (text, code drawing pixel by pixel at the panel's own size).
    void put_px(int x, int y, Rgb c, int alpha = 255) {
        if (x >= 0 && y >= 0 && x < w_ && y < h_) blend(px_[static_cast<size_t>(y) * w_ + x], c, alpha);
    }
    // `src` (pixel_w x pixel_h of a canvas of the same scale) at layout x, y, as it is.
    void paste(const Canvas& src, int x, int y);

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
    void fill_px(int x0, int y0, int x1, int y1, Rgb c, int alpha);
    void rounded_px(int x, int y, int w, int h, int radius, Rgb c, int alpha);
    void blit_px(const uint32_t* src, int sw, int sh, int dx, int dy, int dw, int dh, int alpha, bool keyed,
                 uint32_t key);
    int w_ = 0, h_ = 0;   // the stored pixels
    int lw_ = 0, lh_ = 0; // the layout
    float k_ = 1;
    std::vector<uint32_t> px_;
};

// Text drawn on a canvas, for tools that check a screen's layout (the
// companion lint): where each string went, at what size, in what ink, over
// what ground. Font::draw adds one while a log is set.
struct TextMark {
    int x = 0, y = 0, w = 0, h = 0; // the line box: from the ascent to the baseline plus a fifth
    int size = 0, asked = 0;        // the size drawn and the size the caller asked for
    Rgb ink, ground;                // the ground: the canvas's mean colour under the box before
    std::string text;
};
void set_text_log(std::vector<TextMark>* log);

// The launcher's rounded font (RGF1 atlases from assets/fonts, tools/bake_font.py)
// or, with set_pixel, its Pixel classic 5x7 dots. Text is drawn only at the
// sizes the atlases are baked at, one texel a pixel, so it is as crisp as the
// launcher's: a size in between is drawn at the nearest one (text_sizes).
// Pixel classic draws the dots at the launcher's whole scales for those sizes.
inline constexpr int text_sizes[] = {16, 20, 24, 28, 36};
class Font {
public:
    // The five Latin atlases; `language` ("ja-JP") orders the CJK families tried.
    bool load(const std::string& directory, const std::string& language = "en-US");
    bool loaded() const { return !atlases_.empty(); }
    void set_pixel(bool pixel) { pixel_ = pixel; }
    bool pixel() const { return pixel_; }
    // The layout's scale (Canvas::scale) for measuring: text is laid out in layout units but drawn from the atlas
    // of the panel's own size (rounded-26 for 16 at 1.6x), so measure() and ascent() answer for that atlas.
    void set_scale(float scale) const { scale_ = scale > 0 ? scale : 1; }
    // The size text of `size` is really drawn at.
    static int snap(int size);
    int measure(const std::string& text, int size) const;
    // Baseline at y; returns the advance.
    int draw(Canvas& c, int x, int y, const std::string& text, int size, Rgb color, int alpha = 255) const;
    // Height of capitals and digits above the baseline.
    int ascent(int size) const;
    // The baseline that centres capitals and digits on cy.
    int centred(int size, int cy) const { return cy + ascent(size) / 2; }
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
    struct Found {
        const Atlas* atlas = nullptr;
        const Glyph* glyph = nullptr;
    };
    const Atlas* pick(int size) const;
    Found find(int size, unsigned cp) const;
    static bool read_atlas(const std::string& path, Atlas& a, int size);
    int measure_rounded(const std::string& text, int size) const;
    int draw_rounded(Canvas& c, int x, int y, const std::string& text, int size, Rgb color, int alpha) const;
    int draw_dots(Canvas& c, int x, int y, const std::string& text, int size, Rgb color, int alpha) const;
    std::map<int, Atlas> atlases_;
    mutable std::map<std::pair<std::string, int>, Atlas> extras_; // CJK families, loaded on first use
    std::string directory_;
    std::vector<std::string> families_;
    bool pixel_ = false;
    mutable float scale_ = 1; // set for each canvas it lays out on (Session::render_companion)
    int native(int size, float scale) const;
};

std::vector<unsigned> utf8_codepoints(const std::string& s);

} // namespace play
