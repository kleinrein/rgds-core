// SPDX-License-Identifier: GPL-3.0-or-later
// Display-only 2D scaling for the RG DS 640x480 panel. Guest pixels are untouched.
//
// The enhanced (2x-5x) compositor draws 2D art on the native 256x192 grid, so
// at 2.5x each DS pixel is 2 or 3 panel pixels wide. These filters resample the
// native image to the panel instead:
//
// Sharp is area sampling: a panel pixel inside one DS pixel is that pixel's
// exact colour, and only the panel pixel a DS edge cuts is blended, by
// coverage (at 2.5x one pixel in five, 50/50, on each axis). The blend runs in
// linear light, so a seam keeps the brightness the eye expects and edges read
// crisp instead of dark and soft. Smooth is plain bilinear.
//
// Crisp (RG DS) is for pixel art: each DS pixel becomes four cells, a cell
// taking a neighbour's colour where the two neighbours on its side agree
// (Scale2x), which turns stairs on diagonals and curves into slopes; the
// 512x384 result is then area-sampled to the panel at 1.25x like Sharp.
// Interiors stay exact. It reads one more line above than the others.
//
// Speed: a compose job scales each source line horizontally once (rows())
// and builds its two or three panel rows from those, instead of scaling two
// source lines per panel row; Sharp copies its pure pixels and blends seams
// only.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace rgds {

enum class Filter2D : int { Native = 0, Sharp = 1, Smooth = 2, Crisp = 3 };
constexpr int kFilter2DModes = 4;

class Filter2DPlan {
public:
    static constexpr unsigned src_w = 256, src_h = 192, dst_w = 640, dst_h = 480;

    // Per panel coordinate: the two source indices (clamped to the image)
    // and the weight, 0..256, of the second.
    struct Axis { std::array<int16_t, 640> first{}, second{}; std::array<uint16_t, 640> weight{}; };
    // Smooth's horizontal weights, one per byte lane (w and 256 - w), for the vector blend.
    alignas(16) std::array<uint16_t, 640 * 4> lane_w{}, lane_iw{};

    static const Filter2DPlan& get(Filter2D mode) {
        static const Filter2DPlan plans[3] = {Filter2DPlan(Filter2D::Sharp), Filter2DPlan(Filter2D::Smooth), Filter2DPlan(Filter2D::Crisp)};
        return plans[mode == Filter2D::Smooth ? 1 : mode == Filter2D::Crisp ? 2 : 0];
    }

    Axis x, y;
    bool linear = false;   // seams blended in linear light (Sharp, Crisp)
    bool crisp = false;    // Scale2x cells, area-sampled (Crisp)
    bool fives = false;    // Sharp at exactly 2.5x: every 5 panel pixels are a, a, seam, b, b

    // Panel rows y0..y1-1 of native line `line` into out (row stride `stride`
    // pixels). above/below are the neighbouring native lines (the line itself
    // at the image edges); each holds 256 opaque 0xAARRGGBB pixels. above2 is
    // the line above `above` (Crisp reads it; `above` again at the edge).
    void rows(const uint32_t* above2, const uint32_t* above, const uint32_t* cur, const uint32_t* below,
              unsigned line, unsigned y0, unsigned y1, uint32_t* out, size_t stride) const {
        if (crisp) { const uint32_t* l[4] = {above2, above, cur, below}; crisp_rows(l, line, y0, y1, out, stride); return; }
        const uint32_t* src[3] = {above, cur, below};
        alignas(16) uint32_t scaled[3][dst_w];
        bool done[3] = {false, false, false};
        auto source = [&](int iy) -> const uint32_t* {
            const int k = std::clamp(iy - int(line), -1, 1) + 1;
            if (!done[k]) { horizontal(src[k], scaled[k]); done[k] = true; }
            return scaled[k];
        };
        for (unsigned py = y0; py < y1; ++py) {
            uint32_t* dst = out + (py - y0) * stride;
            const unsigned wy = y.weight[py];
            const uint32_t* ra = wy == 256 ? nullptr : source(y.first[py]);
            const uint32_t* rb = wy == 0 ? nullptr : source(y.second[py]);
            if (!rb || ra == rb) std::memcpy(dst, ra, sizeof(uint32_t) * dst_w);
            else if (!ra) std::memcpy(dst, rb, sizeof(uint32_t) * dst_w);
            else blend_rows(ra, rb, wy, dst);
        }
    }

    void rows(const uint32_t* above, const uint32_t* cur, const uint32_t* below,
              unsigned line, unsigned y0, unsigned y1, uint32_t* out, size_t stride) const {
        rows(above, above, cur, below, line, y0, y1, out, stride);
    }

    // One panel row (tests and callers that need a single row).
    void row(const uint32_t* above2, const uint32_t* above, const uint32_t* cur, const uint32_t* below,
             unsigned line, unsigned py, uint32_t* out) const {
        rows(above2, above, cur, below, line, py, py + 1, out, dst_w);
    }
    void row(const uint32_t* above, const uint32_t* cur, const uint32_t* below,
             unsigned line, unsigned py, uint32_t* out) const {
        rows(above, above, cur, below, line, py, py + 1, out, dst_w);
    }

    // Blend two opaque pixels, weight 0..256 toward b, alpha forced opaque.
    static uint32_t mix(uint32_t a, uint32_t b, unsigned w) {
        const uint32_t rb = (((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
        const uint32_t g = (((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
        return 0xFF000000u | rb | g;
    }

private:
    // sRGB-ish (gamma 2.2) channel <-> linear light, 12-bit linear.
    struct Light {
        std::array<uint16_t, 256> decode{};
        std::array<uint8_t, 4096> encode{};
        Light() {
            for (unsigned i = 0; i < 256; ++i) decode[i] = uint16_t(std::lround(std::pow(i / 255.0, 2.2) * 4095.0));
            for (unsigned i = 0; i < 4096; ++i) encode[i] = uint8_t(std::lround(std::pow(i / 4095.0, 1 / 2.2) * 255.0));
        }
    };
    static const Light& light() { static const Light l; return l; }

    // Linear-light 50/50 blend (a Sharp seam at 2.5x).
    static uint32_t half_linear(uint32_t a, uint32_t b) {
        if (a == b) return a;
        const Light& l = light();
        uint32_t out = 0xFF000000u;
        for (unsigned shift : {0u, 8u, 16u})
            out |= uint32_t(l.encode[(l.decode[(a >> shift) & 255] + l.decode[(b >> shift) & 255]) >> 1]) << shift;
        return out;
    }

    // Linear-light blend, weight 0..256 toward b.
    static uint32_t mix_linear(uint32_t a, uint32_t b, unsigned w) {
        const Light& l = light();
        uint32_t out = 0xFF000000u;
        for (unsigned shift : {0u, 8u, 16u}) {
            const unsigned ca = l.decode[(a >> shift) & 255], cb = l.decode[(b >> shift) & 255];
            out |= uint32_t(l.encode[(ca * (256 - w) + cb * w) >> 8]) << shift;
        }
        return out;
    }

    // Crisp. The expanded image has two cell rows per native line (e = 2 * line
    // + half) and two cell columns per pixel; a panel pixel covers 0.8 of a
    // cell, so of every five it is a cell, a 1:3 blend, a 1:1 blend, a 3:1
    // blend and a cell, on both axes. l[0..3] are lines line-2 .. line+1.
    //
    // Cost: Scale2x changes cells only where two neighbours agree against the
    // pixel, which is rare outside edges, so a line is first scanned; a line
    // it leaves alone has both cell rows equal to Sharp's expansion of the
    // native line (computed once, shared by both halves, and its in-line
    // vertical blends become copies). The work then matches Sharp except on
    // the lines with edges.
    void crisp_rows(const uint32_t* const l[4], unsigned line, unsigned y0, unsigned y1, uint32_t* out, size_t stride) const {
        alignas(16) uint32_t scaled[4][dst_w];
        const uint32_t* row_of[4] = {nullptr, nullptr, nullptr, nullptr};   // by e - 2 * (line - 1)
        int plain[2] = {-1, -1};   // per native line (line-1, line): 1 when Scale2x changes nothing
        auto expanded = [&](unsigned e) -> const uint32_t* {
            const unsigned slot = e - 2 * (line - 1);   // 0..3
            if (row_of[slot]) return row_of[slot];
            const unsigned k = slot >> 1;                // 0: line-1, 1: line
            const uint32_t* above = l[k]; const uint32_t* cur = l[k + 1]; const uint32_t* below = l[k + 2];
            if (plain[k] < 0) plain[k] = scale2x_untouched(above, cur, below) ? 1 : 0;
            if (plain[k]) {
                const unsigned other = slot ^ 1;
                if (!row_of[other]) { expand_pairs(cur, scaled[slot]); row_of[other] = scaled[slot]; }
                row_of[slot] = row_of[other];
            } else {
                half_row(above, cur, below, slot & 1, scaled[slot]);
                row_of[slot] = scaled[slot];
            }
            return row_of[slot];
        };
        for (unsigned py = y0; py < y1; ++py) {
            uint32_t* dst = out + (py - y0) * stride;
            const unsigned j = py % 5, e = (py / 5) * 4;
            if (j == 0) std::memcpy(dst, expanded(e), sizeof(uint32_t) * dst_w);
            else if (j == 4) std::memcpy(dst, expanded(e + 3), sizeof(uint32_t) * dst_w);
            else {
                const uint32_t* a = expanded(e + j - 1); const uint32_t* b = expanded(e + j);
                if (a == b) std::memcpy(dst, a, sizeof(uint32_t) * dst_w);
                else blend_rows(a, b, j == 1 ? 192 : j == 2 ? 128 : 64, dst);
            }
        }
    }

    // True when Scale2x would leave every cell of `cur` as the pixel itself:
    // no pixel has above != below together with left != right and an
    // agreeing neighbour pair.
    static bool scale2x_untouched(const uint32_t* above, const uint32_t* cur, const uint32_t* below) {
        for (unsigned i = 0; i < src_w; ++i) {
            const uint32_t p = cur[i], b = above[i], h = below[i];
            if (b == h) continue;
            const uint32_t d = i ? cur[i - 1] : p, f = i + 1 < src_w ? cur[i + 1] : p;
            if (d == f) continue;
            if (d == b || b == f || d == h || h == f) return false;
        }
        return true;
    }

    // Sharp's expansion of a native line at 2.5x: a, a, seam, b, b per pixel pair.
    static void expand_pairs(const uint32_t* row, uint32_t* out) {
        for (unsigned k = 0; k < src_w / 2; ++k, out += 5) {
            const uint32_t a = row[2 * k], b = row[2 * k + 1];
            out[0] = a; out[1] = a; out[2] = half_linear(a, b); out[3] = b; out[4] = b;
        }
    }

    // One cell row (the top or bottom half of native line `cur`) scaled to
    // the panel width: Scale2x picks each half's two cells from the pixel's
    // above/below and left/right neighbours, then five panel pixels cover
    // every four cells.
    static void half_row(const uint32_t* above, const uint32_t* cur, const uint32_t* below, unsigned half, uint32_t* out) {
        uint32_t q[2 * src_w];
        for (unsigned i = 0; i < src_w; ++i) {
            const uint32_t p = cur[i], b = above[i], h = below[i];
            const uint32_t d = i ? cur[i - 1] : p, f = i + 1 < src_w ? cur[i + 1] : p;
            uint32_t ql = p, qr = p;
            if (b != h && d != f) {
                if (half == 0) { if (d == b) ql = d; if (b == f) qr = f; }
                else           { if (d == h) ql = d; if (h == f) qr = f; }
            }
            q[2 * i] = ql; q[2 * i + 1] = qr;
        }
        for (unsigned k = 0; k < src_w / 2; ++k, out += 5) {
            const uint32_t a = q[4 * k], b = q[4 * k + 1], c = q[4 * k + 2], d = q[4 * k + 3];
            out[0] = a;
            out[1] = a == b ? a : mix_linear(a, b, 192);
            out[2] = half_linear(b, c);
            out[3] = c == d ? c : mix_linear(c, d, 64);
            out[4] = d;
        }
    }

    explicit Filter2DPlan(Filter2D mode) : linear(mode != Filter2D::Smooth), crisp(mode == Filter2D::Crisp) {
        build(x, src_w, dst_w, mode);
        build(y, src_h, dst_h, mode);
        for (unsigned p = 0; p < dst_w; ++p) for (unsigned c = 0; c < 4; ++c) { lane_w[4 * p + c] = x.weight[p]; lane_iw[4 * p + c] = uint16_t(256 - x.weight[p]); }
        fives = linear && dst_w * 2 == src_w * 5;
        for (unsigned p = 0; fives && p < dst_w; ++p) {
            const unsigned k = p / 5, j = p % 5;
            fives = x.first[p] == int(2 * k + (j >= 3)) && x.weight[p] == (j == 2 ? 128u : 0u) && (j != 2 || x.second[p] == int(2 * k + 1));
        }
    }

    static void build(Axis& axis, unsigned src, unsigned dst, Filter2D mode) {
        for (unsigned p = 0; p < dst; ++p) {
            int i, w;
            if (mode == Filter2D::Sharp) {
                // Panel pixel p covers [p, p+1) * src / dst of the source.
                // Inside one source pixel it is that pixel; across an edge it
                // takes the part on the right, by coverage (exact rationals).
                const unsigned lo = p * src, hi = (p + 1) * src;   // in units of 1/dst source pixels
                i = int(lo / dst);
                const unsigned edge = unsigned(i + 1) * dst;
                w = hi <= edge ? 0 : int(((hi - edge) * 256 + src / 2) / src);
            } else {
                // Bilinear: u = (p + 0.5) * src / dst - 0.5, kept as num / (2 * dst).
                const int den = int(2 * dst);
                const int num = int((2 * p + 1) * src) - int(dst);
                i = num / den;
                if (num % den < 0) --i;
                w = (num - i * den) * 256 / den;
            }
            axis.first[p] = int16_t(std::clamp(i, 0, int(src) - 1));
            axis.second[p] = int16_t(std::clamp(i + 1, 0, int(src) - 1));
            axis.weight[p] = uint16_t(axis.first[p] == axis.second[p] ? 0 : w);
        }
    }

    void horizontal(const uint32_t* row, uint32_t* out) const {
        if (fives) { expand_pairs(row, out); return; }
        if (linear) {
            for (unsigned p = 0; p < dst_w; ++p) {
                const unsigned w = x.weight[p];
                out[p] = w == 0 ? row[x.first[p]] : w == 256 ? row[x.second[p]] : mix_linear(row[x.first[p]], row[x.second[p]], w);
            }
            return;
        }
#if defined(__ARM_NEON)
        alignas(16) uint32_t a[dst_w], b[dst_w];
        for (unsigned p = 0; p < dst_w; ++p) { a[p] = row[x.first[p]]; b[p] = row[x.second[p]]; }
        for (unsigned p = 0; p < dst_w; p += 2)
            vst1_u32(out + p, mix2(vld1_u32(a + p), vld1_u32(b + p), vld1q_u16(&lane_iw[4 * p]), vld1q_u16(&lane_w[4 * p])));
#else
        for (unsigned p = 0; p < dst_w; ++p) out[p] = mix(row[x.first[p]], row[x.second[p]], x.weight[p]);
#endif
    }

#if defined(__ARM_NEON)
    // Two pixels: per byte (a * (256 - w) + b * w) >> 8, the same rounding as mix(); alpha forced opaque.
    static uint32x2_t mix2(uint32x2_t a, uint32x2_t b, uint16x8_t iw, uint16x8_t w) {
        const uint16x8_t sum = vmlaq_u16(vmulq_u16(vmovl_u8(vreinterpret_u8_u32(a)), iw), vmovl_u8(vreinterpret_u8_u32(b)), w);
        return vorr_u32(vreinterpret_u32_u8(vshrn_n_u16(sum, 8)), vdup_n_u32(0xFF000000u));
    }
#endif

    void blend_rows(const uint32_t* a, const uint32_t* b, unsigned w, uint32_t* out) const {
        if (linear) {
            if (w == 128) { for (unsigned p = 0; p < dst_w; ++p) out[p] = half_linear(a[p], b[p]); return; }
            for (unsigned p = 0; p < dst_w; ++p) out[p] = a[p] == b[p] ? a[p] : mix_linear(a[p], b[p], w);
            return;
        }
#if defined(__ARM_NEON)
        const uint16x8_t vw = vdupq_n_u16(uint16_t(w)), viw = vdupq_n_u16(uint16_t(256 - w));
        for (unsigned p = 0; p < dst_w; p += 2) vst1_u32(out + p, mix2(vld1_u32(a + p), vld1_u32(b + p), viw, vw));
#else
        for (unsigned p = 0; p < dst_w; ++p) out[p] = mix(a[p], b[p], w);
#endif
    }
};

}  // namespace rgds
