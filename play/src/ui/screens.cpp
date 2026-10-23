// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/screens.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace play {

void draw_game(Canvas& panel, const Picture& game, Fit fit) {
    panel.fill({0, 0, 0});
    if (!game.px || game.w <= 0 || game.h <= 0) return;
    if (fit == Fit::Integer) {
        int scale = std::max(1, std::min(panel.w() / game.w, panel.h() / game.h));
        std::vector<uint32_t> packed(static_cast<size_t>(game.w) * game.h);
        for (int y = 0; y < game.h; ++y)
            std::copy_n(game.px + static_cast<size_t>(y) * game.stride, game.w,
                        packed.begin() + static_cast<size_t>(y) * game.w);
        panel.blit(packed.data(), game.w, game.h, (panel.w() - game.w * scale) / 2, (panel.h() - game.h * scale) / 2,
                   scale);
        return;
    }
    // Fill: keep the aspect, map each panel pixel to its source pixel.
    double s = std::min(double(panel.w()) / game.w, double(panel.h()) / game.h);
    int w = int(game.w * s), h = int(game.h * s), x0 = (panel.w() - w) / 2, y0 = (panel.h() - h) / 2;
    uint32_t* out = panel.data();
    for (int y = 0; y < h; ++y) {
        int sy = std::min(game.h - 1, int(y / s));
        for (int x = 0; x < w; ++x)
            out[static_cast<size_t>(y0 + y) * panel.w() + x0 + x] =
                game.at(std::min(game.w - 1, int(x / s)), sy) | 0xFF000000u;
    }
}

bool write_bmp(const std::string& path, const Canvas& top, const Canvas* bottom) {
    int w = top.w(), h = top.h() + (bottom ? bottom->h() : 0);
    if (bottom && bottom->w() != w) return false;
    int row = (w * 3 + 3) & ~3;
    std::vector<uint8_t> file(54 + static_cast<size_t>(row) * h, 0);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) file[at + i] = uint8_t(v >> (8 * i));
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, uint32_t(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    file[26] = 1;
    file[28] = 24;
    put32(34, uint32_t(row * h));
    for (int y = 0; y < h; ++y) {
        const Canvas& c = y < top.h() ? top : *bottom;
        int cy = y < top.h() ? y : y - top.h();
        uint8_t* out = &file[54 + static_cast<size_t>(h - 1 - y) * row];
        for (int x = 0; x < w; ++x) {
            uint32_t v = c.at(x, cy);
            out[x * 3] = uint8_t(v >> 16);
            out[x * 3 + 1] = uint8_t(v >> 8);
            out[x * 3 + 2] = uint8_t(v);
        }
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    return std::fclose(f) == 0 && ok;
}

uint64_t picture_hash(const Picture& p) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) h = (h ^ (p.at(x, y) & 0xFFFFFF)) * 1099511628211ull;
    return h;
}

} // namespace play
