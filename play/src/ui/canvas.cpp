// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace play {

Rgb mix(Rgb a, Rgb b, int t) {
    t = std::clamp(t, 0, 256);
    return {uint8_t((a.r * (256 - t) + b.r * t) >> 8), uint8_t((a.g * (256 - t) + b.g * t) >> 8),
            uint8_t((a.b * (256 - t) + b.b * t) >> 8)};
}

void Canvas::blend(uint32_t& dst, Rgb c, int alpha) {
    if (alpha >= 255) {
        dst = c.px();
        return;
    }
    dst = mix(Rgb::from_px(dst), c, alpha + (alpha >> 7)).px();
}

void Canvas::fill(Rgb c) { std::fill(px_.begin(), px_.end(), c.px()); }

void Canvas::fill_rect(int x, int y, int w, int h, Rgb c, int alpha) {
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) blend(px_[static_cast<size_t>(yy) * w_ + xx], c, alpha);
}

void Canvas::rounded_rect(int x, int y, int w, int h, int radius, Rgb c, int alpha) {
    radius = std::min({radius, w / 2, h / 2});
    for (int yy = std::max(0, y); yy < std::min(h_, y + h); ++yy) {
        int dy = yy - y < radius ? radius - (yy - y) : (yy >= y + h - radius ? yy - (y + h - radius) + 1 : 0);
        for (int xx = std::max(0, x); xx < std::min(w_, x + w); ++xx) {
            int dx = xx - x < radius ? radius - (xx - x) : (xx >= x + w - radius ? xx - (x + w - radius) + 1 : 0);
            int a = alpha;
            if (dx && dy) {
                double d = std::sqrt(double(dx) * dx + double(dy) * dy) - radius;
                if (d >= 0.5) continue;
                if (d > -0.5) a = static_cast<int>(alpha * (0.5 - d));
            }
            blend(px_[static_cast<size_t>(yy) * w_ + xx], c, a);
        }
    }
}

void Canvas::vertical_gradient(Rgb top, Rgb bottom) {
    for (int y = 0; y < h_; ++y) {
        uint32_t v = mix(top, bottom, h_ > 1 ? y * 256 / (h_ - 1) : 0).px();
        std::fill_n(px_.begin() + static_cast<size_t>(y) * w_, w_, v);
    }
}

namespace {
// One scaled row: each source pixel written `scale` times, no division per
// pixel (the A55 pays for those). Clipped to [0, w).
template <typename Put> void scaled_row(const uint32_t* src, int sw, int dx, int scale, int w, Put put) {
    int sx = 0, tx = dx;
    if (tx < 0) {
        sx = -tx / scale;
        tx += sx * scale;
    }
    for (; sx < sw && tx < w; ++sx) {
        uint32_t v = src[sx];
        for (int k = 0; k < scale; ++k, ++tx)
            if (tx >= 0 && tx < w) put(tx, v);
    }
}
} // namespace

void Canvas::blit(const uint32_t* src, int sw, int sh, int dx, int dy, int scale, int alpha) {
    if (scale < 1) scale = 1;
    for (int sy = 0; sy < sh; ++sy) {
        const uint32_t* row = src + static_cast<size_t>(sy) * sw;
        for (int k = 0; k < scale; ++k) {
            int ty = dy + sy * scale + k;
            if (ty < 0 || ty >= h_) continue;
            uint32_t* out = px_.data() + static_cast<size_t>(ty) * w_;
            if (k > 0 && alpha >= 255 && ty > 0 && dy + sy * scale + k - 1 >= 0) {
                // the same as the row above: copy it
                int x0 = std::max(0, dx), x1 = std::min(w_, dx + sw * scale);
                if (x1 > x0) std::copy(out - w_ + x0, out - w_ + x1, out + x0);
                continue;
            }
            if (alpha >= 255) scaled_row(row, sw, dx, scale, w_, [&](int x, uint32_t v) { out[x] = v | 0xFF000000u; });
            else scaled_row(row, sw, dx, scale, w_, [&](int x, uint32_t v) { blend(out[x], Rgb::from_px(v), alpha); });
        }
    }
}

void Canvas::blit_keyed(const uint32_t* src, int sw, int sh, int dx, int dy, int scale, uint32_t key) {
    if (scale < 1) scale = 1;
    for (int sy = 0; sy < sh; ++sy) {
        const uint32_t* row = src + static_cast<size_t>(sy) * sw;
        for (int k = 0; k < scale; ++k) {
            int ty = dy + sy * scale + k;
            if (ty < 0 || ty >= h_) continue;
            uint32_t* out = px_.data() + static_cast<size_t>(ty) * w_;
            scaled_row(row, sw, dx, scale, w_, [&](int x, uint32_t v) {
                if ((v & 0xFFFFFF) != key) out[x] = v | 0xFF000000u;
            });
        }
    }
}

void Canvas::cover(const uint32_t* src, int sw, int sh, int stride, int level) {
    if (!src || sw <= 0 || sh <= 0) return;
    // source pixels per canvas pixel, in 16.16, the larger scale so it covers
    int64_t step = std::min((int64_t(sw) << 16) / w_, (int64_t(sh) << 16) / h_);
    int64_t x0 = ((int64_t(sw) << 16) - step * w_) / 2, y0 = ((int64_t(sh) << 16) - step * h_) / 2;
    for (int y = 0; y < h_; ++y) {
        const uint32_t* row = src + static_cast<size_t>(std::min<int64_t>(sh - 1, (y0 + step * y) >> 16)) * stride;
        uint32_t* out = px_.data() + static_cast<size_t>(y) * w_;
        int64_t sx = x0;
        for (int x = 0; x < w_; ++x, sx += step) {
            uint32_t v = row[std::min<int64_t>(sw - 1, sx >> 16)];
            out[x] = ((v & 0xFF) * level >> 8) | ((((v >> 8) & 0xFF) * level >> 8) << 8) |
                     ((((v >> 16) & 0xFF) * level >> 8) << 16) | 0xFF000000u;
        }
    }
}

void Canvas::dim(int level) {
    for (auto& v : px_) {
        Rgb c = Rgb::from_px(v);
        v = Rgb{uint8_t(c.r * level >> 8), uint8_t(c.g * level >> 8), uint8_t(c.b * level >> 8)}.px();
    }
}

std::vector<unsigned> utf8_codepoints(const std::string& s) {
    std::vector<unsigned> out;
    for (size_t i = 0; i < s.size();) {
        unsigned cp = static_cast<unsigned char>(s[i++]);
        int extra = cp < 0x80 ? 0 : (cp & 0xE0) == 0xC0 ? 1 : (cp & 0xF0) == 0xE0 ? 2 : (cp & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) {
            out.push_back('?');
            continue;
        }
        cp &= extra ? (0x3F >> extra) : 0x7F;
        for (int k = 0; k < extra && i < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
        out.push_back(cp);
    }
    return out;
}

namespace {
bool read32(std::ifstream& f, uint32_t& v) {
    unsigned char b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    v = b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}
} // namespace

bool Font::load(const std::string& directory) {
    for (int size : {16, 20, 24, 28, 36}) {
        std::ifstream f(directory + "/rounded-" + std::to_string(size) + ".rgf", std::ios::binary);
        char magic[4];
        if (!f.read(magic, 4) || std::string(magic, 4) != "RGF1") continue;
        Atlas a;
        a.size = size;
        uint32_t w, h, asc, count;
        if (!read32(f, w) || !read32(f, h) || !read32(f, asc) || !read32(f, count) || w > 1024 || h > 2048 ||
            count > 512)
            continue;
        a.width = int(w);
        a.height = int(h);
        a.ascent = int(asc);
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; ++i) {
            uint32_t v[8];
            for (auto& x : v) ok = ok && read32(f, x);
            Glyph g{int(v[1]), int(v[2]), int(v[3]), int(v[4]), int32_t(v[5]), int32_t(v[6]), int(v[7])};
            ok = ok && g.x >= 0 && g.y >= 0 && g.x + g.w <= a.width && g.y + g.h <= a.height;
            a.glyphs[v[0]] = g;
        }
        a.alpha.resize(static_cast<size_t>(a.width) * a.height);
        if (!ok || !f.read(reinterpret_cast<char*>(a.alpha.data()), a.alpha.size())) continue;
        atlases_[size] = std::move(a);
    }
    return loaded();
}

const Font::Atlas* Font::pick(int size) const {
    const Atlas* best = nullptr;
    for (const auto& [s, a] : atlases_)
        if (!best || std::abs(s - size) < std::abs(best->size - size)) best = &a;
    return best;
}

int Font::ascent(int size) const {
    const Atlas* a = pick(size);
    return a ? a->ascent * size / a->size : size;
}

int Font::measure(const std::string& text, int size) const {
    const Atlas* a = pick(size);
    if (!a) return 0;
    int w = 0;
    for (unsigned cp : utf8_codepoints(text)) {
        auto it = a->glyphs.find(cp);
        if (it == a->glyphs.end()) it = a->glyphs.find('?');
        if (it != a->glyphs.end()) w += it->second.advance;
    }
    return w * size / (a->size * 64); // advances are in 1/64 pixel
}

int Font::draw(Canvas& c, int x, int y, const std::string& text, int size, Rgb color, int alpha) const {
    const Atlas* a = pick(size);
    if (!a) return 0;
    // Atlases are baked at five sizes; others scale the closest one.
    const int num = size, den = a->size;
    int pen = 0;
    for (unsigned cp : utf8_codepoints(text)) {
        auto it = a->glyphs.find(cp);
        if (it == a->glyphs.end()) it = a->glyphs.find('?');
        if (it == a->glyphs.end()) continue;
        const Glyph& g = it->second;
        // g.top is the glyph's offset from the baseline (negative is up); pen is in 1/64 pixel.
        int gx = x + (pen / 64 + g.left) * num / den, gy = y + g.top * num / den;
        int gw = g.w * num / den, gh = g.h * num / den;
        for (int yy = 0; yy < gh; ++yy)
            for (int xx = 0; xx < gw; ++xx) {
                int tx = gx + xx, ty = gy + yy;
                if (tx < 0 || ty < 0 || tx >= c.w() || ty >= c.h()) continue;
                int al = a->alpha[static_cast<size_t>(g.y + yy * den / num) * a->width + g.x + xx * den / num];
                if (!al) continue;
                c.fill_rect(tx, ty, 1, 1, color, al * alpha / 255);
            }
        pen += g.advance;
    }
    return pen / 64 * num / den;
}

std::string Font::fit(const std::string& text, int size, int max_width) const {
    if (measure(text, size) <= max_width) return text;
    auto cps = utf8_codepoints(text);
    std::string out;
    while (!cps.empty()) {
        cps.pop_back();
        out.clear();
        for (unsigned cp : cps) {
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) out += {char(0xC0 | (cp >> 6)), char(0x80 | (cp & 0x3F))};
            else if (cp < 0x10000)
                out += {char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F))};
            else
                out += {char(0xF0 | (cp >> 18)), char(0x80 | ((cp >> 12) & 0x3F)), char(0x80 | ((cp >> 6) & 0x3F)),
                        char(0x80 | (cp & 0x3F))};
        }
        if (measure(out + "…", size) <= max_width) return out + "…";
    }
    return "";
}

} // namespace play
