// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/app.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>

namespace media {

void resample(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const double fy0 = double(y) * sh / dh, fy1 = double(y + 1) * sh / dh;
        for (int x = 0; x < dw; ++x) {
            const double fx0 = double(x) * sw / dw, fx1 = double(x + 1) * sw / dw;
            double r = 0, g = 0, b = 0, a = 0, total = 0;
            if (fx1 - fx0 <= 1 && fy1 - fy0 <= 1) { // enlarging: bilinear
                double sx = (fx0 + fx1) / 2 - .5, sy = (fy0 + fy1) / 2 - .5;
                int ix = std::clamp(int(std::floor(sx)), 0, sw - 1), iy = std::clamp(int(std::floor(sy)), 0, sh - 1);
                int jx = std::min(ix + 1, sw - 1), jy = std::min(iy + 1, sh - 1);
                double tx = std::clamp(sx - ix, 0.0, 1.0), ty = std::clamp(sy - iy, 0.0, 1.0);
                const uint32_t q[4] = {src[size_t(iy) * sw + ix], src[size_t(iy) * sw + jx], src[size_t(jy) * sw + ix],
                                       src[size_t(jy) * sw + jx]};
                const double wgt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int k = 0; k < 4; ++k) {
                    r += (q[k] & 255) * wgt[k], g += ((q[k] >> 8) & 255) * wgt[k], b += ((q[k] >> 16) & 255) * wgt[k];
                    a += (q[k] >> 24) * wgt[k];
                }
                total = 1;
            } else {
                for (int sy = int(fy0); sy < int(std::ceil(fy1)) && sy < sh; ++sy) {
                    double wy = std::min(fy1, double(sy + 1)) - std::max(fy0, double(sy));
                    for (int sx = int(fx0); sx < int(std::ceil(fx1)) && sx < sw; ++sx) {
                        double wx = std::min(fx1, double(sx + 1)) - std::max(fx0, double(sx));
                        double wgt = wx * wy;
                        uint32_t p = src[size_t(sy) * sw + sx];
                        r += (p & 255) * wgt, g += ((p >> 8) & 255) * wgt, b += ((p >> 16) & 255) * wgt;
                        a += (p >> 24) * wgt;
                        total += wgt;
                    }
                }
            }
            if (total <= 0) total = 1;
            dst[size_t(y) * dw + x] = uint32_t(r / total + .5) | (uint32_t(g / total + .5) << 8) |
                                      (uint32_t(b / total + .5) << 16) | (uint32_t(a / total + .5) << 24);
        }
    }
}

void blit_rounded(Canvas& c, const uint32_t* px, int w, int h, int x, int y, int r) {
    for (int row = 0; row < h; ++row) {
        int cy = y + row;
        if (cy < 0 || cy >= c.h()) continue;
        for (int col = 0; col < w; ++col) {
            int cx = x + col;
            if (cx < 0 || cx >= c.w()) continue;
            // Coverage in the corners: the distance from the corner circle's centre.
            double cover = 1;
            double dx = col < r ? r - col - .5 : col >= w - r ? col - (w - r) + .5 : 0;
            double dy = row < r ? r - row - .5 : row >= h - r ? row - (h - r) + .5 : 0;
            if (dx > 0 && dy > 0) cover = std::clamp(r - std::hypot(dx, dy) + .5, 0.0, 1.0);
            if (cover <= 0) continue;
            uint32_t p = px[size_t(row) * w + col];
            c.fill_rect(cx, cy, 1, 1, Rgb::from_px(p), int(255 * cover));
        }
    }
}

std::vector<std::string> wrap(const play::Font& font, const std::string& text, int size, int width, int max_lines) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string trial = line.empty() ? word : line + " " + word;
            if (font.measure(trial, size) <= width || line.empty()) line = trial;
            else {
                lines.push_back(line);
                line = word;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (int(lines.size()) > max_lines) {
        lines.resize(size_t(max_lines));
        lines.back() = font.fit(lines.back() + "…………", size, width);
    }
    for (auto& l : lines) l = font.fit(l, size, width);
    return lines;
}

// ---- Sheet

void Sheet::show(const std::string& t, std::vector<Row> r, int focus_row, std::function<void(int)> on_pick,
                 bool animate) {
    title = t;
    rows = std::move(r);
    focus = std::clamp(focus_row, 0, std::max(0, int(rows.size()) - 1));
    scroll = std::max(0, focus - rows_shown() / 2);
    scroll = std::min(scroll, std::max(0, int(rows.size()) - rows_shown()));
    chosen = std::move(on_pick);
    open = true;
    rise.target = 1;
    if (!animate) rise.snap(1);
    stale = true;
    pressed = -1;
}

void Sheet::hide() {
    open = false;
    rise.target = 0;
}

int Sheet::rows_shown() const { return std::max(1, (h * 7 / 10 - 64) / 52); }

Box Sheet::panel_box() const {
    const int shown = std::min(rows_shown(), std::max(1, int(rows.size())));
    const int ph = 64 + shown * 52 + 16;
    return {12, h - ph, w - 24, ph + 24}; // runs off the bottom: only its top corners are rounded on screen
}

Box Sheet::row_box(int i) const {
    Box p = panel_box();
    return {p.x + 12, p.y + 60 + (i - scroll) * 52, p.w - 24, 48};
}

void Sheet::key(Key k) {
    if (!open) return;
    const int n = int(rows.size());
    if (k == KUp && n) focus = (focus + n - 1) % n;
    else if (k == KDown && n) focus = (focus + 1) % n;
    else if (k == KLeft && n) focus = std::max(0, focus - rows_shown());
    else if (k == KRight && n) focus = std::min(n - 1, focus + rows_shown());
    else if (k == KA && n) {
        auto pick = chosen;
        int f = focus;
        hide();
        if (pick) pick(f);
    } else if (k == KB || k == KX || k == KY) hide();
    if (focus < scroll) scroll = focus;
    if (focus >= scroll + rows_shown()) scroll = focus - rows_shown() + 1;
    stale = true;
}

void Sheet::touch(const TouchEvent& t) {
    if (!open) return;
    Box p = panel_box();
    static double down_y = 0;
    static int down_scroll = 0;
    if (t.kind == TouchEvent::Down) {
        down_y = t.y;
        down_scroll = scroll;
        pressed = -1;
        if (!p.contains(int(t.x), int(t.y))) {
            hide(); // a tap above the sheet closes it
            return;
        }
        for (int i = scroll; i < std::min(int(rows.size()), scroll + rows_shown()); ++i)
            if (row_box(i).contains(int(t.x), int(t.y))) pressed = i;
        stale = true;
    } else if (t.kind == TouchEvent::Move) {
        int moved = int((down_y - t.y) / 52);
        if (std::abs(down_y - t.y) > 12) pressed = -1;
        int max_scroll = std::max(0, int(rows.size()) - rows_shown());
        int s = std::clamp(down_scroll + moved, 0, max_scroll);
        if (s != scroll) scroll = s, stale = true;
    } else {
        if (pressed >= 0) {
            focus = pressed;
            int f = pressed;
            pressed = -1;
            auto pick = chosen;
            hide();
            if (pick) pick(f);
        }
        stale = true;
    }
}

bool Sheet::update(double dt_ms) { return rise.step(dt_ms, .0040, .80); }

void Sheet::draw(SDL_Renderer* r, Context& c) {
    if (stale || !tex || tex.w != w || tex.h != h) {
        Canvas cv(w, h);
        // Transparent ground: only the panel is drawn.
        std::fill(cv.data(), cv.data() + size_t(w) * h, 0x00000000u);
        Box p = panel_box();
        play::soft_round(cv, p.x, p.y - 4, p.w, p.h, 22, c.look.shadow(), c.look.dark ? 120 : 60);
        play::glossy(cv, p.x, p.y, p.w, p.h, 22, c.look.panel, 0, true);
        // The grab handle and the title.
        cv.rounded_rect(w / 2 - 22, p.y + 8, 44, 5, 2, play::mix(c.look.panel, c.look.muted, 120));
        c.font.draw(cv, p.x + 22, c.font.centred(play::kit::title, p.y + 36), title, play::kit::title, c.look.ink);
        for (int i = scroll; i < std::min(int(rows.size()), scroll + rows_shown()); ++i) {
            Box b = row_box(i);
            const Row& row = rows[size_t(i)];
            const bool f = i == focus, pr = i == pressed;
            if (f || pr) {
                cv.rounded_rect(b.x, b.y, b.w, b.h, 12, pr ? play::mix(c.look.focus, c.look.accent, 90) : c.look.focus);
            }
            int tx = b.x + 16 + row.indent * 18;
            if (row.chosen) {
                play::icon(cv, c.look, play::Icon::Check, b.x + b.w - 26, b.y + b.h / 2, 20, c.look.accent);
            }
            const int vw = row.value.empty() ? 0 : c.font.measure(row.value, play::kit::small) + 12;
            c.font.draw(cv, tx, c.font.centred(play::kit::body, b.y + b.h / 2),
                        c.font.fit(row.label, play::kit::body, b.x + b.w - tx - 48 - vw), play::kit::body,
                        f ? c.look.ink : play::mix(c.look.ink, c.look.muted, 60));
            if (!row.value.empty())
                c.font.draw(cv, b.x + b.w - 44 - vw + 12, c.font.centred(play::kit::small, b.y + b.h / 2), row.value,
                            play::kit::small, c.look.muted);
        }
        // More below / above: a fade at the edge.
        if (scroll + rows_shown() < int(rows.size()))
            for (int i = 0; i < 18; ++i) cv.fill_rect(p.x + 2, h - 18 + i, p.w - 4, 1, c.look.panel, i * 12);
        upload(r, tex, cv, true, false);
        stale = false;
    }
    (void)c;
}

} // namespace media
