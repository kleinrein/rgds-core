// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rgds_experimental {
// The DS has one stylus. Only a down event may claim it; another finger's
// motion/up must never teleport or release the current stroke.
class Stylus {
  bool held_ = false;
  int64_t device_ = 0, finger_ = 0;

public:
  bool down(int64_t device, int64_t finger) {
    if (held_) return false;
    held_ = true;
    device_ = device;
    finger_ = finger;
    return true;
  }
  bool owns(int64_t device, int64_t finger) const { return held_ && device == device_ && finger == finger_; }
  bool up(int64_t device, int64_t finger) {
    if (!owns(device, finger)) return false;
    held_ = false;
    return true;
  }
  bool held() const { return held_; }
  static int pixel(float normalized, int extent) {
    if (extent <= 0 || !std::isfinite(normalized)) return 0;
    return std::min(extent - 1, int(std::clamp(normalized, 0.0f, 1.0f) * extent));
  }
};
}
