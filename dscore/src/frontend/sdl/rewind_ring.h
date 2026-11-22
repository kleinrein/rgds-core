// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS rewind (docs/TIME_TRAVEL_PLAN.md in RG DS Core): the game's recent past as a tree of moments. Every few
// seconds of play the whole machine is taken (on the emulation thread, where run_frame returns) and deflated on a
// worker, with a small picture of both screens, the top over the bottom, as a DS stands. A jump back makes the next
// moment a child of the one jumped to, so the future that was left stays as a branch. Past the memory budget the
// oldest moments go and their children hang from their parent. RG DS's game menu draws the screen
// (rgds_rewind_* in rgds_frontend_ui.hpp); this only keeps the moments.
#pragma once
#include "core/cart/miniz/miniz_tdef.h"
#include "core/cart/miniz/miniz_tinfl.h"
#include "core/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ds::sdl {

class RewindRing {
public:
  static constexpr int thumb_w = 104, thumb_h = 156; // 256x384 (both screens) at 0.40625
  using Save = std::function<bool(std::vector<u8>&)>;
  using Load = std::function<bool(const std::vector<u8>&)>;
  struct Info {
    u32 id = 0, parent = 0;
    u64 position = 0;
    const u32* thumb = nullptr; // thumb_w x thumb_h, bytes R, G, B, X
  };

  RewindRing() { worker_ = std::thread([this] { work(); }); }
  ~RewindRing() {
    {
      std::lock_guard<std::mutex> guard(lock_);
      stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }
  RewindRing(const RewindRing&) = delete;
  RewindRing& operator=(const RewindRing&) = delete;

  // `seconds` between moments; `budget` bytes of deflated states and pictures.
  void configure(double rate, double seconds, size_t budget) {
    rate_ = rate > 1 ? rate : 60;
    step_ = std::max<u64>(1, static_cast<u64>(std::llround(rate_ * seconds)));
    budget_ = budget;
    due_ = position_ + step_;
  }
  double rate() const { return rate_; }
  bool enabled() const { return step_ > 0 && budget_ > 0; }

  // After every emulated frame: a moment when one is due (skipped while the worker is still busy with the last).
  // `screen` the top and bottom screens as presented (ARGB8888, w x h each: native or a scaled picture).
  void after_frame(const Save& save, const u32* const screen[2], const int w[2], const int h[2]) {
    ++position_;
    if (!enabled() || position_ < due_) return;
    if (take(save, screen, w, h, false)) due_ = position_ + step_;
  }
  // A moment now, deflated before it returns (the menu's "now"); the head when it was just taken.
  u32 mark(const Save& save, const u32* const screen[2], const int w[2], const int h[2]) {
    if (!enabled()) return 0;
    settle();
    if (auto it = moments_.find(head_); it != moments_.end() && it->second->info.position == position_) return head_;
    const u32 id = take(save, screen, w, h, true);
    if (id) due_ = position_ + step_;
    return id;
  }
  bool jump(u32 id, const Load& load, std::string& err) {
    settle();
    std::vector<u8> raw;
    u64 position = 0;
    {
      std::lock_guard<std::mutex> guard(lock_);
      auto it = moments_.find(id);
      if (it == moments_.end()) {
        err = "that moment is no longer kept";
        return false;
      }
      const Stored& s = *it->second;
      if (s.packed.empty()) {
        err = "that moment could not be kept";
        return false;
      }
      raw.resize(s.raw);
      const size_t n = tinfl_decompress_mem_to_mem(raw.data(), raw.size(), s.packed.data(), s.packed.size(), 0);
      if (n != s.raw) {
        err = "that moment is damaged";
        return false;
      }
      position = s.info.position;
    }
    if (!load(raw)) {
      err = "the machine did not take that moment";
      return false;
    }
    head_ = id;
    position_ = position;
    due_ = position_ + step_;
    return true;
  }
  void protect(u32 id) { protected_ = id; }

  // The listing, oldest first (emulation thread; the worker only fills in deflated bytes).
  size_t count() const { return moments_.size(); }
  bool moment(size_t index, Info& out) const {
    if (index >= moments_.size()) return false;
    auto it = moments_.begin();
    std::advance(it, static_cast<long>(index));
    out = it->second->info;
    out.thumb = it->second->thumb.data();
    return true;
  }
  size_t bytes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return kept_;
  }
  void settle() {
    std::unique_lock<std::mutex> guard(lock_);
    done_.wait(guard, [this] { return job_ == 0; });
  }

private:
  struct Stored {
    Info info;
    std::vector<u32> thumb;
    std::vector<u8> packed;
    u32 raw = 0;
  };
  static void make_thumb(const u32* const screen[2], const int w[2], const int h[2], std::vector<u32>& out) {
    out.assign(static_cast<size_t>(thumb_w) * thumb_h, 0xFF000000u);
    if (!screen[0] || !screen[1] || w[0] <= 0 || h[0] <= 0 || w[1] <= 0 || h[1] <= 0) return;
    // Each pixel averages 2x2 samples of the native screens (ARGB8888), into bytes R, G, B, X.
    for (int y = 0; y < thumb_h; ++y) {
      for (int x = 0; x < thumb_w; ++x) {
        u32 sum[3] = {};
        for (int sy = 0; sy < 2; ++sy)
          for (int sx = 0; sx < 2; ++sx) {
            // Where in the two stacked screens, each 0..1 across and down.
            const double fy = (y + .25 + .5 * sy) * 2 / thumb_h, fx = (x + .25 + .5 * sx) / thumb_w;
            const int s = fy < 1 ? 0 : 1;
            const int px = std::min(w[s] - 1, static_cast<int>(fx * w[s]));
            const int py = std::min(h[s] - 1, static_cast<int>((fy - s) * h[s]));
            const u32 c = screen[s][static_cast<size_t>(py) * w[s] + px];
            sum[0] += (c >> 16) & 0xFF;
            sum[1] += (c >> 8) & 0xFF;
            sum[2] += c & 0xFF;
          }
        out[static_cast<size_t>(y) * thumb_w + x] =
            0xFF000000u | ((sum[2] + 2) / 4) << 16 | ((sum[1] + 2) / 4) << 8 | ((sum[0] + 2) / 4);
      }
    }
  }

  u32 take(const Save& save, const u32* const screen[2], const int w[2], const int h[2], bool wait) {
    {
      std::unique_lock<std::mutex> guard(lock_);
      if (job_) {
        if (!wait) return 0;
        done_.wait(guard, [this] { return job_ == 0; });
      }
    }
    std::vector<u8> raw;
    if (!save(raw) || raw.empty()) return 0;
    auto stored = std::make_unique<Stored>();
    stored->info.id = next_id_++;
    stored->info.parent = head_;
    stored->info.position = position_;
    make_thumb(screen, w, h, stored->thumb);
    const u32 id = stored->info.id;
    {
      std::lock_guard<std::mutex> guard(lock_);
      kept_ += stored->thumb.size() * 4;
      moments_[id] = std::move(stored);
      job_ = id;
      job_raw_ = std::move(raw);
    }
    wake_.notify_all();
    head_ = id;
    evict();
    if (wait) settle();
    return id;
  }

  void evict() {
    std::lock_guard<std::mutex> guard(lock_);
    while (kept_ > budget_ && moments_.size() > 2) {
      auto victim = moments_.end();
      for (auto it = moments_.begin(); it != moments_.end(); ++it)
        if (it->first != head_ && it->first != protected_ && it->first != job_) {
          victim = it; // the oldest: ids count up
          break;
        }
      if (victim == moments_.end()) return;
      const u32 gone = victim->first, parent = victim->second->info.parent;
      for (auto& [id, s] : moments_)
        if (s->info.parent == gone) s->info.parent = parent;
      kept_ -= victim->second->packed.size() + victim->second->thumb.size() * 4;
      moments_.erase(victim);
    }
  }

  void work() {
    std::unique_lock<std::mutex> guard(lock_);
    for (;;) {
      wake_.wait(guard, [this] { return stop_ || job_ != 0; });
      if (stop_) return;
      const u32 id = job_;
      std::vector<u8> raw;
      raw.swap(job_raw_);
      guard.unlock();
      size_t size = 0;
      void* packed = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &size, 1 /* probes: fastest */);
      guard.lock();
      if (auto it = moments_.find(id); it != moments_.end() && packed) {
        const u8* p = static_cast<const u8*>(packed);
        it->second->packed.assign(p, p + size);
        it->second->raw = static_cast<u32>(raw.size());
        kept_ += size;
      }
      std::free(packed); // miniz allocates with plain malloc
      job_ = 0;
      done_.notify_all();
    }
  }

  double rate_ = 60;
  u64 step_ = 0, position_ = 0, due_ = 0;
  size_t budget_ = 0, kept_ = 0;
  u32 next_id_ = 1, head_ = 0, protected_ = 0;
  std::map<u32, std::unique_ptr<Stored>> moments_;
  mutable std::mutex lock_;
  std::condition_variable wake_, done_;
  std::thread worker_;
  bool stop_ = false;
  u32 job_ = 0;
  std::vector<u8> job_raw_;
};

} // namespace ds::sdl
