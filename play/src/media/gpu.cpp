// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/gpu.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <zlib.h>

namespace media {

bool upload(SDL_Renderer* r, Tex& tex, const uint32_t* px, int w, int h, bool blend, bool linear) {
    if (w <= 0 || h <= 0 || !px) return false;
    if (!tex.t || tex.w != w || tex.h != h) {
        tex.reset();
        tex.t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!tex.t) return false;
        tex.w = w;
        tex.h = h;
    }
    SDL_SetTextureBlendMode(tex.t, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(tex.t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    return SDL_UpdateTexture(tex.t, nullptr, px, w * 4) == 0;
}

void draw(SDL_Renderer* r, const Tex& tex, double x, double y, double w, double h, int alpha) {
    if (!tex.t || alpha <= 0) return;
    SDL_FRect d{float(x), float(y), float(w), float(h)};
    SDL_SetTextureAlphaMod(tex.t, Uint8(std::clamp(alpha, 0, 255)));
    if (alpha < 255) {
        SDL_BlendMode mode;
        SDL_GetTextureBlendMode(tex.t, &mode);
        if (mode == SDL_BLENDMODE_NONE) SDL_SetTextureBlendMode(tex.t, SDL_BLENDMODE_BLEND);
        SDL_RenderCopyF(r, tex.t, nullptr, &d);
        if (mode == SDL_BLENDMODE_NONE) SDL_SetTextureBlendMode(tex.t, SDL_BLENDMODE_NONE);
    } else {
        SDL_RenderCopyF(r, tex.t, nullptr, &d);
    }
}

void draw_turned(SDL_Renderer* r, const Tex& tex, double cx, double cy, double w, double h, double angle, int alpha) {
    if (!tex.t || alpha <= 0) return;
    SDL_FRect d{float(cx - w / 2), float(cy - h / 2), float(w), float(h)};
    SDL_SetTextureAlphaMod(tex.t, Uint8(std::clamp(alpha, 0, 255)));
    SDL_RenderCopyExF(r, tex.t, nullptr, &d, angle, nullptr, SDL_FLIP_NONE);
}

void draw_part(SDL_Renderer* r, const Tex& tex, const SDL_Rect& src, double x, double y, double w, double h,
               int alpha) {
    if (!tex.t || alpha <= 0) return;
    SDL_FRect d{float(x), float(y), float(w), float(h)};
    SDL_SetTextureAlphaMod(tex.t, Uint8(std::clamp(alpha, 0, 255)));
    SDL_RenderCopyF(r, tex.t, &src, &d);
}

void fill(SDL_Renderer* r, double x, double y, double w, double h, Rgb c, int alpha) {
    SDL_SetRenderDrawBlendMode(r, alpha < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, Uint8(std::clamp(alpha, 0, 255)));
    SDL_FRect d{float(x), float(y), float(w), float(h)};
    SDL_RenderFillRectF(r, &d);
}

// ---- Polygons and glyphs

void polygon(Canvas& c, const std::vector<std::pair<double, double>>& pts, Rgb ink, int alpha) {
    if (pts.size() < 3) return;
    double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    for (auto& p : pts) {
        minx = std::min(minx, p.first), maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second), maxy = std::max(maxy, p.second);
    }
    int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(c.w() - 1, int(std::ceil(maxx)));
    int y0 = std::max(0, int(std::floor(miny))), y1 = std::min(c.h() - 1, int(std::ceil(maxy)));
    auto inside = [&](double x, double y) {
        bool in = false;
        for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
            const auto &a = pts[i], &b = pts[j];
            if ((a.second > y) != (b.second > y) &&
                x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first)
                in = !in;
        }
        return in;
    };
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int hits = 0; // 4x4 samples a pixel
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) hits += inside(x + (sx + .5) / 4, y + (sy + .5) / 4);
            if (hits) c.fill_rect(x, y, 1, 1, ink, alpha * hits / 16);
        }
}

namespace {
using Pts = std::vector<std::pair<double, double>>;
void ring_arc(Canvas& c, double cx, double cy, double r, double thick, double from, double to, Rgb ink, int alpha) {
    Pts p;
    const int steps = 28;
    for (int i = 0; i <= steps; ++i) {
        double a = from + (to - from) * i / steps;
        p.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r});
    }
    for (int i = steps; i >= 0; --i) {
        double a = from + (to - from) * i / steps;
        p.push_back({cx + std::cos(a) * (r - thick), cy + std::sin(a) * (r - thick)});
    }
    polygon(c, p, ink, alpha);
}
} // namespace

void glyph(Canvas& c, Glyph g, int cx, int cy, int size, Rgb ink, Rgb ground, int alpha) {
    const double s = size / 24.0; // drawn on a 24-unit grid
    auto P = [&](double x, double y) { return std::make_pair(cx + (x - 12) * s, cy + (y - 12) * s); };
    auto rect = [&](double x, double y, double w, double h, Rgb k) {
        polygon(c, {P(x, y), P(x + w, y), P(x + w, y + h), P(x, y + h)}, k, alpha);
    };
    auto round_box = [&](double x, double y, double w, double h, double r, Rgb k) {
        Pts p;
        const double pi = 3.14159265;
        const double corners[4][3] = {
            {x + w - r, y + r, -pi / 2}, {x + w - r, y + h - r, 0}, {x + r, y + h - r, pi / 2}, {x + r, y + r, pi}};
        for (auto& k4 : corners)
            for (int i = 0; i <= 6; ++i) {
                double a = k4[2] + pi / 2 * i / 6;
                p.push_back(P(k4[0] + std::cos(a) * r, k4[1] + std::sin(a) * r));
            }
        polygon(c, p, k, alpha);
    };
    const double pi = 3.14159265;
    switch (g) {
    case Glyph::Play: polygon(c, {P(6.5, 3), P(21, 12), P(6.5, 21)}, ink, alpha); break;
    case Glyph::Pause:
        round_box(5, 3, 5, 18, 1.2, ink);
        round_box(14, 3, 5, 18, 1.2, ink);
        break;
    case Glyph::Previous:
        round_box(3.5, 4, 3, 16, 1, ink);
        polygon(c, {P(20.5, 4), P(20.5, 20), P(7.5, 12)}, ink, alpha);
        break;
    case Glyph::Next:
        round_box(17.5, 4, 3, 16, 1, ink);
        polygon(c, {P(3.5, 4), P(16.5, 12), P(3.5, 20)}, ink, alpha);
        break;
    case Glyph::Back10:
    case Glyph::Forward10: {
        const bool back = g == Glyph::Back10;
        // An arrow going round, open at the top, and "10" inside.
        if (back) {
            ring_arc(c, cx, cy + 1 * s, 10.5 * s, 2.2 * s, -pi * .5 + .35, pi * 1.5 - .1, ink, alpha);
            polygon(c, {P(13.5, -1.5), P(13.5, 7.5), P(7.5, 3)}, ink, alpha);
        } else {
            ring_arc(c, cx, cy + 1 * s, 10.5 * s, 2.2 * s, -pi * .5 + .1, pi * 1.5 - .35, ink, alpha);
            polygon(c, {P(10.5, -1.5), P(10.5, 7.5), P(16.5, 3)}, ink, alpha);
        }
        // "1" and "0" as bars, so the glyph needs no font.
        rect(8, 9, 1.8, 8, ink);
        round_box(11.5, 9, 5, 8, 2.2, ink);
        round_box(13.1, 10.7, 1.8, 4.6, .8, ground);
        break;
    }
    case Glyph::Subtitles:
        round_box(2, 4, 20, 16, 3, ink);
        rect(5, 11, 5, 2, ground);
        rect(11.5, 11, 7.5, 2, ground);
        rect(5, 15, 9, 2, ground);
        rect(15.5, 15, 3.5, 2, ground);
        break;
    case Glyph::Audio:
        polygon(c, {P(3, 9), P(8, 9), P(14, 3.5), P(14, 20.5), P(8, 15), P(3, 15)}, ink, alpha);
        ring_arc(c, cx + 2 * s, cy, 6.5 * s, 2 * s, -pi / 3.2, pi / 3.2, ink, alpha);
        ring_arc(c, cx + 2 * s, cy, 10.5 * s, 2 * s, -pi / 3.2, pi / 3.2, ink, alpha);
        break;
    case Glyph::Speed: // a gauge with its needle
        ring_arc(c, cx, cy + 3 * s, 10 * s, 2.4 * s, pi * 1.0, pi * 2.0, ink, alpha);
        polygon(c, {P(11, 16), P(13, 16), P(18.5, 6.5)}, ink, alpha);
        round_box(10, 13.5, 4, 4, 2, ink);
        break;
    case Glyph::Picture:
        round_box(1.5, 5, 21, 14, 2.5, ink);
        round_box(3.8, 7.3, 16.4, 9.4, 1, ground);
        polygon(c, {P(5, 15.5), P(10, 10), P(13, 13), P(15, 11), P(19, 15.5)}, ink, alpha);
        break;
    case Glyph::Moon:
        polygon(
            c,
            [&] {
                Pts p;
                for (int i = 0; i <= 24; ++i) {
                    double a = pi * .35 + pi * 1.3 * i / 24;
                    p.push_back(P(12 + std::cos(a) * 9, 12 + std::sin(a) * 9));
                }
                for (int i = 24; i >= 0; --i) {
                    double a = pi * .5 + pi * 1.0 * i / 24;
                    p.push_back(P(16 + std::cos(a) * 7, 8 + std::sin(a) * 7));
                }
                return p;
            }(),
            ink, alpha);
        break;
    case Glyph::Book:
        polygon(c, {P(2, 5), P(11, 6.5), P(11, 21), P(2, 19.5)}, ink, alpha);
        polygon(c, {P(13, 6.5), P(22, 5), P(22, 19.5), P(13, 21)}, ink, alpha);
        break;
    case Glyph::Pages:
        round_box(7, 2, 13, 17, 1.5, ink);
        round_box(4, 5, 13, 17, 1.5, ground);
        round_box(5.3, 6.3, 10.4, 14.4, 1, ink);
        break;
    case Glyph::Contents:
        for (int i = 0; i < 3; ++i) {
            round_box(3, 5 + i * 6, 3, 3, 1.5, ink);
            round_box(8, 5.3 + i * 6, 13, 2.4, 1.2, ink);
        }
        break;
    case Glyph::Paper:
        round_box(5, 2, 14, 20, 2, ink);
        polygon(c, {P(12, 2), P(19, 2), P(19, 9)}, ground, alpha);
        rect(8, 12, 8, 1.8, ground);
        rect(8, 16, 6, 1.8, ground);
        break;
    case Glyph::TextSize: // a small and a large A
        polygon(c, {P(1.5, 20), P(5.5, 9), P(7.5, 9), P(11.5, 20), P(9.3, 20), P(6.5, 12), P(3.7, 20)}, ink, alpha);
        polygon(c, {P(10, 20), P(15.5, 3), P(18.5, 3), P(24, 20), P(21, 20), P(17, 7.5), P(13, 20)}, ink, alpha);
        rect(14, 14, 6, 2, ink);
        break;
    case Glyph::Direction:
        polygon(c, {P(2, 12), P(9, 5), P(9, 9.5), P(22, 9.5), P(22, 14.5), P(9, 14.5), P(9, 19)}, ink, alpha);
        break;
    case Glyph::Film:
        round_box(2, 4, 20, 16, 2.5, ink);
        for (int i = 0; i < 4; ++i) {
            rect(3.6, 6 + i * 3.4, 2, 2, ground);
            rect(18.4, 6 + i * 3.4, 2, 2, ground);
        }
        round_box(7.5, 6.5, 9, 11, 1, ground);
        break;
    case Glyph::Folder: polygon(c, {P(2, 5), P(9, 5), P(11, 7.5), P(22, 7.5), P(22, 20), P(2, 20)}, ink, alpha); break;
    case Glyph::Close:
        polygon(c, {P(4, 6), P(6, 4), P(20, 18), P(18, 20)}, ink, alpha);
        polygon(c, {P(18, 4), P(20, 6), P(6, 20), P(4, 18)}, ink, alpha);
        break;
    case Glyph::Lock:
        ring_arc(c, cx, cy - 2 * s, 6 * s, 2.2 * s, pi, pi * 2, ink, alpha);
        rect(6, 8, 2.2, 3, ink);
        rect(15.8, 8, 2.2, 3, ink);
        round_box(4, 10, 16, 12, 2, ink);
        break;
    }
}

// ---- Motion

double ease_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}
double ease_in_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < .5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

bool Ripples::draw(SDL_Renderer* r, uint32_t now, Rgb tint, bool animations) {
    if (!disc) {
        // A soft-edged white disc, 128 px: one texture, scaled per ripple.
        const int n = 128;
        std::vector<uint32_t> px(size_t(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double d = std::hypot(x + .5 - n / 2.0, y + .5 - n / 2.0) / (n / 2.0);
                double a = d >= 1 ? 0 : d > .86 ? (1 - d) / .14 : 1;
                px[size_t(y) * n + x] = 0x00FFFFFFu | (uint32_t(a * 255) << 24);
            }
        upload(r, disc, px.data(), n, n, true, true);
    }
    if (!animations) {
        live.clear();
        return false;
    }
    SDL_SetTextureColorMod(disc.t, tint.r, tint.g, tint.b);
    for (auto it = live.begin(); it != live.end();) {
        double grow = ease_out((now - it->at) / 320.0);
        double fade = it->held ? 1.0 : 1.0 - ease_out((now - it->released) / 260.0);
        if (!it->held && fade <= 0.01) {
            it = live.erase(it);
            continue;
        }
        double rad = it->radius * (.25 + .75 * grow);
        media::draw(r, disc, it->x - rad, it->y - rad, rad * 2, rad * 2, int(70 * fade));
        ++it;
    }
    return !live.empty();
}

// ---- The status bar

void Power::read(uint32_t now) {
    if (read_at && now - read_at < 10000) return;
    read_at = now ? now : 1;
    const char* fixed = std::getenv("RGDS_MEDIA_BATTERY"); // tests and previews: "82" or "82+"
    if (fixed && *fixed) {
        percent = std::atoi(fixed);
        charging = std::string(fixed).find('+') != std::string::npos;
        return;
    }
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string("/sys/class/power_supply/") + e->d_name;
        std::ifstream type(base + "/type");
        std::string t;
        type >> t;
        if (t != "Battery") continue;
        std::ifstream cap(base + "/capacity"), status(base + "/status");
        int p = -1;
        std::string st;
        cap >> p;
        status >> st;
        percent = p;
        charging = st == "Charging";
        break;
    }
    closedir(dir);
}

void status_bar(Canvas& c, const play::Look& look, const play::Font& font, const Power& power,
                const std::string& left_title) {
    const int h = c.h(), w = c.w();
    // The launcher's bar: the panel colour with a hairline under it.
    c.fill_rect(0, 0, w, h, look.panel);
    c.fill_rect(0, h - 1, w, 1, play::mix(look.panel, look.line, 140));
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    char clock[32];
    if (look.twelve_hour) {
        int hr = local.tm_hour % 12;
        std::snprintf(clock, sizeof clock, "%d/%d  %d:%02d %s", local.tm_mday, local.tm_mon + 1, hr ? hr : 12,
                      local.tm_min, local.tm_hour < 12 ? "AM" : "PM");
    } else {
        std::snprintf(clock, sizeof clock, "%d/%d  %02d:%02d", local.tm_mday, local.tm_mon + 1, local.tm_hour,
                      local.tm_min);
    }
    const int size = 20, base = font.centred(size, h / 2);
    int x = w - 14;
    // Battery: a body with four cells, as the launcher draws it.
    if (power.percent >= 0) {
        const int bw = 34, bh = 18, by = (h - bh) / 2;
        x -= bw + 3;
        c.rounded_rect(x, by, bw, bh, 3, look.muted);
        c.rounded_rect(x + 2, by + 2, bw - 4, bh - 4, 2, look.slot);
        c.fill_rect(x + bw, by + 5, 3, bh - 10, look.muted);
        int bars = power.percent > 75 ? 4 : power.percent > 45 ? 3 : power.percent > 15 ? 2 : power.percent > 0 ? 1 : 0;
        Rgb level = power.percent <= 15 && !power.charging ? Rgb{239, 129, 75} : look.accent;
        for (int i = 0; i < 4; ++i)
            c.fill_rect(x + 4 + i * 7, by + 4, 5, bh - 8, i < bars ? level : play::mix(look.slot, look.muted, 40));
        std::string pct = std::to_string(power.percent) + "%";
        x -= 10 + font.measure(pct, size);
        font.draw(c, x, base, pct, size, look.ink);
        x -= 22;
    }
    x -= font.measure(clock, size);
    font.draw(c, x, base, clock, size, look.ink);
    if (!left_title.empty()) font.draw(c, 16, base, font.fit(left_title, size, x - 40), size, look.ink);
}

void focus_corners(SDL_Renderer* r, double x, double y, double w, double h, Rgb accent, int alpha) {
    const double len = std::min(18.0, std::min(w, h) / 3), t = 3, out = 4;
    x -= out, y -= out, w += 2 * out, h += 2 * out;
    auto bar = [&](double bx, double by, double bw, double bh) { fill(r, bx, by, bw, bh, accent, alpha); };
    bar(x, y, len, t), bar(x, y, t, len);
    bar(x + w - len, y, len, t), bar(x + w - t, y, t, len);
    bar(x, y + h - t, len, t), bar(x, y + h - len, t, len);
    bar(x + w - len, y + h - t, len, t), bar(x + w - t, y + h - len, t, len);
}

// ---- PNG

bool write_png(const std::string& path, const uint32_t* px, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint32_t p = px[size_t(y) * w + x];
            raw.push_back(uint8_t(p)), raw.push_back(uint8_t(p >> 8)), raw.push_back(uint8_t(p >> 16));
        }
    }
    uLongf len = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(len);
    if (compress2(z.data(), &len, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    z.resize(len);
    std::ofstream f(path, std::ios::binary);
    auto be = [&](uint32_t v) {
        uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        f.write(reinterpret_cast<char*>(b), 4);
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be(uint32_t(data.size()));
        f.write(type, 4);
        if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        if (!data.empty()) crc = crc32(crc, data.data(), uInt(data.size()));
        be(uint32_t(crc));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24),
                                 uint8_t(w >> 16),
                                 uint8_t(w >> 8),
                                 uint8_t(w),
                                 uint8_t(h >> 24),
                                 uint8_t(h >> 16),
                                 uint8_t(h >> 8),
                                 uint8_t(h),
                                 8,
                                 2,
                                 0,
                                 0,
                                 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return bool(f);
}

} // namespace media
