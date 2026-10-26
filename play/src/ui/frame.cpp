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
};
Style style_of(int index) {
    const FrameStyleInfo& f = frame_styles[index];
    return {
        f.kind,  Rgb::hex(f.top),  Rgb::hex(f.bottom), f.speckle, Rgb::hex(f.bezel), Rgb::hex(f.rim), Rgb::hex(f.ink),
        f.print, Rgb::hex(f.band), Rgb::hex(f.line),   f.wood};
}

// At the panel's 640x480 scale.
constexpr int lens_side = 30, lens_top = 26, lens_bottom = 46, lens_radius = 18;        // the Game Boy Advance's lens
constexpr int bar_min = 50;                                                             // the black bar
constexpr int sp_side = 20, sp_top = 18, sp_bottom = 62, sp_margin = 9, sp_radius = 10; // the SP
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
    return kind == Kind::Console || kind == Kind::Tv || kind == Kind::TvWood || kind == Kind::Monitor ||
           kind == Kind::Arcade || kind == Kind::Vectrex || inner(kind);
}
Room room_of(Kind kind) {
    switch (kind) {
    // Exactly 3x on a Game Boy's 160x144 picture in 640x480 (480 x 432): whole pixels.
    case Kind::DmgInner: return {88, 72, 34, 14};
    case Kind::PocketInner: return {88, 72, 24, 24};
    case Kind::BezelInner: return {88, 72, 14, 34};
    case Kind::Console: return {54, 54, 34, 96};
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
void battery_light(Canvas& out, const Font& font, double cx, double cy, double unit, int room, Rgb ink) {
    power_light(out, cx, cy, unit, Rgb::hex(0xFF3B3B));
    const int size = 16, spacing = -1; // set a little tight, as the lettering is
    if (!font.loaded() || spaced_width(font, "BATTERY", size, spacing) > room) return;
    name(out, font, "BATTERY", cx, int(cy + 12 * unit + font.ascent(size)), size, spacing, ink, {});
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
                      int((f.left - 6) * sx), Rgb::hex(0xC9CAD3));
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
        const int size = int(28 * unit);
        slanted_from(out, font, s.print, int(bx + 6 * unit), int(by + bh + (f.below * sy + font.ascent(size)) / 2),
                     size, std::max(1, int(2 * unit)), s.ink, {});
    }
}

// Only the screen's bezel, edge to edge: the game as large as the panel allows, its window around it, the
// power light, and what the bezel says (the Game Boy's two lines, the Color's name).
void draw_inner(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy, 0);
    recess(out, screen, std::max(3, int(recess_width * unit)), scale(s.lens, 0.55));
    const double led_x = screen.x / 2.0 - 4 * unit, led_y = screen.y + screen.h * 0.16;
    battery_light(out, font, led_x, led_y, unit, int(screen.x - 8 * sx), Rgb::hex(0xC9CAD3));
    if (s.kind == Kind::DmgInner) dot_matrix(out, font, 0, out.w(), screen.y - 21 * sy, out.w() * 0.62, unit);
    if (s.kind == Kind::BezelInner) {
        const int size = 16, spacing = std::max(1, int(2 * unit)), ring = std::max(3, int(recess_width * unit));
        const int free = out.h() - (screen.y + screen.h) - ring;
        slanted_from(out, font, s.print, int(out.w() / 2.0 - slanted_width(font, s.print, size, spacing) / 2.0),
                     int(screen.y + screen.h + ring + (free + font.ascent(size)) / 2), size, spacing, s.ink,
                     accent_for(s.print));
    }
}

// The Game Boy Advance, the WonderSwan and the PSP: a wide dark lens, the power light left of the screen, the
// name under it, the speaker.
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

// A home console: its body, the screen set into it, its name on the strip under the screen, the power light.
void draw_console(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy), m = 12 * unit;
    body(out, s, sx, sy);
    bezel(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, 10 * unit, 10 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    // The strip: lit along its top edge, a fine line inside each edge.
    const int y0 = int(screen.y + screen.h + m + 16 * sy), y1 = std::min(h - int(12 * sy), y0 + int(56 * sy));
    const int t = std::max(1, int(2 * unit));
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = 0; x < w; ++x) {
            Rgb c = mix(s.band, shade(s.band, -16), (y - y0) * 256 / std::max(1, y1 - y0));
            if (s.wood == 1) c = wood_at(c, x, y, unit);
            if (y == y0) c = shade(c, 30);
            if (y == y1 - 1) c = shade(c, -30);
            if ((y >= y0 + int(6 * unit) && y < y0 + int(6 * unit) + t) ||
                (y >= y1 - int(6 * unit) - t && y < y1 - int(6 * unit)))
                c = s.line;
            out.data()[size_t(y) * w + x] = c.px();
        }
    const int spacing = std::max(1, int(3 * unit));
    int size = int(24 * unit);
    if (spaced_width(font, s.print, size, spacing) > w - 130 * unit) size = int(20 * unit);
    name(out, font, s.print, w / 2.0, (y0 + y1 + font.ascent(size)) / 2, size, spacing, s.ink, accent_for(s.print));
    power_light(out, w - 34 * sx, (y0 + y1) / 2.0, unit, Rgb::hex(0xFF3B3B));
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
            if (k == 0) put(out, x, y, s.band, 210);
            else if (k == 2) put(out, x, y, s.line, 210);
        }
    // The marquee: a light box, brightest in the middle.
    const double mx = 24 * unit, my = 10 * unit, mw = w - 2 * mx, mh = screen.y - glass - 24 * unit;
    rounded(out, mx - 4 * unit, my - 4 * unit, mw + 8 * unit, mh + 8 * unit, 8 * unit, Rgb::hex(0x050506));
    for (int y = int(my); y < int(my + mh); ++y)
        for (int x = int(mx); x < int(mx + mw); ++x) {
            const double dx = (x - (mx + mw / 2)) / (mw / 2), dy = (y - (my + mh / 2)) / (mh / 2);
            const double lit = std::clamp(1.0 - 0.55 * (dx * dx) - 0.35 * (dy * dy), 0.0, 1.0);
            put(out, x, y, mix(scale(s.band, 0.55), mix(s.band, Rgb{255, 255, 240}, 110), int(256 * lit)), 256);
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

// The Vectrex: its own upright screen in the black body, the name under it.
void draw_vectrex(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const double sx = out.w() / 640.0, sy = out.h() / 480.0, unit = std::min(sx, sy), glass = 16 * unit;
    body(out, s, sx, sy);
    lens(out, s, screen.x - glass, screen.y - glass, screen.w + 2 * glass, screen.h + 2 * glass, 14 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    const int size = int(28 * unit), bottom = screen.y + screen.h + int(glass);
    name(out, font, s.print, screen.x + screen.w / 2.0, bottom + int((out.h() - bottom + font.ascent(size)) / 2), size,
         std::max(1, int(6 * unit)), s.ink, {});
    knob(out, screen.x + screen.w + glass + 30 * unit, bottom + 30 * unit, 10 * unit, s.band, unit, 20);
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
    body(out, s, sx, sy, 14, 0.10);
    // The clamshell's hinge along the bottom edge.
    for (int y = h - int(10 * sy); y < h; ++y)
        for (int x = 0; x < w; ++x) put(out, x, y, scale(s.bottom, 0.55), 150);
    const double m = sp_margin * unit;
    lens(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, sp_radius * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    const int size = int(20 * unit), baseline = int(screen.y + screen.h + m + (sp_bottom * sy - m) / 2 + size / 3.0);
    name(out, font, s.print, w / 2.0, baseline, size, std::max(1, int(2 * unit)), s.ink,
         s.bottom.r + s.bottom.g + s.bottom.b > 420 ? std::vector<Rgb>{} : accent_for(s.print)); // not on a light shell
}

} // namespace

FrameLayout frame_layout(int panel_w, int panel_h, int game_w, int game_h, int style) {
    FrameLayout l;
    const double sx = panel_w / 640.0, sy = panel_h / 480.0, unit = std::min(sx, sy);
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
    if (kind == Kind::Bar || kind == Kind::Sp) {
        // As large as fits: across the full width at the top, or inside the slim SP lens.
        const double room_w = kind == Kind::Bar ? panel_w : panel_w - 2 * (sp_side * sx + sp_margin * unit);
        const double room_h =
            kind == Kind::Bar ? panel_h - bar_min * sy : panel_h - (sp_top + sp_bottom) * sy - 2 * sp_margin * unit;
        const double s = std::min(room_w / game_w, room_h / game_h);
        place(s, 0, 0, panel_w, 0);
        l.y = kind == Kind::Bar ? 0 : int(sp_top * sy + sp_margin * unit);
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
    case Kind::Sp: draw_sp(out, screen, s, font); break;
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
