// SPDX-License-Identifier: GPL-3.0-or-later
// The Super Nintendo's Mode 7 and sprites as the core's record of a frame has
// them (Core::Mode7), for the race drawn on down the top screen (companion.cpp)
// and the course seen from above (course.cpp): where a scanline looks on the
// course, which lines are the race and which the map under it, and the
// sprites of a table in OAM's layout, grouped into the things they draw.
#pragma once
#include "core/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace play::mode7 {

inline const int16_t* line(const Core::Mode7& m, int l) { return m.lines + size_t(l) * 8; }
inline bool has_matrix(const Core::Mode7& m, int l) {
    const int16_t* k = line(m, l);
    return (k[0] | k[1] | k[2] | k[3]) != 0;
}

// Where a scanline looks on the course with this matrix: the place under its
// first pixel is (aa + bb, cc + dd) / 256 and a pixel steps A and C on, in
// snes9x's arithmetic (its rounding to 1/64, before the course wraps).
struct Ray {
    int aa, bb, cc, dd;
};
inline Ray ray(const int mat[4], const int16_t* k, int starty) {
    auto signed13 = [](int v) { return (v << 19) >> 19; };
    auto clip10 = [](int v) { return (v & 0x2000) ? (v | ~0x3ff) : (v & 0x3ff); };
    const int x0 = signed13(k[4]), y0 = signed13(k[5]), h = signed13(k[6]), v = signed13(k[7]);
    const int xx = clip10(h - x0), yy = clip10(v - y0);
    return {(mat[0] * xx) & ~63, ((mat[1] * starty) & ~63) + ((mat[1] * yy) & ~63) + (x0 << 8), (mat[2] * xx) & ~63,
            ((mat[3] * starty) & ~63) + ((mat[3] * yy) & ~63) + (y0 << 8)};
}
// The place on the course pixel x of line l shows, with the line's own matrix.
inline void course_at(const Core::Mode7& m, int l, double x, double& wx, double& wy) {
    const int16_t* k = line(m, l);
    const int mat[4] = {k[0], k[1], k[2], k[3]};
    const Ray r = ray(mat, k, l + 1);
    wx = (r.aa + r.bb + x * mat[0]) / 256.0, wy = (r.cc + r.dd + x * mat[2]) / 256.0;
}

// The race: the first run of 16 or more lines sharing a centre and scroll whose
// matrix shrinks line by line, as a perspective does. snes9x rewrites a line's
// record only when that line is Mode 7, so lines that are something else now
// (the sky) keep whatever an earlier screen left (a restart's full-screen Mode 7
// picture); such a stale block is flat and never passes.
inline bool race_lines(const Core::Mode7& m, int& s, int& e) {
    auto size = [&](int l) {
        const int16_t* k = line(m, l);
        return std::abs(k[0]) + std::abs(k[1]) + std::abs(k[2]) + std::abs(k[3]);
    };
    for (int l = m.first_line; l < 240;) {
        if (!has_matrix(m, l)) {
            ++l;
            continue;
        }
        int end = l;
        bool shrinks = true;
        while (end + 1 < 240 && has_matrix(m, end + 1) &&
               std::equal(line(m, end + 1) + 4, line(m, end + 1) + 8, line(m, l) + 4)) {
            shrinks = shrinks && size(end + 1) <= size(end);
            ++end;
        }
        if (shrinks && end - l >= 16 && size(l) > size(end) * 3 / 2) {
            s = l, e = end;
            return true;
        }
        l = end + 1;
    }
    return false;
}

// The map under the race: the next run of 16 or more lines that share a centre
// and scroll (a line or two between the pictures keeps an older screen's).
inline bool map_lines(const Core::Mode7& m, int after, int& ms, int& me) {
    for (ms = after + 1; ms < 240; ms = me + 1) {
        while (ms < 240 && !has_matrix(m, ms)) ++ms;
        for (me = ms; me + 1 < 240 && has_matrix(m, me + 1) &&
                      std::equal(line(m, me + 1) + 4, line(m, me + 1) + 8, line(m, ms) + 4);)
            ++me;
        if (ms < 240 && me - ms >= 16) return true;
    }
    return false;
}

// A colour of the core's palette as its picture shows it (BGR555 through its RGB565).
inline uint32_t colour(uint16_t v) {
    const uint32_t r5 = v & 31, g5 = (v >> 5) & 31, b5 = (v >> 10) & 31, g6 = (g5 << 1) | ((g5 >> 4) & 1);
    return rgb_to_px(((r5 * 255 / 31) << 16) | ((g6 * 255 / 63) << 8) | (b5 * 255 / 31));
}
// The course's colour index at a place (1024 x 1024, wrapping): its tile map in
// the even bytes of video memory, the tiles' pixels in the odd ones.
inline uint8_t course_index(const uint8_t* vram, int x, int y) {
    x &= 0x3ff, y &= 0x3ff;
    const uint8_t tile = vram[((y & ~7) << 5) + ((x >> 2) & ~1)];
    return vram[1 + (tile << 7) + ((y & 7) << 4) + ((x & 7) << 1)];
}

// The sprites of a table in OAM's layout (544 bytes), grouped as a game lists
// the pieces of one thing (a kart is four 16x16 sprites): one after another in
// one palette and priority, touching, no larger than 2x2 large sprites.
struct Piece {
    int x, y, w, h;
    const uint8_t* o;
    int tile() const { return o[2] | ((o[3] & 1) << 8); }
};
struct Sprite {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<Piece> pieces;
    bool drawn_from(int tile) const {
        for (const Piece& p : pieces)
            if (p.tile() == tile) return true;
        return false;
    }
};
inline std::vector<Sprite> sprites(const uint8_t* oam, int obj_sizes) {
    static const int sizes[8][4] = {{8, 8, 16, 16},   {8, 8, 32, 32},   {8, 8, 64, 64},   {16, 16, 32, 32},
                                    {16, 16, 64, 64}, {32, 32, 64, 64}, {16, 32, 32, 64}, {16, 32, 32, 32}};
    const int* size = sizes[obj_sizes & 7];
    std::vector<Sprite> out;
    for (int i = 0; i < 128; ++i) {
        const uint8_t* o = oam + 4 * i;
        const int hi = (oam[512 + i / 4] >> (2 * (i & 3))) & 3;
        int x = o[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        const Piece piece{x, o[1], size[hi & 2 ? 2 : 0], size[hi & 2 ? 3 : 1], o};
        if (piece.y >= 224 || piece.x + piece.w <= 0) continue;
        if (!out.empty()) {
            Sprite& t = out.back();
            const int x0 = std::min(t.x, piece.x), y0 = std::min(t.y, piece.y);
            const int x1 = std::max(t.x + t.w, piece.x + piece.w), y1 = std::max(t.y + t.h, piece.y + piece.h);
            const bool touching =
                piece.x <= t.x + t.w && t.x <= piece.x + piece.w && piece.y <= t.y + t.h && t.y <= piece.y + piece.h;
            if ((t.pieces[0].o[3] & 0x3E) == (o[3] & 0x3E) && touching && x1 - x0 <= 2 * size[2] &&
                y1 - y0 <= 2 * size[3]) {
                t.x = x0, t.y = y0, t.w = x1 - x0, t.h = y1 - y0;
                t.pieces.push_back(piece);
                continue;
            }
        }
        Sprite t;
        t.x = piece.x, t.y = piece.y, t.w = piece.w, t.h = piece.h;
        t.pieces.push_back(piece);
        out.push_back(std::move(t));
    }
    return out;
}
// A piece's pixel: its index in the palette (the sprite half), 0 where it is clear.
inline int pixel(const Core::Mode7& m, const Piece& p, int sx, int sy) {
    const uint8_t* o = p.o;
    const int name = p.tile();
    if (o[3] & 0x40) sx = p.w - 1 - sx;
    if (o[3] & 0x80) sy = p.h - 1 - sy;
    const int tile = (((name & 0xf0) + (sy >> 3) * 16) & 0xf0) | ((name + (sx >> 3)) & 0x0f);
    const int at = (((name & 0x100) ? m.obj_second : m.obj_base) + tile * 32 + (sy & 7) * 2) & 0xffff;
    const int bit = 7 - (sx & 7);
    const int c = ((m.vram[at] >> bit) & 1) | (((m.vram[(at + 1) & 0xffff] >> bit) & 1) << 1) |
                  (((m.vram[(at + 16) & 0xffff] >> bit) & 1) << 2) | (((m.vram[(at + 17) & 0xffff] >> bit) & 1) << 3);
    return c ? 128 + ((o[3] >> 1) & 7) * 16 + c : 0;
}
// A thing's index at a pixel of its box: the first piece listed is in front, as the PPU has it.
inline int pixel(const Core::Mode7& m, const Sprite& s, int x, int y) {
    const int sx = s.x + x, sy = s.y + y;
    for (const Piece& p : s.pieces) {
        if (sx < p.x || sx >= p.x + p.w || sy < p.y || sy >= p.y + p.h) continue;
        if (const int c = pixel(m, p, sx - p.x, sy - p.y)) return c;
    }
    return 0;
}

} // namespace play::mode7
