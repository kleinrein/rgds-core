// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS rewind (docs/TIME_TRAVEL_PLAN.md in RG DS Core): the game's recent past as a tree of moments. Every few
// seconds of play the whole machine is taken (on the emulation thread, where run_frame returns) and kept as it is,
// with a small picture of both screens, the top over the bottom, as a DS stands. A jump back makes the next moment a
// child of the one jumped to, so the future that was left stays as a branch. Past the memory budget the oldest
// moments go and their children hang from their parent. RG DS's game menu draws the screen (rgds_rewind_* in
// rgds_frontend_ui.hpp); this only keeps the moments.
//
// Nothing is compressed. Moments used to be deflated on a worker thread, which was born real-time (main.cpp turns the
// whole process SCHED_RR before any thread exists) and spent ~0.4 s of a core on every ~6 MB state, taking turns with
// the present and audio threads. Mario Kart 2x on the handheld (23 Nov 2026), 45 s from the same resume point: rewind
// on ~45 fps shown, ~510 of 2500 frames not presented, ~100 underruns; DS_REWIND=0 58.5 fps, 8 and 0. A raw moment is
// one copy on the emulation thread and no work after it; the cost is memory (about 2.2x what deflate kept).
#pragma once
#include "core/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

class RewindRing {
public:
  // Both screens at their own size, the top over the bottom: the menu shows this picture itself, both as the small
  // one in its strip and as the moment on the top screen, so a look never loads or runs anything.
  static constexpr int thumb_w = 256, thumb_h = 384;
  using Save = std::function<bool(std::vector<u8>&)>;
  using Load = std::function<bool(const std::vector<u8>&)>;
  struct Info {
    u32 id = 0, parent = 0;
    u64 position = 0;
    const u32* thumb = nullptr; // thumb_w x thumb_h, bytes R, G, B, X
  };

  RewindRing() = default;
  ~RewindRing() {
    if (taken_)
      std::fprintf(stderr, "rewind: %llu moments taken, %.2f ms mean, %.2f ms max on the emulation thread\n",
                   static_cast<unsigned long long>(taken_), take_ms_ / static_cast<double>(taken_), take_max_ms_);
  }
  RewindRing(const RewindRing&) = delete;
  RewindRing& operator=(const RewindRing&) = delete;

  // `seconds` between moments; `budget` bytes of states and pictures.
  void configure(double rate, double seconds, size_t budget) {
    rate_ = rate > 1 ? rate : 60;
    step_ = std::max<u64>(1, static_cast<u64>(std::llround(rate_ * seconds)));
    budget_ = budget;
    due_ = position_ + step_;
  }
  double rate() const { return rate_; }
  bool enabled() const { return step_ > 0 && budget_ > 0; }

  // After every emulated frame: a moment when one is due.
  // `screen` the top and bottom screens as presented (ARGB8888, w x h each: native or a scaled picture).
  void after_frame(const Save& save, const u32* const screen[2], const int w[2], const int h[2]) {
    ++position_;
    if (!enabled() || position_ < due_) return;
    // A refused save (not at a frame boundary) tries again next frame.
    if (take(save, screen, w, h)) due_ = position_ + step_;
  }
  // A moment now (the menu's "now"); the head when it was just taken.
  u32 mark(const Save& save, const u32* const screen[2], const int w[2], const int h[2]) {
    if (!enabled()) return 0;
    if (auto it = moments_.find(head_); it != moments_.end() && it->second->info.position == position_) return head_;
    const u32 id = take(save, screen, w, h);
    if (id) due_ = position_ + step_;
    return id;
  }
  bool jump(u32 id, const Load& load, std::string& err) {
    auto it = moments_.find(id);
    if (it == moments_.end()) {
      err = "that moment is no longer kept";
      return false;
    }
    const Stored& s = *it->second;
    if (s.state.empty()) {
      err = "that moment could not be kept";
      return false;
    }
    if (!load(s.state)) {
      err = "the machine did not take that moment";
      return false;
    }
    head_ = id;
    position_ = s.info.position;
    due_ = position_ + step_;
    return true;
  }
  void protect(u32 id) { protected_ = id; }

  // The listing, oldest first (emulation thread only).
  size_t count() const { return moments_.size(); }
  bool moment(size_t index, Info& out) const {
    if (index >= moments_.size()) return false;
    auto it = moments_.begin();
    std::advance(it, static_cast<long>(index));
    out = it->second->info;
    out.thumb = it->second->thumb.data();
    return true;
  }
  size_t bytes() const { return kept_; }
  void settle() {} // nothing runs in the background any more

private:
  struct Stored {
    Info info;
    std::vector<u32> thumb;
    std::vector<u8> state;
  };
  // What a moment holds in memory (capacity, not size: the budget is real RAM).
  static size_t size_of(const Stored& s) { return s.state.capacity() + s.thumb.capacity() * 4; }

  static void make_thumb(const u32* const screen[2], const int w[2], const int h[2], std::vector<u32>& out) {
    out.assign(static_cast<size_t>(thumb_w) * thumb_h, 0xFF000000u);
    if (!screen[0] || !screen[1] || w[0] <= 0 || h[0] <= 0 || w[1] <= 0 || h[1] <= 0) return;
    // One sample a pixel (ARGB8888 into bytes R, G, B, X): a plain copy for a native screen, and cheap for a scaled
    // one, since this runs on the emulation thread between two frames.
    const int half = thumb_h / 2;
    for (int y = 0; y < thumb_h; ++y) {
      const int s = y < half ? 0 : 1;
      const u32* row = screen[s] + static_cast<size_t>((y % half) * h[s] / half) * w[s];
      u32* dst = out.data() + static_cast<size_t>(y) * thumb_w;
      for (int x = 0; x < thumb_w; ++x) {
        const u32 c = row[x * w[s] / thumb_w];
        dst[x] = 0xFF000000u | (c & 0xFFu) << 16 | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
      }
    }
  }

  u32 take(const Save& save, const u32* const screen[2], const int w[2], const int h[2]) {
    const auto t0 = std::chrono::steady_clock::now();
    auto stored = std::make_unique<Stored>();
    stored->state.swap(spare_); // the last evicted moment's buffer, written into again
    if (!save(stored->state) || stored->state.empty()) {
      spare_.swap(stored->state);
      return 0;
    }
    stored->info.id = next_id_++;
    stored->info.parent = head_;
    stored->info.position = position_;
    make_thumb(screen, w, h, stored->thumb);
    const u32 id = stored->info.id;
    kept_ += size_of(*stored);
    moments_[id] = std::move(stored);
    head_ = id;
    evict();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++taken_;
    take_ms_ += ms;
    take_max_ms_ = std::max(take_max_ms_, ms);
    return id;
  }

  void evict() {
    while (kept_ > budget_ && moments_.size() > 2) {
      auto victim = moments_.end();
      for (auto it = moments_.begin(); it != moments_.end(); ++it)
        if (it->first != head_ && it->first != protected_) {
          victim = it; // the oldest: ids count up
          break;
        }
      if (victim == moments_.end()) return;
      const u32 gone = victim->first, parent = victim->second->info.parent;
      for (auto& [id, s] : moments_)
        if (s->info.parent == gone) s->info.parent = parent;
      kept_ -= size_of(*victim->second);
      if (victim->second->state.capacity() > spare_.capacity()) spare_.swap(victim->second->state);
      moments_.erase(victim);
    }
  }

  double rate_ = 60;
  u64 step_ = 0, position_ = 0, due_ = 0;
  size_t budget_ = 0, kept_ = 0;
  u32 next_id_ = 1, head_ = 0, protected_ = 0;
  std::map<u32, std::unique_ptr<Stored>> moments_;
  std::vector<u8> spare_; // one evicted state's buffer, kept for the next take (outside the budget: one state)
  u64 taken_ = 0;
  double take_ms_ = 0, take_max_ms_ = 0;
};

} // namespace ds::sdl
