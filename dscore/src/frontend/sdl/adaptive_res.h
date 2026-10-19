// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS adaptive resolution: decides when a 2x-5x frame stream should hand
// its 3D to the native raster for a while, from frame times and GPU waits.
//
// 6 of the last 30 frames over budget start a hold (2 s), if composition
// spent at least 2 ms a frame waiting for the GPU meanwhile: overruns the
// GPU did not cause (the CPU, a game's own spikes) stay at high resolution,
// where the native raster would only add CPU work. Right after a return a
// short probe (3 of 10 frames over) relapses at once, doubling the hold up
// to 32 s; a clean minute resets it. A hold whose frames were no faster
// than the window that started it (a CPU-bound title) ends with high
// resolution for a minute, five minutes on a repeat.
#pragma once

#include <algorithm>

namespace ds::sdl {

class AdaptiveRes {
public:
  enum class Event { None, Hold, Release, ReleaseNoGain };
  static constexpr unsigned kWindow = 30, kOverLimit = 6, kProbeWindow = 10, kProbeOver = 3;
  static constexpr double kFirstHoldMs = 2000, kMaxHoldMs = 32000, kRelapseMs = 3000, kCleanMs = 60000, kNoGainMs = 60000, kNoGainAgainMs = 300000, kGpuWaitMs = 2;

  // One real-time frame at high resolution: `work_ms` against `budget_ms`,
  // and how long composition waited for the GPU's 3D in it.
  Event frame(double now_ms, double work_ms, double budget_ms, double gpu_wait_ms) {
    Event event = Event::None;
    ++window_; if (work_ms > budget_ms * 0.97) ++over_;
    gpu_wait_ += gpu_wait_ms; window_work_ += work_ms;
    if (hold_) { hold_work_ += work_ms; ++hold_n_; }
    if (hold_ && now_ms >= until_) {
      hold_avg_ = hold_n_ ? hold_work_ / hold_n_ : 0;
      hold_ = false; released_ = now_ms; probing_ = true; window_ = over_ = 0; gpu_wait_ = 0; window_work_ = 0;
      if (hold_avg_ > hires_avg_ * 0.9) { quiet_until_ = now_ms + (no_gain_ ? kNoGainAgainMs : kNoGainMs); no_gain_ = true; probing_ = false; event = Event::ReleaseNoGain; }
      else event = Event::Release;
    }
    // Right after a return, a short window decides; otherwise a full one.
    const unsigned span = probing_ ? kProbeWindow : kWindow, limit = probing_ ? kProbeOver : kOverLimit;
    if (window_ >= span) {
      if (!hold_ && over_ >= limit && gpu_wait_ >= kGpuWaitMs * window_ && now_ms >= quiet_until_) {
        if (now_ms - released_ < kRelapseMs) hold_ms_ = std::min(kMaxHoldMs, hold_ms_ * 2);
        hires_avg_ = window_work_ / window_;   // the frames that asked for help: what the hold must beat
        hold_ = true; until_ = now_ms + hold_ms_; hold_work_ = 0; hold_n_ = 0; last_over_ = over_;
        event = Event::Hold;
      }
      probing_ = false;
      if (!hold_ && over_ == 0) { if (clean_from_ < 0) clean_from_ = now_ms; else if (now_ms - clean_from_ > kCleanMs) hold_ms_ = kFirstHoldMs; }
      else clean_from_ = -1;
      window_ = over_ = 0; gpu_wait_ = 0; window_work_ = 0;
    }
    return event;
  }
  // High resolution is off or fast forward runs: no budget to keep. True if a hold was cut short.
  bool pause() { const bool was = hold_; hold_ = false; probing_ = false; window_ = over_ = 0; gpu_wait_ = 0; window_work_ = 0; return was; }

  bool holding() const { return hold_; }
  double hold_ms() const { return hold_ms_; }
  unsigned last_over() const { return last_over_; }
  double hires_avg() const { return hires_avg_; }
  double hold_avg() const { return hold_avg_; }

private:
  unsigned window_ = 0, over_ = 0, last_over_ = 0, hold_n_ = 0;
  bool hold_ = false, probing_ = false, no_gain_ = false;
  double hold_ms_ = kFirstHoldMs, until_ = 0, released_ = -1e18, clean_from_ = -1, quiet_until_ = -1e18;
  double hold_work_ = 0, hires_avg_ = 0, hold_avg_ = 0, gpu_wait_ = 0, window_work_ = 0;
};

}  // namespace ds::sdl
