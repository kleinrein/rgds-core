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
constexpr int lens_side = 30, lens_top = 26, lens_bottom = 46, lens_radius = 18;        // the Game Boy Advance's lens
constexpr int bar_min = 50;                                                             // the black bar
constexpr int sp_side = 20, sp_top = 18, sp_bottom = 62, sp_margin = 9, sp_radius = 10; // the SP
// The Game Boy Advance's own proportions, at the panel's scale. The AGB is a wide, squat body (about 2:1): the screen
// sits in a dark lens in the upper half, the lower edge of the shell tapers in, and a strip of body below the screen
// carries the logo on the left and the speaker's round holes on the right.
constexpr int agb_side = 26, agb_top = 20, agb_bottom = 26, agb_radius = 14, agb_edge = 22;
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
    case Kind::Console: return {54, 54, 34, 116};
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

// The Game Boy Advance: a wide, squat body with a dark lens in the upper half, the green power light beside the
// screen, the GAME BOY ADVANCE logo low on the left and the speaker's round holes low on the right.
void draw_agb(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    // The lens the screen sits in.
    const double lx = screen.x - agb_side * sx, ly = screen.y - agb_top * sy, lw = screen.w + 2 * agb_side * sx,
                 lh = screen.h + (agb_top + agb_bottom) * sy;
    // The shell: the AGB's body is wider than its lens on both sides and runs a strip below it for the logo.
    const double body_w = std::min(w - 8 * sx, lw + 60 * sx);
    const double body_h = std::min(h - 8 * sy, lh + 2 * 20 * sy + 96 * sy);
    const double bx = (w - body_w) / 2, bw = body_w;
    const double by = std::max(0.0, std::min(ly - 20 * sy, h - body_h)), bh = body_h;
    // The AGB's body: a wide shell whose lower edge tapers in, the way the real one does at its belly.
    const double strip_h = 96 * sy; // the strip of body under the lens, for the logo and the speaker
    for (int y = int(by); y < int(by + bh); ++y) {
        const double t = (y - by) / std::max(1.0, bh); // 0 at the top, 1 at the bottom
        // The taper: the shell narrows towards the bottom, in the last third of its height.
        const double k = std::max(0.0, (t - 0.55) / 0.45);
        const double taper = 30 * sx * k * k;
        for (int x = int(bx + taper); x < int(bx + bw - taper); ++x) {
            if (x < 0 || x >= w) continue;
            const double ex = std::min(x - bx, bx + bw - x) / (30 * sx);
            const double ey = std::min(y - by, by + bh - y) / (16 * sy);
            Rgb c = mix(s.top, s.bottom, int(200 * t));
            c = scale(c, 0.88 + 0.12 * std::sqrt(std::clamp(std::min(ex, ey), 0.0, 1.0)));
            if (y < by + 14 * sy) c = mix(c, Rgb{255, 255, 255}, int(36 * (1 - (y - by) / (14 * sy))));
            const uint32_t n = hash(x, y);
            if (s.speckle && (n & 7) == 0) c = shade(c, int(n >> 8) % (2 * s.speckle + 1) - s.speckle);
            out.data()[size_t(y) * w + x] = c.px();
        }
    }
    // The seam where the lower shell meets the upper one.
    const double seam = by + bh - strip_h;
    for (int y = int(seam); y < int(seam + 2 * sy); ++y)
        for (int x = int(bx); x < int(bx + bw); ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            Rgb c = Rgb::from_px(out.data()[size_t(y) * w + x]);
            out.data()[size_t(y) * w + x] = shade(c, -20).px();
        }

    lens(out, s, lx, ly, lw, lh, agb_radius * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    // The green power light, left of the screen, in the dark lens.
    power_light(out, lx + agb_side * sx / 2, screen.y + screen.h * 0.30, unit, Rgb::hex(0x4CFA7A));

    // The logo, low on the left of the body: "GAME BOY" over "ADVANCE", the ADVANCE letters in the sweep. The ink is
    // picked so it reads on the body it is printed on: dark on a pale shell, pale on a dark one.
    const int size = int(16 * unit), spacing = std::max(1, int(1 * unit));
    const double logo_x = bx + 28 * sx, logo_y = seam + 14 * sy;
    const bool pale = s.bottom.r + s.bottom.g + s.bottom.b > 430;
    const Rgb logo_ink = pale ? Rgb::hex(0x2A2A34) : s.ink;
    name_from(out, font, "GAME BOY", int(logo_x), int(logo_y + font.ascent(size)), size, spacing, logo_ink, {});
    name_from(out, font, "ADVANCE", int(logo_x), int(logo_y + font.ascent(size) + int(16 * unit)), size, spacing,
              logo_ink, accent_for("GAME BOY ADVANCE"));

    // The speaker: round holes in a wide patch, low on the right of the body.
    const double gx = bx + bw - 150 * sx, gy = seam + 12 * sy;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 8; ++col) {
            const double hx = gx + col * 15 * unit + (row % 2) * 7.5 * unit, hy = gy + row * 13 * unit;
            if (hx > bx + bw - 20 * sx || hy > by + bh - 6 * sy) continue;
            Rgb at =
                Rgb::from_px(out.data()[size_t(std::clamp(int(hy), 0, h - 1)) * w + std::clamp(int(hx), 0, w - 1)]);
            disc(out, hx, hy + 0.8 * unit, 3.6 * unit, shade(at, 26), 130);
            disc(out, hx, hy, 3.6 * unit, scale(at, 0.42), 256);
        }
}

// The WonderSwan Color and the Neo Geo Pocket Color: tall portrait handhelds, the screen in the upper half and the
// controls below it.
void draw_portrait(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    // The body: a tall shell, wider than the screen, with the screen in the upper part of it.
    const double body_h = h - 12 * sy;
    const double body_w = std::min(w - 20 * sx, body_h / 1.35);
    const double bx = (w - body_w) / 2, bw = body_w;
    const double by = std::max(0.0, (h - body_h) / 2), bh = body_h;
    body(out, s, sx, sy, 16, 0.16);

    // The screen: a dark window in the upper half of the shell.
    const double m = 12 * unit;
    lens(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, 8 * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    // The green power light above the screen.
    power_light(out, screen.x + screen.w / 2, screen.y - m - 8 * unit, unit, Rgb::hex(0x4CFA7A));

    // The controls below the screen: the D-pad on the left, the A and B buttons on the right, the speaker between.
    const double cy = by + bh - 82 * sy;
    // The D-pad: a cross, in the shell's own dark grey.
    const double dx = bx + 46 * sx, dsize = 13 * unit;
    const Rgb dark = scale(s.bottom, 0.55);
    rounded(out, dx - dsize, cy - dsize * 0.5, dsize * 2, dsize, dsize * 0.4, dark, 255);
    rounded(out, dx - dsize * 0.5, cy - dsize, dsize, dsize * 2, dsize * 0.4, dark, 255);
    // The A and B buttons, angled as the WonderSwan's are.
    disc(out, bx + bw - 66 * sx, cy - 10 * unit, 12 * unit, dark, 255);
    disc(out, bx + bw - 36 * sx, cy + 10 * unit, 12 * unit, dark, 255);
    // The speaker's holes, between them.
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            disc(out, bx + bw / 2 - 14 * unit + col * 14 * unit, cy - 14 * unit + row * 14 * unit, 4 * unit,
                 scale(s.bottom, 0.3), 245);

    // The name under the screen, on the shell.
    const int size = int(14 * unit);
    name(out, font, s.print, w / 2.0, int(by + bh - 18 * sy), size, std::max(1, int(2 * unit)), s.ink,
         accent_for(s.print));
}

// The PlayStation Portable: a wide lens across the body, the buttons and the speaker under it.
void draw_psp(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    (void)font; // the PSP carries no name on its face
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy, 16, 0.16);
    // The lens: a wide, shallow window across the top of the body.
    const double m = 18 * unit;
    lens(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, 12 * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    // The power light beside the screen.
    power_light(out, screen.x + screen.w + m + 10 * unit, screen.y + 12 * unit, unit, Rgb::hex(0x4CFA7A));
    // The controls under the screen: the D-pad on the left, the face buttons on the right, the speaker between.
    const double cy = screen.y + screen.h + m + 46 * sy;
    const double dx = screen.x + 44 * sx, dsize = 13 * unit;
    rounded(out, dx - dsize, cy - dsize * 0.5, dsize * 2, dsize, dsize * 0.4, s.bottom, 255);
    rounded(out, dx - dsize * 0.5, cy - dsize, dsize, dsize * 2, dsize * 0.4, s.bottom, 255);
    // The face buttons: the four symbols of the PSP, in their own colours.
    const Rgb face[4] = {Rgb::hex(0x6E8FD8), Rgb::hex(0xD85A72), Rgb::hex(0x62C86E), Rgb::hex(0xE0B24C)};
    for (int i = 0; i < 4; ++i) {
        const double fx = screen.x + screen.w - 74 * sx + (i % 2) * 30 * unit;
        const double fy = cy - 15 * unit + (i / 2) * 30 * unit;
        disc(out, fx, fy + 1.5 * unit, 11 * unit, scale(s.bottom, 0.35), 220);
        disc(out, fx, fy, 11 * unit, face[i], 256);
    }
    // The speaker's slots, between the controls.
    slots(out, screen.x + screen.w / 2 - 30 * unit, cy - 14 * unit, 60 * unit, 4, s.bottom, unit);
}

// The WonderSwan Color and the PSP: a wide dark lens, the power light left of the screen, the name under it.
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

// The Nintendo Entertainment System: the dark grey console with the red stripe across its front and the thin blue
// line under it, the front badge on the left and the power light on the right.
void draw_nes(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy, 18, 0.16);

    // The stripe: the red band across the front with the thin blue line under it, above the screen.
    const double stripe_h = 16 * sy, blue_h = 5 * sy;
    const double sy0 = screen.y - 30 * sy;
    for (int y = int(sy0); y < int(sy0 + stripe_h + blue_h); ++y)
        for (int x = 0; x < w; ++x) {
            const bool red = y < sy0 + stripe_h;
            Rgb c = red ? Rgb::hex(0xD8282E) : Rgb::hex(0x2A5FBF);
            // Lit along its top edge, a shade darker at its bottom.
            c = shade(c, red ? (y == int(sy0) ? 26 : 0) : (y == int(sy0 + stripe_h) ? 18 : 0));
            if (y == int(sy0 + stripe_h)) c = shade(c, -40);
            out.data()[size_t(y) * w + x] = c.px();
        }

    // The screen, set into the front panel.
    bezel(out, s, screen.x - 14 * sx, screen.y - 14 * sy, screen.w + 28 * sx, screen.h + 28 * sy, 10 * unit, 10 * unit,
          unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));

    // The front badge on the left: the "Nintendo Entertainment System" mark in a rounded box.
    const double badge_x = 16 * sx, badge_y = screen.y + screen.h + 18 * sy;
    const double badge_w = 232 * sx, badge_h = 54 * sy;
    rounded(out, badge_x, badge_y, badge_w, badge_h, 8 * unit, Rgb::hex(0x2A2A2E), 235);
    rounded(out, badge_x + 3 * sx, badge_y + 3 * sx, badge_w - 6 * sx, badge_h - 6 * sx, 6 * unit, Rgb::hex(0x1C1C20),
            235);
    // The badge's own lettering: "Nintendo" over "Entertainment System", the way the badge is stacked.
    const int size = int(15 * unit);
    name(out, font, "Nintendo", badge_x + badge_w / 2, int(badge_y + 23 * sy), size, std::max(1, int(1 * unit)),
         Rgb::hex(0xD8D8DC), {});
    name(out, font, "ENTERTAINMENT SYSTEM", badge_x + badge_w / 2, int(badge_y + 41 * sy), int(9 * unit),
         std::max(1, int(1 * unit)), Rgb::hex(0xC0C0C6), {});

    // The power light on the right, in the front.
    power_light(out, w - 40 * sx, screen.y + screen.h + 34 * sy, unit, Rgb::hex(0xFF3B3B));
    // The vents on the right, the way the console's top is ribbed.
    for (int i = 0; i < 6; ++i) {
        const double vx = w - 120 * sx + i * 11 * unit;
        rounded(out, vx, screen.y + screen.h + 22 * sy, 5 * unit, 22 * sy, 2 * unit, scale(s.bottom, 0.45), 200);
    }
}

// A console's own mark, drawn from shapes in the console's colours: the Super Nintendo's purple on its dark
// panel, the Mega Drive's SEGA above the red 16-BIT band, the PlayStation's blue line, the Nintendo 64's N, the
// Saturn's planet ring, the Dreamcast's orange spiral.
void draw_badge(Canvas& out, const Font& font, Badge badge, double cx, double cy, double unit, const Style& s) {
    (void)s; // each mark carries its own colours
    switch (badge) {
    case Badge::Snes: {
        // The Super Nintendo: the mark on the console's dark front panel, the purple letters and the three blocks.
        const int size = int(13 * unit);
        const double panel_h = 46 * unit;
        rounded(out, cx - 78 * unit, cy - panel_h / 2, 156 * unit, panel_h, 4 * unit, Rgb::hex(0x2A2A32), 235);
        name(out, font, "SUPER", cx, int(cy - 12 * unit), size, std::max(1, int(3 * unit)), Rgb::hex(0xB4A0DC), {});
        name(out, font, "NINTENDO", cx, int(cy + 3 * unit), size, std::max(1, int(3 * unit)), Rgb::hex(0xB4A0DC), {});
        const Rgb blocks[3] = {Rgb::hex(0xD8282E), Rgb::hex(0xF2F2F4), Rgb::hex(0x2A5FBF)};
        for (int i = 0; i < 3; ++i)
            rounded(out, cx - 20 * unit + i * 14 * unit, cy + 9 * unit, 11 * unit, 7 * unit, 1.5 * unit, blocks[i]);
        break;
    }
    case Badge::Md: {
        // The Mega Drive: SEGA's mark above the red band with 16-BIT in it.
        const int size = int(17 * unit);
        name(out, font, "SEGA", cx, int(cy - 6 * unit), size, std::max(1, int(4 * unit)), Rgb::hex(0xE8E8EC), {});
        rounded(out, cx - 58 * unit, cy + 2 * unit, 116 * unit, 20 * unit, 3 * unit, Rgb::hex(0xC8202A));
        name(out, font, "16-BIT", cx, int(cy + 17 * unit), int(12 * unit), std::max(1, int(3 * unit)),
             Rgb::hex(0xF6F6F8), {});
        break;
    }
    case Badge::Sms: {
        // The Master System: the red 16-BIT band with the name under it.
        rounded(out, cx - 58 * unit, cy - 8 * unit, 116 * unit, 20 * unit, 3 * unit, Rgb::hex(0xD0202A));
        name(out, font, "16-BIT", cx, int(cy + 7 * unit), int(12 * unit), std::max(1, int(3 * unit)),
             Rgb::hex(0xF6F6F8), {});
        name(out, font, "MASTER SYSTEM", cx, int(cy + 28 * unit), int(12 * unit), std::max(1, int(2 * unit)),
             Rgb::hex(0xF0F0F0), {});
        break;
    }
    case Badge::Pce: {
        // The PC Engine: the orange band with the name on it.
        rounded(out, cx - 66 * unit, cy - 10 * unit, 132 * unit, 22 * unit, 3 * unit, Rgb::hex(0xE06A1A));
        name(out, font, "PC ENGINE", cx, int(cy + 6 * unit), int(13 * unit), std::max(1, int(3 * unit)),
             Rgb::hex(0xFFFFFF), {});
        break;
    }
    case Badge::Neo: {
        // The Neo Geo: the gold band with the name in it.
        rounded(out, cx - 56 * unit, cy - 10 * unit, 112 * unit, 22 * unit, 3 * unit, Rgb::hex(0xC9A34E));
        name(out, font, "NEO GEO", cx, int(cy + 6 * unit), int(13 * unit), std::max(1, int(3 * unit)),
             Rgb::hex(0x1A1A1C), {});
        break;
    }
    case Badge::Psx: {
        // The PlayStation: the blue PS line over the wordmark.
        const int size = int(15 * unit);
        name(out, font, "PlayStation", cx, int(cy + 12 * unit), size, std::max(1, int(2 * unit)), Rgb::hex(0xD8D8DE),
             {});
        // The PS mark: the two strokes of the P and the S, in the console's blue.
        const double px = cx - 46 * unit, py = cy - 16 * unit;
        for (int i = 0; i < 3; ++i) {
            const double o = i * 7 * unit;
            for (int y = 0; y < int(12 * unit); ++y)
                for (int x = 0; x < int(26 * unit); ++x) {
                    const double fx = x / (26 * unit), fy = y / (12 * unit);
                    if (std::abs(fx - fy * 0.9) > 0.09 && std::abs(fx + fy * 0.9 - 0.95) > 0.09) continue;
                    put(out, int(px + o + x), int(py + y), Rgb::hex(0x2A5FBF), 220);
                }
        }
        break;
    }
    case Badge::N64: {
        // The Nintendo 64: the wordmark with the N in the console's colours, the N standing between the words.
        const int size = int(15 * unit);
        name(out, font, "NINTENDO", cx - 30 * unit, int(cy), size, std::max(1, int(2 * unit)), Rgb::hex(0xE0E0E6), {});
        name(out, font, "64", cx + 44 * unit, int(cy), size, std::max(1, int(2 * unit)), Rgb::hex(0xE0E0E6), {});
        // The N: the three strokes, in red, green and blue.
        const double nx = cx - 8 * unit, ny = cy - 13 * unit, nw = 9 * unit, nh = 26 * unit;
        const Rgb n_col[3] = {Rgb::hex(0xD8282E), Rgb::hex(0x2A8A3E), Rgb::hex(0x2A5FBF)};
        for (int i = 0; i < 3; ++i) {
            const double o = i * (nw - 2 * unit);
            for (int y = 0; y < int(nh); ++y) {
                const double fy = y / nh;
                for (int x = 0; x < int(nw - 2 * unit); ++x) {
                    const double fx = x / (nw - 2 * unit);
                    if (i == 0 && std::abs(fx - fy) > 0.30) continue; // the first stroke leans
                    if (i == 2 && std::abs(fx + fy - 1.0) > 0.30) continue;
                    put(out, int(nx + o + x), int(ny + y), n_col[i], 235);
                }
            }
        }
        break;
    }
    case Badge::Saturn: {
        // The Saturn: the wordmark with the planet's ring round it.
        const int size = int(16 * unit);
        name(out, font, "SATURN", cx + 10 * unit, int(cy + 6 * unit), size, std::max(1, int(3 * unit)),
             Rgb::hex(0xB4C4E4), {});
        // The planet: a disc with the ring across it.
        const double px = cx - 44 * unit, py = cy - 6 * unit, pr = 11 * unit;
        disc(out, px, py, pr, Rgb::hex(0x3A6FD8), 235);
        for (int y = -int(pr) - 4; y <= int(pr) + 4; ++y)
            for (int x = -int(pr) - 12; x <= int(pr) + 12; ++x) {
                const double rx = (x - 4) / 1.0, ry = (y - 2) / 0.34;
                const double d = std::abs(std::hypot(rx, ry) - pr);
                if (d < 1.4) put(out, int(px + x), int(py + y), Rgb::hex(0xB4C4E4), 220);
            }
        break;
    }
    case Badge::Dc: {
        // The Dreamcast: the orange spiral, with the wordmark under it.
        const double px = cx, py = cy - 8 * unit;
        for (int y = -22; y <= 22; ++y)
            for (int x = -22; x <= 22; ++x) {
                const double r = std::hypot(x, y), a = std::atan2(y, x);
                // The spiral: the radius grows with the angle.
                const double target = 3.0 + 6.5 * (a + 3.14159) / (2 * 3.14159);
                if (std::abs(r - target) < 1.5) put(out, int(px + x), int(py + y), Rgb::hex(0xE86A18), 235);
            }
        name(out, font, "Dreamcast", px, int(py + 34 * unit), int(13 * unit), std::max(1, int(1 * unit)),
             Rgb::hex(0x2B2B2E), {});
        break;
    }
    case Badge::Atari: {
        // The Atari 2600: the gold Atari mark on the faux woodgrain front.
        const int size = int(20 * unit);
        name(out, font, "ATARI", cx, int(cy), size, std::max(1, int(4 * unit)), Rgb::hex(0xE8D8B0), {});
        // The three lines of the Atari fuji.
        for (int i = 0; i < 3; ++i) {
            const double o = (i - 1) * 12 * unit;
            for (int y = 0; y < int(16 * unit); ++y)
                for (int x = 0; x < int(5 * unit); ++x)
                    put(out, int(cx + o + x - 6 * unit), int(cy - 30 * unit + y), Rgb::hex(0xE8D8B0), 235);
        }
        break;
    }
    case Badge::Coleco: {
        // The ColecoVision: the white mark with the gold line under it.
        const int size = int(15 * unit);
        name(out, font, "COLECO", cx, int(cy - 2 * unit), size, std::max(1, int(3 * unit)), Rgb::hex(0xE6E6E6), {});
        name(out, font, "VISION", cx, int(cy + 14 * unit), size, std::max(1, int(3 * unit)), Rgb::hex(0xE6E6E6), {});
        for (int x = int(cx - 54 * unit); x < int(cx + 54 * unit); ++x)
            put(out, x, int(cy + 22 * unit), Rgb::hex(0xB8BAC2), 235);
        break;
    }
    case Badge::Msx: {
        // The MSX: the mark in the console's own blocky letters, on its own black band.
        rounded(out, cx - 60 * unit, cy - 15 * unit, 120 * unit, 30 * unit, 3 * unit, Rgb::hex(0x141416));
        const int size = int(17 * unit);
        name(out, font, "MSX", cx, int(cy + 6 * unit), size, std::max(1, int(5 * unit)), Rgb::hex(0xE6E6E6), {});
        break;
    }
    case Badge::Intellivision: {
        // The Intellivision: the tan badge with the name across it.
        const int size = int(17 * unit);
        rounded(out, cx - 78 * unit, cy - 16 * unit, 156 * unit, 32 * unit, 3 * unit, Rgb::hex(0x2A1A0C));
        name(out, font, "INTELLIVISION", cx, int(cy + 6 * unit), size, std::max(1, int(2 * unit)), Rgb::hex(0xC9A34E),
             {});
        break;
    }
    case Badge::Pico8: {
        // The PICO-8: its own palette, a cart of its own, with the colourful label.
        const double px = cx, py = cy - 4 * unit;
        rounded(out, px - 52 * unit, py - 26 * unit, 104 * unit, 52 * unit, 4 * unit, Rgb::hex(0x1D2B53));
        rounded(out, px - 48 * unit, py - 22 * unit, 96 * unit, 44 * unit, 3 * unit, Rgb::hex(0x111A35));
        // The label: the PICO-8's colourful bars.
        const Rgb bars[4] = {Rgb::hex(0xFF004D), Rgb::hex(0xFFEC27), Rgb::hex(0x00A8C6), Rgb::hex(0x62D60F)};
        for (int i = 0; i < 4; ++i)
            rounded(out, px - 42 * unit + i * 22 * unit, py - 18 * unit, 17 * unit, 16 * unit, 2 * unit, bars[i]);
        name(out, font, "PICO-8", px, int(py + 12 * unit), int(13 * unit), std::max(1, int(2 * unit)),
             Rgb::hex(0xFFF1E8), {});
        break;
    }
    case Badge::None: break;
    }
}

// A home console: its body, the screen set into it, the console's own mark on the strip under the screen, the
// power light.
void draw_console(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy), m = 12 * unit;
    body(out, s, sx, sy);
    bezel(out, s, screen.x - m, screen.y - m, screen.w + 2 * m, screen.h + 2 * m, 10 * unit, 10 * unit, unit);
    screen_edge(out, screen, std::max(2, int(2 * unit)));
    // The strip: lit along its top edge, a fine line inside each edge.
    const int y0 = int(screen.y + screen.h + m + 16 * sy), y1 = std::min(h - int(10 * sy), y0 + int(84 * sy));
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
    // The console's own mark on the strip, in its own colours; without one, its name in plain ink.
    if (s.badge != Badge::None) draw_badge(out, font, s.badge, w / 2.0, (y0 + y1) / 2.0, unit, s);
    else {
        const int spacing = std::max(1, int(3 * unit));
        int size = int(24 * unit);
        if (spaced_width(font, s.print, size, spacing) > w - 130 * unit) size = int(20 * unit);
        name(out, font, s.print, w / 2.0, (y0 + y1 + font.ascent(size)) / 2, size, spacing, s.ink, accent_for(s.print));
    }
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

// The Game Boy Advance SP: a slim clamshell, the game in a small lens in the lid, the logo under it and the hinge
// along the bottom edge.
void draw_sp(Canvas& out, const FrameLayout& screen, const Style& s, const Font& font) {
    const int w = out.w(), h = out.h();
    const double sx = w / 640.0, sy = h / 480.0, unit = std::min(sx, sy);
    body(out, s, sx, sy, 14, 0.10);

    // The lid: the SP is a slim clamshell, so its body is a little wider and taller than the screen, with a
    // soft shadow where the lid's lower edge overhangs the hinge.
    const double m = sp_margin * unit;
    const double lx = screen.x - m, ly = screen.y - m, lw = screen.w + 2 * m, lh = screen.h + 2 * m;
    const double bx = std::max(0.0, lx - 16 * sx), bw = std::min(w - bx, lw + 32 * sx);
    const double by = std::max(0.0, ly - 14 * sy), bh = std::min(h - by, lh + 14 * sy + 54 * sy);
    // The lid's own soft edge: the shell is lit from above and rounds away at its sides.
    for (int y = int(by); y < int(by + bh); ++y)
        for (int x = int(bx); x < int(bx + bw); ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            const double ex = std::min(x - bx, bx + bw - x) / (20 * sx);
            const double ey = std::min(y - by, by + bh - y) / (12 * sy);
            Rgb c = Rgb::from_px(out.data()[size_t(y) * w + x]);
            c = scale(c, 0.90 + 0.10 * std::sqrt(std::clamp(std::min(ex, ey), 0.0, 1.0)));
            out.data()[size_t(y) * w + x] = c.px();
        }
    // The hinge along the bottom edge, and the seam above it.
    const double hinge = by + bh - 14 * sy;
    for (int y = int(hinge - 2 * sy); y < int(hinge); ++y)
        for (int x = int(bx); x < int(bx + bw); ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            Rgb c = Rgb::from_px(out.data()[size_t(y) * w + x]);
            out.data()[size_t(y) * w + x] = shade(c, -22).px();
        }
    for (int y = int(hinge); y < h; ++y)
        for (int x = 0; x < w; ++x) put(out, x, y, scale(s.bottom, 0.5), 170);

    lens(out, s, lx, ly, lw, lh, sp_radius * unit, unit);
    recess(out, screen, std::max(3, int(4 * unit)), scale(s.lens, 0.6));
    // The logo under the screen, in the lid: "GAME BOY ADVANCE SP", with SP in the sweep.
    const int size = int(20 * unit);
    const double baseline = screen.y + screen.h + m + 18 * sy;
    const bool pale = s.bottom.r + s.bottom.g + s.bottom.b > 430;
    name(out, font, s.print, w / 2.0, int(baseline), size, std::max(1, int(2 * unit)),
         pale ? Rgb::hex(0x2A2A34) : s.ink, pale ? std::vector<Rgb>{} : accent_for(s.print));
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
        // As large as fits: across the full width at the top, or inside the slim SP lens, which leaves room for the
        // lid around it and the logo under the screen.
        const double room_w = kind == Kind::Bar ? panel_w : panel_w - 2 * (sp_side * sx + sp_margin * unit);
        const double room_h = kind == Kind::Bar ? panel_h - bar_min * sy
                                                : panel_h - (sp_top + sp_bottom) * sy - 2 * sp_margin * unit - 70 * sy;
        const double s = std::min(room_w / game_w, room_h / game_h);
        place(s, 0, 0, panel_w, 0);
        l.y = kind == Kind::Bar ? 0 : int(sp_top * sy + sp_margin * unit);
        return l;
    }
    if (kind == Kind::Wsc || kind == Kind::Ngpc) {
        // The WonderSwan Color and the Neo Geo Pocket Color are tall portrait handhelds: the picture sits in their
        // upper half, and the body below it carries the controls.
        const double body_h = panel_h - 12 * sy;
        const double body_w = std::min(panel_w - 20 * sx, body_h / 1.35);
        // The screen across the body's width, filling it.
        const double s = std::min((body_w - 2 * 20 * sx) / game_w, (body_h * 0.55) / game_h);
        l.w = int(game_w * s);
        l.h = int(game_h * s);
        l.whole = std::abs(s - std::round(s)) < 1e-6;
        l.x = (panel_w - l.w) / 2;
        l.y = int((panel_h - body_h) / 2 + 18 * sy);
        return l;
    }
    if (kind == Kind::Psp) {
        // The PSP: a wide lens across the top of the body, the controls under it.
        const int room_w = panel_w - int(2 * 46 * sx), room_h = panel_h - int(2 * 90 * sy);
        const int s = std::max(1, std::min(room_w / game_w, room_h / game_h));
        l.w = game_w * s;
        l.h = game_h * s;
        l.x = (panel_w - l.w) / 2;
        l.y = int(30 * sy);
        return l;
    }
    if (kind == Kind::Nes) {
        // The NES: the screen in the middle of the front, the red stripe above it and the badge below. The screen
        // sits a little left of centre, the way it does on the console, with the badge beside it.
        const int room_w = panel_w - int(2 * 40 * sx), room_h = panel_h - int((26 + 74) * sy);
        const int s = std::max(1, std::min(room_w / game_w, room_h / game_h));
        l.w = game_w * s;
        l.h = game_h * s;
        l.x = (panel_w - l.w) / 2 - int(78 * sx);
        l.y = int((panel_h - l.h) / 2 - 4 * sy);
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
        // The Game Boy Advance: a wide body with the screen in a lens in its upper half and a strip of body below it
        // for the logo and the speaker. The body is the anchor: as wide as the panel allows, and as tall as the lens
        // and that strip need.
        const double body_w = panel_w - 2 * agb_edge * sx;
        // The scale: the lens is the game with the AGB's margins around it, and the body is the lens plus the strip.
        const int s = std::max(1, int((body_w - 2 * 52 * sx) / game_w));
        const double lens_h = game_h * s + (agb_top + agb_bottom) * sy;
        const double body_h = std::min(panel_h - 2 * agb_edge * sy, lens_h + 2 * 20 * sy + 96 * sy);
        l.w = game_w * s;
        l.h = game_h * s;
        l.x = (panel_w - l.w) / 2;
        l.y = int(std::max(agb_edge * sy, (panel_h - body_h) / 2 + 10 * sy));
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
    case Kind::Sp: draw_sp(out, screen, s, font); break;
    case Kind::Agb: draw_agb(out, screen, s, font); break;
    case Kind::Nes: draw_nes(out, screen, s, font); break;
    case Kind::Wsc:
    case Kind::Ngpc: draw_portrait(out, screen, s, font); break;
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
