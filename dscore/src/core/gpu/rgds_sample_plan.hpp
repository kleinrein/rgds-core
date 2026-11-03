// SPDX-License-Identifier: GPL-3.0-or-later
// Display-only sampling for the RG DS 640x480 panel. Guest pixels are untouched.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace rgds {
// The 3D scale: 1 to 5, or kPanelScale, 2.5x: the 640x480 panel itself, one rendered pixel to one panel pixel. Positions
// in the scaled image are worked out in half units (half_units(scale) / 2 rendered pixels a DS pixel).
constexpr unsigned kPanelScale = 25;
constexpr bool valid_scale(int scale) { return (scale >= 1 && scale <= 5) || scale == int(kPanelScale); }
constexpr unsigned half_units(unsigned scale) { return scale == kPanelScale ? 5 : 2 * scale; }
constexpr int scale_or_nearest(int scale) { return valid_scale(scale) ? scale : scale < 1 ? 1 : 5; }

struct SamplePlan {
  static constexpr uint16_t absent = 0xffff;
  struct Tap {
    uint16_t first, second;
  };
  std::array<std::array<Tap, 256>, 3> phase{};
  unsigned key = ~0u;

  // DS HOFS wraps at 512 native pixels; the second half is transparent.
  // Keeping that hole in the plan avoids a modulus and two bounds checks
  // per output sample. Most games keep the same offset across whole frames.
  void update(unsigned scale, unsigned offset, bool smooth) {
    offset &= 511;
    const unsigned s2 = half_units(scale), next = s2 | (offset << 4) | (unsigned(smooth) << 13);
    if (key == next) return;
    key = next;
    for (unsigned p = 0; p < 3; ++p)
      for (unsigned x = 0; x < 256; ++x) {
        const unsigned begin = x * 5 / 2, count = (x + 1) * 5 / 2 - begin;
        unsigned a, b;
        if (smooth) {
          const unsigned dx = begin + std::min(p, count - 1);
          a = (4 * dx + 1) * s2 / 20 + offset * s2 / 2;
          b = (4 * dx + 3) * s2 / 20 + offset * s2 / 2;
          if (a >= 256 * s2) a -= 256 * s2;
          if (b >= 256 * s2) b -= 256 * s2;
        } else {
          const unsigned sx = (x + offset) & 511;
          a = b = sx * s2 / 2 + std::min((s2 + 1) / 2 - 1, (2 * p + 1) * s2 / (4 * count));
        }
        phase[p][x] = {a < 128 * s2 ? uint16_t(a) : absent, b < 128 * s2 ? uint16_t(b) : absent};
      }
  }
};
}
