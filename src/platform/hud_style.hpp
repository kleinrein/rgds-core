#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rgds::hud {
// The in-game FPS counter, volume bar and fast-forward badge, drawn in the
// launcher's own look: its rounded font atlases, theme colours and accent.
// Anything missing (no atlas directory, bad theme) reports false so the caller
// keeps the built-in pixel HUD instead of drawing nothing.
struct Rgb {
    int r, g, b;
};
struct Palette {
    Rgb panel{23, 32, 46}, ink{233, 244, 255}, muted{145, 167, 190}, accent{78, 187, 255}, line{45, 67, 90};
};

// RGDS_HUD_THEME=panel,ink,muted,accent,line as RRGGBB, written by the runtime
// from themes/ and the user's mode and accent. Absent or malformed: the dark theme.
inline const Palette& palette() {
    static const Palette value = [] {
        Palette p;
        const char* text = std::getenv("RGDS_HUD_THEME");
        if (!text) return p;
        std::array<Rgb, 5> colours{};
        std::string rest = text;
        for (size_t i = 0; i < colours.size(); i++) {
            auto end = rest.find(',');
            auto token = rest.substr(0, end);
            if (token.size() != 6 || token.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return p;
            unsigned v = static_cast<unsigned>(std::strtoul(token.c_str(), nullptr, 16));
            colours[i] = {int(v >> 16) & 255, int(v >> 8) & 255, int(v) & 255};
            if (end == std::string::npos) {
                if (i != colours.size() - 1) return p;
                break;
            }
            rest = rest.substr(end + 1);
        }
        p.panel = colours[0];
        p.ink = colours[1];
        p.muted = colours[2];
        p.accent = colours[3];
        p.line = colours[4];
        return p;
    }();
    return value;
}

// Reads the launcher's RGF1 atlases (assets/fonts/rounded-N.rgf) from RGDS_FONT_DIR.
class Font {
    struct Glyph {
        int x, y, w, h, left, top, advance;
    };
    struct Atlas {
        int width = 0, height = 0, ascent = 0;
        std::map<unsigned, Glyph> glyphs;
        std::vector<unsigned char> alpha;
    };
    std::map<int, std::unique_ptr<Atlas>> atlases;
    static bool read32(std::ifstream& f, int32_t& out) {
        unsigned char b[4];
        if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
        out = int32_t(b[0] | (unsigned(b[1]) << 8) | (unsigned(b[2]) << 16) | (unsigned(b[3]) << 24));
        return true;
    }
    static std::unique_ptr<Atlas> load(int size) {
        const char* directory = std::getenv("RGDS_FONT_DIR");
        if (!directory || !*directory) return nullptr;
        std::ifstream file(std::string(directory) + "/rounded-" + std::to_string(size) + ".rgf", std::ios::binary);
        char magic[4];
        if (!file.read(magic, 4) || std::string(magic, 4) != "RGF1") return nullptr;
        auto a = std::make_unique<Atlas>();
        int32_t count = 0;
        if (!read32(file, a->width) || !read32(file, a->height) || !read32(file, a->ascent) || !read32(file, count))
            return nullptr;
        if (a->width < 1 || a->width > 1024 || a->height < 1 || a->height > 2048 || a->ascent < 0 || a->ascent > 128 ||
            count < 0 || count > 512)
            return nullptr;
        for (int i = 0; i < count; i++) {
            int32_t cp = 0;
            Glyph g{};
            if (!read32(file, cp) || !read32(file, g.x) || !read32(file, g.y) || !read32(file, g.w) ||
                !read32(file, g.h) || !read32(file, g.left) || !read32(file, g.top) || !read32(file, g.advance))
                return nullptr;
            if (g.x < 0 || g.y < 0 || g.w < 0 || g.h < 0 || g.x + g.w > a->width || g.y + g.h > a->height ||
                g.advance < 0 || g.advance > 8192)
                return nullptr;
            a->glyphs[unsigned(cp)] = g;
        }
        a->alpha.resize(size_t(a->width) * size_t(a->height));
        if (!file.read(reinterpret_cast<char*>(a->alpha.data()), std::streamsize(a->alpha.size()))) return nullptr;
        return a;
    }

public:
    // Baked sizes only: 16, 20, 24, 28 and 36.
    const Atlas* atlas(int size) {
        auto found = atlases.find(size);
        if (found == atlases.end()) found = atlases.emplace(size, load(size)).first;
        return found->second.get();
    }
    float width(int size, const std::string& text) {
        auto* a = atlas(size);
        if (!a) return 0;
        float total = 0;
        for (unsigned char c : text) {
            auto it = a->glyphs.find(c);
            if (it != a->glyphs.end()) total += it->second.advance / 64.0f;
        }
        return total;
    }
};
inline Font& font() {
    static Font value;
    return value;
}

// Premultiplied RGBA, 640 wide, the layout of an overlay panel.
struct Canvas {
    unsigned char* pixels;
    void blend(int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= 640 || y >= 480 || a <= 0) return;
        auto* p = pixels + (size_t(y) * 640 + size_t(x)) * 4;
        int inverse = 255 - a;
        p[0] = static_cast<unsigned char>((c.r * a + p[0] * inverse + 127) / 255);
        p[1] = static_cast<unsigned char>((c.g * a + p[1] * inverse + 127) / 255);
        p[2] = static_cast<unsigned char>((c.b * a + p[2] * inverse + 127) / 255);
        p[3] = static_cast<unsigned char>(a + (p[3] * inverse + 127) / 255);
    }
    // Anti-aliased rounded rectangle, from the signed distance to its edge.
    void rounded(float x, float y, float w, float h, float radius, Rgb c, int alpha = 255) {
        radius = std::min({radius, w / 2, h / 2});
        for (int py = int(std::floor(y)); py < int(std::ceil(y + h)); py++)
            for (int px = int(std::floor(x)); px < int(std::ceil(x + w)); px++) {
                float dx = std::abs(px + .5f - (x + w / 2)) - (w / 2 - radius),
                      dy = std::abs(py + .5f - (y + h / 2)) - (h / 2 - radius);
                float distance =
                    std::hypot(std::max(dx, 0.f), std::max(dy, 0.f)) + std::min(std::max(dx, dy), 0.f) - radius;
                float cover = std::clamp(.5f - distance, 0.f, 1.f);
                if (cover > 0) blend(px, py, c, int(std::lround(alpha * cover)));
            }
    }
    // Card: a hairline border, then the panel colour inside it.
    void card(float x, float y, float w, float h, float radius) {
        auto& p = palette();
        rounded(x, y, w, h, radius, p.line, 245);
        rounded(x + 1, y + 1, w - 2, h - 2, radius - 1, p.panel, 245);
    }
    // Polygon and ring coverage by 4x4 supersampling; shapes are tiny and drawn only when the HUD changes.
    template <class Inside> void shape(int x0, int y0, int x1, int y1, Rgb c, Inside inside) {
        for (int py = y0; py < y1; py++)
            for (int px = x0; px < x1; px++) {
                int hits = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) hits += inside(px + (sx + .5f) / 4, py + (sy + .5f) / 4);
                if (hits) blend(px, py, c, 255 * hits / 16);
            }
    }
    int text(const std::string& value, float x, int y, int size, Rgb c) {
        auto* a = font().atlas(size);
        if (!a) return 0;
        float pen = x;
        for (unsigned char ch : value) {
            auto it = a->glyphs.find(ch);
            if (it == a->glyphs.end()) continue;
            const auto& g = it->second;
            const int left = int(std::lround(pen + g.left)), top = y + a->ascent + g.top;
            for (int gy = 0; gy < g.h; gy++)
                for (int gx = 0; gx < g.w; gx++) {
                    int alpha = a->alpha[size_t(g.y + gy) * size_t(a->width) + size_t(g.x + gx)];
                    if (alpha) blend(left + gx, top + gy, c, alpha);
                }
            pen += g.advance / 64.0f;
        }
        return int(std::lround(pen - x));
    }
};
inline bool even_odd(const std::vector<std::pair<float, float>>& poly, float x, float y) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].second > y) != (poly[j].second > y) &&
            x < (poly[j].first - poly[i].first) * (y - poly[i].second) / (poly[j].second - poly[i].second) +
                    poly[i].first)
            in = !in;
    return in;
}
inline bool ready() { return font().atlas(20) && font().atlas(16); }

// The launcher's speaker glyph (a 24-unit grid), scaled to `size` pixels.
inline void speaker(Canvas& canvas, int x, int y, int size, int level) {
    const float k = size / 24.0f;
    auto& p = palette();
    static const std::vector<std::pair<float, float>> body = {{3, 9}, {7, 9}, {12, 5}, {12, 19}, {7, 15}, {3, 15}};
    auto scaled = body;
    for (auto& v : scaled) {
        v.first = x + v.first * k;
        v.second = y + v.second * k;
    }
    canvas.shape(x, y, x + size, y + size, p.accent, [&](float px, float py) { return even_odd(scaled, px, py); });
    if (level > 0) {
        const float weight = std::max(1.4f, 1.7f * k);
        const int waves = level >= 50 ? 2 : 1;
        for (int w = 0; w < waves; w++) {
            const float radius = (w ? 10.f : 6.f) * k;
            canvas.shape(x, y, x + size, y + size, p.accent, [&](float px, float py) {
                float dx = px - (x + 12 * k), dy = py - (y + 12 * k);
                float r = std::hypot(dx, dy);
                return dx > 0 && std::abs(r - radius) <= weight / 2 && std::abs(std::atan2(dy, dx)) <= 0.87f;
            });
        }
    } else {
        const Rgb red{226, 96, 108};
        const float weight = std::max(1.6f, 1.9f * k);
        auto line = [&](float ax, float ay, float bx, float by) {
            canvas.shape(x, y, x + size, y + size, red, [&](float px, float py) {
                float vx = bx - ax, vy = by - ay,
                      t = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.f, 1.f);
                return std::hypot(px - (ax + vx * t), py - (ay + vy * t)) <= weight / 2;
            });
        };
        line(x + 15 * k, y + 8 * k, x + 22 * k, y + 16 * k);
        line(x + 22 * k, y + 8 * k, x + 15 * k, y + 16 * k);
    }
}

// Same footprint as the original bar: x 108..532, y 104..186.
inline bool volume(unsigned char* pixels, int level) {
    if (!ready()) return false;
    Canvas canvas{pixels};
    auto& p = palette();
    level = std::clamp(level, 0, 100);
    const int x = 108, y = 104, w = 424, h = 82;
    canvas.card(float(x), float(y), float(w), float(h), 20);
    speaker(canvas, x + 24, y + 14, 28, level);
    canvas.text(level ? "Volume" : "Muted", float(x + 64), y + 14, 24, p.ink);
    const std::string percent = std::to_string(level) + "%";
    canvas.text(percent, x + w - 24 - font().width(24, percent), y + 14, 24, level ? p.accent : p.muted);
    const float bx = float(x + 24), by = float(y + 57), bw = float(w - 48), bh = 10;
    canvas.rounded(bx, by, bw, bh, bh / 2, p.line, 255);
    if (level > 0) canvas.rounded(bx, by, std::max(bh, bw * level / 100), bh, bh / 2, p.accent, 255);
    return true;
}

inline bool fps(unsigned char* pixels, double rate, bool) {
    if (!ready()) return false;
    Canvas canvas{pixels};
    auto& p = palette();
    char number[16];
    std::snprintf(number, sizeof number, "%.1f", std::clamp(rate, 0.0, 999.9));
    const float number_width = font().width(28, number), unit = font().width(16, "FPS");
    canvas.card(24, 24, 18 + number_width + 8 + unit + 18, 44, 22);
    float pen = 24 + 18;
    canvas.text(number, pen, 24 + 4, 28, p.ink);
    pen += number_width + 8;
    canvas.text("FPS", pen, 24 + 14, 16, p.muted);
    return true;
}

inline bool fast_forward(unsigned char* pixels) {
    if (!ready()) return false;
    Canvas canvas{pixels};
    auto& p = palette();
    const float w = 34 + font().width(20, "Fast") + 30;
    canvas.card(24, 420, w, 38, 19);
    for (int n = 0; n < 2; n++) {
        const float x = 40 + n * 13;
        const std::vector<std::pair<float, float>> triangle = {{x, 439 - 8.f}, {x + 11, 439.f}, {x, 439 + 8.f}};
        canvas.shape(int(x) - 1, 419 + 8, int(x) + 13, 459 - 8, p.accent,
                     [&](float px, float py) { return even_odd(triangle, px, py); });
    }
    canvas.text("Fast", 72, 420 + 7, 20, p.ink);
    return true;
}
}
