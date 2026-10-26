// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>

namespace play {

namespace {
// The launcher's blend(a, b, amount%).
Rgb blend(Rgb a, Rgb b, int percent) { return mix(a, b, percent * 256 / 100); }

int lum(Rgb c) { return (c.r * 3 + c.g * 6 + c.b) / 10; }

// The inset of a rounded rectangle's edge on a row, as the launcher's round_rect.
int inset_at(int row, int h, int r) {
    double dy = row < r ? r - row - .5 : row >= h - r ? row - (h - r) + .5 : -1;
    return dy < 0 ? 0 : r - int(std::sqrt(std::max(0.0, r * r - dy * dy)));
}
} // namespace

namespace {
std::vector<Box>* target_log = nullptr;
}
void touch_target(const Box& b) {
    if (target_log) target_log->push_back(b);
}
void set_target_log(std::vector<Box>* log) { target_log = log; }

void backdrop(Canvas& out, const Look& look) {
    const Rgb bg = look.background, fg = look.line;
    out.fill(bg);
    if (look.grid) {
        // The DS touch screen: square tiles, 20 px a side, each rule with a
        // pale edge after it so the tiles read as pressed into the screen.
        const Rgb rule = blend(bg, fg, look.dark ? 30 : 34), edge = blend(bg, Rgb{255, 255, 255}, look.dark ? 4 : 55);
        for (int row = 19; row < out.h(); row += 20) {
            out.fill_rect(0, row, out.w(), 1, rule);
            out.fill_rect(0, row + 1, out.w(), 1, edge);
        }
        for (int column = 19; column < out.w(); column += 20) {
            out.fill_rect(column, 0, 1, out.h(), rule);
            out.fill_rect(column + 1, 0, 1, out.h(), edge);
        }
        return;
    }
    const Rgb line{uint8_t((bg.r * 19 + fg.r) / 20), uint8_t((bg.g * 19 + fg.g) / 20),
                   uint8_t((bg.b * 19 + fg.b) / 20)};
    for (int row = 0; row < out.h(); row += 4) out.fill_rect(0, row, out.w(), 1, line);
}

void soft_round(Canvas& out, int x, int y, int w, int h, int r, Rgb c, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    out.rounded_rect(x, y, w, h, r, c, std::min(alpha, 255));
}

void glossy(Canvas& out, int x, int y, int w, int h, int r, Rgb base, double press, bool highlight) {
    r = std::clamp(r, 0, std::min(w, h) / 2);
    const Rgb white{255, 255, 255}, black{0, 0, 0};
    const int l = lum(base);
    // Pale bodies (white panels) need a visible rim; saturated ones a deeper one.
    out.rounded_rect(x, y, w, h, r, blend(base, black, l > 200 ? 16 : 26));
    if (w <= 2 || h <= 2) return;
    out.rounded_rect(x + 1, y + 1, w - 2, h - 2, std::max(0, r - 1), base);
    const double lift = l > 200 ? 0.0 : l < 90 ? 10.0 : 30.0, deepen = l > 200 ? 9.0 : 14.0;
    for (int row = 1; row < h - 1; ++row) {
        const int inset = inset_at(row, h, r) + 1;
        if (w - 2 * inset <= 0) continue;
        double t = double(row - 1) / std::max(1, h - 3);
        if (press > 0) t = t + (1 - 2 * t) * press * .8;
        const Rgb c = t < .5 ? blend(base, white, int(std::lround(lift * (1 - 2 * t))))
                             : blend(base, black, int(std::lround(deepen * (2 * t - 1))));
        out.fill_rect(x + inset, y + row, w - 2 * inset, 1, c);
    }
    const int inset = inset_at(1, h, r) + 2;
    if (highlight && w - 2 * inset > 0 && h > 6)
        out.fill_rect(x + inset, y + 1, w - 2 * inset, 1, blend(base, white, int(55 * (1 - .7 * press))));
}

void raised(Canvas& out, const Look& look, int x, int y, int w, int h, int r, Rgb fill, double press, bool shadow,
            bool highlight) {
    const int sink = int(std::lround(2 * press));
    if (shadow) soft_round(out, x, y + 3, w, h, r, look.shadow(), int((look.dark ? 70 : 28) * (1 - .5 * press)));
    glossy(out, x, y + sink, w, h, r, fill, press, highlight);
}

void card(Canvas& out, const Look& look, const Box& b, bool focused, int r) {
    out.rounded_rect(b.x, b.y + 3, b.w, b.h, r, look.slot);
    out.rounded_rect(b.x, b.y, b.w, b.h, r, focused ? look.accent : look.panel);
    if (focused) out.rounded_rect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, std::max(0, r - 2), look.focus);
}

void sunken(Canvas& out, const Look& look, const Box& b, int r) {
    out.rounded_rect(b.x, b.y, b.w, b.h, r, look.line);
    out.rounded_rect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, std::max(0, r - 1), look.background);
    soft_round(out, b.x + 2, b.y + 2, b.w - 4, std::min(4, std::max(1, b.h - 4)), std::max(0, r - 2), look.shadow(),
               look.dark ? 50 : 14);
}

void meter(Canvas& out, const Look& look, const Box& b, double fraction, Rgb fill) {
    sunken(out, look, b, b.h / 2);
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction <= 0) return;
    const int w = std::max(b.h - 2, int(std::lround((b.w - 2) * fraction)));
    glossy(out, b.x + 1, b.y + 1, w, b.h - 2, (b.h - 2) / 2, fill);
}

void icon(Canvas& out, const Look& look, Icon which, int cx, int cy, int size, Rgb ink, int alpha) {
    const std::string_view g = icon_glyph(which);
    if (g.size() != 144) return;
    auto at = [&](int x, int y) -> double {
        if (x < 0 || y < 0 || x >= 12 || y >= 12) return 0;
        const char c = g[size_t(y * 12 + x)];
        return c == '#' ? 1.0 : c == '+' ? 0.45 : 0.0;
    };
    // Smoothing needs room: small icons keep their pixels in either style.
    if (!look.rich_icons || size < 22) {
        // Whole-pixel steps: 12 px glyphs grow to 24 or 36, never in between.
        const int k = std::max(1, (size + 4) / 12), left = cx - 6 * k, top = cy - 6 * k;
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x)
                if (double v = at(x, y); v > 0) out.fill_rect(left + x * k, top + y * k, k, k, ink, int(alpha * v));
        return;
    }
    // Rich: the same shape, smoothed. The glyph's coverage is sampled with a
    // bilinear filter and sharpened to an edge, which rounds its corners off.
    const double k = size / 12.0;
    const int left = int(std::lround(cx - 6 * k)), top = int(std::lround(cy - 6 * k)), n = int(std::ceil(12 * k));
    for (int py = 0; py < n; ++py)
        for (int px = 0; px < n; ++px) {
            const double gx = (px + .5) / k - .5, gy = (py + .5) / k - .5;
            const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
            const double fx = gx - x0, fy = gy - y0;
            const double v = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                             (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
            // Ink cells keep their full strength, half-tones theirs.
            const double edge = std::clamp((v - 0.22) * 4.0, 0.0, 1.0), tone = std::clamp(v * 1.6, 0.0, 1.0);
            const double a = edge * std::min(1.0, tone + 0.15);
            if (a > 0.01) out.fill_rect(left + px, top + py, 1, 1, ink, int(alpha * a));
        }
}

void icon_badge(Canvas& out, const Look& look, int x, int y, Icon which, bool lit, int size) {
    out.rounded_rect(x, y, size, size, size * 12 / 44, lit ? look.accent : look.slot);
    icon(out, look, which, x + size / 2, y + size / 2, size * 25 / 44 + 1, lit ? look.on_accent : look.accent);
}

void art_badge(Canvas& out, const Look& look, int x, int y, const GameArt& art, const uint32_t shades[4], int size) {
    // The art on a little screen of the game's lightest shade, in a slot rim.
    out.rounded_rect(x, y, size, size, size * 12 / 44, look.slot);
    const Rgb paper = Rgb::from_px(shades[0]);
    out.rounded_rect(x + 3, y + 3, size - 6, size - 6, size * 9 / 44, paper);
    if (art.empty()) return;
    const int inner = size - 10, k = std::max(1, std::min(inner / art.w, inner / art.h));
    const int ox = x + (size - art.w * k) / 2, oy = y + (size - art.h * k) / 2;
    for (int ty = 0; ty < art.h; ++ty)
        for (int tx = 0; tx < art.w; ++tx) {
            const uint8_t i = art.index[size_t(ty * art.w + tx)];
            if (i < 4) out.fill_rect(ox + tx * k, oy + ty * k, k, k, Rgb::from_px(shades[i]));
        }
}

int chip(Canvas& out, const Look& look, const Font& font, int x, int cy, const std::string& text, bool accent) {
    const int size = kit::small, w = font.measure(text, size) + 24, h = 28;
    const Rgb fill = accent ? look.accent : look.slot, ink = accent ? look.on_accent : look.ink;
    out.rounded_rect(x, cy - h / 2, w, h, h / 2, fill);
    font.draw(out, x + 12, font.centred(size, cy), text, size, ink);
    return w;
}

void disc(Canvas& out, int cx, int cy, int r, Rgb c, int alpha) {
    out.rounded_rect(cx - r, cy - r, r * 2, r * 2, r, c, alpha);
}

void ring(Canvas& out, int cx, int cy, int r, int thickness, double fraction, Rgb c, int alpha) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction <= 0) return;
    const double end = fraction * 2 * M_PI;
    for (int y = cy - r - 1; y <= cy + r + 1; ++y)
        for (int x = cx - r - 1; x <= cx + r + 1; ++x) {
            const double dx = x + .5 - cx, dy = y + .5 - cy, d = std::sqrt(dx * dx + dy * dy);
            const double cover = std::clamp(std::min(r - d + .5, d - (r - thickness) + .5), 0.0, 1.0);
            if (cover <= 0) continue;
            double a = std::atan2(dx, -dy); // clockwise from the top
            if (a < 0) a += 2 * M_PI;
            if (a > end) continue;
            out.fill_rect(x, y, 1, 1, c, int(alpha * cover));
        }
}

GameArt gb_tiles(const std::vector<uint8_t>& bytes, int cols, int rows, int clear) {
    GameArt art;
    if (cols <= 0 || rows <= 0 || bytes.size() < size_t(cols * rows * 16)) return art;
    art.w = cols * 8;
    art.h = rows * 8;
    art.index.assign(size_t(art.w * art.h), 255);
    for (int t = 0; t < cols * rows; ++t)
        for (int y = 0; y < 8; ++y) {
            const uint8_t lo = bytes[size_t(t * 16 + y * 2)], hi = bytes[size_t(t * 16 + y * 2 + 1)];
            for (int x = 0; x < 8; ++x) {
                const int v = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
                art.index[size_t(((t / cols) * 8 + y) * art.w + (t % cols) * 8 + x)] = v == clear ? 255 : uint8_t(v);
            }
        }
    return art;
}

double ease(const Look& look, double t) {
    if (!look.animations) return 1;
    t = std::clamp(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

} // namespace play
