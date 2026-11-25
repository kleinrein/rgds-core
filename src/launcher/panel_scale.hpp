#pragma once
// Panels larger than the 640x480 layout (the RG DS Plus's 1024x768). SDL scales the layout by 1.6 there, which is
// sharp for fills and lines but uneven for dot art: a 2-pixel dot becomes 3.2 panel pixels, so neighbouring dots
// come out 3 and 4 pixels wide. DotGrid gives every dot of one glyph or line of pixel text the same whole number
// of panel pixels, from a corner on a whole panel pixel. On the RG DS (scale 1) nothing changes.
#include "platform/sdl_dynamic.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace rgds {

// RGDS_PANEL_NATIVE=0 (developers, for measuring): a larger panel drawn as before, the layout stretched.
inline bool panel_native() {
    static const bool on = [] {
        const char* v = std::getenv("RGDS_PANEL_NATIVE");
        return !v || std::string(v) != "0";
    }();
    return on;
}

// RGDS_PANEL_OFF=rounds,text,canvas,reduced,tiles,dots (developers, for measuring): those parts drawn as before.
inline bool panel_part(const char* part) {
    static const std::string off = [] {
        const char* v = std::getenv("RGDS_PANEL_OFF");
        return "," + std::string(v ? v : "") + ",";
    }();
    return panel_native() && off.find("," + std::string(part) + ",") == std::string::npos;
}

// RGDS_BENCH_OFF=sound,haptics,rowtext,rows (developers, for measuring): those parts left out.
inline bool bench_off(const char* part) {
    static const std::string off = [] {
        const char* v = std::getenv("RGDS_BENCH_OFF");
        return "," + std::string(v ? v : "") + ",";
    }();
    return off.size() > 2 && off.find("," + std::string(part) + ",") != std::string::npos;
}

// Panel pixels per layout pixel: 1 on the RG DS (and in any 640-wide target), 1.6 on the RG DS Plus.
inline float panel_scale(Sdl& s, SDL_Renderer* r) {
    if (!panel_native()) return 1;
    int w = 0, h = 0, lw = 0, lh = 0;
    if (!s.RenderGetLogicalSize || s.GetRendererOutputSize(r, &w, &h) != 0) return 1;
    s.RenderGetLogicalSize(r, &lw, &lh);
    return lw > 0 && w > lw ? float(w) / lw : 1;
}

// Dots of `unit` layout pixels laid out from (x, y) in whole units.
struct DotGrid {
    float k = 1;
    int x = 0, y = 0, unit = 1, cell = 1, px = 0, py = 0;
    DotGrid(Sdl& s, SDL_Renderer* r, int origin_x, int origin_y, int dot)
        : k(panel_part("dots") ? panel_scale(s, r) : 1), x(origin_x), y(origin_y), unit(std::max(1, dot)) {
        cell = std::max(1, int(std::lround(unit * k)));
        px = int(std::lround(x * k));
        py = int(std::lround(y * k));
    }
    // Worth the float path: the panel is larger than the layout and RenderFillRectF is there.
    bool active(Sdl& s) const { return k > 1.2f && s.RenderFillRectF; }
    static int units(int offset, int unit) { return offset >= 0 ? offset / unit : -((-offset + unit - 1) / unit); }
    SDL_FRect map(const SDL_Rect& p) const {
        const int cx = px + units(p.x - x, unit) * cell, cy = py + units(p.y - y, unit) * cell;
        const int cw = std::max(1, p.w / unit) * cell, ch = std::max(1, p.h / unit) * cell;
        return {cx / k, cy / k, cw / k, ch / k};
    }
    void fill(Sdl& s, SDL_Renderer* r, const SDL_Rect& p) const {
        if (!active(s)) {
            s.RenderFillRect(r, &p);
            return;
        }
        const SDL_FRect f = map(p);
        s.RenderFillRectF(r, &f);
    }
};

// A rounded rectangle's rows worked out in panel pixels (the radius times 1.6 on the RG DS Plus), so a corner steps
// one panel pixel at a time, not 1.6; the same rows as the layout-pixel code (corner rows, then the middle, nothing
// drawn twice, so it suits a translucent fill too). False on a panel the size of the layout: the caller draws as
// before.
inline bool fill_round_on_panel(Sdl& s, SDL_Renderer* r, int x, int y, int w, int h, int radius,
                                std::vector<SDL_FRect>& rows) {
    const float k = panel_scale(s, r);
    if (k <= 1.2f || !s.RenderFillRectsF || w <= 0 || h <= 0 || !panel_part("rounds")) return false;
    const int x0 = int(std::lround(x * k)), y0 = int(std::lround(y * k)), x1 = int(std::lround((x + w) * k)),
              y1 = int(std::lround((y + h) * k)), pw = x1 - x0, ph = y1 - y0;
    const int rp = std::clamp(int(std::lround(radius * k)), 0, std::min(pw, ph) / 2);
    rows.clear();
    for (int dy = 0; dy < rp; dy++) {
        const int inset = rp - int(std::sqrt(double(rp) * rp - (rp - dy - .5) * (rp - dy - .5)));
        rows.push_back({(x0 + inset) / k, (y0 + dy) / k, (pw - inset * 2) / k, 1 / k});
        rows.push_back({(x0 + inset) / k, (y1 - 1 - dy) / k, (pw - inset * 2) / k, 1 / k});
    }
    rows.push_back({x0 / k, (y0 + rp) / k, pw / k, (ph - rp * 2) / k});
    s.RenderFillRectsF(r, rows.data(), int(rows.size()));
    return true;
}

}
