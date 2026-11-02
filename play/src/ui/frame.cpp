// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/frame.hpp"

#include "ui/frame_styles.hpp"
#include "ui/png.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace play {
namespace {
using Kind = FrameKind;

struct Style {
    Kind kind;
    Rgb top, bottom;   // the body, lit from above
    int speckle;       // plastic grain (a clear shell shows its insides)
    Rgb lens, rim;     // the bezel, lens or tube, and the light catching its edge
    Rgb ink;           // the name printed on the body
    std::string print; // that name
    Rgb band, line;    // a console's strip and its lines; a set's controls; a cabinet's marquee and side art
    int wood;          // wood grain: 1 on the strip, 2 on the body
    Badge badge;       // the mark a console carries on its front
};
Style style_of(int index) {
    const FrameStyleInfo& f = frame_styles[index];
    return {
        f.kind,  Rgb::hex(f.top),  Rgb::hex(f.bottom), f.speckle, Rgb::hex(f.bezel), Rgb::hex(f.rim), Rgb::hex(f.ink),
        f.print, Rgb::hex(f.band), Rgb::hex(f.line),   f.wood,    badge_of(f.slug)};
}

// At the panel's 640x480 scale.
constexpr int lens_side = 30, lens_top = 26, lens_bottom = 46, lens_radius = 18; // the Game Boy Advance's lens
constexpr int bar_min = 50;                                                      // the black bar
// The Game Boy Advance SP's own measurements, in millimetres: the screen, the window it sits in (wider below for
// the logo), the lid around that, the hinge's barrel, the lid's top corners and where its bumpers sit.
constexpr double sp_screen_mm = 61, sp_window_side = 3, sp_window_top = 3, sp_window_bottom = 9.5, sp_lid_mm = 82,
                 sp_lid_top = 5, sp_lid_bottom = 5.5, sp_hinge_mm = 11, sp_lid_radius = 5, sp_window_radius = 1.5,
                 sp_bumper_in = 3.4;
// The flat bezels of the Game Boy, the Pocket and the Color: their margins from the game to the bezel's edge (the
// power light sits in the wider left one), the body left showing above and below them, and their corners (the
// low-right one is the Game Boy's big curve).
struct Face {
    int left, right, top, bottom, above, below, radius, low_right;
};
constexpr int recess_width = 6; // the dark frame the glass sits in, inside those margins
constexpr Face dmg_face{84, 48, 46, 26, 8, 50, 10, 60}, pocket_face{84, 44, 38, 30, 12, 46, 14, 14},
    bezel_face{84, 44, 32, 62, 14, 20, 24, 24};
const Face& face_of(Kind kind) {
    return kind == Kind::Dmg ? dmg_face : kind == Kind::Pocket ? pocket_face : bezel_face;
}
bool flat_face(Kind kind) { return kind == Kind::Dmg || kind == Kind::Pocket || kind == Kind::Bezel; }
// The consoles, sets, monitors and cabinets: the room each leaves around the game.
struct Room {
    int left, right, top, bottom;
};
bool inner(Kind kind) { return kind == Kind::DmgInner || kind == Kind::PocketInner || kind == Kind::BezelInner; }
bool roomy(Kind kind) {
    return kind == Kind::Tv || kind == Kind::TvWood || kind == Kind::Monitor || kind == Kind::Arcade ||
           kind == Kind::Vectrex || inner(kind);
}
Room room_of(Kind kind) {
    switch (kind) {
    // Exactly 3x on a Game Boy's 160x144 picture in 640x480 (480 x 432): whole pixels.
    case Kind::DmgInner: return {88, 72, 34, 14};
    case Kind::PocketInner: return {88, 72, 24, 24};
    case Kind::BezelInner: return {88, 72, 14, 34};
    case Kind::Tv: return {46, 46, 38, 84};
    case Kind::TvWood: return {40, 160, 42, 42};
    case Kind::Monitor: return {48, 48, 40, 82};
    case Kind::Arcade: return {58, 58, 96, 76};
    case Kind::Vectrex: return {60, 60, 34, 78};
    default: return {0, 0, 0, 0};
    }
}

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
// Wood grain: light and dark bands along the board, wavering, with fine dark streaks.
Rgb wood_at(Rgb base, int x, int y, double unit) {
    const double u = x / unit, v = y / unit;
    const double wave = v * 0.35 + 6 * std::sin(u * 0.011 + v * 0.004) + 2.5 * std::sin(u * 0.047);
    Rgb c = scale(base, 0.84 + 0.24 * (0.5 + 0.5 * std::sin(wave)));
    if ((hash(int(u / 3), y) & 15) == 0) c = shade(c, -10);
    return c;
}
// Signed distance to a rounded rectangle (negative inside).
double rounded_distance(double px, double py, double x, double y, double w, double h, double r) {
    double cx = x + w / 2, cy = y + h / 2;
    double qx = std::abs(px - cx) - (w / 2 - r), qy = std::abs(py - cy) - (h / 2 - r);
    double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    return outside + std::min(std::max(qx, qy), 0.0) - r;
}
// The same with its low-right corner rounded by its own radius.
double face_distance(double px, double py, double x, double y, double w, double h, double r, double low_right) {
    return rounded_distance(px, py, x, y, w, h, px > x + w / 2 && py > y + h / 2 ? low_right : r);
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
// A filled rounded rectangle, smoothed at its edge.
void rounded(Canvas& out, double x, double y, double w, double h, double r, Rgb c, int alpha = 256) {
    for (int py = std::max(0, int(y - 1)); py < std::min(out.h(), int(y + h + 2)); ++py)
        for (int px = std::max(0, int(x - 1)); px < std::min(out.w(), int(x + w + 2)); ++px) {
            double d = rounded_distance(px + 0.5, py + 0.5, x, y, w, h, r);
            if (d < 0.5) put(out, px, py, c, int(alpha * std::clamp(0.5 - d, 0.0, 1.0)));
        }
}

// The body: lit from above, rounding away at the sides and corners, with grain.
// `edge` is how far in the shell rounds away (0: not at all), `depth` how dark it gets at the very edge.
void body(Canvas& out, const Style& s, double sx, double sy, double edge = 40, double depth = 0.20) {
    const int w = out.w(), h = out.h();
    const double unit = std::min(sx, sy);
    for (int y = 0; y < h; ++y) {
        Rgb row = mix(s.top, s.bottom, y * 256 / std::max(1, h - 1));
        for (int x = 0; x < w; ++x) {
            double ex = edge > 0 ? std::min(x, w - 1 - x) / (edge * sx) : 1,
                   ey = edge > 0 ? std::min(y, h - 1 - y) / (edge * sy) : 1;
            Rgb c = scale(row, 1 - depth + depth * std::sqrt(std::min(1.0, std::min(ex, ey))));
            if (s.wood == 2) c = wood_at(c, x, y, unit);
            if (edge > 0 && y < 14 * sy) c = mix(c, Rgb{255, 255, 255}, int(40 * (1 - y / (14 * sy))));
            uint32_t n = hash(x, y);
            if (s.speckle && (n & 7) == 0) c = shade(c, int(n >> 8) % (2 * s.speckle + 1) - s.speckle);
            out.data()[size_t(y) * w + x] = c.px();
        }
    }
}

// A raised glass lens (or a picture tube) with a soft shadow below it, light on its top edge and a faint glare.
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
                // Its edge catches the light at the top, fading to a shade at the bottom (no step between).
                const double t = (y - ly) / lh;
                if (d > -2.5 * unit)
                    glass = t < 0.5 ? mix(glass, s.rim, int(150 * (1 - 2 * t))) : shade(glass, -int(12 * (t - 0.5)));
                double band = (x - lx) - 0.55 * (y - ly);
                if (band > lw * 0.10 && band < lw * 0.22) glass = shade(glass, 5);
                c = mix(c, glass, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
            }
            px = c.px();
        }
}

// A flat bezel set into the body: a soft shadow under its low edge, a faint light along its top edge.
void bezel(Canvas& out, const Style& s, double bx, double by, double bw, double bh, double r, double low_right,
           double unit) {
    for (int y = std::max(0, int(by - 4 * unit)); y < std::min(out.h(), int(by + bh + 6 * unit)); ++y)
        for (int x = std::max(0, int(bx - 4 * unit)); x < std::min(out.w(), int(bx + bw + 6 * unit)); ++x) {
            uint32_t& px = out.data()[size_t(y) * out.w() + x];
            Rgb c = Rgb::from_px(px);
            double d = face_distance(x + 0.5, y + 0.5, bx, by, bw, bh, r, low_right);
            double below = face_distance(x + 0.5, y + 0.5 - 2 * unit, bx, by, bw, bh, r, low_right);
            if (below < 4 * unit && d > 0) c = scale(c, 0.80 + 0.20 * std::clamp(below / (4 * unit), 0.0, 1.0));
            if (d < 0.5) {
                Rgb face = shade(s.lens, int(6 - 12.0 * (y - by) / bh));
                if (d > -1.5 * unit && y < by + r + 2 * unit) face = mix(face, s.rim, 140);
                c = mix(c, face, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
            }
            px = c.px();
        }
}
// The same in another colour (a set's front around its tube, a control panel).
void panel(Canvas& out, const Style& s, Rgb fill, double x, double y, double w, double h, double r, double unit) {
    Style p = s;
    p.lens = fill;
    p.rim = shade(fill, 30);
    bezel(out, p, x, y, w, h, r, r, unit);
}

// A lit power light: a glow, the light and a speck of shine.
void power_light(Canvas& out, double x, double y, double unit, Rgb c) {
    disc(out, x, y, 9 * unit, c, 40);
    disc(out, x, y, 3.5 * unit, c, 256);
    disc(out, x - 1 * unit, y - 1 * unit, 1.2 * unit, mix(c, Rgb{255, 255, 255}, 200), 200);
}
// A round knob or dial: its shadow, the knob lit from above, a mark showing where it is turned.
void knob(Canvas& out, double x, double y, double r, Rgb c, double unit, double turned) {
    disc(out, x, y + 1.5 * unit, r, scale(c, 0.35), 200);
    disc(out, x, y, r, shade(c, -18), 256);
    disc(out, x, y - r * 0.12, r * 0.82, shade(c, 10), 256);
    const double a = turned * 3.14159 / 180;
    disc(out, x + std::sin(a) * r * 0.6, y - std::cos(a) * r * 0.6, std::max(1.0, r * 0.12), scale(c, 0.3), 256);
}
// Speaker slots: rows of short rounded slits.
void slots(Canvas& out, double x, double y, double w, int rows, Rgb c, double unit) {
    for (int i = 0; i < rows; ++i) {
        rounded(out, x, y + i * 7 * unit + 1 * unit, w, 3 * unit, 1.5 * unit, shade(c, 25), 120);
        rounded(out, x, y + i * 7 * unit, w, 3 * unit, 1.5 * unit, scale(c, 0.35), 256);
    }
}

// The screen's own black edge, just outside the game.
void screen_edge(Canvas& out, const FrameLayout& screen, int edge) {
    for (int y = screen.y - edge; y < screen.y + screen.h + edge; ++y)
        for (int x = screen.x - edge; x < screen.x + screen.w + edge; ++x)
            if (y < screen.y || y >= screen.y + screen.h || x < screen.x || x >= screen.x + screen.w)
                put(out, x, y, Rgb::hex(0x050506), 230);
}
int spaced_width(const Font& font, const std::string& text, int size, int spacing) {
    int width = 0;
    for (size_t i = 0; i < text.size(); ++i) width += font.measure(text.substr(i, 1), size) + spacing;
    return width - spacing;
}

// Spaced capitals from x; the last word in `accent` colours, a letter each (none: ink).
void name_from(Canvas& out, const Font& font, const std::string& text, int x, int baseline, int size, int spacing,
               Rgb ink, const std::vector<Rgb>& accent) {
    if (!font.loaded() || text.empty()) return;
    const size_t last = accent.empty() ? std::string::npos : text.rfind(' ');
    for (size_t i = 0, k = 0; i < text.size(); ++i) {
        Rgb c = ink;
        if (last != std::string::npos && i > last) c = accent[std::min(k++, accent.size() - 1)];
        x += font.draw(out, x, baseline, text.substr(i, 1), size, c) + spacing;
    }
}
// The same, centred on cx.
void name(Canvas& out, const Font& font, const std::string& text, double cx, int baseline, int size, int spacing,
          Rgb ink, const std::vector<Rgb>& accent) {
    if (!font.loaded() || text.empty()) return;
    name_from(out, font, text, int(cx - spaced_width(font, text, size, spacing) / 2.0), baseline, size, spacing, ink,
              accent);
}

// ADVANCE in a sweep from violet through blue to pink; COLOR a letter each; 16-BIT in gold; 64 in red and green.
std::vector<Rgb> accent_for(const std::string& text) {
    std::string last = text.substr(text.rfind(' ') + 1);
    if (last == "COLOR")
        return {Rgb::hex(0xE8524A), Rgb::hex(0xF2B92E), Rgb::hex(0x58C35A), Rgb::hex(0x3E8DE0), Rgb::hex(0x9A62D6)};
    if (last == "16-BIT") return {Rgb::hex(0xE0B850)};
    if (last == "64") return {Rgb::hex(0xE4262C), Rgb::hex(0x22A84A)};
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

// Lettering slanted like the Game Boy's own logo: each letter drawn white on black in a scratch picture, then its
// rows shifted along by their height. Capitals and digits are heavier (drawn twice, a pixel apart).
int slanted_letter(Canvas& out, const Font& font, char ch, int x, int baseline, int size, Rgb ink, bool bold) {
    const std::string glyph(1, ch);
    const int advance = font.measure(glyph, size), pad = size / 2 + 2, w = advance + 2 * pad, h = size * 2,
              base = size + size / 3;
    Canvas scratch(w, h);
    font.draw(scratch, pad, base, glyph, size, Rgb{255, 255, 255});
    if (bold) font.draw(scratch, pad + 1, base, glyph, size, Rgb{255, 255, 255});
    for (int y = 0; y < h; ++y) {
        const double shift = (base - y) * 0.22;
        for (int i = 0; i < w; ++i) {
            const double from = i - shift;
            const int i0 = int(std::floor(from));
            const double f = from - i0;
            auto cover = [&](int sx) { return sx < 0 || sx >= w ? 0 : int(scratch.at(sx, y) & 0xFF); };
            const int a = int(cover(i0) * (1 - f) + cover(i0 + 1) * f);
            put(out, x - pad + i, baseline - base + y, ink, a * 256 / 255);
        }
    }
    return advance + (bold ? 1 : 0);
}
// Spaced slanted lettering from x; the last word in `accent` colours, a letter each (none: ink).
void slanted_from(Canvas& out, const Font& font, const std::string& text, int x, int baseline, int size, int spacing,
                  Rgb ink, const std::vector<Rgb>& accent) {
    if (!font.loaded() || text.empty()) return;
    const size_t last = accent.empty() ? std::string::npos : text.rfind(' ');
    for (size_t i = 0, k = 0; i < text.size(); ++i) {
        Rgb c = ink;
        if (last != std::string::npos && i > last) c = accent[std::min(k++, accent.size() - 1)];
        const bool heavy = std::isupper(uint8_t(text[i])) || std::isdigit(uint8_t(text[i]));
        x += slanted_letter(out, font, text[i], x, baseline, size, c, heavy) + spacing;
    }
}
int slanted_width(const Font& font, const std::string& text, int size, int spacing) {
    return spaced_width(font, text, size, spacing) + 1;
}

// The window the glass sits in: a dark frame around the game, shadowed where the bezel overhangs it, a
// little lit along its low edge, and the darkest line against the picture itself.
void recess(Canvas& out, const FrameLayout& screen, int ring, Rgb dark) {
    for (int y = screen.y - ring; y < screen.y + screen.h + ring; ++y)
        for (int x = screen.x - ring; x < screen.x + screen.w + ring; ++x) {
            const bool above = y < screen.y, below = y >= screen.y + screen.h;
            const bool left = x < screen.x, right = x >= screen.x + screen.w;
            if (!above && !below && !left && !right) continue;
            const int depth = std::max({screen.x - x, x - (screen.x + screen.w - 1), screen.y - y,
                                        y - (screen.y + screen.h - 1)}); // 1 beside the picture, ring at the rim
            Rgb c = shade(dark, above ? -10 : below ? 8 : left ? -5 : 2);
            if (depth == ring) c = shade(c, below ? 18 : above ? -6 : 4); // the step up to the bezel
            if (depth == 1) c = scale(c, 0.35);
            put(out, x, y, c, 256);
        }
}

// The power light with BATTERY under it, as printed on the Game Boy's face.
void battery_light(Canvas& out, const Font& font, double cx, double cy, double unit, int room, Rgb ink,
                   const char* label = "BATTERY") {
    power_light(out, cx, cy, unit, Rgb::hex(0xFF3B3B));
    const int size = 16, spacing = -1; // set a little tight, as the lettering is
    if (!font.loaded() || spaced_width(font, label, size, spacing) > room) return;
    name(out, font, label, cx, int(cy + 12 * unit + font.ascent(size)), size, spacing, ink, {});
}
bool gameboy_name(const Style& s) { return s.print.rfind("GAME BOY", 0) == 0; }

// The Game Boy's two lines over the screen, broken by the screen's description, across x0..x1.
void dot_matrix(Canvas& out, const Font& font, double x0, double x1, double mid, double text_cx, double unit) {
    const std::string text = "DOT MATRIX WITH STEREO SOUND";
    const int size = 16, spacing = 1; // a baked size: smaller loses strokes
    const int text_w = font.loaded() ? spaced_width(font, text, size, spacing) : 0;
    const double gap_from = text_cx - text_w / 2.0 - 8 * unit, gap_to = text_cx + text_w / 2.0 + 8 * unit;
    const Rgb lines[2] = {Rgb::hex(0x9A2A66), Rgb::hex(0x2D3A8C)};
    const double thick = std::max(2.0, 3.5 * unit), gap = 2.5 * unit;
    for (int k = 0; k < 2; ++k) {
        const double top = mid - gap / 2 - thick + k * (thick + gap);
        for (int y = int(std::round(top)); y < int(std::round(top + thick)); ++y)
            for (int x = int(x0); x < int(x1); ++x)
                if (x < gap_from || x > gap_to) put(out, x, y, lines[k], 240);
    }
    name(out, font, text, text_cx, int(mid + font.ascent(size) / 2.0), size, spacing, Rgb::hex(0xD6D7DF), {});
}

// The Game Boy, the Pocket, the Color and the Neo Geo Pocket Color: a flat bezel on the body, the glass in its
// dark window, the power light left of the screen.
void draw_face(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, unit = std::min(sx, sy);
    const Face& f = face_of(s.kind);
    const double bx = screen.x - f.left * sx, by = screen.y - f.top * sy, bw = screen.w + (f.left + f.right) * sx,
                 bh = screen.h + (f.top + f.bottom) * sy;
    body(out, s, sx, sy, 14, 0.10);
    bezel(out, s, bx, by, bw, bh, f.radius * unit, f.low_right * unit, unit);
    recess(out, screen, std::max(3, int(recess_width * unit)), scale(s.lens, 0.55));
    if (gameboy_name(s) || s.kind == Kind::Dmg)
        battery_light(out, font, bx + f.left * sx / 2 - 3 * unit, screen.y + screen.h * 0.22, unit,
                      int((f.left - 6) * sx), Rgb::hex(0xC9CAD3), s.kind == Kind::Bezel ? "POWER" : "BATTERY");
    else power_light(out, bx + f.left * sx / 2, screen.y + screen.h * 0.22, unit, Rgb::hex(0xFF3B3B));
    if (s.kind == Kind::Dmg) {
        dot_matrix(out, font, bx + 2 * unit, bx + bw - 2 * unit, by + 16 * sy, bx + bw * 0.60, unit);
    }
    if (s.kind == Kind::Bezel) {
        // The name on the bezel under the screen, COLOR a colour a letter.
        int size = int(24 * unit);
        const int spacing = std::max(1, int(2 * unit));
        if (slanted_width(font, s.print, size, spacing) > bw - 24 * unit) size = int(20 * unit);
        slanted_from(out, font, s.print,
                     int(screen.x + screen.w / 2.0 - slanted_width(font, s.print, size, spacing) / 2.0),
                     int(screen.y + screen.h + (f.bottom * sy + font.ascent(size)) / 2 + 2 * sy), size, spacing, s.ink,
                     accent_for(s.print));
    } else {
        // The name on the body under the bezel, from its left edge.
        const int size = int(28 * unit), base = int(by + bh + (f.below * sy + font.ascent(size)) / 2);
        const int word = s.kind == Kind::Dmg ? font.measure("Nintendo", 20) + int(12 * unit) : 0;
        if (word) name_from(out, font, "Nintendo", int(bx + 6 * unit), base, 20, 0, s.ink, {});
        slanted_from(out, font, s.print, int(bx + 6 * unit) + word, base, size, std::max(1, int(2 * unit)), s.ink, {});
    }
}

// Only the screen's bezel, edge to edge: the game as large as the panel allows, its window around it, the
// power light, and what the bezel says (the Game Boy's two lines, the Color's name).
void draw_inner(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy, 0);
    recess(out, screen, std::max(3, int(recess_width * unit)), scale(s.lens, 0.55));
    const double led_x = screen.x / 2.0 - 4 * unit, led_y = screen.y + screen.h * 0.16;
    battery_light(out, font, led_x, led_y, unit, int(screen.x - 8 * sx), Rgb::hex(0xC9CAD3),
                  s.kind == Kind::BezelInner ? "POWER" : "BATTERY");
    if (s.kind == Kind::DmgInner) dot_matrix(out, font, 0, out.w(), screen.y - 21 * sy, out.w() * 0.62, unit);
    if (s.kind == Kind::BezelInner) {
        const int size = 16, spacing = std::max(1, int(2 * unit)), ring = std::max(3, int(recess_width * unit));
        const int free = out.h() - (screen.y + screen.h) - ring;
        slanted_from(out, font, s.print, int(out.w() / 2.0 - slanted_width(font, s.print, size, spacing) / 2.0),
                     int(screen.y + screen.h + ring + (free + font.ascent(size)) / 2), size, spacing, s.ink,
                     accent_for(s.print));
    }
}

// The WonderSwan Color: a wide dark lens, the power light left of the screen, the name under it.
void draw_lens(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    const double lx = screen.x - lens_side * sx, ly = screen.y - lens_top * sy, lw = screen.w + 2 * lens_side * sx,
                 lh = screen.h + (lens_top + lens_bottom) * sy;
    body(out, s, sx, sy, 14, 0.10);
    lens(out, s, lx, ly, lw, lh, lens_radius * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    power_light(out, lx + lens_side * sx / 2, screen.y + screen.h * 0.28, unit, Rgb::hex(0x4CFA7A));
    const int size = int(16 * unit);
    name(out, font, s.print, screen.x + screen.w / 2.0,
         int(screen.y + screen.h + (lens_bottom * sy + font.ascent(size)) / 2), size, std::max(1, int(3 * unit)), s.ink,
         {});
    // The speaker grille, low on the right of the body.
    const double grille_x = lx + lw + (w - (lx + lw)) / 2, grille_y = ly + lh - 40 * sy;
    for (int row = 0; row < 6; ++row)
        for (int col = -1; col <= 1; ++col) {
            double hx = grille_x + col * 9 * unit + (row % 2) * 4.5 * unit, hy = grille_y + row * 8 * unit;
            if (hx > w - 8 * sx) continue;
            Rgb at =
                Rgb::from_px(out.data()[size_t(std::clamp(int(hy), 0, h - 1)) * w + std::clamp(int(hx), 0, w - 1)]);
            disc(out, hx, hy + 0.8 * unit, 2.2 * unit, shade(at, 30), 120);
            disc(out, hx, hy, 2.2 * unit, scale(at, 0.40), 256);
        }
}

// Text laid along a rail, reading upward, from the same crisp atlases: drawn level in a scratch picture, then turned.
void rail_text(Canvas& out, const Font& font, const std::string& text, double cx, double cy, int size, int spacing,
               Rgb ink, const std::vector<Rgb>& accent = {}) {
    if (!font.loaded() || text.empty()) return;
    const int w = spaced_width(font, text, size, spacing) + 4, h = size * 2, base = size + size / 3;
    Canvas colour(w, h), cover(w, h);
    colour.fill(ink);
    name_from(colour, font, text, 2, base, size, spacing, ink, accent);
    name_from(cover, font, text, 2, base, size, spacing, Rgb{255, 255, 255}, {});
    const int left = int(cx - base + font.ascent(size) / 2.0), bottom = int(cy + w / 2.0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            put(out, left + y, bottom - x, Rgb::from_px(colour.at(x, y)), int(cover.at(x, y) & 0xFF) * 256 / 255);
}
// The largest baked size whose lettering fits `room` (0: not even the smallest).
int size_fitting(const Font& font, const std::string& text, int spacing, double room) {
    int best = 0;
    for (const int size : text_sizes)
        if (spaced_width(font, text, size, spacing) <= room) best = size;
    return best;
}
// Lines moulded into the shell: each a dark groove with light on its low edge.
void ribs(Canvas& out, double x, double y, double w, double h, double pitch, Rgb on, bool across = true) {
    const double length = across ? w : h, room = across ? h : w;
    for (int i = 0; (i + 0.5) * pitch < room; ++i) {
        const double at = (across ? y : x) + i * pitch;
        const double dark_x = across ? x : at, dark_y = across ? at : y, tw = across ? length : 2,
                     th = across ? 2 : length;
        rounded(out, dark_x + (across ? 0 : 1), dark_y + (across ? 1 : 0), tw, th, 1, shade(on, 24), 150);
        rounded(out, dark_x, dark_y, tw, th, 1, scale(on, 0.42), 230);
    }
}
// A round button: its well, the cap lit from above, a speck of shine.
void cap(Canvas& out, double x, double y, double r, Rgb c, double unit) {
    disc(out, x, y + 1.5 * unit, r + 1.5 * unit, Rgb{0, 0, 0}, 170);
    disc(out, x, y, r, scale(c, 0.78), 256);
    disc(out, x, y - r * 0.10, r * 0.86, c, 256);
    disc(out, x - r * 0.30, y - r * 0.36, r * 0.24, mix(c, Rgb{255, 255, 255}, 170), 190);
}
// A plus-shaped pad in its round dish.
void dpad(Canvas& out, double x, double y, double r, Rgb c, Rgb dish, double unit) {
    disc(out, x, y, r * 1.22, dish, 256);
    const double t = r * 0.66;
    for (const double shift : {1.6 * unit, 0.0}) {
        const Rgb k = shift > 0 ? Rgb{0, 0, 0} : c;
        const int a = shift > 0 ? 150 : 256;
        rounded(out, x - r, y - t / 2 + shift, 2 * r, t, 2 * unit, k, a);
        rounded(out, x - t / 2, y - r + shift, t, 2 * r, 2 * unit, k, a);
    }
    disc(out, x, y, t * 0.30, scale(c, 0.68), 256);
}
// A rectangular push button in a dark surround.
void push(Canvas& out, double x, double y, double w, double h, Rgb c, double unit) {
    rounded(out, x - 2 * unit, y - 2 * unit, w + 4 * unit, h + 4 * unit, 2.5 * unit, Rgb::hex(0x0C0C0E));
    rounded(out, x, y, w, h, 1.5 * unit, scale(c, 0.8));
    rounded(out, x, y, w, h - 2 * unit, 1.5 * unit, c);
}
// A socket the controller plugs into.
void port(Canvas& out, double x, double y, double w, double h, double unit) {
    rounded(out, x, y, w, h, 3 * unit, Rgb::hex(0x050506));
    rounded(out, x + 2 * unit, y + 2 * unit, w - 4 * unit, h - 4 * unit, 2 * unit, Rgb::hex(0x26262A));
}
Rgb ink_on(Rgb ground) { return ground.r + ground.g + ground.b > 400 ? Rgb::hex(0x24242A) : Rgb::hex(0xE6E6EA); }

// What a home console frame shares: the shell, the game seated in it, and the rails and bands left around the game.
struct Front {
    double rail, above, below, unit;
    int w, h;
};
Front console_front(Canvas& out, const FrameLayout& screen, const Style& s) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0;
    body(out, s, sx, sy, 10, 0.12);
    return {double(screen.x), double(screen.y), double(out.h() - screen.y - screen.h),
            std::min(sx, sy), out.w(),          out.h()};
}
// The game's seat, drawn last so nothing on the shell runs into the picture.
void seat(Canvas& out, const FrameLayout& screen, double unit) {
    recess(out, screen, std::max(3, int(5 * unit)), Rgb::hex(0x121214));
}
// Fills a rectangle of the shell with one colour, lit along its top edge.
void shell_band(Canvas& out, double x, double y, double w, double h, Rgb c) {
    for (int py = std::max(0, int(y)); py < std::min(out.h(), int(y + h)); ++py)
        for (int px = std::max(0, int(x)); px < std::min(out.w(), int(x + w)); ++px) {
            Rgb k = mix(c, scale(c, 0.86), int(256 * (py - y) / std::max(1.0, h)));
            if (py == int(y)) k = shade(k, 28);
            out.data()[size_t(py) * out.w() + px] = k.px();
        }
}

// The NES: a pale grey case over a dark front; POWER and RESET with the red light and the two sockets on the left,
// the ribbed top and the name on the right.
void front_nes(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, split = f.h * 0.56, cx = f.rail / 2, rx = f.w - f.rail / 2;
    const Rgb dark = Rgb::hex(0x2B2B2E);
    shell_band(out, 0, split, f.w, f.h - split, dark);
    for (const double x : {0.0, f.w - f.rail})
        ribs(out, x + 6 * u, 18 * u, f.rail - 12 * u, split - 34 * u, 9 * u, s.top);
    rounded(out, 0, split - 7 * u, f.w, 4 * u, 0, Rgb::hex(0xC8202A));
    const double bw = std::min(f.rail - 22 * u, 32 * u);
    disc(out, cx, split + 26 * u, 3.5 * u, Rgb::hex(0xFF3B3B), 256);
    disc(out, cx, split + 26 * u, 8 * u, Rgb::hex(0xFF3B3B), 46);
    push(out, cx - bw / 2, split + 42 * u, bw, 15 * u, Rgb::hex(0xC8C6BF), u);
    push(out, cx - bw / 2, split + 68 * u, bw, 15 * u, Rgb::hex(0xC8C6BF), u);
    port(out, cx - bw / 2, split + 100 * u, bw, 36 * u, u);
    port(out, cx - bw / 2, split + 146 * u, bw, 36 * u, u);
    const std::string word = "Nintendo";
    if (const int size = size_fitting(font, word, 1, (f.h - split) * 0.7)) {
        rounded(out, rx - 13 * u, split + 34 * u, 26 * u, spaced_width(font, word, size, 1) + 20 * u, 4 * u,
                Rgb::hex(0xC8202A));
        rail_text(out, font, word, rx, split + 44 * u + spaced_width(font, word, size, 1) / 2.0, size, 1,
                  Rgb::hex(0xF4F4F4));
    }
    seat(out, screen, u);
}

// The Super Nintendo: two greys, its sliders and four-colour mark on the left, its sockets and the pad's four coloured
// buttons on the right.
void front_snes(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, cx = f.rail / 2, rx = f.w - f.rail / 2, r = std::min(f.rail / 7.0, 8.5 * u);
    const Rgb low = Rgb::hex(0x8F8D96), plum = Rgb::hex(0x4F4A78);
    shell_band(out, 0, f.h - std::max(f.below, 6 * u), f.w, std::max(f.below, 6 * u), low);
    const double sw = std::min(f.rail - 24 * u, 28 * u);
    rounded(out, cx - sw / 2, 18 * u, sw, 62 * u, 3 * u, scale(low, 0.5));
    rounded(out, cx - sw / 2 + 3 * u, 24 * u, sw - 6 * u, 22 * u, 2 * u, Rgb::hex(0xE4E2E8));
    rounded(out, cx - sw / 2, 90 * u, sw, 40 * u, 3 * u, scale(low, 0.5));
    rounded(out, cx - sw / 2 + 3 * u, 95 * u, sw - 6 * u, 18 * u, 2 * u, plum);
    const std::string word = "SUPER NINTENDO";
    if (const int size = std::min(size_fitting(font, word, 2, f.h - 210 * u), int(f.rail * 0.52)))
        rail_text(out, font, word, cx, 273 * u, Font::snap(size), 2, plum);
    const Rgb dots[4] = {Rgb::hex(0xD8282E), Rgb::hex(0xF2C21E), Rgb::hex(0x2DA84A), Rgb::hex(0x2F62C8)};
    for (int i = 0; i < 4; ++i) disc(out, cx, f.h - 58 * u + i * 10 * u, 3.6 * u, dots[i], 256);
    ribs(out, f.w - f.rail + 8 * u, 18 * u, f.rail - 16 * u, 74 * u, 9 * u, s.top);
    port(out, rx - sw / 2, 108 * u, sw, 44 * u, u);
    port(out, rx - sw / 2, 162 * u, sw, 44 * u, u);
    const double py = f.h - 84 * u, gap = r * 1.5;
    disc(out, rx, py, gap + r + 5 * u, scale(low, 0.72), 256);
    cap(out, rx, py - gap, r, dots[2 + 1], u); // X, blue
    cap(out, rx + gap, py, r, dots[0], u);     // A, red
    cap(out, rx, py + gap, r, dots[1], u);     // B, yellow
    cap(out, rx - gap, py, r, dots[2], u);     // Y, green
    seat(out, screen, u);
}

// The Mega Drive: gloss black, the gold ring of its cartridge door running behind the game, HIGH DEFINITION GRAPHICS
// over it, the name with 16-BIT in gold under it, the red light and volume slider on the right.
void front_md(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, mx = f.w / 2.0, my = f.h / 2.0;
    const Rgb gold = Rgb::hex(0xCDAA55);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            const double d = std::hypot(x + 0.5 - mx, y + 0.5 - my);
            const double a = std::abs(d - 322 * u), b = std::abs(d - 336 * u), c = std::abs(d - 366 * u);
            if (a < 1.3 * u) put(out, x, y, gold, 225);
            else if (b < 5 * u) put(out, x, y, Rgb{0, 0, 0}, 150);
            else if (c < 0.8 * u) put(out, x, y, scale(gold, 0.55), 180);
        }
    const double rail_ribs = std::max(4.0, f.rail - 16 * u);
    ribs(out, 8 * u, f.h * 0.34, rail_ribs, f.h * 0.32, 8 * u, s.top);
    ribs(out, f.w - f.rail + 8 * u, f.h * 0.46, rail_ribs, f.h * 0.18, 8 * u, s.top);
    const double rx = f.w - f.rail / 2;
    disc(out, rx, f.h * 0.28, 3.5 * u, Rgb::hex(0xFF2A2A), 256);
    disc(out, rx, f.h * 0.28, 8 * u, Rgb::hex(0xFF2A2A), 55);
    rounded(out, rx - 3 * u, f.h * 0.33, 6 * u, 46 * u, 3 * u, Rgb::hex(0x050506));
    rounded(out, rx - 7 * u, f.h * 0.33 + 24 * u, 14 * u, 8 * u, 2 * u, Rgb::hex(0x77777C));
    const std::string top = "HIGH DEFINITION GRAPHICS";
    if (f.above >= 22 * u) {
        const int base = font.centred(16, int(f.above / 2));
        const int tw = spaced_width(font, top, 16, 2);
        rounded(out, mx - tw / 2.0 - 10 * u, f.above / 2 - 11 * u, tw + 20 * u, 22 * u, 0, s.top);
        name(out, font, top, mx, base, 16, 2, gold, {});
    }
    if (f.below >= 26 * u) {
        const int size = f.below >= 40 * u ? 24 : 20, base = font.centred(size, int(f.h - f.below / 2));
        const int mw = spaced_width(font, "MEGA DRIVE", size, 3), gw = spaced_width(font, "16-BIT", size, 3);
        rounded(out, mx - (mw + gw + 34) / 2.0 - 10 * u, f.h - f.below + 2 * u, mw + gw + 54, f.below - 4 * u, 0,
                s.top);
        const int x0 = int(mx - (mw + gw + 34) / 2.0);
        name_from(out, font, "MEGA DRIVE", x0, base, size, 3, Rgb::hex(0xEDEDED), {});
        name_from(out, font, "16-BIT", x0 + mw + 34, base, size, 3, gold, {});
    }
    seat(out, screen, u);
}

// The Master System: black with its fine white grid, the red band with the name, the power and pause buttons.
void front_sms(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, pitch = 16 * u, band_h = std::max(f.below * 0.82, 10 * u), band_y = f.h - band_h - 2 * u;
    const Rgb red = Rgb::hex(0xD0202A);
    for (int y = 0; y < int(band_y); ++y)
        for (int x = 0; x < f.w; ++x)
            if (std::fmod(x, pitch) < 1 || std::fmod(y, pitch) < 1) put(out, x, y, Rgb::hex(0xE8E8EC), 56);
    shell_band(out, 0, band_y, f.w, band_h, red);
    if (const int size = size_fitting(font, "MASTER SYSTEM", 4, f.w * 0.6); size && band_h >= 28 * u) {
        const int shown = std::min(size, band_h >= 40 * u ? 28 : 20);
        name(out, font, "MASTER SYSTEM", f.w / 2.0, font.centred(shown, int(band_y + band_h / 2)), shown, 4,
             Rgb::hex(0xF6F6F8), {});
    }
    if (f.above >= 28 * u) {
        const int size = f.above >= 40 * u ? 28 : 20, tw = spaced_width(font, "SEGA", size, 5);
        rounded(out, f.w / 2.0 - tw / 2.0 - 12 * u, f.above / 2 - size * 0.66, tw + 24 * u, size * 1.32, 2 * u, s.top);
        name(out, font, "SEGA", f.w / 2.0, font.centred(size, int(f.above / 2)), size, 5, Rgb::hex(0xF0F0F0), {});
    }
    const double cx = f.rail / 2, rx = f.w - f.rail / 2, bw = std::min(f.rail - 22 * u, 30 * u);
    push(out, cx - bw / 2, f.h * 0.44, bw, 16 * u, Rgb::hex(0x3A3A3E), u);
    disc(out, cx, f.h * 0.40, 3.5 * u, Rgb::hex(0xFF2A2A), 256);
    push(out, rx - bw / 2, f.h * 0.44, bw, 16 * u, red, u);
    seat(out, screen, u);
}

// The PlayStation: pale grey, its round Open button, the pad's four
// shapes in their colours, the name under the game.
void front_psx(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, mx = f.w / 2.0, cx = f.rail / 2, rx = f.w - f.rail / 2;
    const double open_r = std::min(f.rail * 0.34, 18 * u);
    disc(out, cx, 78 * u, open_r + 2.5 * u, scale(s.bottom, 0.62), 256);
    disc(out, cx, 78 * u, open_r, s.top, 256);
    disc(out, cx, 76 * u, open_r - 2 * u, shade(s.top, 10), 256);
    cap(out, cx, 150 * u, std::min(f.rail * 0.18, 8 * u), s.bottom, u);
    cap(out, cx, 182 * u, std::min(f.rail * 0.18, 8 * u), s.bottom, u);
    ribs(out, 8 * u, f.h * 0.58, f.rail - 16 * u, f.h * 0.3, 8 * u, s.top);
    // The four shapes, down the right-hand rail.
    const double r = std::min(f.rail * 0.26, 11 * u), t = std::max(2.0, r * 0.28);
    const auto ring = [&](double y, auto inside) {
        disc(out, rx, y, r + 5 * u, Rgb::hex(0x3B3B40), 256);
        inside(y);
    };
    ring(f.h * 0.34, [&](double y) { // triangle, green
        const Rgb c = Rgb::hex(0x3FC88C);
        for (int py = int(y - r); py <= int(y + r); ++py)
            for (int px = int(rx - r - 1); px <= int(rx + r + 1); ++px) {
                const double fy = (py - (y - r)) / (2 * r), half = fy * r * 1.05, row = std::abs(px + 0.5 - rx);
                if (fy >= 0 && fy <= 1 && row <= half && (row >= half - t * 1.25 || fy > 1 - t / r / 1.8))
                    put(out, px, py, c, 256);
            }
    });
    ring(f.h * 0.46, [&](double y) { // circle, red
        disc(out, rx, y, r * 0.86, Rgb::hex(0xF0555A), 256);
        disc(out, rx, y, r * 0.86 - t, Rgb::hex(0x3B3B40), 256);
    });
    ring(f.h * 0.58, [&](double y) { // cross, blue
        for (int py = int(y - r); py <= int(y + r); ++py)
            for (int px = int(rx - r); px <= int(rx + r); ++px) {
                const double dx = px + 0.5 - rx, dy = py + 0.5 - y;
                if (std::abs(std::abs(dx) - std::abs(dy)) < t && std::abs(dx) < r * 0.8)
                    put(out, px, py, Rgb::hex(0x6E9BE8), 256);
            }
    });
    ring(f.h * 0.70, [&](double y) { // square, pink
        const double e = r * 0.72;
        rounded(out, rx - e, y - e, 2 * e, 2 * e, 0, Rgb::hex(0xEE8FD0));
        rounded(out, rx - e + t, y - e + t, 2 * e - 2 * t, 2 * e - 2 * t, 0, Rgb::hex(0x3B3B40));
    });
    if (f.below >= 24 * u) {
        const int size = f.below >= 36 * u ? 24 : 20;
        name(out, font, "PlayStation", mx, font.centred(size, int(f.h - f.below / 2)), size, 1, Rgb::hex(0x3A3A40), {});
    }
    seat(out, screen, u);
}

// The Nintendo 64: charcoal, its four sockets under the game, the pad's stick and red Start on the left, the yellow C
// buttons with blue A and green B on the right.
void front_n64(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, cx = f.rail / 2, rx = f.w - f.rail / 2, r = std::min(f.rail * 0.13, 6.5 * u);
    const Rgb red = Rgb::hex(0xE0322E), green = Rgb::hex(0x2BAA52), blue = Rgb::hex(0x2F6BDE),
              yellow = Rgb::hex(0xF4BE1E);
    ribs(out, 8 * u, 14 * u, f.rail - 16 * u, 70 * u, 8 * u, s.top);
    ribs(out, f.w - f.rail + 8 * u, 14 * u, f.rail - 16 * u, 70 * u, 8 * u, s.top);
    const double stick = std::min(f.rail * 0.34, 18 * u), sy = f.h * 0.36;
    disc(out, cx, sy, stick + 3 * u, Rgb::hex(0x0E0E10), 256);
    cap(out, cx, sy, stick * 0.66, Rgb::hex(0x8E9096), u);
    cap(out, cx, sy + 62 * u, r * 1.1, red, u);
    const double gap = r * 1.5, cy = f.h * 0.32;
    for (int i = 0; i < 4; ++i)
        cap(out, rx + (i == 0 ? gap : i == 1 ? -gap : 0), cy + (i == 2 ? -gap : i == 3 ? gap : 0), r * 0.8, yellow, u);
    cap(out, rx + r * 0.3, cy + 62 * u, r * 1.25, blue, u);
    cap(out, rx - r * 0.6, cy + 92 * u, r * 1.1, green, u);
    if (f.below >= 24 * u) {
        const int size = f.below >= 36 * u ? 24 : 20, spacing = 3, base = font.centred(size, int(f.h - f.below / 2));
        const int nw = spaced_width(font, "NINTENDO", size, spacing), dw = spaced_width(font, "64", size, spacing);
        const int x0 = int(f.w / 2.0 - (nw + dw + 12) / 2.0);
        name_from(out, font, "NINTENDO", x0, base, size, spacing, Rgb::hex(0xE4E4EA), {});
        const int d = font.draw(out, x0 + nw + 12, base, "6", size, red) + spacing;
        font.draw(out, x0 + nw + 12 + d, base, "4", size, green);
        // The four sockets, either side of the name.
        const double pw = 20 * u, py = f.h - f.below / 2 - 9 * u;
        for (int i = 0; i < 2; ++i) {
            port(out, x0 - 66 * u + i * 28 * u, py, pw, 18 * u, u);
            port(out, x0 + nw + dw + 12 + 18 * u + i * 28 * u, py, pw, 18 * u, u);
        }
    }
    seat(out, screen, u);
}

// The ZX Spectrum: black with rows of grey rubber keys, the name, and the four stripes across the low right corner.
void front_zx(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit;
    rounded(out, 0, 0, f.w, 3 * u, 0, Rgb::hex(0xB6B8BE));
    const Rgb rubber = Rgb::hex(0x8A929C);
    const double kw = std::max(10.0, (f.rail - 16 * u)), kh = 15 * u;
    for (const double x : {8 * u, f.w - f.rail + 8 * u})
        for (int i = 0; i < 11; ++i) {
            if (x > f.rail && i > 6) break;
            const double ky = 22 * u + i * (kh + 11 * u);
            rounded(out, x, ky + 2 * u, kw, kh, 2.5 * u, Rgb{0, 0, 0}, 200);
            rounded(out, x, ky, kw, kh, 2.5 * u, rubber);
            rounded(out, x + 1.5 * u, ky + 1 * u, kw - 3 * u, 3 * u, 1.5 * u, shade(rubber, 26));
        }
    const Rgb stripes[4] = {Rgb::hex(0xD8282E), Rgb::hex(0xF2C21E), Rgb::hex(0x2AA84A), Rgb::hex(0x22A6D8)};
    const double sw = 9 * u, across = f.w + f.h * 0.68; // the four run up to the right, x + y constant
    for (int y = f.h / 2; y < f.h; ++y)
        for (int x = f.w / 2; x < f.w; ++x) {
            const int k = int(std::floor((x + y - across) / sw));
            if (k >= 0 && k < 4) put(out, x, y, stripes[k], 256);
        }
    if (f.below >= 20 * u) {
        const int size = f.below >= 34 * u ? 24 : 20, base = font.centred(size, int(f.h - f.below / 2));
        name_from(out, font, "ZX Spectrum", int(f.rail + 4 * u), base, size, 1, Rgb::hex(0xE8E8EC), {});
    }
    if (f.above >= 20 * u)
        name_from(out, font, "sinclair", int(f.rail + 4 * u), font.centred(20, int(f.above / 2 + 2 * u)), 20, 1,
                  Rgb::hex(0xE8E8EC), {});
    seat(out, screen, u);
}

// The Commodore 64: the bread bin's beige, moulded lines, the red light, brown function keys, the dark badge with its
// rainbow.
void front_c64(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit, cx = f.rail / 2, rx = f.w - f.rail / 2;
    ribs(out, 7 * u, 18 * u, f.rail - 14 * u, f.h * 0.44, 8 * u, s.top);
    disc(out, cx, f.h * 0.62, 3.5 * u, Rgb::hex(0xFF3030), 256);
    disc(out, cx, f.h * 0.62, 8 * u, Rgb::hex(0xFF3030), 60);
    const Rgb key = Rgb::hex(0x5A4A3A);
    const double kw = std::max(12.0, f.rail - 12 * u);
    for (int i = 0; i < 4; ++i) {
        const double ky = f.h * 0.58 + i * 30 * u;
        rounded(out, rx - kw / 2 - 2 * u, ky - 2 * u, kw + 4 * u, 26 * u, 3 * u, Rgb::hex(0x2A241E));
        rounded(out, rx - kw / 2, ky, kw, 20 * u, 2.5 * u, key);
        rounded(out, rx - kw / 2 + 2 * u, ky + 2 * u, kw - 4 * u, 12 * u, 2 * u, shade(key, 22));
    }
    ribs(out, f.w - f.rail + 7 * u, 18 * u, f.rail - 14 * u, f.h * 0.44, 8 * u, s.top);
    if (f.below >= 24 * u) {
        const double bh = f.below - 8 * u, by = f.h - f.below + 4 * u, bw = 300 * u, bx = (f.w - bw) / 2;
        rounded(out, bx, by, bw, bh, 3 * u, Rgb::hex(0x3A3026));
        const int size = 20, base = font.centred(size, int(by + bh / 2));
        const int tw = spaced_width(font, "commodore", size, 1);
        name_from(out, font, "commodore", int(bx + 12 * u), base, size, 1, Rgb::hex(0xEDE6D6), {});
        const Rgb rainbow[5] = {Rgb::hex(0xD8282E), Rgb::hex(0xEE8A1E), Rgb::hex(0xF2C21E), Rgb::hex(0x2AA84A),
                                Rgb::hex(0x2A6FD8)};
        const double sx = bx + 12 * u + tw + 10 * u, stripe = bh * 0.7 / 5, sy0 = by + bh * 0.15;
        const double sw = bx + bw - 44 * u - sx;
        for (int i = 0; i < 5; ++i) rounded(out, sx, sy0 + i * stripe, sw, stripe, 0, rainbow[i]);
        name_from(out, font, "64", int(bx + bw - 36 * u), base, size, 1, Rgb::hex(0xEDE6D6), {});
    }
    seat(out, screen, u);
}

// Any other console: its colours, a stripe down each rail in its accent, the name along the left one.
void front_plain(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const Front f = console_front(out, screen, s);
    const double u = f.unit;
    const Rgb ground = s.wood == 2 ? scale(s.top, 0.8) : s.top;
    const double stripe = std::max(3.0, f.rail * 0.08);
    const double rail_room = f.h - 160 * u;
    int size = 16;
    for (const int k : text_sizes)
        if (spaced_width(font, s.print, k, 3) <= rail_room && k <= f.rail * 0.62) size = k;
    rounded(out, f.rail - 8 * u - stripe, 0, stripe, f.h, 0, s.line);
    rounded(out, f.w - f.rail + 14 * u, 0, stripe, f.h, 0, s.line);
    rounded(out, f.w - f.rail + 14 * u + stripe + 3 * u, 0, std::max(1.0, stripe / 2), f.h, 0, s.band, 200);
    rail_text(out, font, s.print, (f.rail - 8 * u - stripe) / 2, f.h / 2.0, size, 3, ink_on(ground));
    ribs(out, f.w - f.rail + 14 * u + 2 * stripe + 8 * u, f.h * 0.12,
         std::max(6.0, f.rail - 14 * u - 2 * stripe - 16 * u), f.h * 0.34, 8 * u, s.top);
    power_light(out, f.w - (f.rail - 14 * u - 2 * stripe) / 2, f.h * 0.82, u, Rgb::hex(0xFF3B3B));
    seat(out, screen, u);
}

void draw_console(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    switch (s.badge) {
    case Badge::Nes: front_nes(out, screen, s, font); break;
    case Badge::Snes: front_snes(out, screen, s, font); break;
    case Badge::Md: front_md(out, screen, s, font); break;
    case Badge::Sms: front_sms(out, screen, s, font); break;
    case Badge::Psx: front_psx(out, screen, s, font); break;
    case Badge::N64: front_n64(out, screen, s, font); break;
    case Badge::Zx: front_zx(out, screen, s, font); break;
    case Badge::C64: front_c64(out, screen, s, font); break;
    case Badge::None: front_plain(out, screen, s, font); break;
    }
}

// The handhelds held sideways, whole on the panel: the shell, the dark lens around the screen, the controls beside it.
void draw_wide_handheld(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, u = std::min(sx, sy);
    const double lens_x = s.kind == Kind::Wsc ? 12 * u : 16 * u, above = s.kind == Kind::Wsc ? 18 * u : 12 * u;
    const double name_h = out.h() - screen.y - screen.h >= 60 * u ? 40 * u : 26 * u;
    body(out, s, sx, sy, 12, 0.12);
    lens(out, s, screen.x - lens_x, screen.y - above, screen.w + 2 * lens_x, screen.h + above + name_h, 10 * u, u);
    recess(out, screen, std::max(3, int(3 * u)), scale(s.lens, 0.6));
    const double left = (screen.x - lens_x) / 2, right = out.w() - left, cy = screen.y + screen.h * 0.46;
    const double r = std::min(left * 0.5, 24 * u);
    const Rgb dish = shade(s.bottom, -18), dark = Rgb::hex(0x1C1C20);
    const int base = font.centred(16, int(screen.y + screen.h + name_h / 2));
    if (s.kind == Kind::Gg) {
        dpad(out, left, cy, r, dark, dish, u);
        cap(out, right + r * 0.52, cy - r * 0.42, r * 0.5, Rgb::hex(0x3A4BA8), u);
        cap(out, right - r * 0.56, cy + r * 0.42, r * 0.5, Rgb::hex(0x3A4BA8), u);
        cap(out, right - 2 * u, cy - r * 1.7, r * 0.26, Rgb::hex(0x3A4BA8), u);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 5; ++col)
                if (right - 28 * u + col * 11 * u < out.w())
                    disc(out, right - 22 * u + col * 11 * u, cy + r * 1.8 + row * 11 * u, 3 * u, Rgb{0, 0, 0}, 220);
        power_light(out, left, cy - r * 1.9, u, Rgb::hex(0xFF3B3B));
        const int tw = spaced_width(font, s.print, 20, 3);
        name(out, font, s.print, screen.x + screen.w / 2.0, font.centred(20, int(screen.y + screen.h + name_h / 2)), 20,
             3, s.ink, {});
        const Rgb bars[3] = {Rgb::hex(0xD8282E), Rgb::hex(0xF2C21E), Rgb::hex(0x2F62C8)};
        for (int i = 0; i < 3; ++i)
            rounded(out, screen.x + screen.w / 2.0 + tw / 2.0 + 12 * u + i * 7 * u,
                    screen.y + screen.h + name_h / 2 - 7 * u, 5 * u, 14 * u, 1, bars[i]);
        name(out, font, "SEGA", screen.x + screen.w / 2.0, font.centred(16, int(screen.y / 2 - above / 2 + 2 * u)), 16,
             4, s.bottom.r > 128 ? Rgb::hex(0x25252A) : Rgb::hex(0xE8E8EC), {});
    } else if (s.kind == Kind::Ngpc) {
        // The clicking stick in its round well, A over B set on a slant.
        disc(out, left, cy, r * 1.05, dish, 256);
        disc(out, left, cy, r * 0.82, scale(dish, 0.65), 256);
        cap(out, left, cy, r * 0.56, Rgb::hex(0x9CA0A8), u);
        cap(out, right + r * 0.5, cy - r * 0.5, r * 0.52, dark, u);
        cap(out, right - r * 0.5, cy + r * 0.5, r * 0.52, dark, u);
        power_light(out, screen.x - lens_x / 2, screen.y + 24 * u, u, Rgb::hex(0x4CFA7A));
        name(out, font, s.print, screen.x + screen.w / 2.0, base, 16, 2, s.ink, accent_for(s.print));
    } else {
        // The WonderSwan: X pad and Y pad in the shell's own colour, A and B on the right, the speaker under them.
        const Rgb pad = shade(s.bottom, -40);
        dpad(out, left, cy - r * 0.2, r * 0.8, pad, dish, u);
        dpad(out, left, cy - r * 2.5, r * 0.62, pad, dish, u);
        cap(out, right + r * 0.46, cy - r * 0.46, r * 0.48, pad, u);
        cap(out, right - r * 0.46, cy + r * 0.46, r * 0.48, pad, u);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                disc(out, right - 11 * u + col * 11 * u, cy + r * 2 + row * 11 * u, 3 * u, scale(s.bottom, 0.4), 240);
        power_light(out, screen.x + screen.w + lens_x / 2, screen.y + 20 * u, u, Rgb::hex(0x4CFA7A));
        name(out, font, s.print, screen.x + screen.w / 2.0, base, 16, 2, s.ink, accent_for(s.print));
    }
}

// The Game Boy Advance, whole: the wide shell filling the panel with its low corners rounded off and its shoulder
// buttons, the dark lens with the name under the screen, the pad and START/SELECT on the left, A and B and the speaker
// on the right.
void draw_agb(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, u = std::min(sx, sy);
    const double lens_x = 14 * sx, lens_top = 16 * sy, lens_bottom = 46 * sy;
    const double lx = screen.x - lens_x, ly = screen.y - lens_top, lw = screen.w + 2 * lens_x,
                 lh = screen.h + lens_top + lens_bottom;
    const Rgb room = Rgb::hex(0x121216);
    for (int y = 0; y < h; ++y) {
        const double t = double(y) / std::max(1, h - 1);
        const Rgb row = mix(s.top, s.bottom, int(210 * t));
        for (int x = 0; x < w; ++x) {
            const double d = rounded_distance(x + 0.5, y + 0.5, 0, -40 * u, w, h + 40 * u, 58 * u);
            Rgb c = scale(row, 0.90 + 0.10 * std::sqrt(std::clamp(-d / (22 * u), 0.0, 1.0)));
            if (y < 12 * sy) c = mix(c, Rgb{255, 255, 255}, int(30 * (1 - y / (12 * sy))));
            const uint32_t n = hash(x, y);
            if (s.speckle && (n & 7) == 0) c = shade(c, int(n >> 8) % (2 * s.speckle + 1) - s.speckle);
            out.data()[size_t(y) * w + x] = mix(room, c, int(256 * std::clamp(0.5 - d, 0.0, 1.0))).px();
        }
    }
    // The shoulders: L and R in the top corners, a shade darker, parted from the shell by a seam.
    for (const double x : {0.0, w - 72.0 * u}) {
        rounded(out, x, -8 * u, 72 * u, 24 * u, 8 * u, scale(s.bottom, 0.72));
        rounded(out, x + 4 * u, -8 * u, 64 * u, 20 * u, 6 * u, scale(s.top, 0.82));
    }
    lens(out, s, lx, ly, lw, lh, 12 * u, u);
    recess(out, screen, std::max(3, int(3 * u)), scale(s.lens, 0.6));
    const double left = lx / 2, right = w - left, cy = screen.y + screen.h * 0.42;
    const double r = std::min(left * 0.68, 25 * u);
    const Rgb dark = shade(s.bottom, -60);
    dpad(out, left, cy, r, s.bottom.r + s.bottom.g + s.bottom.b > 420 ? Rgb::hex(0x34343A) : Rgb::hex(0x17171B),
         s.bottom.r + s.bottom.g + s.bottom.b > 200 ? scale(s.bottom, 0.82) : shade(s.bottom, 30), u);
    const Rgb ab = s.bottom.r + s.bottom.g + s.bottom.b > 420 ? Rgb::hex(0x4A4A52) : shade(s.top, 34);
    cap(out, right + r * 0.28, cy - r * 0.46, r * 0.46, ab, u);
    cap(out, right - r * 0.42, cy + r * 0.40, r * 0.46, ab, u);
    power_light(out, left, cy - r * 2.0, u, Rgb::hex(0x4CFA7A));
    // START and SELECT, low on the left; the speaker's holes, low on the right.
    const double below = ly + lh + (h - ly - lh) / 2;
    for (int i = 0; i < 2; ++i) rounded(out, lx + 4 * u + i * 44 * u, below - 4 * u, 34 * u, 8 * u, 4 * u, dark);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 8; ++col) {
            const double hx = w - 160 * u + col * 12 * u + (row % 2) * 6 * u, hy = below - 12 * u + row * 11 * u;
            disc(out, hx, hy + 0.8 * u, 3.2 * u, shade(s.bottom, 26), 120);
            disc(out, hx, hy, 3.2 * u, scale(s.bottom, 0.38), 256);
        }
    // The name on the lens, under the screen, ADVANCE in its sweep.
    const int size = 20, spacing = 2;
    slanted_from(out, font, "GAME BOY ADVANCE",
                 int(screen.x + screen.w / 2.0 - (slanted_width(font, "GAME BOY ADVANCE", size, spacing) + 12) / 2.0),
                 font.centred(size, int(screen.y + screen.h + lens_bottom / 2 + 1 * sy)), size, spacing, s.ink,
                 accent_for("GAME BOY ADVANCE"));
}

// The PlayStation Portable: the game across the panel's whole width (the real face's controls beside the screen would
// cost a quarter of the picture), gloss above and below it, SONY over the screen, the row of small buttons under it.
void draw_psp(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sy = h / 480.0, u = std::min(w / 640.0, sy), low = screen.y + screen.h;
    for (int y = 0; y < h; ++y) {
        const double t = y < screen.y ? double(y) / std::max(1, screen.y) : (y - low) / std::max(1.0, h - low);
        const Rgb c = mix(s.top, s.bottom, int(256 * std::clamp(t, 0.0, 1.0)));
        for (int x = 0; x < w; ++x) out.data()[size_t(y) * w + x] = c.px();
    }
    const bool pale = s.top.r > 128;
    const Rgb ink = pale ? Rgb::hex(0x55555C) : Rgb::hex(0xB4B4BC),
              pill = pale ? Rgb::hex(0xC8C8C4) : Rgb::hex(0x33333A);
    rounded(out, 0, 0, w, 2 * u, 0, pale ? Rgb::hex(0xFFFFFF) : Rgb::hex(0x55555E));
    recess(out, screen, std::max(2, int(3 * u)), s.lens);
    if (screen.y >= 28 * u) name(out, font, "SONY", w / 2.0, font.centred(16, screen.y / 2), 16, 5, ink, {});
    if (h - low >= 28 * u) {
        const double cy = low + (h - low) / 2;
        name(out, font, "PSP", 44 * u, font.centred(16, int(cy)), 16, 3, ink, {});
        const double pw = 40 * u, gap = 12 * u, x0 = w / 2.0 - (6 * pw + 5 * gap) / 2;
        for (int i = 0; i < 6; ++i) rounded(out, x0 + i * (pw + gap), cy - 4 * u, pw, 8 * u, 4 * u, pill);
        for (int col = 0; col < 7; ++col)
            rounded(out, w - 90 * u + col * 9 * u, cy - 8 * u, 3 * u, 16 * u, 1.5 * u, scale(s.top, pale ? 0.72 : 0.4));
    }
}

// A television: the set's front around its tube, the curved glass, and its controls under the screen (a grey
// set) or beside it (a wood-grain one, on its silver panel).
void draw_tv(Canvas& out, const FrameLayout& screen, const Style& s) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy), ring = 28 * unit, glass = 13 * unit;
    body(out, s, sx, sy);
    const Rgb front = s.kind == Kind::TvWood ? Rgb::hex(0x26262A) : scale(s.bottom, 0.82);
    panel(out, s, front, screen.x - ring, screen.y - ring, screen.w + 2 * ring, screen.h + 2 * ring, 22 * unit, unit);
    lens(out, s, screen.x - glass, screen.y - glass, screen.w + 2 * glass, screen.h + 2 * glass, 20 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    if (s.kind == Kind::TvWood) {
        // The silver panel beside the screen: two dials, the power light, the speaker.
        const double px = screen.x + screen.w + ring + 18 * unit, pw = w - px - 22 * unit;
        const double py = screen.y - ring, ph = screen.h + 2 * ring;
        panel(out, s, s.band, px, py, pw, ph, 10 * unit, unit);
        const double cx = px + pw / 2;
        knob(out, cx, py + 46 * unit, 24 * unit, s.band, unit, -40);
        knob(out, cx, py + 112 * unit, 18 * unit, s.band, unit, 70);
        power_light(out, cx, py + 150 * unit, unit, s.line);
        slots(out, px + 16 * unit, py + 172 * unit, pw - 32 * unit, int((ph - 190 * unit) / (7 * unit)), s.band, unit);
        return;
    }
    // Under the screen: the speaker on the left, the power button and its light and two small knobs on the right.
    const double cy = screen.y + screen.h + ring + (h - (screen.y + screen.h + ring)) / 2;
    slots(out, screen.x, cy - 12 * unit, 120 * unit, 4, s.bottom, unit);
    const double right = screen.x + screen.w;
    knob(out, right - 10 * unit, cy, 9 * unit, s.band, unit, 30);
    knob(out, right - 36 * unit, cy, 9 * unit, s.band, unit, -60);
    rounded(out, right - 104 * unit, cy - 8 * unit, 44 * unit, 16 * unit, 4 * unit, scale(s.band, 0.7));
    rounded(out, right - 104 * unit, cy - 9 * unit, 44 * unit, 15 * unit, 4 * unit, s.band);
    power_light(out, right - 120 * unit, cy, unit, s.line);
}

// A beige computer monitor: a deep frame around the tube, the power button and its green light under it.
void draw_monitor(Canvas& out, const FrameLayout& screen, const Style& s) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy), ring = 26 * unit, glass = 12 * unit;
    body(out, s, sx, sy);
    panel(out, s, scale(s.bottom, 0.9), screen.x - ring, screen.y - ring, screen.w + 2 * ring, screen.h + 2 * ring,
          8 * unit, unit);
    lens(out, s, screen.x - glass, screen.y - glass, screen.w + 2 * glass, screen.h + 2 * glass, 12 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    const double cy = screen.y + screen.h + ring + (h - (screen.y + screen.h + ring)) / 2;
    slots(out, screen.x + 10 * unit, cy - 9 * unit, 90 * unit, 3, s.bottom, unit);
    const double right = screen.x + screen.w;
    rounded(out, right - 44 * unit, cy - 9 * unit, 40 * unit, 18 * unit, 3 * unit, scale(s.band, 0.62));
    rounded(out, right - 44 * unit, cy - 10 * unit, 40 * unit, 17 * unit, 3 * unit, shade(s.band, -6));
    power_light(out, right - 62 * unit, cy, unit, s.line);
}

// An arcade cabinet: the lit marquee with the name over the screen, side art either side, the control panel
// with a stick and six buttons under it.
void draw_arcade(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy);
    const double glass = 10 * unit, panel_top = screen.y + screen.h + glass + 14 * unit;
    // Side art: bold stripes rising to the right, in the marquee's colour and the art's own.
    const double stripe = 16 * unit;
    for (int y = int(screen.y - glass); y < int(panel_top); ++y)
        for (int x = 0; x < w; ++x) {
            if (x > screen.x - glass - 12 * unit && x < screen.x + screen.w + glass + 12 * unit) continue;
            const int k = int(std::floor((x + y) / stripe)) & 3;
            if (k == 0) put(out, x, y, s.band, 120);
            else if (k == 2) put(out, x, y, s.line, 120);
        }
    // The marquee: a light box, brightest in the middle.
    const double mx = 24 * unit, my = 10 * unit, mw = w - 2 * mx, mh = screen.y - glass - 24 * unit;
    rounded(out, mx - 4 * unit, my - 4 * unit, mw + 8 * unit, mh + 8 * unit, 8 * unit, Rgb::hex(0x050506));
    for (int y = int(my); y < int(my + mh); ++y)
        for (int x = int(mx); x < int(mx + mw); ++x) {
            const double dx = (x - (mx + mw / 2)) / (mw / 2), dy = (y - (my + mh / 2)) / (mh / 2);
            const double lit = std::clamp(1.0 - 0.55 * (dx * dx) - 0.35 * (dy * dy), 0.0, 1.0);
            put(out, x, y, mix(scale(s.band, 0.72), mix(s.band, Rgb{255, 214, 150}, 90), int(256 * lit)), 256);
        }
    const int size = int(36 * unit), spacing = std::max(1, int(5 * unit));
    const int baseline = int(my + (mh + font.ascent(size)) / 2);
    name(out, font, s.print, w / 2.0 + 2 * unit, baseline + int(2 * unit), size, spacing, scale(s.band, 0.35), {});
    name(out, font, s.print, w / 2.0, baseline, size, spacing, s.ink, {});
    // The screen behind its glass.
    lens(out, s, screen.x - glass, screen.y - glass, screen.w + 2 * glass, screen.h + 2 * glass, 8 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    // The control panel.
    for (int y = int(panel_top); y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Rgb c =
                mix(Rgb::hex(0x2C2C33), Rgb::hex(0x141418), int((y - panel_top) * 256 / std::max(1.0, h - panel_top)));
            if (y == int(panel_top)) c = Rgb::hex(0x55555F);
            out.data()[size_t(y) * w + x] = c.px();
        }
    const double cy = panel_top + (h - panel_top) / 2;
    disc(out, w * 0.26, cy + 3 * unit, 7 * unit, Rgb::hex(0x050506), 220);
    disc(out, w * 0.26, cy - 4 * unit, 11 * unit, Rgb::hex(0xD8262C), 256);
    disc(out, w * 0.26 - 3 * unit, cy - 7 * unit, 3.5 * unit, Rgb::hex(0xFF9A9A), 200);
    const Rgb buttons[6] = {Rgb::hex(0xD8262C), Rgb::hex(0xF2C21E), Rgb::hex(0x2DA84A),
                            Rgb::hex(0x2A6FE0), Rgb::hex(0xEDEDED), Rgb::hex(0xD8262C)};
    for (int i = 0; i < 6; ++i) {
        const double bx = w * 0.56 + (i % 3) * 30 * unit + (i / 3) * 8 * unit, by = cy - 9 * unit + (i / 3) * 18 * unit;
        disc(out, bx, by + 1.5 * unit, 8 * unit, Rgb::hex(0x050506), 200);
        disc(out, bx, by, 8 * unit, buttons[i], 256);
        disc(out, bx - 2 * unit, by - 2 * unit, 2.5 * unit, mix(buttons[i], Rgb{255, 255, 255}, 170), 200);
    }
}

// The Vectrex: its own upright black screen in the black body, the name under it and the joystick on its right.
void draw_vectrex(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, unit = std::min(sx, sy), glass = 16 * unit;
    body(out, s, sx, sy);
    lens(out, s, screen.x - glass, screen.y - glass, screen.w + 2 * glass, screen.h + 2 * glass, 14 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    const int size = int(24 * unit), bottom = screen.y + screen.h + int(glass);
    name(out, font, s.print, screen.x + screen.w / 2.0, bottom + int((out.h() - bottom + font.ascent(size)) / 2), size,
         std::max(1, int(6 * unit)), s.ink, {});
    // The joystick the console has built in, on a knob in the body's right-hand corner.
    const double jx = screen.x + screen.w + glass + 34 * unit, jy = bottom + 26 * unit;
    knob(out, jx, jy, 13 * unit, s.band, unit, 20);
    // The button beside it.
    disc(out, jx, jy + 40 * unit, 8 * unit, s.band, 255);
}

void draw_bar(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font, const std::string& system) {
    out.fill(s.top);
    const int top = screen.y + screen.h;
    for (int x = 0; x < out.w(); ++x) put(out, x, top, s.rim, 256);
    const int bar = out.h() - top, size = std::clamp(bar * 9 / 20, 14, 28);
    name(out, font, system, out.w() / 2.0, top + (bar + font.ascent(size)) / 2 - 1, size, std::max(1, size / 10), s.ink,
         accent_for(system));
}

// The Game Boy Advance SP (AGS-001), open and seen from the front, to its own measurements in millimetres (the
// screen is 61 wide): a near-square glossy lid with a round bumper in each corner, a dark grey glass window holding
// the screen with the white logo under it, the hinge's barrel under the lid (its long middle knuckle between the
// lower half's two short ones) and a strip of the lower half below.
void draw_sp(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double k = screen.w / sp_screen_mm; // pixels to the millimetre
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = (x - w / 2.0) / w, dy = (y - h * 0.45) / h;
            out.data()[size_t(y) * w + x] = scale(Rgb::hex(0x1A1B20), 1 - 0.9 * (dx * dx + dy * dy)).px();
        }
    const double wx = screen.x - sp_window_side * k, ww = screen.w + 2 * sp_window_side * k;
    const double wy = screen.y - sp_window_top * k, wh = screen.h + (sp_window_top + sp_window_bottom) * k;
    const double lx = (w - sp_lid_mm * k) / 2, lw = sp_lid_mm * k;
    const double ly = wy - sp_lid_top * k, lh = wh + (sp_lid_top + sp_lid_bottom) * k;
    const double hinge_d = sp_hinge_mm * k, by = ly + lh - 3 * k; // the barrel, its top behind the lid
    // The shell at a point: glossy, lit from the upper left, a broad sheen across it, with its grain.
    auto shell = [&](int x, int y, double t, double lit) {
        Rgb c = mix(s.top, s.bottom, int(256 * std::clamp(t, 0.0, 1.0)));
        const double u = (x - lx) / lw - 0.6 * (y - ly) / lh;
        c = scale(c, lit * (0.94 + 0.12 * std::exp(-std::pow((u - 0.15) / 0.22, 2))));
        const uint32_t n = hash(x, y);
        if (s.speckle && (n & 7) == 0) c = shade(c, int(n >> 8) % (2 * s.speckle + 1) - s.speckle);
        return c;
    };

    // The lower half, in shade under the open lid.
    const double base_y = by + hinge_d * 0.6, base_r = 4 * k;
    for (int y = std::max(0, int(base_y)); y < h; ++y)
        for (int x = std::max(0, int(lx)); x < std::min(w, int(lx + lw + 1)); ++x) {
            const double d = rounded_distance(x + 0.5, y + 0.5, lx, base_y, lw, h + 2 * base_r, base_r);
            if (d >= 0.5) continue;
            const double edge = std::clamp(-d / (1.5 * k), 0.0, 1.0);
            put(out, x, y, shell(x, y, 1, 0.55 + 0.12 * std::sqrt(edge)), int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
        }
    // The barrel: round, so a bright highlight along its upper middle and dark above and below; the knuckles parted by
    // narrow gaps.
    const double bx = lx + 1 * k, bw = lw - 2 * k;
    const double gap0 = bx + bw * 0.17, gap1 = bx + bw * 0.83;
    for (int y = std::max(0, int(by)); y < std::min(h, int(by + hinge_d + 1)); ++y) {
        const double t = std::clamp((y + 0.5 - by) / hinge_d, 0.0, 1.0);
        const double lit = 0.40 + 0.55 * std::sin(3.14159 * t) + 0.30 * std::exp(-std::pow((t - 0.38) / 0.08, 2));
        for (int x = std::max(0, int(bx)); x < std::min(w, int(bx + bw + 1)); ++x) {
            const double d = rounded_distance(x + 0.5, y + 0.5, bx, by, bw, hinge_d, 1.5 * k);
            if (d >= 0.5) continue;
            Rgb c = shell(x, y, 1, lit);
            const double g = std::min(std::abs(x + 0.5 - gap0), std::abs(x + 0.5 - gap1));
            if (g < 0.25 * k) c = scale(c, 0.3);
            put(out, x, y, c, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
        }
    }

    // The lid, its soft shadow falling on the barrel.
    const double top_r = sp_lid_radius * k, low_r = 2 * k;
    for (int y = std::max(0, int(ly - 1)); y < std::min(h, int(ly + lh + 2 * k)); ++y)
        for (int x = std::max(0, int(lx - 1)); x < std::min(w, int(lx + lw + 2)); ++x) {
            const double r = y + 0.5 > ly + lh / 2 ? low_r : top_r;
            const double d = rounded_distance(x + 0.5, y + 0.5, lx, ly, lw, lh, r);
            if (d >= 0.5) {
                const double below = rounded_distance(x + 0.5, y + 0.5 - 1.2 * k, lx, ly, lw, lh, r);
                if (below < 0 && y > ly + lh / 2)
                    put(out, x, y, Rgb{0, 0, 0}, int(130 * std::clamp(-below / (0.6 * k), 0.0, 1.0)));
                continue;
            }
            // The edge is bevelled: light along the top and left, a shade along the bottom and right.
            const double t = (y - ly) / lh;
            Rgb c = shell(x, y, t, 1);
            if (-d < 0.7 * k) {
                const double side = (x - lx) / lw - t; // < 0 towards the upper left
                c = side < 0 ? mix(c, Rgb{255, 255, 255}, int(70 * std::min(1.0, -side * 2)))
                             : scale(c, 1 - 0.25 * std::min(1.0, side * 2));
            }
            put(out, x, y, c, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
        }
    // The round bumpers the closed lid rests on, one in each corner.
    for (int i = 0; i < 4; ++i) {
        const double cx = i % 2 ? lx + lw - sp_bumper_in * k : lx + sp_bumper_in * k;
        const double cy = i < 2 ? ly + sp_bumper_in * k : ly + lh - sp_bumper_in * k;
        disc(out, cx, cy + 0.12 * k, 1.25 * k, shade(s.top, 30), 110);
        disc(out, cx, cy, 1.25 * k, scale(s.bottom, 0.85), 256);
        disc(out, cx - 0.15 * k, cy - 0.15 * k, 0.95 * k, scale(s.top, 0.97), 256);
    }

    // The window: dark grey glass, flush with the lid, a fine light edge and a faint glare across it.
    for (int y = std::max(0, int(wy - 1)); y < std::min(h, int(wy + wh + 2)); ++y)
        for (int x = std::max(0, int(wx - 1)); x < std::min(w, int(wx + ww + 2)); ++x) {
            const double d = rounded_distance(x + 0.5, y + 0.5, wx, wy, ww, wh, sp_window_radius * k);
            if (d >= 0.5) continue;
            Rgb c = shade(s.lens, int(6 - 12.0 * (y - wy) / wh));
            if (-d < 0.35 * k) c = mix(c, s.rim, 200);
            else if (-d < 0.7 * k) c = scale(c, 0.8);
            const double band = (x - wx) - 0.55 * (y - wy);
            if (band > ww * 0.55 && band < ww * 0.68) c = shade(c, 6);
            put(out, x, y, c, int(256 * std::clamp(0.5 - d, 0.0, 1.0)));
        }
    recess(out, screen, std::max(2, int(0.4 * k)), scale(s.lens, 0.5));
    // The logo: GAME BOY ADVANCE in small slanted capitals and a larger SP, centred in the strip under the screen.
    if (!font.loaded()) return;
    auto baked = [](double px) { return px < 18 ? 16 : px < 22 ? 20 : px < 26 ? 24 : px < 32 ? 28 : 36; };
    const int small = baked(2.3 * k), large = baked(3.4 * k), gap = large / 3;
    const std::string words = "GAME BOY ADVANCE";
    // Each capital is drawn a pixel heavier than slanted_width counts (GAME BOY's own logo never needed it).
    const int capitals =
        int(std::count_if(words.begin(), words.end(), [](char c) { return std::isupper(uint8_t(c)); }));
    const int w0 = slanted_width(font, words, small, 1) + capitals, w1 = slanted_width(font, "SP", large, 1) + 2;
    const int x0 = int(screen.x + (screen.w - (w0 + gap + w1)) / 2.0);
    const int baseline = int(screen.y + screen.h + (sp_window_bottom * k + font.ascent(large)) / 2);
    slanted_from(out, font, words, x0, baseline, small, 1, s.ink, {});
    slanted_from(out, font, "SP", x0 + w0 + gap, baseline, large, 1, s.ink, {});
}

} // namespace

FrameLayout frame_layout(int panel_w, int panel_h, int game_w, int game_h, int style) {
    FrameLayout l;
    const double sx = panel_w / 640.0, sy = panel_h / 480.0;
    game_w = std::max(1, game_w);
    game_h = std::max(1, game_h);
    const Kind kind = style > FrameOff && style < frame_style_count ? frame_styles[style].kind : Kind::Lens;
    auto place = [&](double s, double left, double top, double room_w, double room_h) {
        l.w = int(game_w * s);
        l.h = int(game_h * s);
        l.x = int(left + (room_w - l.w) / 2);
        l.y = int(top + (room_h - l.h) / 2);
        l.whole = std::abs(s - std::round(s)) < 1e-6;
        return l;
    };
    if (kind == Kind::Bar) {
        // As large as fits, across the full width at the top.
        place(std::min(double(panel_w) / game_w, (panel_h - bar_min * sy) / game_h), 0, 0, panel_w, 0);
        l.y = 0;
        return l;
    }
    if (kind == Kind::SpInner) {
        // The lid close up at the largest whole scale (2x on the 640x480 panel): the window and the strip of lid
        // under it end at the panel's foot, the lid's top and sides run off the panel and the hinge is below it.
        const double below_mm = sp_window_bottom + sp_lid_bottom, mm = double(game_w) / sp_screen_mm;
        const int s = std::max(1, int(std::min((panel_w - 16 * sx) / (game_w + 2 * sp_window_side * mm),
                                               (panel_h - 8 * sy) / (game_h + (sp_window_top + below_mm) * mm))));
        l.w = game_w * s;
        l.h = game_h * s;
        l.x = (panel_w - l.w) / 2;
        l.y = int(panel_h - l.h - below_mm * l.w / sp_screen_mm);
        return l;
    }
    if (kind == Kind::Sp) {
        // The whole lid to the SP's own proportions, as large as the panel's height allows with the barrel showing
        // under it (about 1.7x on the 640x480 panel): the screen centred across, the lid's top near the panel's.
        const double lid_mm = sp_lid_top + sp_window_top + sp_window_bottom + sp_lid_bottom, below_mm = 7;
        const double k = std::min((panel_h - 4 * sy) / (lid_mm + game_h * sp_screen_mm / game_w + below_mm),
                                  (panel_w - 8 * sx) / sp_lid_mm);
        place(k * sp_screen_mm / game_w, 0, 0, panel_w, 0);
        l.y = int(4 * sy + (sp_lid_top + sp_window_top) * k);
        return l;
    }
    if (kind == Kind::Console) {
        // As large as leaves the machine's rails beside the game, in steps of a quarter so a 256-wide game is 2x and a
        // 320-wide one 1.75x. No room is kept above and below: the rails carry the machine (the NES fills the height).
        const int rail = int((style > FrameOff && badge_of(frame_styles[style].slug) == Badge::C64 ? 32 : 40) * sx);
        const double fit = std::min(double(panel_w - 2 * rail) / game_w, double(panel_h) / game_h);
        const double s = std::max(1.0, std::floor(fit * 4 + 1e-6) / 4);
        return place(s, 0, 0, panel_w, panel_h);
    }
    if (kind == Kind::Wsc || kind == Kind::Ngpc || kind == Kind::Gg) {
        // Whole on the panel: the controls take the sides, the name the strip under the screen. The WonderSwan's wide
        // picture has room at 2x, the square ones at 2.75x (3x would leave no name).
        const double s = kind == Kind::Wsc ? 2 : 2.75 * std::min(sx, sy) * 160 / std::max(160, game_w);
        place(s, 0, 0, panel_w, 0);
        const double room = panel_h - l.h;
        l.y = int(room * (kind == Kind::Ngpc ? 0.30 : 0.42));
        return l;
    }
    if (kind == Kind::Psp) {
        // The PSP: as wide as the panel, smoothed like sharp fill.
        l.w = panel_w;
        l.h = panel_w * game_h / game_w;
        l.whole = false;
        l.x = 0;
        l.y = (panel_h - l.h) / 2;
        return l;
    }
    if (flat_face(kind)) {
        // As large as the bezel leaves room for, a strip of body either side; the whole face centred.
        const Face& f = face_of(kind);
        const double around = (f.above + f.top + f.bottom + f.below) * sy, across = (f.left + f.right + 48) * sx;
        const double s = std::min((panel_w - across) / game_w, (panel_h - around) / game_h);
        place(s, 0, 0, panel_w, 0);
        l.x = int((panel_w - (f.left + f.right) * sx - l.w) / 2 + f.left * sx);
        l.y = int((panel_h - around - l.h) / 2 + (f.above + f.top) * sy);
        return l;
    }
    if (roomy(kind)) {
        // As large as the room it leaves, centred in that room.
        const Room r = room_of(kind);
        const double room_w = panel_w - (r.left + r.right) * sx, room_h = panel_h - (r.top + r.bottom) * sy;
        return place(std::min(room_w / game_w, room_h / game_h), r.left * sx, r.top * sy, room_w, room_h);
    }
    // The lens: the largest whole scale that leaves the body room around it.
    if (kind == Kind::Agb) {
        // The Game Boy Advance: the largest whole scale that leaves the pad and the buttons their sides, a little above
        // the panel's middle so the lens's name strip and the speaker fit under it.
        const int s = std::max(1, std::min(int((panel_w - 2 * 76 * sx) / game_w), int((panel_h - 112 * sy) / game_h)));
        l.w = game_w * s;
        l.h = game_h * s;
        l.x = (panel_w - l.w) / 2;
        l.y = int(28 * sy + (panel_h - 112 * sy - l.h) * 0.4);
        return l;
    }
    const int room_w = panel_w - int(2 * (lens_side + 50) * sx),
              room_h = panel_h - int((lens_top + lens_bottom + 60) * sy);
    const int s = std::max(1, std::min(room_w / game_w, room_h / game_h));
    l.w = game_w * s;
    l.h = game_h * s;
    const int lens_h = l.h + int((lens_top + lens_bottom) * sy);
    l.x = (panel_w - l.w) / 2;
    l.y = (panel_h - lens_h) / 2 + int(lens_top * sy);
    return l;
}

void draw_frame(Canvas& out, const FrameLayout& screen, int style, const Font& font, const std::string& system) {
    if (style <= FrameOff || style >= frame_style_count) return;
    const Style s = style_of(style);
    switch (s.kind) {
    case Kind::Bar: draw_bar(out, screen, s, font, system); break;
    case Kind::Sp:
    case Kind::SpInner: draw_sp(out, screen, s, font); break;
    case Kind::Agb: draw_agb(out, screen, s, font); break;
    case Kind::Wsc:
    case Kind::Ngpc:
    case Kind::Gg: draw_wide_handheld(out, screen, s, font); break;
    case Kind::Psp: draw_psp(out, screen, s, font); break;
    case Kind::Dmg:
    case Kind::Pocket:
    case Kind::Bezel: draw_face(out, screen, s, font); break;
    case Kind::DmgInner:
    case Kind::PocketInner:
    case Kind::BezelInner: draw_inner(out, screen, s, font); break;
    case Kind::Console: draw_console(out, screen, s, font); break;
    case Kind::Tv:
    case Kind::TvWood: draw_tv(out, screen, s); break;
    case Kind::Monitor: draw_monitor(out, screen, s); break;
    case Kind::Arcade: draw_arcade(out, screen, s, font); break;
    case Kind::Vectrex: draw_vectrex(out, screen, s, font); break;
    default: draw_lens(out, screen, s, font);
    }
}

int parse_frame(const std::string& name, const std::string& system) {
    // A slug may have a row per system (the arcade cabinet): the first row that fits wins.
    for (int i = 0; i < frame_style_count; ++i)
        if (name == frame_styles[i].slug && frame_fits(frame_styles[i], system)) return i;
    return -1;
}
const char* frame_name(int style) {
    return frame_styles[style > FrameOff && style < frame_style_count ? style : FrameOff].slug;
}

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
