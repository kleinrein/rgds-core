// SPDX-License-Identifier: GPL-3.0-or-later
// HD 2D rules (rgds_hd2d_rules.hpp): flat art does nothing, stairs get corners, gradients get smoothing, 3D-owned
// pixels never smooth, HD smooth rebuilds the soft stairs HD leaves alone, and a frame computed in bands equals one
// computed whole.
#include "check.h"
#include "core/gpu/rgds_hd2d_rules.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using rgds::Hd2DRules;

static std::vector<uint32_t> rules_of(const std::vector<uint32_t>& image, const uint8_t* owner = nullptr,
                                      bool smooth = false) {
  std::vector<uint32_t> out(256 * 192, 0xDEADBEEFu);
  Hd2DRules::get(smooth).lines(image.data(), owner, 0, 192, out.data());
  return out;
}
static unsigned smooth_of(uint32_t word) { return word >> 24; }
static bool owned(uint32_t word) { return (word >> 20) & 1; }

static void flat_does_nothing() {
  std::vector<uint32_t> image(256 * 192, 0xFF336699u);
  for (uint32_t w : rules_of(image)) CHECK(w == (1u << 20)); // owned, nothing else
}

static void stairs_get_corners() {
  const uint32_t ink = 0xFF000000u, paper = 0xFFFFFFFFu;
  std::vector<uint32_t> image(256 * 192);
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) image[y * 256 + x] = x >= y + 64 ? paper : ink; // a 45 degree staircase
  const auto rules = rules_of(image);
  // Pixel (101, 37): ink, paper to its right and below-right; its bottom-right corner is on the stair.
  unsigned corners = 0;
  for (unsigned y = 10; y < 180; ++y) {
    const uint32_t w = rules[y * 256 + y + 63]; // the last ink pixel of each row
    if (Hd2DRules::corner_state(w, 0) || Hd2DRules::corner_state(w, 1) || Hd2DRules::corner_state(w, 2) ||
        Hd2DRules::corner_state(w, 3))
      ++corners;
  }
  CHECK(corners > 150);
  // Far from the stair: nothing.
  CHECK(rules[100 * 256 + 10] == (1u << 20));
  CHECK(rules[100 * 256 + 250] == (1u << 20));
  // A straight vertical edge is not a stair: no corner anywhere.
  std::vector<uint32_t> edge(256 * 192);
  for (unsigned i = 0; i < edge.size(); ++i) edge[i] = (i % 256) < 128 ? ink : paper;
  for (uint32_t w : rules_of(edge)) CHECK((w & 0xFFFF) == 0);
}

static void gradients_smooth() {
  std::vector<uint32_t> image(256 * 192);
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) {
      const uint32_t v = 64 + (x / 4) * 2; // steps of 2 levels: close neighbours
      image[y * 256 + x] = 0xFF000000u | v << 16 | v << 8 | v;
    }
  const auto rules = rules_of(image);
  // A pixel on a step smooths toward its differing neighbour on that side, and has no corner.
  const uint32_t w = rules[50 * 256 + 3]; // x 3 and 4 differ by 2 levels
  CHECK((smooth_of(w) & 1) != 0);
  CHECK((w & 0xFFFF) == 0);
  // 3D-owned pixels never smooth and are not smoothed toward.
  std::vector<uint8_t> owner(256 * 192, 1);
  for (unsigned y = 0; y < 192; ++y) owner[y * 256 + 4] = 0;
  const auto mixed = rules_of(image, owner.data());
  CHECK(!owned(mixed[50 * 256 + 4]) && smooth_of(mixed[50 * 256 + 4]) == 0);
  CHECK((smooth_of(mixed[50 * 256 + 3]) & 1) == 0);
}

static void smooth_takes_soft_stairs() {
  // Two shades 30 levels apart: shading, below HD's structural floor and above its smoothing distance.
  const uint32_t dark = 0xFF606060u, light = 0xFF7E7E7Eu;
  std::vector<uint32_t> image(256 * 192);
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) image[y * 256 + x] = x >= y + 64 ? light : dark;
  auto corners = [](const std::vector<uint32_t>& rules) {
    unsigned n = 0;
    for (unsigned y = 10; y < 180; ++y)
      for (unsigned c = 0; c < 4; ++c) n += Hd2DRules::corner_state(rules[y * 256 + y + 63], c) != 0;
    return n;
  };
  const auto crisp = rules_of(image), smooth = rules_of(image, nullptr, true);
  CHECK(corners(crisp) == 0);
  CHECK(corners(smooth) > 150);
  CHECK(smooth_of(crisp[50 * 256 + 113]) == 0);
  CHECK(smooth_of(smooth[50 * 256 + 113]) != 0); // the last dark pixel of row 50 blends toward the light
  for (uint32_t w : rules_of(std::vector<uint32_t>(256 * 192, light), nullptr, true)) CHECK(w == (1u << 20));
}

static void bands_equal_whole() {
  std::vector<uint32_t> image(256 * 192);
  uint32_t s = 7;
  for (auto& p : image) {
    s = s * 1664525u + 1013904223u;
    p = (s >> 28) < 3 ? 0xFF000000u | (s >> 8) : 0xFFFFFFFFu; // sparse noise on white: corners and smoothing
  }
  const auto whole = rules_of(image);
  std::vector<uint32_t> banded(256 * 192, 0);
  for (unsigned l = 0; l < 192; l += 8) Hd2DRules::get().lines(image.data(), nullptr, l, l + 8, banded.data());
  CHECK(banded == whole);
  const auto soft = rules_of(image, nullptr, true);
  for (unsigned l = 0; l < 192; l += 8) Hd2DRules::get(true).lines(image.data(), nullptr, l, l + 8, banded.data());
  CHECK(banded == soft);
}

int main() {
  flat_does_nothing();
  stairs_get_corners();
  gradients_smooth();
  smooth_takes_soft_stairs();
  bands_equal_whole();
  std::puts("hd2d rules ok");
  return 0;
}
