// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS 2D panel filters: a Smooth row must match a float bilinear reference,
// Sharp must keep pixel interiors exact and blend only at seams.
#include "check.h"
#include "core/gpu/rgds_filter2d.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using rgds::Filter2D;
using rgds::Filter2DPlan;

static std::vector<uint32_t> noise_image() {
  std::vector<uint32_t> image(256 * 192);
  uint32_t s = 12345;
  for (auto& p : image) {
    s = s * 1664525u + 1013904223u;
    p = 0xFF000000u | (s >> 8);
  }
  return image;
}

// Panel rows of one native line come from the line above/itself/below only.
static void render(const Filter2DPlan& plan, const std::vector<uint32_t>& image, std::vector<uint32_t>& panel) {
  panel.assign(640 * 480, 0);
  for (unsigned line = 0; line < 192; ++line) {
    const uint32_t* cur = &image[line * 256];
    const uint32_t* above = line ? cur - 256 : cur;
    const uint32_t* below = line < 191 ? cur + 256 : cur;
    const uint32_t* above2 = line >= 2 ? cur - 512 : above;
    for (unsigned y = line * 480 / 192; y < (line + 1) * 480 / 192; ++y)
      plan.row(above2, above, cur, below, line, y, &panel[y * 640]);
  }
}

static int channel(uint32_t c, int shift) { return int((c >> shift) & 255); }

static void smooth_matches_reference() {
  const auto image = noise_image();
  std::vector<uint32_t> panel;
  render(Filter2DPlan::get(Filter2D::Smooth), image, panel);
  int worst = 0;
  for (unsigned y = 0; y < 480; ++y)
    for (unsigned x = 0; x < 640; ++x) {
      const double u = (x + 0.5) * 256 / 640 - 0.5, v = (y + 0.5) * 192 / 480 - 0.5;
      const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
      const double fx = u - x0, fy = v - y0;
      auto at = [&](int xx, int yy) {
        return image[std::min(std::max(yy, 0), 191) * 256 + std::min(std::max(xx, 0), 255)];
      };
      for (int shift : {0, 8, 16}) {
        const double top = channel(at(x0, y0), shift) * (1 - fx) + channel(at(x0 + 1, y0), shift) * fx;
        const double bottom = channel(at(x0, y0 + 1), shift) * (1 - fx) + channel(at(x0 + 1, y0 + 1), shift) * fx;
        const double want = top * (1 - fy) + bottom * fy;
        worst = std::max(worst, int(std::lround(std::fabs(want - channel(panel[y * 640 + x], shift)))));
      }
      CHECK((panel[y * 640 + x] >> 24) == 0xFF);
    }
  CHECK(worst <= 3);
}

static void sharp_keeps_interiors() {
  const auto image = noise_image();
  std::vector<uint32_t> panel;
  render(Filter2DPlan::get(Filter2D::Sharp), image, panel);
  unsigned exact = 0, blended = 0;
  for (unsigned y = 0; y < 480; ++y)
    for (unsigned x = 0; x < 640; ++x) {
      const uint32_t out = panel[y * 640 + x];
      const unsigned nx = x * 2 / 5, ny = y * 2 / 5; // the pixel a nearest 2.5x would use
      if (out == image[ny * 256 + nx]) ++exact;
      else ++blended;
    }
  // 2.5x: the seam is about one panel pixel per axis, so roughly 0.6 * 0.6 of
  // the panel stays pure native colour (noise has no accidental matches).
  CHECK(exact * 100 / (exact + blended) >= 30);
  CHECK(blended > 0);
  // The first and last panel pixels are the first and last native pixels.
  CHECK(panel[0] == image[0]);
  CHECK(panel[479 * 640 + 639] == image[191 * 256 + 255]);
}

// Sharp is area sampling: at 2.5x four panel pixels in five, on each axis,
// are exactly one native pixel, and the fifth (the one a native edge cuts in
// half) is a linear-light 50/50 blend.
static void sharp_is_area_sampling() {
  const auto image = noise_image();
  std::vector<uint32_t> panel;
  render(Filter2DPlan::get(Filter2D::Sharp), image, panel);
  for (unsigned y = 0; y < 480; ++y)
    for (unsigned x = 0; x < 640; ++x) {
      if (x % 5 == 2 || y % 5 == 2) continue;
      CHECK(panel[y * 640 + x] == image[(y * 2 / 5) * 256 + x * 2 / 5]);
    }
  std::vector<uint32_t> stripes(256 * 192), out;
  for (unsigned i = 0; i < stripes.size(); ++i) stripes[i] = (i % 2) ? 0xFFFFFFFFu : 0xFF000000u;
  render(Filter2DPlan::get(Filter2D::Sharp), stripes, out);
  // Black | white seam: half the light, not half the code value (128).
  const int seam = channel(out[0 * 640 + 2], 0);
  CHECK(seam >= 180 && seam <= 192);
  CHECK(out[0] == 0xFF000000u && out[3] == 0xFFFFFFFFu);
}

// rows() builds a native line's panel rows at once; each equals row().
static void rows_match_row() {
  const auto image = noise_image();
  for (Filter2D mode : {Filter2D::Sharp, Filter2D::Smooth, Filter2D::Crisp}) {
    const auto& plan = Filter2DPlan::get(mode);
    for (unsigned line : {0u, 1u, 2u, 95u, 190u, 191u}) {
      const uint32_t* cur = &image[line * 256];
      const uint32_t* above = line ? cur - 256 : cur;
      const uint32_t* below = line < 191 ? cur + 256 : cur;
      const uint32_t* above2 = line >= 2 ? cur - 512 : above;
      const unsigned y0 = line * 480 / 192, y1 = (line + 1) * 480 / 192;
      std::vector<uint32_t> many(640 * 3), one(640);
      plan.rows(above2, above, cur, below, line, y0, y1, many.data(), 640);
      for (unsigned y = y0; y < y1; ++y) {
        plan.row(above2, above, cur, below, line, y, one.data());
        for (unsigned x = 0; x < 640; ++x) CHECK(one[x] == many[(y - y0) * 640 + x]);
      }
    }
  }
}

static void flat_image_stays_flat() {
  std::vector<uint32_t> image(256 * 192, 0xFF336699u), panel;
  for (Filter2D mode : {Filter2D::Sharp, Filter2D::Smooth, Filter2D::Crisp}) {
    render(Filter2DPlan::get(mode), image, panel);
    for (uint32_t p : panel) CHECK(p == 0xFF336699u);
  }
}

static void sharp_seam_is_narrow() {
  // A hard vertical edge between native columns 99 and 100 spreads over at most 2 panel pixels.
  std::vector<uint32_t> image(256 * 192), panel;
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) image[y * 256 + x] = x < 100 ? 0xFF000000u : 0xFFFFFFFFu;
  render(Filter2DPlan::get(Filter2D::Sharp), image, panel);
  unsigned between = 0;
  for (unsigned x = 0; x < 640; ++x) {
    const uint32_t p = panel[240 * 640 + x];
    if (p != 0xFF000000u && p != 0xFFFFFFFFu) ++between;
  }
  CHECK(between <= 2);
  render(Filter2DPlan::get(Filter2D::Smooth), image, panel);
  between = 0;
  for (unsigned x = 0; x < 640; ++x) {
    const uint32_t p = panel[240 * 640 + x];
    if (p != 0xFF000000u && p != 0xFFFFFFFFu) ++between;
  }
  CHECK(between >= 2);
}

// Crisp: a staircase edge gains sloped cells (Scale2x), interiors away from
// edges stay exact, a checkerboard (no agreeing neighbours) is untouched, and
// a panel pixel never leaves the colours of the source.
static void crisp_slopes_stairs() {
  const uint32_t black = 0xFF000000u, white = 0xFFFFFFFFu;
  std::vector<uint32_t> image(256 * 192), panel;
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) image[y * 256 + x] = x >= y + 1 ? white : black;
  render(Filter2DPlan::get(Filter2D::Crisp), image, panel);
  // DS pixel (4, 3) is a white edge pixel with black left and below: its
  // bottom-left cell turns black. Panel (10, 9) is exactly that cell.
  CHECK(panel[9 * 640 + 10] == black);
  render(Filter2DPlan::get(Filter2D::Sharp), image, panel);
  CHECK(panel[9 * 640 + 10] == white); // Sharp keeps the stair
  render(Filter2DPlan::get(Filter2D::Crisp), image, panel);
  CHECK(panel[9 * 640 + 14] == white); // (5, 3): bottom-right cell, white neighbours on that side
  // Far from the edge, every panel pixel is the exact source colour.
  for (unsigned y = 300; y < 480; ++y)
    for (unsigned x = 0; x < 100; ++x) CHECK(panel[y * 640 + x] == black);
  for (unsigned y = 0; y < 100; ++y)
    for (unsigned x = 400; x < 640; ++x) CHECK(panel[y * 640 + x] == white);
  std::vector<uint32_t> board(256 * 192), sharp;
  for (unsigned y = 0; y < 192; ++y)
    for (unsigned x = 0; x < 256; ++x) board[y * 256 + x] = ((x ^ y) & 1) ? white : black;
  render(Filter2DPlan::get(Filter2D::Crisp), board, panel);
  render(Filter2DPlan::get(Filter2D::Sharp), board, sharp);
  // Away from the image corners (where both edge clamps meet and Scale2x
  // sees two agreeing clamped neighbours) the two are identical.
  for (unsigned y = 5; y < 475; ++y)
    for (unsigned x = 5; x < 635; ++x) CHECK(panel[y * 640 + x] == sharp[y * 640 + x]);
}

int main() {
  crisp_slopes_stairs();
  smooth_matches_reference();
  sharp_keeps_interiors();
  flat_image_stays_flat();
  sharp_seam_is_narrow();
  sharp_is_area_sampling();
  rows_match_row();
  std::puts("filter2d ok");
  return 0;
}
