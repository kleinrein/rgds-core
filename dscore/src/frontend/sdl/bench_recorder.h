// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS: what one test of the Impeccable benchmark ROM (bench/ds) measured. The frame loop feeds it; the ROM reads
// the summary through core/io/bench_port.h and scores it. Arithmetic on timestamps only, so it can be tested
// (tests/bench_recorder_test.cpp), and nothing allocates while a test runs.
#pragma once
#include "core/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ds::sdl {

class BenchRecorder {
public:
  // The words the ROM reads, in this order (bench/ds/port.h has the same list). Times are microseconds.
  enum Field : int {
    FRAMES,      // emulated frames in the window
    PRESENTS,    // of them handed to the display
    SKIPPED,     // of them not drawn at all
    HIRES,       // of them with the GPU raster running
    WALL_US,     // first frame's start to the last one's
    FLIPS_0,     // page flips seen on each panel; 0 where the display reports none
    FLIPS_1,     //
    MISSED_0,    // refreshes each panel showed an old picture in, worked out from the flips
    MISSED_1,    //
    DROPPED,     // finished frames replaced before their vblank
    LATE,        // frames that began past their deadline
    GAP_US,      // the longest time any panel went without a new picture
    WORK_P50_US, // frame start to hand-off to the display
    WORK_P99_US, //
    WORK_P999_US,
    WORK_MAX_US,
    LAT_P50_US, // swap to flip
    LAT_P99_US,
    PERIOD_US,  // a refresh as this window saw it
    AUDIO_DRY,  // audio underruns in the window
    TEMP_START, // CPU temperature, millidegrees
    TEMP_END,
    LOCKED, // frames paced to the panel
    FIELD_COUNT
  };
  using Summary = std::array<u32, FIELD_COUNT>;

  static constexpr size_t CAPACITY = 20000; // 5.5 minutes at 60 fps; frames past it still count, unsampled

  BenchRecorder() {
    work_.reserve(CAPACITY);
    for (auto& f : flips_) f.reserve(CAPACITY);
    latency_.reserve(CAPACITY);
  }

  bool active() const { return active_; }

  void begin(u32 temperature, u64 audio_dry) {
    work_.clear();
    latency_.clear();
    for (auto& f : flips_) f.clear();
    result_ = {};
    result_[TEMP_START] = temperature;
    audio_dry_start_ = audio_dry;
    first_ns_ = last_ns_ = 0;
    frames_ = presents_ = skipped_ = hires_ = dropped_ = late_ = locked_ = 0;
    active_ = true;
  }

  void frame(u64 start_ns, u64 done_ns, bool present, bool skipped, bool hires, bool locked) {
    if (!active_) return;
    if (!first_ns_) first_ns_ = start_ns;
    last_ns_ = start_ns;
    ++frames_;
    presents_ += present;
    skipped_ += skipped;
    hires_ += hires;
    locked_ += locked;
    if (work_.size() < CAPACITY) work_.push_back(static_cast<u32>((done_ns - start_ns) / 1000));
  }

  void flip(int panel, u64 at_ns, u64 issue_ns, int tag) {
    if (!active_ || panel < 0 || panel > 1) return;
    if (flips_[panel].size() < CAPACITY) flips_[panel].push_back(at_ns);
    if (latency_.size() < CAPACITY && issue_ns && at_ns > issue_ns)
      latency_.push_back(static_cast<u32>((at_ns - issue_ns) / 1000));
    dropped_ += (tag & 4) != 0;
    late_ += (tag & 1) != 0;
  }

  const Summary& end(u32 temperature, u64 audio_dry) {
    active_ = false;
    Summary& r = result_;
    r[FRAMES] = frames_;
    r[PRESENTS] = presents_;
    r[SKIPPED] = skipped_;
    r[HIRES] = hires_;
    r[WALL_US] = static_cast<u32>((last_ns_ - first_ns_) / 1000);
    r[DROPPED] = dropped_;
    r[LATE] = late_;
    r[LOCKED] = locked_;
    r[TEMP_END] = temperature;
    r[AUDIO_DRY] = static_cast<u32>(audio_dry - audio_dry_start_);
    std::sort(work_.begin(), work_.end());
    r[WORK_P50_US] = percentile(work_, 500);
    r[WORK_P99_US] = percentile(work_, 990);
    r[WORK_P999_US] = percentile(work_, 999);
    r[WORK_MAX_US] = work_.empty() ? 0 : work_.back();
    std::sort(latency_.begin(), latency_.end());
    r[LAT_P50_US] = percentile(latency_, 500);
    r[LAT_P99_US] = percentile(latency_, 990);
    u32 gap = 0, period = 0;
    for (int panel = 0; panel < 2; ++panel) {
      const Panel p = read_panel(flips_[panel]);
      r[FLIPS_0 + panel] = static_cast<u32>(flips_[panel].size());
      r[MISSED_0 + panel] = p.missed;
      gap = std::max(gap, p.gap_us);
      if (!period) period = p.period_us;
    }
    r[GAP_US] = gap;
    r[PERIOD_US] = period;
    return r;
  }

  // nth is in thousandths: 990 is the 99th percentile. Sorted input.
  static u32 percentile(const std::vector<u32>& sorted, u32 nth) {
    if (sorted.empty()) return 0;
    const size_t at = static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * nth / 1000.0));
    return sorted[std::min(sorted.size() - 1, at ? at - 1 : 0)];
  }

  struct Panel {
    u32 missed = 0, gap_us = 0, period_us = 0;
  };
  // The refresh is the 10th percentile interval (a median would call a steady 30 fps perfect), never longer than
  // a 55 Hz panel's, so a run that never once kept up still counts every repeat.
  static Panel read_panel(const std::vector<u64>& at) {
    Panel p;
    if (at.size() < 2) return p;
    std::vector<u64> gaps;
    gaps.reserve(at.size());
    for (size_t i = 1; i < at.size(); ++i) gaps.push_back(at[i] - at[i - 1]);
    std::vector<u64> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    const u64 period = std::clamp<u64>(sorted[sorted.size() / 10], 8'000'000, 18'180'000);
    p.period_us = static_cast<u32>(period / 1000);
    for (const u64 gap : gaps) {
      const u64 refreshes = (gap + period / 2) / period;
      if (refreshes > 1) p.missed += static_cast<u32>(refreshes - 1);
    }
    p.gap_us = static_cast<u32>(sorted.back() / 1000);
    return p;
  }

private:
  bool active_ = false;
  Summary result_{};
  std::vector<u32> work_, latency_;
  std::array<std::vector<u64>, 2> flips_;
  u64 first_ns_ = 0, last_ns_ = 0, audio_dry_start_ = 0;
  u32 frames_ = 0, presents_ = 0, skipped_ = 0, hires_ = 0, dropped_ = 0, late_ = 0, locked_ = 0;
};

} // namespace ds::sdl
