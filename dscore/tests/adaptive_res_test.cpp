// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS adaptive resolution: when the 3D falls back to the native raster and returns.
#include "check.h"
#include "frontend/sdl/adaptive_res.h"

using ds::sdl::AdaptiveRes;
using Event = AdaptiveRes::Event;

namespace {
constexpr double kBudget = 16.7, kFrame = 1000.0 / 60;

// Runs `frames` frames from `now`, `over` of every 30 over budget (the first
// ones in each window), each slow frame `gpu_wait` ms of it waiting on the
// GPU; returns the last event other than None.
Event run(AdaptiveRes& a, double& now, int frames, unsigned over, double fast_ms = 12, double slow_ms = 22,
          double gpu_wait = 12) {
  Event last = Event::None;
  for (int i = 0; i < frames; ++i, now += kFrame) {
    const bool slow = unsigned(i % 30) < over;
    const Event e = a.frame(now, slow ? slow_ms : fast_ms, kBudget, slow ? gpu_wait : 0);
    if (e != Event::None) last = e;
  }
  return last;
}
}

int main() {
  { // Light frames, or 5 slow ones in 30, never leave high resolution.
    AdaptiveRes a;
    double now = 0;
    CHECK(run(a, now, 3600, 0) == Event::None);
    CHECK(run(a, now, 3600, 5) == Event::None && !a.holding());
  }
  { // Overruns the GPU did not cause (no GPU wait, or too little) never leave high resolution.
    AdaptiveRes a;
    double now = 0;
    CHECK(run(a, now, 3600, 30, 12, 25, 0) == Event::None && !a.holding());
    CHECK(run(a, now, 3600, 6, 12, 25, 9) == Event::None && !a.holding()); // 6 x 9 ms < 2 ms x 30 frames
  }
  { // 6 slow frames in a window start a 2 s hold, which ends on time.
    AdaptiveRes a;
    double now = 0;
    CHECK(run(a, now, 30, 6) == Event::Hold && a.holding() && a.hold_ms() == 2000 && a.last_over() == 6);
    CHECK(run(a, now, 110, 0, 9) == Event::None && a.holding());
    CHECK(run(a, now, 15, 0, 9) == Event::Release && !a.holding());
    CHECK(a.hold_avg() < a.hires_avg());
  }
  { // Relapsing within 3 s doubles the hold each time, up to 32 s.
    AdaptiveRes a;
    double now = 0;
    double expected = 2000;
    for (int round = 0; round < 7; ++round) {
      CHECK(run(a, now, 30, 6) == Event::Hold && a.hold_ms() == expected);
      while (a.holding()) run(a, now, 1, 0, 9);
      expected = expected * 2 > 32000 ? 32000 : expected * 2;
    }
    CHECK(a.hold_ms() == 32000);
    // A clean minute brings the first hold back to 2 s.
    run(a, now, 60 * 62, 0);
    CHECK(run(a, now, 30, 6) == Event::Hold && a.hold_ms() == 2000);
  }
  { // A hold that is no faster (CPU-bound) keeps high resolution for a minute.
    AdaptiveRes a;
    double now = 0;
    CHECK(run(a, now, 30, 30, 22, 22) == Event::Hold);
    Event end = Event::None;
    while (a.holding()) end = run(a, now, 1, 30, 22, 22);
    CHECK(end == Event::ReleaseNoGain);
    CHECK(run(a, now, 60 * 55, 30, 22, 22) == Event::None && !a.holding());
    CHECK(run(a, now, 60 * 6, 30, 22, 22) == Event::Hold && a.holding()); // the minute is over: it may try again
    end = Event::None;
    while (a.holding()) end = run(a, now, 1, 30, 22, 22);
    CHECK(end == Event::ReleaseNoGain); // no gain twice: five minutes now
    CHECK(run(a, now, 60 * 290, 30, 22, 22) == Event::None && !a.holding());
    CHECK(run(a, now, 60 * 11, 30, 22, 22) == Event::Hold && a.holding());
  }
  { // Right after a return, 3 slow frames in the next 10 relapse at once (a short dip, not a 30-frame one).
    AdaptiveRes a;
    double now = 0;
    CHECK(run(a, now, 30, 6) == Event::Hold);
    Event end = Event::None;
    while (a.holding()) end = run(a, now, 1, 0, 9);
    CHECK(end == Event::Release);
    CHECK(run(a, now, 9, 3) == Event::None && !a.holding());
    CHECK(run(a, now, 1, 0) == Event::Hold && a.hold_ms() == 4000);
    // Away from a return, the full window applies again: 3 slow frames in 30 are not enough.
    while (a.holding()) run(a, now, 1, 0, 9);
    run(a, now, 10, 0);
    CHECK(run(a, now, 60 * 30, 3) == Event::None);
  }
  { // Pausing (fast forward, native scale) ends a hold at once.
    AdaptiveRes a;
    double now = 0;
    run(a, now, 30, 6);
    CHECK(a.holding() && a.pause() && !a.holding() && !a.pause());
  }
  return 0;
}
