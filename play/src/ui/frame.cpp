// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/frame.hpp"

#include "ui/png.hpp"

#include <algorithm>
#include <cmath>

namespace play {
namespace {

enum Kind { Shell, Bar, Sp };

struct Style {
    const char* name;
    Kind kind;
    Rgb top, bottom; // the body, lit from above
    int speckle;     // plastic grain (Glacier shows its insides)
    Rgb lens, rim;   // the screen lens and the light catching its edge
    Rgb ink;         // the name printed on the body
};

const Style styles[FrameStyles] = {
    {"off", Shell, {}, {}, 0, {}, {}, {}},
    {"indigo", Shell, Rgb::hex(0x5B4FAF), Rgb::hex(0x3B3283), 7, Rgb::hex(0x22222C), Rgb::hex(0x55556A), {}},
    {"glacier", Shell, Rgb::hex(0xB5D6EE), Rgb::hex(0x86AFD3), 16, Rgb::hex(0x252A34), Rgb::hex(0x5C6678), {}},
    {"black", Shell, Rgb::hex(0x34343A), Rgb::hex(0x1E1E22), 5, Rgb::hex(0x141417), Rgb::hex(0x4A4A52), {}},
    {"platinum", Shell, Rgb::hex(0xD9D9DE), Rgb::hex(0xAEAEB6), 5, Rgb::hex(0x2A2A31), Rgb::hex(0x60606C), {}},
    {"bar", Bar, Rgb::hex(0x060607), Rgb::hex(0x060607), 0, {}, Rgb::hex(0x1E1E24), Rgb::hex(0xC4C4CC)},
    {"sp-onyx", Sp, Rgb::hex(0x2A2A30), Rgb::hex(0x16161A), 4, Rgb::hex(0x0E0E11), Rgb::hex(0x4C4C56),
     Rgb::hex(0xB8B8C2)},
    {"sp-cobalt", Sp, Rgb::hex(0x3A5BB8), Rgb::hex(0x253E86), 5, Rgb::hex(0x10121A), Rgb::hex(0x5D72B0),
     Rgb::hex(0xD6DEF5)},
    {"sp-platinum", Sp, Rgb::hex(0xDCDEE4), Rgb::hex(0xA9ACB6), 3, Rgb::hex(0x16171C), Rgb::hex(0x6A6C78),
     Rgb::hex(0x4A4C56)},
};

// At the panel's 640x480 scale.
constexpr int lens_side = 30, lens_top = 26, lens_bottom = 46, lens_radius = 18; // the shells
constexpr int bar_min = 50;                                                      // the black bar
constexpr int sp_side = 20, sp_top = 18, sp_bottom = 62, sp_margin = 9, sp_radius = 10; // the SP

uint32_t hash(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
int clamp8(int v) { return std::clamp(v, 0, 255); }
Rgb shade(Rgb c, int delta) {
    return {uint8_t(clamp8(c.r + delta)), uint8_t(clamp8(c.g + delta)), uint8_t(clamp8(c.b + delta))};
}
Rgb scale(Rgb c, double f) {
    return {uint8_t(clamp8(int(c.r * f))), uint8_t(clamp8(int(c.g * f))), uint8_t(clamp8(int(c.b * f)))};
}
// Signed distance to a rounded rectangle (negative inside).
double rounded_distance(double px, double py, double x, double y, double w, double h, double r) {
    double cx = x + w / 2, cy = y + h / 2;
    double qx = std::abs(px - cx) - (w / 2 - r), qy = std::abs(py - cy) - (h / 2 - r);
    double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    return outside + std::min(std::max(qx, qy), 0.0) - r;
}
void put(Canvas& out, int x, int y, Rgb c, int alpha) {
    if (x < 0 || y < 0 || x >= out.w() || y >= out.h() || alpha <= 0) return;
    uint32_t& d = out.data()[size_t(y) * out.w() + x];
    d = mix(Rgb::from_px(d), c, std::min(alpha, 256)).px();
}
void disc(Canvas& out, double cx, double cy, double r, Rgb c, int alpha) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
        for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) - r;
            if (d < 0.5) put(out, x, y, c, int(alpha * std::clamp(0.5 - d, 0.0, 1.0)));
        }
}

// The body: lit from above, rounding away at the sides and corners, with grain.
void body(Canvas& out, const Style& s, double sx, double sy) {
    const int w = out.w(), h = out.h();
    for (int y = 0; y < h; ++y) {
        Rgb row = mix(s.top, s.bottom, y * 256 / std::max(1, h - 1));
        for (int x = 0; x < w; ++x) {
            double ex = std::min(x, w - 1 - x) / (40.0 * sx), ey = std::min(y, h - 1 - y) / (40.0 * sy);
            Rgb c = scale(row, 0.80 + 0.20 * std::sqrt(std::min(1.0, std::min(ex, ey))));
            if (y < 14 * sy) c = mix(c, Rgb{255, 255, 255}, int(40 * (1 - y / (14 * sy))));
            uint32_t n = hash(x, y);
            if (s.speckle && (n & 7) == 0) c = shade(c, int(n >> 8) % (2 * s.speckle + 1) - s.speckle);
            out.data()[size_t(y) * w + x] = c.px();
        }
    }
}

// A raised glass lens with a soft shadow below it, light on its top edge and a faint glare.
void lens(Canvas& out, const Style& s, double lx, double ly, double lw, double lh, double lr, double unit) {
    for (int y = std::max(0, int(ly - 8 * unit)); y < std::min(out.h(), int(ly + lh + 8 * unit)); ++y)
        for (int x = std::max(0, int(lx - 8 * unit)); x < std::min(out.w(), int(lx + lw + 8 * unit)); ++x) {
            uint32_t& px = out.data()[size_t(y) * out.w() + x];
            Rgb c = Rgb::from_px(px);
            double d = rounded_distance(x + 0.5, y + 0.5, lx, ly, lw, lh, lr);
            double below = rounded_distance(x + 0.5, y + 0.5 - 3 * unit, lx, ly, lw, lh, lr);
            if (below < 6 * unit && d > 0) c = scale(c, 0.78 + 0.22 * std::clamp(below / (6 * unit), 0.0, 1.0));
            if (d < 0.5) {
                Rgb glass = shade(s.lens, int(8 - 10.0 * (y - ly) / lh));
                if (d > -2.5 * unit) glass = y < ly + lh / 2 ? mix(glass, s.rim, 150) : shade(glass, -6);
                double band = (x - lx) - 0.55 * (y - ly);
                if (band > lw * 0.10 && band < lw * 0.22) glass = shade(glass, 5);
                c = mix(c, glass, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
            }
            px = c.px();
        }
}

// The screen's own black edge, just outside the game.
void screen_edge(Canvas& out, const FrameLayout& screen, int edge) {
    for (int y = screen.y - edge; y < screen.y + screen.h + edge; ++y)
        for (int x = screen.x - edge; x < screen.x + screen.w + edge; ++x)
            if (y < screen.y || y >= screen.y + screen.h || x < screen.x || x >= screen.x + screen.w)
                put(out, x, y, Rgb::hex(0x050506), 230);
}

// Spaced capitals, centred on cx; the last word in `accent` colours, a letter each (none: ink).
void name(Canvas& out, const Font& font, const std::string& text, double cx, int baseline, int size, int spacing,
          Rgb ink, const std::vector<Rgb>& accent) {
    if (!font.loaded() || text.empty()) return;
    const size_t last = accent.empty() ? std::string::npos : text.rfind(' ');
    int width = 0;
    for (size_t i = 0; i < text.size(); ++i) width += font.measure(text.substr(i, 1), size) + spacing;
    int x = int(cx - (width - spacing) / 2.0);
    for (size_t i = 0, k = 0; i < text.size(); ++i) {
        Rgb c = ink;
        if (last != std::string::npos && i > last) c = accent[std::min(k++, accent.size() - 1)];
        x += font.draw(out, x, baseline, text.substr(i, 1), size, c) + spacing;
    }
}

// ADVANCE in a sweep from violet through blue to pink; COLOR a letter each.
std::vector<Rgb> accent_for(const std::string& text) {
    std::string last = text.substr(text.rfind(' ') + 1);
    if (last == "COLOR")
        return {Rgb::hex(0xE8524A), Rgb::hex(0xF2B92E), Rgb::hex(0x58C35A), Rgb::hex(0x3E8DE0), Rgb::hex(0x9A62D6)};
    if (last == "ADVANCE" || last == "SP") {
        std::vector<Rgb> sweep;
        const Rgb stops[3] = {Rgb::hex(0x8B7BFF), Rgb::hex(0x5BC8F5), Rgb::hex(0xF07BC4)};
        const int n = int(last.size());
        for (int i = 0; i < n; ++i) {
            int t = n > 1 ? i * 512 / (n - 1) : 0;
            sweep.push_back(t < 256 ? mix(stops[0], stops[1], t) : mix(stops[1], stops[2], t - 256));
        }
        return sweep;
    }
    return {};
}

void draw_shell(Canvas& out, const FrameLayout& screen, const Style& s) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    const double lx = screen.x - lens_side * sx, ly = screen.y - lens_top * sy, lw = screen.w + 2 * lens_side * sx,
                 lh = screen.h + (lens_top + lens_bottom) * sy;
    body(out, s, sx, sy);
    lens(out, s, lx, ly, lw, lh, lens_radius * unit, unit);
    screen_edge(out, screen, std::max(2, int(3 * unit)));
    // The power light, left of the screen.
    const double led_x = lx + lens_side * sx / 2, led_y = screen.y + screen.h * 0.28;
    disc(out, led_x, led_y, 9 * unit, Rgb::hex(0x3EF070), 40);
    disc(out, led_x, led_y, 3.5 * unit, Rgb::hex(0x56FF84), 256);
    disc(out, led_x - 1 * unit, led_y - 1 * unit, 1.2 * unit, Rgb::hex(0xE8FFF0), 200);
    // Below the screen: a line either side of three dots, red, green and blue.
    const double mid_y = screen.y + screen.h + (lens_bottom * sy) / 2, cx = screen.x + screen.w / 2.0;
    const Rgb dots[3] = {Rgb::hex(0xE45A5A), Rgb::hex(0x5CC86A), Rgb::hex(0x5A8CE6)};
    for (int i = 0; i < 3; ++i) disc(out, cx + (i - 1) * 11 * unit, mid_y, 3 * unit, dots[i], 230);
    const int line_y = int(mid_y), gap = int(26 * unit), reach = int(screen.w * 0.30);
    for (int x = int(cx) - gap - reach; x < int(cx) - gap; ++x) put(out, x, line_y, s.rim, 170);
    for (int x = int(cx) + gap; x < int(cx) + gap + reach; ++x) put(out, x, line_y, s.rim, 170);
    // The speaker grille, low on the right of the body.
    const double grille_x = lx + lw + (w - (lx + lw)) / 2, grille_y = ly + lh - 40 * sy;
    for (int row = 0; row < 6; ++row)
        for (int col = -1; col <= 1; ++col) {
            double hx = grille_x + col * 9 * unit + (row % 2) * 4.5 * unit, hy = grille_y + row * 8 * unit;
            if (hx > w - 8 * sx) continue;
            Rgb at = Rgb::from_px(out.data()[size_t(std::clamp(int(hy), 0, h - 1)) * w + std::clamp(int(hx), 0, w - 1)]);
            disc(out, hx, hy + 0.8 * unit, 2.2 * unit, shade(at, 30), 120);
            disc(out, hx, hy, 2.2 * unit, scale(at, 0.40), 256);
        }
}

void draw_bar(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font, const std::string& system) {
    out.fill(s.top);
    const int top = screen.y + screen.h;
    for (int x = 0; x < out.w(); ++x) put(out, x, top, s.rim, 256);
    const int bar = out.h() - top, size = std::clamp(bar * 9 / 20, 14, 28);
    name(out, font, system, out.w() / 2.0, top + (bar + font.ascent(size)) / 2 - 1, size, std::max(1, size / 10), s.ink,
         accent_for(system));
}

void draw_sp(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy);
    // The clamshell's hinge along the bottom edge.
    for (int y = h - int(10 * sy); y < h; ++y)
        for (int x = 0; x < w; ++x) put(out, x, y, scale(s.bottom, 0.55), 150);
    const double m = sp_margin * unit;
    lens(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, sp_radius * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    const int size = int(20 * unit), baseline = int(screen.y + screen.h + m + (sp_bottom * sy - m) / 2 + size / 3.0);
    name(out, font, "GAME BOY ADVANCE SP", w / 2.0, baseline, size, std::max(1, int(2 * unit)), s.ink,
         accent_for("GAME BOY ADVANCE SP"));
}

} // namespace

FrameLayout frame_layout(int panel_w, int panel_h, int game_w, int game_h, int style) {
    FrameLayout l;
    const double sx = panel_w / 640.0, sy = panel_h / 480.0, unit = std::min(sx, sy);
    game_w = std::max(1, game_w);
    game_h = std::max(1, game_h);
    const Kind kind = style > FrameOff && style < FrameStyles ? styles[style].kind : Shell;
    if (kind == Bar || kind == Sp) {
        // As large as fits: across the full width at the top, or inside the slim SP lens.
        const double room_w = kind == Bar ? panel_w : panel_w - 2 * (sp_side * sx + sp_margin * unit);
        const double room_h = kind == Bar ? panel_h - bar_min * sy : panel_h - (sp_top + sp_bottom) * sy - 2 * sp_margin * unit;
        const double s = std::min(room_w / game_w, room_h / game_h);
        l.w = int(game_w * s);
        l.h = int(game_h * s);
        l.x = (panel_w - l.w) / 2;
        l.y = kind == Bar ? 0 : int(sp_top * sy + sp_margin * unit);
        l.whole = std::abs(s - std::round(s)) < 1e-6;
        return l;
    }
    const int room_w = panel_w - int(2 * (lens_side + 50) * sx), room_h = panel_h - int((lens_top + lens_bottom + 60) * sy);
    const int s = std::max(1, std::min(room_w / game_w, room_h / game_h));
    l.w = game_w * s;
    l.h = game_h * s;
    const int lens_h = l.h + int((lens_top + lens_bottom) * sy);
    l.x = (panel_w - l.w) / 2;
    l.y = (panel_h - lens_h) / 2 + int(lens_top * sy);
    return l;
}

void draw_frame(Canvas& out, const FrameLayout& screen, int style, const Font& font, const std::string& system) {
    if (style <= FrameOff || style >= FrameStyles) return;
    const Style& s = styles[style];
    if (s.kind == Bar) draw_bar(out, screen, s, font, system);
    else if (s.kind == Sp) draw_sp(out, screen, s, font);
    else draw_shell(out, screen, s);
}

int parse_frame(const std::string& name) {
    for (int i = 0; i < FrameStyles; ++i)
        if (name == styles[i].name) return i;
    return -1;
}
const char* frame_name(int style) { return styles[style > FrameOff && style < FrameStyles ? style : FrameOff].name; }

bool load_picture_frame(const std::string& path, PictureFrame& frame, std::string& error) {
    if (!read_png(path, frame.px, frame.w, frame.h, error)) return false;
    // The see-through part: where the picture is mostly clear (a grid laid over the game stays light).
    int x0 = frame.w, y0 = frame.h, x1 = -1, y1 = -1;
    for (int y = 0; y < frame.h; ++y)
        for (int x = 0; x < frame.w; ++x)
            if ((frame.px[size_t(y) * frame.w + x] >> 24) < 128) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) frame.hole = {0, 0, frame.w, frame.h};
    else frame.hole = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    // Over the game the picture is clear: overlays like the "Perfect" ones lay a dark LCD grid there,
    // which would stay on whatever the Shader setting says. The screen's look is the shader's.
    for (int y = frame.hole.y; y < frame.hole.y + frame.hole.h; ++y)
        std::fill_n(frame.px.begin() + size_t(y) * frame.w + frame.hole.x, frame.hole.w, 0u);
    return true;
}

FrameLayout picture_layout(const PictureFrame& frame, int panel_w, int panel_h, int game_w, int game_h) {
    const double fx = double(panel_w) / std::max(1, frame.w), fy = double(panel_h) / std::max(1, frame.h);
    const double hx = frame.hole.x * fx, hy = frame.hole.y * fy, hw = frame.hole.w * fx, hh = frame.hole.h * fy;
    const double s = std::min(hw / std::max(1, game_w), hh / std::max(1, game_h));
    FrameLayout l;
    l.w = int(std::lround(game_w * s));
    l.h = int(std::lround(game_h * s));
    l.x = int(std::lround(hx + (hw - l.w) / 2));
    l.y = int(std::lround(hy + (hh - l.h) / 2));
    l.whole = std::abs(s - std::round(s)) < 1e-6;
    return l;
}

void draw_picture_frame(Canvas& out, const PictureFrame& frame) {
    if (frame.px.empty()) return;
    for (int y = 0; y < out.h(); ++y) {
        const int sy = std::min(frame.h - 1, int(int64_t(y) * frame.h / out.h()));
        for (int x = 0; x < out.w(); ++x)
            out.data()[size_t(y) * out.w() + x] =
                frame.px[size_t(sy) * frame.w + std::min(frame.w - 1, int(int64_t(x) * frame.w / out.w()))];
    }
}

} // namespace play
