// SPDX-License-Identifier: GPL-3.0-or-later
// HD 2D rules on the CPU: what the RG DS bridge's GPU rules pass computes (src/platform/hd2d_shader.hpp), per
// native pixel, so the GPU only has to draw. Display only; guest pixels are untouched.
//
// Per pixel, one word, bytes R G B A as the GPU pass writes them (an SDL ABGR8888 texture):
//   R = state0 * 8 + state1, G = state2 * 8 + state3: the four corners (bottom-right, top-right, top-left,
//       bottom-left), 0 none, 1 an edge at 45 degrees, 2 plus the shallow 2:1 slope, 3 plus the steep one, 4 both;
//   B = bit k: corner k turns into its first-axis neighbour (else its second), + 16 where 2D owns the pixel;
//   A = the neighbours the pixel smooths toward: right 1, down 2, left 4, up 8, down-right 16, down-left 32,
//       up-right 64, up-left 128.
// The corner tests are Hyllian's xBR level 2 (MIT) on the 5x5 neighbourhood. HD keeps them for structural edges
// only: a corner whose pixel differs from both neighbours it faces, each by 48 or more; a pixel smooths toward 2D
// neighbours that differ from it by less than 16. HD smooth tests every corner whose faced neighbours differ by 12 or
// more and smooths below 64, which turns shading and texture steps into slopes too (below 12 the corner would blend
// colours the smoothing already blends: the same picture, at twice the corner tests on busy art). Distances are luma plus a quarter of both chroma differences.
//
// Cost: every distance between neighbours is computed once per band of lines, sixteen pixel pairs at a time with
// NEON (four maps: across, down and both diagonals), and the classification reads them eight pixels at a time; only
// the corner candidates run the scalar corner tests.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace rgds {

class Hd2DRules {
public:
  static constexpr unsigned src_w = 256, src_h = 192;
  static constexpr int same = 48; // xBR's own "equal" distance, for its level 1 test
  enum State : unsigned { None = 0, Edge45 = 1, Shallow = 2, Steep = 3, Both = 4 };

  // HD, or HD smooth. The GPU rules pass takes the same two numbers as u_hd_edge and u_hd_smooth.
  static const Hd2DRules& get(bool smooth = false) {
    static const Hd2DRules crisp(48, 16), soft(12, 64);
    return smooth ? soft : crisp;
  }
  // `floor`: a corner's two faced neighbours must both differ by this much; `near`: smooth below this distance.
  Hd2DRules(int floor, int near) : floor_(floor), near_(near) {}

  // Rules of native lines l0..l1-1 of `frame` (256x192 0xAARRGGBB) into the same rows of `out` (256x192 words).
  // `owner` (256x192 bytes, nonzero where 2D owns the pixel) may be null: all 2D. Reads lines l0-2..l1+1.
  void lines(const uint32_t* frame, const uint8_t* owner, unsigned l0, unsigned l1, uint32_t* out) const {
    for (unsigned first = l0; first < l1; first += band) {
      const unsigned last = std::min(l1, first + band);
      Window w;
      w.load(frame, owner, int(first) - 2, int(last - first) + 4);
      for (unsigned l = first; l < last; ++l) line(w, int(l - first), out + l * src_w);
    }
  }

  static unsigned corner_state(uint32_t word, unsigned corner) {
    const unsigned pair = corner < 2 ? word & 255 : (word >> 8) & 255;
    return corner & 1 ? pair & 7 : pair >> 3;
  }

private:
  static constexpr unsigned band = 8;
  const int floor_, near_;
  static constexpr int pad = 2, row_w = int(src_w) + 2 * pad;

  // Lines from `first` on, edge-clamped, with 2 px of clamped padding each side, their owners, and every distance
  // between neighbours the lines' rules read, each computed once for the band: a row's line above is the line below
  // of the row before, and the corner tests read the same diagonals the classification does.
  struct Window {
    static constexpr int max_rows = band + 4;
    uint32_t px[max_rows][row_w];
    uint8_t own[max_rows][row_w];
    // hz[r][c]: distance of (r, c-1) and (r, c); vt[r][c]: of (r, c) and (r+1, c); dd[r][c]: of (r, c) and
    // (r+1, c+1); ad[r][c]: of (r, c) and (r+1, c-1).
    alignas(16) int16_t hz[max_rows][row_w], vt[max_rows - 1][row_w], dd[max_rows - 1][row_w], ad[max_rows - 1][row_w];
    int rows = 0;
    void load(const uint32_t* frame, const uint8_t* owner, int first, int count) {
      rows = count;
      for (int r = 0; r < rows; ++r) {
        const int line = std::clamp(first + r, 0, int(src_h) - 1);
        const uint32_t* src = frame + line * src_w;
        uint32_t* row = px[r];
        std::memcpy(row + pad, src, sizeof(uint32_t) * src_w);
        row[0] = row[1] = src[0];
        row[row_w - 1] = row[row_w - 2] = src[src_w - 1];
        uint8_t* o = own[r];
        if (owner) {
          for (unsigned x = 0; x < src_w; ++x) o[x + pad] = owner[line * src_w + x] != 0;
          o[0] = o[1] = o[pad];
          o[row_w - 1] = o[row_w - 2] = o[row_w - 1 - pad];
        } else std::memset(o, 1, row_w);
      }
      // The lines are rows 2..rows-3. Their classification reads hz of their own row and vt of theirs and the one
      // above, columns 2..258; the corner tests read dd and ad of rows 0..rows-2, columns 0..259.
      for (int r = 0; r + 1 < rows; ++r) {
        const uint32_t* a = px[r];
        const uint32_t* n = px[r + 1];
        const bool line = r >= 2 && r <= rows - 3, upper = r >= 1 && r <= rows - 3;
#if defined(__ARM_NEON)
        for (int c = 1; c + 16 <= row_w - 3; c += 16) {
          df16(a + c, n + c + 1, dd[r] + c);
          df16(a + c, n + c - 1, ad[r] + c);
        }
        for (int c : {0, row_w - 3, row_w - 2, row_w - 1}) {
          dd[r][c] = int16_t(c + 1 < row_w ? df(a[c], n[c + 1]) : 0);
          ad[r][c] = int16_t(c > 0 ? df(a[c], n[c - 1]) : 0);
        }
        if (upper)
          for (int c = pad; c < pad + int(src_w); c += 16) df16(a + c, n + c, vt[r] + c);
        if (line) {
          for (int c = pad; c < pad + int(src_w); c += 16) df16(a + c - 1, a + c, hz[r] + c);
          hz[r][pad + src_w] = int16_t(df(a[pad + src_w - 1], a[pad + src_w]));
        }
#else
        for (int c = 0; c < row_w; ++c) {
          dd[r][c] = int16_t(c + 1 < row_w ? df(a[c], n[c + 1]) : 0);
          ad[r][c] = int16_t(c > 0 ? df(a[c], n[c - 1]) : 0);
        }
        if (upper)
          for (int c = pad; c < pad + int(src_w); ++c) vt[r][c] = int16_t(df(a[c], n[c]));
        if (line)
          for (int c = pad; c <= pad + int(src_w); ++c) hz[r][c] = int16_t(df(a[c - 1], a[c]));
#endif
      }
    }
  };

  // Luma (weights in 128ths, so 16-bit lanes hold it, rounded toward zero so df(a, b) == df(b, a)) plus a quarter
  // of the two chroma differences.
  static int df(uint32_t a, uint32_t b) {
    if (a == b) return 0;
    const int dr = int((a >> 16) & 255) - int((b >> 16) & 255), dg = int((a >> 8) & 255) - int((b >> 8) & 255),
              db = int(a & 255) - int(b & 255);
    const int dy = (27 * dr + 92 * dg + 9 * db) / 128;
    return std::abs(dy) + ((std::abs(db - dy) + std::abs(dr - dy)) >> 2);
  }
#if defined(__ARM_NEON)
  // df of 16 pixel pairs at once (the same arithmetic, lane by lane).
  static void df16(const uint32_t* a, const uint32_t* b, int16_t* out) {
    const uint8x16x4_t pa = vld4q_u8(reinterpret_cast<const uint8_t*>(a));
    const uint8x16x4_t pb = vld4q_u8(reinterpret_cast<const uint8_t*>(b));
    auto eight = [](uint8x8_t ab, uint8x8_t ag, uint8x8_t ar, uint8x8_t bb, uint8x8_t bg, uint8x8_t br) {
      const int16x8_t db = vreinterpretq_s16_u16(vsubl_u8(ab, bb)), dg = vreinterpretq_s16_u16(vsubl_u8(ag, bg)),
                      dr = vreinterpretq_s16_u16(vsubl_u8(ar, br));
      const int16x8_t sum = vmlaq_n_s16(vmlaq_n_s16(vmulq_n_s16(dr, 27), dg, 92), db, 9);
      const int16x8_t dy = vshrq_n_s16(vaddq_s16(sum, vandq_s16(vshrq_n_s16(sum, 15), vdupq_n_s16(127))), 7);
      return vaddq_s16(vabsq_s16(dy), vshrq_n_s16(vaddq_s16(vabdq_s16(db, dy), vabdq_s16(dr, dy)), 2));
    };
    vst1q_s16(out, eight(vget_low_u8(pa.val[0]), vget_low_u8(pa.val[1]), vget_low_u8(pa.val[2]), vget_low_u8(pb.val[0]),
                         vget_low_u8(pb.val[1]), vget_low_u8(pb.val[2])));
    vst1q_s16(out + 8, eight(vget_high_u8(pa.val[0]), vget_high_u8(pa.val[1]), vget_high_u8(pa.val[2]),
                             vget_high_u8(pb.val[0]), vget_high_u8(pb.val[1]), vget_high_u8(pb.val[2])));
  }
#endif

  // Neighbour offsets of the k = 0 (bottom-right) corner, rotated per corner, all compile-time.
  enum Slot { SB, SC, SD, SF, SG, SH, SI, SI4, SI5, SH5, SF4, SE };
  struct Offset {
    int dx, dy;
  };
  // Each corner's first and second axes: right/down, up/right, left/up, down/left.
  static constexpr int axes[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
  static constexpr Offset slot(int k, int s) {
    constexpr Offset base[SE + 1] = {{0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1},
                                     {1, 1},  {2, 1},  {1, 2},  {0, 2}, {2, 0},  {0, 0}};
    return {base[s].dx * axes[k][0] + base[s].dy * axes[k][2], base[s].dx * axes[k][1] + base[s].dy * axes[k][3]};
  }
  // A distance between diagonal neighbours a and b as a map entry relative to the pixel.
  struct Diagonal {
    bool anti;
    int dr, dc;
  };
  static constexpr Diagonal diagonal(int k, int a, int b) {
    Offset p = slot(k, a), q = slot(k, b);
    if (q.dy < p.dy) { // p is the upper one, q one row below
      const Offset t = p;
      p = q;
      q = t;
    }
    return {q.dx < p.dx, p.dy, p.dx};
  }
  template <int K, int A, int B> static int dist(const Window& w, int row, int x) {
    constexpr Diagonal g = diagonal(K, A, B);
    if constexpr (g.anti) return w.ad[row + g.dr][x + g.dc];
    else return w.dd[row + g.dr][x + g.dc];
  }
  template <int K, int S> static uint32_t at(const Window& w, int row, int x) {
    constexpr Offset o = slot(K, S);
    return w.px[row + o.dy][x + o.dx];
  }
  template <int K, int A, int B> static int knight(const Window& w, int row, int x) {
    constexpr Offset a = slot(K, A), b = slot(K, B);
    return df(w.px[row + a.dy][x + a.dx], w.px[row + b.dy][x + b.dx]);
  }

  // Rules of every pixel of window row `top` + 2 (which reads rows top..top+4). Every pixel's ownership and
  // smoothing, and which of its corners face two other colours beyond the floor, eight pixels at a time; then the
  // corner tests for those candidates alone.
  void line(const Window& w, int top, uint32_t* out) const {
    const int row = top + 2;
    const uint32_t* mid = w.px[row] + pad;
    const uint32_t* above = w.px[row - 1] + pad;
    const uint32_t* below = w.px[row + 1] + pad;
    const uint8_t* own_mid = w.own[row] + pad;
    const uint8_t* own_above = w.own[row - 1] + pad;
    const uint8_t* own_below = w.own[row + 1] + pad;
    // Distances to the eight neighbours, from the window's maps: left[x] is pixel x-1 to x (so x's right is
    // left[x+1]); up-right is the line above's down-left one pixel on, up-left its down-right one pixel back.
    const int16_t* left = w.hz[row] + pad;
    const int16_t* down = w.vt[row] + pad;
    const int16_t* up = w.vt[row - 1] + pad;
    const int16_t* dr = w.dd[row] + pad;
    const int16_t* dl = w.ad[row] + pad;
    const int16_t* ur = w.ad[row - 1] + pad + 1;
    const int16_t* ul = w.dd[row - 1] + pad - 1;
    alignas(16) uint8_t candidates[src_w];
#if defined(__ARM_NEON)
    // Lanes are all ones where they hold.
    auto differs = [](const uint32_t* a, const uint32_t* b) {
      return vcombine_u16(vmovn_u32(vmvnq_u32(vceqq_u32(vld1q_u32(a), vld1q_u32(b)))),
                          vmovn_u32(vmvnq_u32(vceqq_u32(vld1q_u32(a + 4), vld1q_u32(b + 4)))));
    };
    auto owns = [](const uint8_t* o) { return vtstq_u16(vmovl_u8(vld1_u8(o)), vdupq_n_u16(1)); };
    auto bit = [](uint16x8_t lanes, unsigned value) { return vandq_u16(lanes, vdupq_n_u16(uint16_t(value))); };
    const int16x8_t floor = vdupq_n_s16(int16_t(floor_)), close = vdupq_n_s16(int16_t(near_));
    for (int x = 0; x < int(src_w); x += 8) {
      const uint32_t* e = mid + x;
      const uint16x8_t nr = differs(e, mid + x + 1), nl = differs(e, mid + x - 1), nd = differs(e, below + x),
                       nu = differs(e, above + x);
      const int16x8_t d_r = vld1q_s16(left + x + 1), d_l = vld1q_s16(left + x), d_d = vld1q_s16(down + x),
                      d_u = vld1q_s16(up + x);
      // Corner candidates: both neighbours a corner faces differ by the floor or more.
      const uint16x8_t r = vandq_u16(nr, vcgeq_s16(d_r, floor)), l = vandq_u16(nl, vcgeq_s16(d_l, floor)),
                       d = vandq_u16(nd, vcgeq_s16(d_d, floor)), u = vandq_u16(nu, vcgeq_s16(d_u, floor));
      const uint16x8_t corners = vorrq_u16(vorrq_u16(bit(vandq_u16(r, d), 1), bit(vandq_u16(u, r), 2)),
                                           vorrq_u16(bit(vandq_u16(l, u), 4), bit(vandq_u16(d, l), 8)));
      vst1_u8(candidates + x, vmovn_u16(corners));
      // Smoothing: a 2D pixel toward 2D neighbours that differ from it by less than `near_`.
      const uint16x8_t own = owns(own_mid + x);
      auto near = [&](uint16x8_t differ, const uint8_t* o, int16x8_t distance, unsigned value) {
        return bit(vandq_u16(vandq_u16(differ, owns(o)), vcltq_s16(distance, close)), value);
      };
      const uint16x8_t ndr = differs(e, below + x + 1), ndl = differs(e, below + x - 1),
                       nur = differs(e, above + x + 1), nul = differs(e, above + x - 1);
      const uint16x8_t towards = vandq_u16(
          own, vorrq_u16(vorrq_u16(vorrq_u16(near(nr, own_mid + x + 1, d_r, 1), near(nd, own_below + x, d_d, 2)),
                                   vorrq_u16(near(nl, own_mid + x - 1, d_l, 4), near(nu, own_above + x, d_u, 8))),
                         vorrq_u16(vorrq_u16(near(ndr, own_below + x + 1, vld1q_s16(dr + x), 16),
                                             near(ndl, own_below + x - 1, vld1q_s16(dl + x), 32)),
                                   vorrq_u16(near(nur, own_above + x + 1, vld1q_s16(ur + x), 64),
                                             near(nul, own_above + x - 1, vld1q_s16(ul + x), 128)))));
      // The top half of each word: B = 16 where 2D owns the pixel, A = the smoothing flags.
      const uint16x8_t high = vorrq_u16(bit(own, 16), vshlq_n_u16(towards, 8));
      vst1q_u32(out + x, vshll_n_u16(vget_low_u16(high), 16));
      vst1q_u32(out + x + 4, vshll_n_u16(vget_high_u16(high), 16));
    }
#else
    for (int x = 0; x < int(src_w); ++x) {
      const uint32_t e = mid[x];
      const unsigned r = e != mid[x + 1] && left[x + 1] >= floor_, l = e != mid[x - 1] && left[x] >= floor_,
                     d = e != below[x] && down[x] >= floor_, u = e != above[x] && up[x] >= floor_;
      candidates[x] = uint8_t((r & d) | (u & r) << 1 | (l & u) << 2 | (d & l) << 3);
      const unsigned own = own_mid[x] != 0;
      unsigned towards = 0;
      if (own) {
        auto near = [&](uint32_t n, uint8_t n_own, int distance) {
          return unsigned(n != e && n_own && distance < near_);
        };
        towards = near(mid[x + 1], own_mid[x + 1], left[x + 1]) | near(below[x], own_below[x], down[x]) << 1 |
                  near(mid[x - 1], own_mid[x - 1], left[x]) << 2 | near(above[x], own_above[x], up[x]) << 3 |
                  near(below[x + 1], own_below[x + 1], dr[x]) << 4 | near(below[x - 1], own_below[x - 1], dl[x]) << 5 |
                  near(above[x + 1], own_above[x + 1], ur[x]) << 6 | near(above[x - 1], own_above[x - 1], ul[x]) << 7;
      }
      out[x] = own << 20 | towards << 24;
    }
#endif
    for (int x = 0; x < int(src_w); ++x) {
      const unsigned c = candidates[x];
      if (!c) continue;
      // Each corner's distances to the neighbours it faces: (right, down), (up, right), (left, up), (down, left).
      const int d_r = left[x + 1], d_d = down[x], d_l = left[x], d_u = up[x];
      const int px = x + pad;
      unsigned states[4] = {0, 0, 0, 0}, take = 0;
      if (c & 1) states[0] = corner<0>(w, row, px, mid[x], d_r, d_d, take);
      if (c & 2) states[1] = corner<1>(w, row, px, mid[x], d_u, d_r, take);
      if (c & 4) states[2] = corner<2>(w, row, px, mid[x], d_l, d_u, take);
      if (c & 8) states[3] = corner<3>(w, row, px, mid[x], d_d, d_l, take);
      out[x] |= (states[0] * 8 + states[1]) | (states[2] * 8 + states[3]) << 8 | take << 16;
    }
  }

  // Corner K's state; sets bit K of `take` when it turns into its first-axis neighbour. ef, eh: the pixel's
  // distances to the corner's two neighbours.
  template <int K> unsigned corner(const Window& w, int row, int x, uint32_t e, int ef, int eh, unsigned& take) const {
    const int fb = dist<K, SF, SB>(w, row, x), hd = dist<K, SH, SD>(w, row, x), ei = dist<K, SE, SI>(w, row, x),
              fi4 = dist<K, SF, SI4>(w, row, x), hi5 = dist<K, SH, SI5>(w, row, x), eg = dist<K, SE, SG>(w, row, x),
              ec = dist<K, SE, SC>(w, row, x);
    auto eq = [](int distance) { return distance < same; };
    // Level 1: no rule on a real corner of the art.
    if (!((!eq(fb) && !eq(hd)) || (eq(ei) && !eq(fi4) && !eq(hi5)) || eq(eg) || eq(ec))) return None;
    // The edge runs along f-h when the e-i direction is the busier one.
    const int along =
        ec + eg + dist<K, SI, SH5>(w, row, x) + dist<K, SI, SF4>(w, row, x) + 4 * dist<K, SH, SF>(w, row, x);
    if (along >= hd + hi5 + fi4 + fb + 4 * ei) return None;
    const uint32_t g = at<K, SG>(w, row, x), c = at<K, SC>(w, row, x);
    const int fg = knight<K, SF, SG>(w, row, x), hc = knight<K, SH, SC>(w, row, x);
    const bool shallow = 2 * fg <= hc && e != g && at<K, SD>(w, row, x) != g;
    const bool steep = fg >= 2 * hc && e != c && at<K, SB>(w, row, x) != c;
    if (ef <= eh) take |= 1u << K;
    return shallow && steep ? Both : shallow ? Shallow : steep ? Steep : Edge45;
  }
};

} // namespace rgds
