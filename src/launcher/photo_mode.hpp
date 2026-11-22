#pragma once
// Photo mode: dress up a screenshot from the Album. Our own take on the handheld camera apps of the DS era: lenses
// that colour, mirror or bend the picture, stamps, a pen, frames, and the orange date a film camera printed in the
// corner. Every lens, stamp and frame here is drawn from code. The console frames are the player's own
// (play/src/ui/frame.cpp, shapes only, no pictures of real consoles).
//
// Pure CPU on a 640x480 play::Canvas, so the preview is the saved picture pixel for pixel:
//   source (one screen of the shot) -> lens -> frame (the photo scaled into its window) -> ink layer -> date stamp.
// The ink layer (pen strokes and stamps) is kept apart so a stroke recomposes only the last two steps.
#include "ui/canvas.hpp"
#include "ui/frame.hpp"
#include "ui/frame_styles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

namespace photo {

inline constexpr int W = 640, H = 480;

// Bytes R, G, B, A, as play::Canvas keeps them.
inline uint32_t rgba(int r, int g, int b, int a = 255) {
    return uint32_t(std::clamp(r, 0, 255)) | uint32_t(std::clamp(g, 0, 255)) << 8 |
           uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(a, 0, 255)) << 24;
}
inline int red(uint32_t v) { return int(v & 255); }
inline int green(uint32_t v) { return int(v >> 8 & 255); }
inline int blue(uint32_t v) { return int(v >> 16 & 255); }
inline int alpha(uint32_t v) { return int(v >> 24 & 255); }
inline uint32_t over(uint32_t dst, uint32_t src) {
    const int a = alpha(src);
    if (a == 255) return src | 0xFF000000u;
    if (!a) return dst;
    const int k = 255 - a;
    return rgba((red(src) * a + red(dst) * k) / 255, (green(src) * a + green(dst) * k) / 255,
                (blue(src) * a + blue(dst) * k) / 255);
}

// ---- Lenses ---------------------------------------------------------------------------------------------------------
enum class Lens { Normal, Sepia, Mono, Pop, Mosaic, Mirror, Kaleido, Fisheye, Vintage, Count };
inline constexpr std::array<const char*, size_t(Lens::Count)> lens_names = {
    "Normal", "Sepia", "Black & white", "Pop", "Mosaic", "Mirror", "Kaleidoscope", "Fisheye", "Old photo"};

inline void apply_lens(Lens lens, const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    out.resize(size_t(W) * H);
    auto tone = [&](auto fn) {
        for (size_t i = 0; i < out.size(); i++) out[i] = fn(in[i], int(i % W), int(i / W));
    };
    auto luma = [](uint32_t v) { return (red(v) * 77 + green(v) * 150 + blue(v) * 29) >> 8; };
    switch (lens) {
    case Lens::Sepia:
        tone([&](uint32_t v, int, int) {
            const int r = red(v), g = green(v), b = blue(v);
            return rgba((r * 101 + g * 197 + b * 48) >> 8, (r * 89 + g * 176 + b * 43) >> 8,
                        (r * 70 + g * 137 + b * 34) >> 8);
        });
        break;
    case Lens::Mono:
        tone([&](uint32_t v, int, int) {
            const int l = std::clamp((luma(v) - 128) * 9 / 8 + 128, 0, 255);
            return rgba(l, l, l);
        });
        break;
    case Lens::Pop:
        // More colour, then six steps a channel: a poster print.
        tone([&](uint32_t v, int, int) {
            const int l = luma(v);
            auto step = [&](int c) {
                c = std::clamp(l + (c - l) * 17 / 10, 0, 255);
                return std::min(255, (c * 6 / 256) * 51);
            };
            return rgba(step(red(v)), step(green(v)), step(blue(v)));
        });
        break;
    case Lens::Mosaic: {
        constexpr int cell = 16;
        for (int by = 0; by < H; by += cell)
            for (int bx = 0; bx < W; bx += cell) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int y = by; y < std::min(H, by + cell); y++)
                    for (int x = bx; x < std::min(W, bx + cell); x++, n++) {
                        const uint32_t v = in[size_t(y) * W + x];
                        r += red(v), g += green(v), b += blue(v);
                    }
                const uint32_t mean = rgba(r / n, g / n, b / n);
                for (int y = by; y < std::min(H, by + cell); y++)
                    for (int x = bx; x < std::min(W, bx + cell); x++)
                        out[size_t(y) * W + x] = (x == bx || y == by) ? over(mean, rgba(0, 0, 0, 28)) : mean;
            }
        break;
    }
    case Lens::Mirror:
        tone([&](uint32_t, int x, int y) { return in[size_t(y) * W + (x < W / 2 ? x : W - 1 - x)]; });
        break;
    case Lens::Kaleido:
        tone([&](uint32_t, int x, int y) {
            return in[size_t(y < H / 2 ? y : H - 1 - y) * W + (x < W / 2 ? x : W - 1 - x)];
        });
        break;
    case Lens::Fisheye: {
        // A bulge: the middle swells, the edges are pulled in; outside the lens is black.
        const double cx = W / 2.0, cy = H / 2.0, radius = std::hypot(cx, cy) * .82;
        tone([&](uint32_t, int x, int y) {
            const double dx = (x - cx) / radius, dy = (y - cy) / radius, d = std::hypot(dx, dy);
            if (d >= 1) return rgba(10, 10, 14);
            const double k = d > 0 ? std::pow(d, 1.7) / d : 0;
            const int sx = std::clamp(int(cx + dx * k * radius), 0, W - 1),
                      sy = std::clamp(int(cy + dy * k * radius), 0, H - 1);
            const uint32_t v = in[size_t(sy) * W + sx];
            const int shade = 255 - int(std::pow(d, 6) * 120);
            return rgba(red(v) * shade / 255, green(v) * shade / 255, blue(v) * shade / 255);
        });
        break;
    }
    case Lens::Vintage: {
        // Warm, faded, a soft dark corner and a little grain (seeded by position: the same every time).
        tone([&](uint32_t v, int x, int y) {
            const int l = luma(v);
            int r = (red(v) + l) / 2 + 22, g = (green(v) + l) / 2 + 10, b = (blue(v) + l) / 2 - 12;
            r = 40 + r * 180 / 255, g = 34 + g * 180 / 255, b = 30 + b * 170 / 255;
            const double dx = (x - W / 2.0) / (W / 2.0), dy = (y - H / 2.0) / (H / 2.0);
            const int vignette = 255 - int(std::clamp((dx * dx + dy * dy) - .45, 0.0, 1.0) * 150);
            const uint32_t hash = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u;
            const int grain = int((hash >> 7) % 17) - 8;
            return rgba((r + grain) * vignette / 255, (g + grain) * vignette / 255, (b + grain) * vignette / 255);
        });
        break;
    }
    default: out = in;
    }
}

// ---- Frames ---------------------------------------------------------------------------------------------------------
// Our own frames first, then every console frame the player draws (play/src/ui/frame_styles.hpp).
enum class Decor { Off, Instant, Film, Postage, Card, Hearts, Stars, Count };
inline constexpr std::array<const char*, size_t(Decor::Count)> decor_names = {
    "Off", "Instant photo", "Film strip", "Postage stamp", "Rounded card", "Hearts", "Stars"};

// The console frames that suit a single photo: a whole machine or set around one screen. The PSP's is left out: it
// frames a wide picture and leaves a 4:3 photo only a sliver of colour.
inline std::vector<int> console_frames() {
    std::vector<int> styles;
    for (int i = 1; i < play::frame_style_count; i++) {
        const auto kind = play::frame_styles[i].kind;
        if (kind == play::FrameKind::Off || kind == play::FrameKind::Bar || kind == play::FrameKind::Ctr ||
            kind == play::FrameKind::DmgInner || kind == play::FrameKind::PocketInner ||
            kind == play::FrameKind::BezelInner || kind == play::FrameKind::SpInner || kind == play::FrameKind::Psp)
            continue;
        styles.push_back(i);
    }
    return styles;
}
// A frame choice: 0..Decor::Count-1 our own, then console_frames() in order.
inline int frame_count(const std::vector<int>& consoles) { return int(Decor::Count) + int(consoles.size()); }
inline std::string frame_name(int choice, const std::vector<int>& consoles) {
    if (choice < int(Decor::Count)) return decor_names[size_t(choice)];
    return play::frame_styles[consoles.at(size_t(choice - int(Decor::Count)))].name;
}

// The photo scaled to fill `hole` (cropping what overflows, keeping its shape), bilinear: frames make it smaller.
inline void place(std::vector<uint32_t>& out, const std::vector<uint32_t>& photo, int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const double scale = std::max(double(w) / W, double(h) / H);
    const double ox = (W - w / scale) / 2, oy = (H - h / scale) / 2;
    for (int y = 0; y < h; y++) {
        if (y0 + y < 0 || y0 + y >= H) continue;
        const double sy = std::clamp(oy + (y + .5) / scale - .5, 0.0, H - 1.001);
        const int iy = int(sy), fy = int((sy - iy) * 256);
        for (int x = 0; x < w; x++) {
            if (x0 + x < 0 || x0 + x >= W) continue;
            const double sx = std::clamp(ox + (x + .5) / scale - .5, 0.0, W - 1.001);
            const int ix = int(sx), fx = int((sx - ix) * 256);
            const uint32_t a = photo[size_t(iy) * W + ix], b = photo[size_t(iy) * W + ix + 1],
                           c = photo[size_t(iy + 1) * W + ix], d = photo[size_t(iy + 1) * W + ix + 1];
            auto mixc = [&](int (*ch)(uint32_t)) {
                const int top = ch(a) * (256 - fx) + ch(b) * fx, bottom = ch(c) * (256 - fx) + ch(d) * fx;
                return (top * (256 - fy) + bottom * fy) >> 16;
            };
            out[size_t(y0 + y) * W + x0 + x] = rgba(mixc(red), mixc(green), mixc(blue));
        }
    }
}

inline void disc(std::vector<uint32_t>& px, double cx, double cy, double r, uint32_t colour) {
    for (int y = std::max(0, int(cy - r - 1)); y <= std::min(H - 1, int(cy + r + 1)); y++)
        for (int x = std::max(0, int(cx - r - 1)); x <= std::min(W - 1, int(cx + r + 1)); x++) {
            const double d = std::hypot(x + .5 - cx, y + .5 - cy) - r;
            if (d >= 1) continue;
            const int a = d <= 0 ? alpha(colour) : int(alpha(colour) * (1 - d));
            px[size_t(y) * W + x] = over(px[size_t(y) * W + x], (colour & 0xFFFFFFu) | uint32_t(a) << 24);
        }
}
inline void box(std::vector<uint32_t>& px, int x0, int y0, int w, int h, uint32_t colour) {
    for (int y = std::max(0, y0); y < std::min(H, y0 + h); y++)
        for (int x = std::max(0, x0); x < std::min(W, x0 + w); x++)
            px[size_t(y) * W + x] = over(px[size_t(y) * W + x], colour);
}
// A small heart or five-pointed star, filled, for the border patterns.
inline bool in_heart(double x, double y) { // x, y in -1..1, y down
    y = -y + .25;
    const double a = x * x + y * y - .55;
    return a * a * a - x * x * y * y * y <= 0;
}
inline bool in_star(double x, double y) {
    const double angle = std::atan2(y, x) + M_PI / 2, d = std::hypot(x, y);
    const double sector = std::fmod(angle + 10 * M_PI, 2 * M_PI / 5) - M_PI / 5;
    const double edge = .42 / std::cos(std::min(std::abs(sector), 1.2)) * (1 - std::abs(sector) / (M_PI / 5) * .55);
    return d <= std::max(edge, .38);
}
inline void shape(std::vector<uint32_t>& px, int cx, int cy, int size, uint32_t colour, bool heart) {
    for (int y = -size; y <= size; y++)
        for (int x = -size; x <= size; x++)
            if ((heart ? in_heart(x / double(size), y / double(size)) : in_star(x / double(size), y / double(size))) &&
                cx + x >= 0 && cx + x < W && cy + y >= 0 && cy + y < H)
                px[size_t(cy + y) * W + cx + x] = over(px[size_t(cy + y) * W + cx + x], colour);
}

inline void draw_decor(Decor decor, const std::vector<uint32_t>& photo, std::vector<uint32_t>& out) {
    out.assign(size_t(W) * H, rgba(0, 0, 0));
    switch (decor) {
    case Decor::Instant: // a white print, the wide margin underneath to write on
        out.assign(out.size(), rgba(246, 244, 238));
        place(out, photo, 30, 26, W - 60, H - 26 - 92);
        box(out, 30, 26, W - 60, 2, rgba(0, 0, 0, 30));
        break;
    case Decor::Film: { // a strip of film: dark bands with sprocket holes, a frame number
        out.assign(out.size(), rgba(22, 20, 18));
        place(out, photo, 24, 58, W - 48, H - 116);
        for (int x = 14; x < W; x += 40)
            for (int band : {16, H - 40}) {
                box(out, x, band, 22, 24, rgba(236, 230, 214));
                box(out, x, band, 22, 2, rgba(0, 0, 0, 60));
            }
        break;
    }
    case Decor::Postage: { // a stamp: perforated white edge on a soft blue backing
        out.assign(out.size(), rgba(198, 216, 236));
        box(out, 22, 22, W - 44, H - 44, rgba(252, 251, 247));
        for (int x = 22; x <= W - 22; x += 22) {
            disc(out, x, 22, 8, rgba(198, 216, 236));
            disc(out, x, H - 22, 8, rgba(198, 216, 236));
        }
        for (int y = 22; y <= H - 22; y += 22) {
            disc(out, 22, y, 8, rgba(198, 216, 236));
            disc(out, W - 22, y, 8, rgba(198, 216, 236));
        }
        place(out, photo, 52, 52, W - 104, H - 104);
        break;
    }
    case Decor::Card: { // rounded corners on a coloured card
        out.assign(out.size(), rgba(64, 132, 246));
        std::vector<uint32_t> inner(out.size(), 0);
        place(inner, photo, 28, 28, W - 56, H - 56);
        const int r = 34;
        for (int y = 28; y < H - 28; y++)
            for (int x = 28; x < W - 28; x++) {
                const int dx = std::max({28 + r - x, 0, x - (W - 29 - r)}),
                          dy = std::max({28 + r - y, 0, y - (H - 29 - r)});
                if (dx * dx + dy * dy <= r * r) out[size_t(y) * W + x] = inner[size_t(y) * W + x];
            }
        break;
    }
    case Decor::Hearts:
    case Decor::Stars: {
        const bool hearts = decor == Decor::Hearts;
        out.assign(out.size(), hearts ? rgba(250, 198, 214) : rgba(28, 36, 82));
        place(out, photo, 40, 40, W - 80, H - 80);
        int n = 0;
        for (int x = 20; x < W; x += 40, n++)
            for (int y : {20, H - 20})
                shape(out, x, y, 13, hearts ? (n % 2 ? rgba(232, 72, 120) : rgba(255, 255, 255)) : rgba(255, 214, 64),
                      hearts);
        for (int y = 60; y < H - 40; y += 40, n++)
            for (int x : {20, W - 20})
                shape(out, x, y, 13, hearts ? (n % 2 ? rgba(232, 72, 120) : rgba(255, 255, 255)) : rgba(255, 214, 64),
                      hearts);
        break;
    }
    default: out = photo;
    }
}

// ---- Stamps ---------------------------------------------------------------------------------------------------------
// 12x12 pixel stamps, our own. K outline, W white, R red, P pink, O orange, Y yellow, G green, B blue, V violet,
// N brown, . clear.
struct Stamp {
    const char* name;
    std::array<const char*, 12> rows;
};
inline const std::array<Stamp, 12> stamps = {{
    {"Heart",
     {"............", ".KKK...KKK..", "KRRRK.KRRRK.", "KRWRRKRRRRK.", "KRRRRRRRRRK.", "KRRRRRRRRRK.", ".KRRRRRRRK..",
      "..KRRRRRK...", "...KRRRK....", "....KRK.....", ".....K......", "............"}},
    {"Star",
     {".....KK.....", "....KYYK....", "....KYYK....", "...KYWYYK...", "KKKKYWYYKKKK", "KYYYYYYYYYYK", ".KYYYYYYYYK.",
      "..KYYYYYYK..", "..KYYYYYYK..", ".KYYYKKYYYK.", ".KYYK..KYYK.", ".KKK....KKK."}},
    {"Smile",
     {"...KKKKKK...", "..KYYYYYYK..", ".KYYYYYYYYK.", "KYYKYYYYKYYK", "KYYKYYYYKYYK", "KYYYYYYYYYYK", "KYPYYYYYYPYK",
      "KYYKYYYYKYYK", ".KYYKKKKYYK.", "..KYYYYYYK..", "...KKKKKK...", "............"}},
    {"Flower",
     {"....KKKK....", "...KPPPPK...", ".KKKPPPPKKK.", "KPPPKKKKPPPK", "KPPKYYYYKPPK", "KPPKYYYYKPPK", ".KKKKYYKKKK.",
      "...KPKKPK...", "...KPPPPK.K.", "....KKKKKGK.", "......KGGK..", "......KGK..."}},
    {"Music note",
     {"......KKKKK.", "......KBBBK.", "......KBKKK.", "......KBK...", "......KBK...", "......KBK...", "......KBK...",
      "...KKKKBK...", "..KBBBBBK...", "..KBBBBBK...", "...KKKKK....", "............"}},
    {"Sparkle",
     {".....K......", ".....K......", "....KWK.....", "....KWK.....", "..KKWWWKK...", "KKWWWWWWWKK.", "..KKWWWKK...",
      "....KWK.....", "....KWK..K..", ".....K..KWK.", ".....K...K..", "............"}},
    {"Crown",
     {"............", "K....KK....K", "KK..KYYK..KK", "KYKKYYYYKKYK", "KYYYYYYYYYYK", "KYYRYYYYBYYK", "KYYYYYYYYYYK",
      "KYYYYYYYYYYK", "KKKKKKKKKKKK", "KOOOOOOOOOOK", "KKKKKKKKKKKK", "............"}},
    {"Cat",
     {"K.........K.", "KK.......KK.", "KNK.KKK.KNK.", "KNNKNNNKNNK.", "KNNNNNNNNNK.", "KNKKNNNKKNK.", "KNWKNNNKWNK.",
      "KNNNNPNNNNK.", "KNNNKKKNNNK.", ".KNNNNNNNK..", "..KKKKKKK...", "............"}},
    {"Speech",
     {".KKKKKKKKKK.", "KWWWWWWWWWWK", "KWWWWKKWWWWK", "KWWWWKKWWWWK", "KWWWWKKWWWWK", "KWWWWWWWWWWK", "KWWWWKKWWWWK",
      "KWWWWWWWWWWK", ".KKKWWKKKKK.", "...KWK......", "...KK.......", "............"}},
    {"Lightning",
     {".....KKKKK..", "....KYYYK...", "...KYYYK....", "..KYYYK.....", ".KYYYYKKKK..", ".KKKKYYYYK..", "....KYYYK...",
      "...KYYYK....", "..KYYK......", ".KYK........", ".KK.........", "............"}},
    {"Ghost",
     {"...KKKKKK...", "..KWWWWWWK..", ".KWWWWWWWWK.", "KWWKKWWKKWWK", "KWWKBWWKBWWK", "KWWWWWWWWWWK", "KWWWWPPWWWWK",
      "KWWWWWWWWWWK", "KWWWWWWWWWWK", "KWKWWKKWWKWK", "KK.KK..KK.KK", "............"}},
    {"Arrow",
     {"....KK......", "....KGK.....", "....KGGK....", "KKKKKGGGK...", "KGGGGGGGGK..", "KGGGGGGGGGK.", "KGGGGGGGGK..",
      "KKKKKGGGK...", "....KGGK....", "....KGK.....", "....KK......", "............"}},
}};
inline uint32_t stamp_colour(char c) {
    switch (c) {
    case 'K': return rgba(28, 28, 34);
    case 'W': return rgba(255, 255, 255);
    case 'R': return rgba(232, 56, 64);
    case 'P': return rgba(248, 140, 182);
    case 'O': return rgba(250, 140, 40);
    case 'Y': return rgba(255, 212, 50);
    case 'G': return rgba(60, 184, 92);
    case 'B': return rgba(60, 120, 232);
    case 'V': return rgba(150, 90, 220);
    case 'N': return rgba(176, 120, 72);
    default: return 0;
    }
}
inline constexpr std::array<int, 3> stamp_scales = {3, 5, 8};
// A stamp's main colour (its most used one apart from the outline and white): the sparkles it throws when placed.
inline uint32_t stamp_tint(int index) {
    std::array<int, 128> counts{};
    for (const char* row : stamps[size_t(index)].rows)
        for (const char* c = row; *c; c++)
            if (*c != 'K' && *c != 'W' && *c != '.') counts[size_t(*c) & 127]++;
    const auto most = std::max_element(counts.begin(), counts.end()) - counts.begin();
    return counts[size_t(most)] ? stamp_colour(char(most)) : rgba(255, 255, 255);
}

// ---- Pen ------------------------------------------------------------------------------------------------------------
inline constexpr std::array<uint32_t, 8> inks = {0xFF221C1Cu, 0xFFFFFFFFu, 0xFF4038E8u, 0xFF288CFAu,
                                                 0xFF32D4FFu, 0xFF5CB83Cu, 0xFFE8783Cu, 0xFFB68CF8u};
inline constexpr std::array<const char*, 10> ink_names = {"Black", "White", "Red",  "Orange",  "Yellow",
                                                          "Green", "Blue",  "Pink", "Rainbow", "Eraser"};
inline constexpr int rainbow_ink = 8, eraser_ink = 9;
inline constexpr std::array<int, 3> pen_sizes = {3, 7, 14};
inline uint32_t rainbow(double t) {
    const double h = std::fmod(t, 1.0) * 6;
    const int i = int(h);
    const double f = h - i;
    const int up = int(255 * f), down = int(255 * (1 - f));
    switch (i) {
    case 0: return rgba(255, up, 0);
    case 1: return rgba(down, 255, 0);
    case 2: return rgba(0, 255, up);
    case 3: return rgba(0, down, 255);
    case 4: return rgba(up, 0, 255);
    default: return rgba(255, 0, down);
    }
}

// ---- Date stamp -----------------------------------------------------------------------------------------------------
// The orange seven-segment date a film camera burned into the corner: '26 11 20 (in the player's date order).
inline void segment_digit(std::vector<uint32_t>& px, int x, int y, int w, int h, char digit, uint32_t colour) {
    static const std::array<uint8_t, 10> masks = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    if (digit < '0' || digit > '9') return;
    const int t = std::max(2, w / 5), m = masks[size_t(digit - '0')];
    auto bar = [&](int bx, int by, int bw, int bh) {
        for (int yy = by; yy < by + bh; yy++)
            for (int xx = bx; xx < bx + bw; xx++)
                if (xx >= 0 && xx < W && yy >= 0 && yy < H) {
                    // bevelled ends, like the real segments
                    const bool horizontal = bw > bh;
                    const int along = horizontal ? std::min(xx - bx, bx + bw - 1 - xx)
                                                 : std::min(yy - by, by + bh - 1 - yy),
                              across = horizontal ? std::abs(2 * (yy - by) - (bh - 1)) / 2
                                                  : std::abs(2 * (xx - bx) - (bw - 1)) / 2;
                    if (across <= along) px[size_t(yy) * W + xx] = over(px[size_t(yy) * W + xx], colour);
                }
    };
    const int half = h / 2;
    if (m & 0x01) bar(x + 1, y, w - 2, t);
    if (m & 0x02) bar(x + w - t, y + 1, t, half - 1);
    if (m & 0x04) bar(x + w - t, y + half, t, half - 1);
    if (m & 0x08) bar(x + 1, y + h - t, w - 2, t);
    if (m & 0x10) bar(x, y + half, t, half - 1);
    if (m & 0x20) bar(x, y + 1, t, half - 1);
    if (m & 0x40) bar(x + 1, y + half - t / 2, w - 2, t);
}
// `order`: 0 day month, 1 month day, 2 year first (System > Date format).
inline std::string date_text(int year, int month, int day, int order) {
    auto two = [](int v) { return std::string(1, char('0' + v / 10 % 10)) + char('0' + v % 10); };
    const std::string y = "'" + two(year % 100);
    return order == 2   ? y + " " + two(month) + " " + two(day)
           : order == 1 ? two(month) + " " + two(day) + " " + y
                        : two(day) + " " + two(month) + " " + y;
}
inline void date_stamp(std::vector<uint32_t>& px, const std::string& text) {
    const int dw = 18, dh = 30, gap = 6, space = 16, apostrophe = 9;
    int width = 0;
    for (char c : text) width += c == ' ' ? space : c == '\'' ? apostrophe : dw + gap;
    const int x0 = W - 34 - width, y0 = H - 34 - dh;
    // A soft orange glow under bright digits, as burned into film.
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t colour = pass ? rgba(255, 150, 48, 240) : rgba(255, 96, 16, 70);
        const int grow = pass ? 0 : 2;
        int x = x0;
        for (char c : text) {
            if (c == ' ') x += space;
            else if (c == '\'') {
                box(px, x - grow, y0 - grow, 3 + 2 * grow, 8 + 2 * grow, colour);
                x += apostrophe;
            } else {
                segment_digit(px, x - grow, y0 - grow, dw + 2 * grow, dh + 2 * grow, c, colour);
                x += dw + gap;
            }
        }
    }
}

// ---- The editor -----------------------------------------------------------------------------------------------------
enum class Tool { Pen, Stamp, Frame, Lens, Date, Count };
inline constexpr std::array<const char*, size_t(Tool::Count)> tool_names = {"Pen", "Stamps", "Frames", "Lenses",
                                                                            "Date"};

class Editor {
public:
    Editor() : consoles_(console_frames()) {}
    // The shot's screen, 640x480 bytes R, G, B (any alpha), and its day for the date stamp.
    void open(std::vector<uint32_t> source, int year, int month, int day) {
        source_ = std::move(source);
        source_.resize(size_t(W) * H, rgba(0, 0, 0));
        year_ = year, month_ = month, day_ = day;
        ink_.assign(size_t(W) * H, 0);
        undo_.clear();
        lens = Lens::Normal;
        frame = 0;
        date = false;
        edited = false;
        base_dirty_ = true;
        revision_++;
    }
    Tool tool = Tool::Pen;
    Lens lens = Lens::Normal;
    int frame = 0, ink = 0, pen_size = 1, stamp = 0, stamp_size = 1, date_order = 0;
    bool date = false, edited = false;

    const std::vector<int>& consoles() const { return consoles_; }
    int frames() const { return frame_count(consoles_); }
    std::string frame_title() const { return frame_name(frame, consoles_); }
    void set_lens(Lens value) {
        if (value == lens) return;
        lens = value;
        base_dirty_ = edited = true;
        revision_++;
    }
    void set_frame(int value) {
        value = std::clamp(value, 0, frames() - 1);
        if (value == frame) return;
        frame = value;
        base_dirty_ = edited = true;
        revision_++;
    }
    void set_date(bool on) {
        if (on != date) date = on, edited = true, revision_++;
    }

    // A pen or eraser stroke from the last point to (x, y) in picture pixels; begin() starts a new one.
    void begin(double x, double y) {
        snapshot();
        last_x_ = x, last_y_ = y;
        stroke_length_ = 0;
        dab(x, y);
    }
    void stroke(double x, double y) {
        const double dx = x - last_x_, dy = y - last_y_, d = std::hypot(dx, dy);
        const double step = std::max(1.0, pen_sizes[size_t(pen_size)] * .35);
        // The rainbow ink turns along the stroke dab by dab, not once per move.
        const double start = stroke_length_;
        for (double t = step; t <= d; t += step) {
            stroke_length_ = start + t;
            dab(last_x_ + dx * t / d, last_y_ + dy * t / d);
        }
        stroke_length_ = start;
        if (d >= step) {
            stroke_length_ += d;
            last_x_ = x, last_y_ = y;
        }
    }
    void place_stamp(double cx, double cy) {
        snapshot();
        const auto& s = stamps[size_t(stamp)];
        const int scale = stamp_scales[size_t(stamp_size)], x0 = int(cx) - 6 * scale, y0 = int(cy) - 6 * scale;
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 12; x++) {
                const uint32_t c = stamp_colour(s.rows[size_t(y)][x]);
                if (c) box(ink_, x0 + x * scale, y0 + y * scale, scale, scale, c);
            }
        edited = true;
        revision_++;
    }
    // Clear all: every stroke and stamp at once (one undo step brings them back).
    bool clear() {
        if (std::all_of(ink_.begin(), ink_.end(), [](uint32_t v) { return v == 0; })) return false;
        snapshot();
        std::fill(ink_.begin(), ink_.end(), 0);
        edited = true;
        revision_++;
        return true;
    }
    bool undo() {
        if (undo_.empty()) return false;
        ink_ = std::move(undo_.back());
        undo_.pop_back();
        revision_++;
        return true;
    }
    bool can_undo() const { return !undo_.empty(); }

    // The finished picture (redone as little as possible).
    const std::vector<uint32_t>& compose() {
        if (!base_dirty_ && composed_ == revision_) return out_;
        if (base_dirty_) {
            std::vector<uint32_t> lensed;
            apply_lens(lens, source_, lensed);
            if (frame < int(Decor::Count)) draw_decor(Decor(frame), lensed, base_);
            else {
                play::Canvas canvas(W, H);
                const int style = consoles_.at(size_t(frame - int(Decor::Count)));
                const auto layout = play::frame_layout(W, H, 256, 192, style);
                if (font) play::draw_frame(canvas, layout, style, *font);
                base_.assign(canvas.data(), canvas.data() + size_t(W) * H);
                place(base_, lensed, layout.x, layout.y, layout.w, layout.h);
            }
            base_dirty_ = false;
        }
        out_ = base_;
        for (size_t i = 0; i < out_.size(); i++)
            if (ink_[i]) out_[i] = over(out_[i], ink_[i]);
        if (date) date_stamp(out_, date_text(year_, month_, day_, date_order));
        composed_ = revision_;
        return out_;
    }
    // Bumped by every change to the picture: a texture showing it is uploaded again only when this moves.
    uint32_t revision() const { return revision_; }
    const play::Font* font = nullptr; // for the console frames' printed names

private:
    void snapshot() {
        undo_.push_back(ink_);
        if (undo_.size() > 8) undo_.pop_front();
    }
    void dab(double cx, double cy) {
        const double r = pen_sizes[size_t(pen_size)];
        for (int y = std::max(0, int(cy - r - 1)); y <= std::min(H - 1, int(cy + r + 1)); y++)
            for (int x = std::max(0, int(cx - r - 1)); x <= std::min(W - 1, int(cx + r + 1)); x++) {
                const double d = std::hypot(x + .5 - cx, y + .5 - cy) - r;
                if (d >= 1) continue;
                auto& p = ink_[size_t(y) * W + x];
                if (ink == eraser_ink) {
                    p = 0;
                    continue;
                }
                const uint32_t c = ink == rainbow_ink ? rainbow(stroke_length_ / 400.0) : inks[size_t(ink)];
                const int a = d <= 0 ? 255 : int(255 * (1 - d));
                p = p ? over(p, (c & 0xFFFFFFu) | uint32_t(a) << 24) : ((c & 0xFFFFFFu) | uint32_t(a) << 24);
            }
        edited = true;
        revision_++;
    }
    std::vector<int> consoles_;
    std::vector<uint32_t> source_, base_, ink_, out_;
    std::deque<std::vector<uint32_t>> undo_;
    bool base_dirty_ = true;
    uint32_t revision_ = 1, composed_ = 0;
    int year_ = 2026, month_ = 1, day_ = 1;
    double last_x_ = 0, last_y_ = 0, stroke_length_ = 0;
};

} // namespace photo
