// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The emulation thread under RLIMIT_RTTIME. PipeWire's client library sets
// the limit to 200 ms in every process it joins, and the kernel kills a
// real-time thread that runs that long without sleeping. Only a sleep taken
// *as* a real-time thread resets the count; the emulation thread's waits are
// taken as SCHED_OTHER (the present's vsync wait, the pacer's spin), so on
// its own it was killed within a second. Paced, it naps briefly once a
// frame as a real-time thread (nap()). Unpaced (fast forward, or the
// limiter off) it never waits at all, so it is an ordinary task until
// pacing resumes (set_unpaced()). One-shot work that can outlast the limit
// (inflating a zipped ROM, loading or saving a state) runs under an
// OffRealtime guard instead. Linux only; elsewhere all of it does nothing.
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#endif

namespace ds::sdl {

class RtBudget {
public:
  // Unpaced: drop the calling thread's real-time policy until pacing
  // resumes, then restore it. Logs through `log` when it drops (verbose).
  void set_unpaced(bool on, bool log) {
    if (on == unpaced_) return;
    unpaced_ = on;
#if defined(__linux__)
    if (on) {
      if (pthread_getschedparam(pthread_self(), &policy_, &param_) != 0 ||
          (policy_ != SCHED_RR && policy_ != SCHED_FIFO)) {
        policy_ = SCHED_OTHER;
        return;
      }
      sched_param z{};
      if (const int e = pthread_setschedparam(pthread_self(), SCHED_OTHER, &z)) {
        std::fprintf(stderr, "unpaced: real-time policy not dropped: %s\n", std::strerror(e));
        policy_ = SCHED_OTHER;
        return;
      }
      if (log) std::fprintf(stderr, "unpaced: emulation thread SCHED_OTHER until pacing resumes\n");
    } else if (policy_ != SCHED_OTHER) {
      pthread_setschedparam(pthread_self(), policy_, &param_);
      policy_ = SCHED_OTHER;
    }
#else
    (void)log;
#endif
  }

  // Once a paced frame: a sleep long enough that the thread really leaves
  // the CPU (a timer that has expired before it schedules out is no sleep),
  // when it is real-time under a finite RLIMIT_RTTIME.
  void nap() const {
#if defined(__linux__)
    if (unpaced_) return;
    int pol = 0;
    sched_param sp{};
    if (pthread_getschedparam(pthread_self(), &pol, &sp) != 0 || (pol != SCHED_RR && pol != SCHED_FIFO)) return;
    rlimit rl{};
    if (getrlimit(RLIMIT_RTTIME, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return;
    timespec ts{0, 20000};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {}
#endif
  }

private:
  bool unpaced_ = false;
#if defined(__linux__)
  int policy_ = SCHED_OTHER; // the policy dropped while unpaced
  sched_param param_{};
#endif
};

// For as long as it lives, the calling thread is an ordinary task: for
// one-shot work that may run longer than RLIMIT_RTTIME allows without a
// sleep (a big zipped ROM inflates for longer than 200 ms on a Cortex-A55).
// Threads started meanwhile inherit SCHED_OTHER too. On the way out the real-time
// policy comes back and the thread naps once as a real-time thread, so the
// count starts from nothing. Nests: an inner guard finds nothing to drop.
class OffRealtime {
public:
  OffRealtime() {
#if defined(__linux__)
    if (pthread_getschedparam(pthread_self(), &policy_, &param_) != 0 ||
        (policy_ != SCHED_RR && policy_ != SCHED_FIFO)) {
      policy_ = SCHED_OTHER;
      return;
    }
    sched_param z{};
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &z) != 0) policy_ = SCHED_OTHER;
#endif
  }
  ~OffRealtime() {
#if defined(__linux__)
    if (policy_ == SCHED_OTHER) return;
    if (pthread_setschedparam(pthread_self(), policy_, &param_) != 0) return;
    timespec ts{0, 20000};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {}
#endif
  }
  OffRealtime(const OffRealtime&) = delete;
  OffRealtime& operator=(const OffRealtime&) = delete;

private:
#if defined(__linux__)
  int policy_ = SCHED_OTHER;
  sched_param param_{};
#endif
};

} // namespace ds::sdl
