// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS: helper threads for the display-only enhanced (2x-5x) compositor.
//
// Engine A publishes one job per native display line it has planned; helpers
// claim jobs in order and write disjoint rows of the 640x480 presentation
// image. Jobs never touch guest-visible state. The producer side joins (and
// helps) before the image is presented or its inputs change.
#pragma once

#include "core/host_cores.h"
#include "core/types.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ds::gpu {

class ComposePool {
public:
  using Job = void (*)(void* context, u32 job);
  ComposePool() = default;
  ~ComposePool() { stop(); }
  ComposePool(const ComposePool&) = delete;
  ComposePool& operator=(const ComposePool&) = delete;

  // DS_COMPOSE_THREADS (default 2, 0 = compose inline on engine A).
  static u32 configured() {
    static const u32 n = [] {
      const char* value = std::getenv("DS_COMPOSE_THREADS");
      const int wanted = value ? std::atoi(value) : 2;
      return static_cast<u32>(wanted < 0 ? 0 : wanted > 2 ? 2 : wanted);
    }();
    return n;
  }

  void start(Job job, void* context) {
    if (!threads_.empty() || !configured()) return;
    job_ = job;
    context_ = context;
    quit_.store(false, std::memory_order_relaxed);
    for (u32 i = 0; i < configured(); ++i) threads_.emplace_back([this, i] { loop(i); });
  }

  void stop() {
    if (threads_.empty()) return;
    join();
    quit_.store(true, std::memory_order_seq_cst);
    { std::lock_guard<std::mutex> lock(m_); }
    cv_.notify_all();
    for (auto& thread : threads_) thread.join();
    threads_.clear();
  }

  bool active() const { return !threads_.empty(); }

  // Single producer (engine A's line stepping is never concurrent with itself).
  // Parked helpers are woken in batches (`flush`, or a backlog of kBatch):
  // a futex wake per line sat on engine A's critical path, and composition
  // only has to finish by the line-0 join.
  static constexpr u64 kBatch = 16;
  void publish(u32 job, bool flush = false) {
    const u64 next = published_.load(std::memory_order_relaxed);
    if (next - finished_.load(std::memory_order_acquire) >= queue_.size()) join(); // never expected: < 192 per frame
    queue_[next % queue_.size()] = job;
    published_.store(next + 1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst); // Dekker pair with the park in loop()
    if (parked_.load(std::memory_order_seq_cst) &&
        (flush || next + 1 - claimed_.load(std::memory_order_relaxed) >= kBatch)) {
      std::lock_guard<std::mutex> lock(m_);
      cv_.notify_all();
    }
  }

  // Claim and run one published job; false when none is waiting.
  bool help() {
    u64 claimed = claimed_.load(std::memory_order_relaxed);
    while (claimed < published_.load(std::memory_order_acquire)) {
      if (claimed_.compare_exchange_weak(claimed, claimed + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        job_(context_, queue_[claimed % queue_.size()]);
        finished_.fetch_add(1, std::memory_order_release);
        return true;
      }
    }
    return false;
  }

  // Producer side: every published job has run when this returns.
  void join() {
    while (finished_.load(std::memory_order_acquire) < published_.load(std::memory_order_acquire))
      if (!help()) relax();
  }

private:
  static void relax() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }
  bool pending() const { return claimed_.load(std::memory_order_seq_cst) < published_.load(std::memory_order_seq_cst); }

  void loop(u32 index) {
    name_current_thread(index ? "2d-compose-2" : "2d-compose-1");
    // Beside engine B (core 2) and on the aux core. Normal priority: engine
    // B's real-time worker and the audio thread always win over a helper.
    place_current_thread(index ? ThreadRole::Aux : ThreadRole::Line);
#if defined(__linux__)
    sched_param normal{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
#endif
    for (;;) {
      if (help()) continue;
      // Engine A plans a line every few tens of microseconds while it draws.
      for (int spin = 0; spin < 3000 && !pending() && !quit_.load(std::memory_order_relaxed); ++spin) relax();
      if (pending()) continue;
      std::unique_lock<std::mutex> lock(m_);
      parked_.fetch_add(1, std::memory_order_seq_cst);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      cv_.wait(lock, [this] { return quit_.load(std::memory_order_relaxed) || pending(); });
      parked_.fetch_sub(1, std::memory_order_seq_cst);
      if (quit_.load(std::memory_order_relaxed)) return;
    }
  }

  Job job_ = nullptr;
  void* context_ = nullptr;
  std::array<u32, 256> queue_{};
  std::atomic<u64> published_{0}, claimed_{0}, finished_{0};
  std::atomic<u32> parked_{0};
  std::atomic<bool> quit_{false};
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<std::thread> threads_;
};

} // namespace ds::gpu
