// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ds::sdl {

// From capture's bursts to one even slice per emulated frame. The core spreads a frame's samples across the frame, so
// a short slice plays stretched and an empty one is silence: the capture period (25 ms) against the frame (16.7 ms)
// made both, many times a second, which a game that records and plays back turns into a gargle. So: a cushion before
// the first sample, then slices at the rate samples arrive, leaning slowly towards the cushion. Frames before the first
// sample say nothing about that rate: a game may open the microphone minutes in.
class MicQueue {
public:
  explicit MicQueue(size_t per_frame)
      : per_frame_(per_frame), target_(per_frame * 4), arrival_(static_cast<double>(per_frame)) {}

  // Once per frame, also with nothing.
  void push(const std::vector<s16>& samples) {
    if (samples.empty() && !flowing_) return;
    flowing_ = true;
    q_.insert(q_.end(), samples.begin(), samples.end());
    arrival_ += (static_cast<double>(samples.size()) - arrival_) / SMOOTHING;
    // a stall (a menu, a slow load): skip ahead instead of turning it into latency
    if (q_.size() > per_frame_ * 8) q_.erase(q_.begin(), q_.end() - static_cast<std::ptrdiff_t>(target_));
  }

  void take(std::vector<s16>& out) {
    out.clear();
    if (!primed_ && q_.size() < target_) return;
    primed_ = true;
    const double want = arrival_ + (static_cast<double>(q_.size()) - static_cast<double>(target_)) / SMOOTHING + carry_;
    // never a sliver spread across a whole frame, whatever the estimate says
    const double slice = std::max(want, static_cast<double>(per_frame_ / 2));
    const size_t n = static_cast<size_t>(slice);
    carry_ = slice - static_cast<double>(n);
    if (n > q_.size()) { // ran dry: silence now beats a stretched slice, and the cushion comes back
      primed_ = false;
      carry_ = 0;
      return;
    }
    out.assign(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
    q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
  }

private:
  static constexpr double SMOOTHING = 256;
  size_t per_frame_, target_;
  double arrival_, carry_ = 0;
  bool primed_ = false, flowing_ = false;
  std::vector<s16> q_;
};

// The speech band, before anything else sees the samples. The microphone's quiet noise on the RG DS is mostly rumble under
// 500 Hz, and a game reads the microphone at 8 kHz or so with nothing in between, so what lies over 4 kHz folds down as
// hiss. Butterworth biquads.
class MicBand {
public:
  explicit MicBand(double rate) : low_(lowpass(5000, rate)), high_(highpass(120, rate)) {}

  double apply(double x) { return run(high_, run(low_, x)); }

private:
  struct Biquad {
    double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  };

  static double run(Biquad& f, double x) {
    const double y = f.b0 * x + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
    f.x2 = f.x1;
    f.x1 = x;
    f.y2 = f.y1;
    f.y1 = y;
    return y;
  }

  static Biquad lowpass(double cutoff, double rate) {
    const double w = 2 * 3.14159265358979 * cutoff / rate, c = std::cos(w), a = std::sin(w) / std::sqrt(2.0), a0 = 1 + a;
    return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - a) / a0};
  }

  static Biquad highpass(double cutoff, double rate) {
    const double w = 2 * 3.14159265358979 * cutoff / rate, c = std::cos(w), a = std::sin(w) / std::sqrt(2.0), a0 = 1 + a;
    return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - a) / a0};
  }

  Biquad low_, high_;
};

// Noise gate: the floor is the slowest-rising rms seen, and a frame under `gate` times it is silence. Speech dips under
// that between syllables, and a gate that shut for each dip chopped it up, so it stays open a moment after sound.
class MicGate {
public:
  explicit MicGate(double gate) : gate_(gate) {}

  void apply(std::vector<s16>& mic) {
    if (gate_ <= 0 || mic.empty()) return;
    double sq = 0;
    for (s16 v : mic) sq += double(v) * v;
    const double rms = std::sqrt(sq / static_cast<double>(mic.size()));
    floor_ = rms < floor_ ? rms : floor_ + (rms - floor_) * 0.002; // drops at once, creeps up
    if (rms >= floor_ * gate_ + 16) hold_ = HOLD_FRAMES;
    else if (hold_ > 0) --hold_;
    else std::fill(mic.begin(), mic.end(), 0);
  }

private:
  static constexpr int HOLD_FRAMES = 15; // 250 ms
  double gate_, floor_ = 1e9;
  int hold_ = 0;
};

} // namespace ds::sdl
