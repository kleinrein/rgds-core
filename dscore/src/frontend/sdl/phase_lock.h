// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RG DS: phase lock of the frame pacer (pacer.h) to a panel's page flips
// (kms_flips.h). The emulator presents with vsync off and the panel
// refreshes at its own rate (59.2 to 60.3 Hz on the RG DS, and it wanders),
// so under a free-running pacer each finished frame waited anywhere from 0 to
// a whole refresh for its flip, and more often than not a whole one: a frame
// handed to the present while the previous flip was still pending waits for
// that flip, swaps just after its tick, and is shown a refresh later.
//
// Locked, the pacer runs one frame per refresh and each frame starts at
//
//     deadline = tick - work - band
//
// where `tick` is the panel's vblank the frame is meant for (the flips put
// the ticks on a grid: every flip completes on one, so the grid's phase and
// period are read straight off them), `work` is the learned time from a
// frame's start to its swap (emulation, the hand-off and the present
// thread's upload and draw) and `band` the margin left for the GPU and for
// jitter. Nothing is steered from the swap-to-flip latency: that sample
// arrives a frame or two late and the loop it closed overshot. The target
// tick advances by one per frame, snapped to the grid the latest flip
// places; a frame that cannot make its tick starts at once, and when its
// flip is then seen a tick late the target moves on past the frames that
// were held behind it, so one long frame costs one repeated frame and not a
// run of them. With two panels the earlier tick sets the swap, so neither
// panel is pushed over its own.
//
// Everything here is arithmetic on timestamps; the clock, SDL and the DRM fd
// stay outside, which is what makes it testable (tests/phase_lock_test.cpp).
#pragma once
#include "core/types.h"

#include <cmath>

namespace ds::sdl {

class PhaseLock {
public:
  static constexpr int PANELS = 2;
  static constexpr int RING = 8;

  struct Params {
    double band_ns = 2.5e6;        // margin between the swap and its tick, to begin with
    double band_min_ns = 1.5e6;    // the band never decays below this
    double band_max_ns = 8.0e6;    // nor grows above it
    double band_bump_ns = 0.5e6;   // what a missed tick adds
    double band_decay = 1.0 - 1.0 / 4096.0;   // per flip; halves in ~47 s at 60 Hz
    double period_alpha = 1.0 / 32.0;         // period EMA weight
    double period_tol = 0.02;      // a gap this close to the EMA counts as consistent
    int settle_flips = 8;          // consistent gaps before the lock is trusted
    double work_alpha = 1.0 / 16.0;           // start-to-swap EMA weight (mean and deviation)
    double work_spread = 2.0;      // deviations above the mean the deadline allows for
    double work_cap_ns = 4.0e6;    // ... but never more than this
  };

  PhaseLock() : PhaseLock(Params()) {}
  explicit PhaseLock(const Params& p) : p_(p), band_(p.band_ns) {}

  // One completed flip of `panel` (0 sets the period): completion and swap
  // times on the same clock, ns. issue_ns 0: the swap time is unknown.
  // wait_ns: how long the present held that swap for the panel's previous
  // flip; the frame itself was ready that much earlier.
  void flip(int panel, u64 at_ns, u64 issue_ns, u64 wait_ns = 0) {
    if (panel < 0 || panel >= PANELS) return;
    Panel& s = panels_[panel];
    if (panel == 0 && s.last_at && at_ns > s.last_at) learn_period(at_ns - s.last_at);
    s.last_at = at_ns;
    if (!issue_ns || at_ns <= issue_ns || period_ema_ <= 0.0) return;
    // The frame that swapped it: of the starts before the frame was ready to
    // swap (the swap itself may have been held past the next frame's start),
    // the one whose start-to-ready is nearest the work learned so far. The
    // latest start alone would be wrong for exactly the frames that matter,
    // the long ones: the next frame has begun before they are ready.
    const u64 ready = wait_ns < issue_ns ? issue_ns - wait_ns : issue_ns;
    const Start* e = nullptr;
    for (const Start& c : ring_) {
      if (!c.start || c.start >= ready) continue;
      if (!e) { e = &c; continue; }
      if (work_ema_ > 0.0) {
        if (std::fabs(static_cast<double>(ready - c.start) - work_ema_) < std::fabs(static_cast<double>(ready - e->start) - work_ema_)) e = &c;
      } else if (c.start > e->start) e = &c;
    }
    if (!e) return;
    const double period = period_ns();
    ++samples_;
    last_lat_ = static_cast<double>(at_ns - issue_ns);
    // The work, from every frame whose swap was not held (free-running
    // frames too: it is known by the time the lock engages, and when
    // emulation falls behind every frame starts late and the work must still
    // follow). A held swap kept its present lane busy, the next frame's swap
    // queued behind it, and that queueing would read as work: frames would
    // then start earlier, be held more, and the estimate run away.
    if (wait_ns < 100'000) {
      const double w = static_cast<double>(ready - e->start);
      if (work_ema_ <= 0.0) { work_ema_ = w; work_dev_ = 0.0; }
      else {
        work_ema_ += (w - work_ema_) * p_.work_alpha;
        work_dev_ += (std::fabs(w - work_ema_) - work_dev_) * p_.work_alpha;
      }
    }
    if (!e->target || !locked()) return;   // a free-running frame: no tick was aimed at
    // Its tick on this panel, and whether the flip was taken there or after.
    const double expected = tick_after(panel, static_cast<double>(e->target) - period * 0.5);
    const double at = static_cast<double>(at_ns);
    if (at > expected + period * 0.5) {
      // A frame that began late ran out of time, one that began while the
      // lock was engaging or whose swap was held for the flip before it was
      // the victim of the miss before: none of them says anything about the
      // band.
      if (!e->late && !e->engaging && wait_ns < 500'000) {
        ++late_;
        band_ = band_ + p_.band_bump_ns < p_.band_max_ns ? band_ + p_.band_bump_ns : p_.band_max_ns;
      }
      // Where the frames started since will land, given this flip: one that
      // is ready before the flip before it is held and takes the tick after
      // that flip, one that is not takes the first tick after it is ready.
      // The next frame aims past the last of them, or every frame from here
      // on would be held the same way.
      double prev = at;
      for (;;) {
        const Start* next = nullptr;
        for (const Start& c : ring_) if (c.start > e->start && (!next || c.start < next->start) && static_cast<double>(c.start) > prev - 2.0 * period) next = &c;
        if (!next) break;
        const double ready = static_cast<double>(next->start) + work_ema_;
        prev = tick_after(panel, (ready > prev ? ready : prev) + 1.0);
        e = next;
      }
      // Back onto panel 0's grid, where the target lives.
      const double rebased = tick_after(0, prev - period * 0.5);
      if (rebased > target_) target_ = rebased;
    } else {
      band_ *= p_.band_decay;
      if (band_ < p_.band_min_ns) band_ = p_.band_min_ns;
    }
  }

  // The next frame's deadline, ns on the flips' clock: the earliest panel's
  // tick for it, less the learned work and the band. 0 when not locked.
  // `now_ns`: the time the call is made (the current frame has been handed
  // to the present).
  u64 deadline(u64 now_ns) {
    if (!locked()) return 0;
    const double period = period_ns();
    const double now = static_cast<double>(now_ns);
    const double lead = work_ns() + band_;
    if (target_ <= 0.0) {
      target_ = tick_after(0, now + lead);
      // The frame in flight takes the first tick after it is ready: aim past
      // it. It may itself be held for the flip before it and take the one
      // after; the first frames are then held too, until the first flip
      // seen moves the target on (engaging_: no band is blamed for that).
      const Start* last = nullptr;
      for (const Start& c : ring_) if (c.start && (!last || c.start > last->start)) last = &c;
      if (last && work_ema_ > 0.0) {
        const double t = tick_after(0, static_cast<double>(last->start) + work_ema_) + period;
        if (t > target_) target_ = t;
      }
      engaging_ = 3;
    } else {
      // One tick on, then back onto the grid as the latest flip places it:
      // the period estimate lags a wandering panel, and a target stepped by
      // it alone would walk off the ticks.
      target_ = tick_after(0, target_ + period * 0.5);
    }
    // A frame starting now is ready at now + work and takes the first tick
    // after that: when emulation cannot keep one frame per refresh, the tick
    // on from the last target is already gone, and aiming at it would count
    // a miss the band cannot cure. The target is that first tick instead, the
    // deadline is then in the past and the frame starts at once: every tick
    // the machine can fill is filled, and none is idled past.
    const double feasible = tick_after(0, now + work_ema_);
    if (feasible > target_) target_ = feasible;
    // Emulation that takes a refresh or more per frame (the frames then start
    // back to back, and their spacing is that time) can fill at best every
    // tick it is ready for: no frame waits for its deadline then, since every
    // moment idled is a tick lost later. (The free-running pacer would not
    // wait either; the lock keeps the period and the audio rate.)
    if (emu_ema_ >= period * 0.98) return now_ns;
    const double d = swap_for(target_) - work_ns();
    return static_cast<u64>(d > 0.0 ? d : 0.0);
  }

  // A frame began at `start_ns` (the wait ended), aiming at the target the
  // last deadline() set (none while free-running: the work is still learned
  // from it); `late`: it began past its deadline.
  void started(u64 start_ns, bool late = false) {
    // Back-to-back starts measure the emulation time itself.
    if (late && last_start_ && start_ns > last_start_) {
      const double gap = static_cast<double>(start_ns - last_start_);
      emu_ema_ = emu_ema_ <= 0.0 ? gap : emu_ema_ + (gap - emu_ema_) * p_.work_alpha;
    }
    last_start_ = start_ns;
    Start& e = ring_[ring_next_];
    ring_next_ = (ring_next_ + 1) % RING;
    e.start = start_ns;
    e.target = static_cast<u64>(target_ > 0.0 ? target_ : 0.0);
    e.late = late;
    e.engaging = engaging_ > 0;
    if (engaging_ > 0) --engaging_;
  }

  // The panel's refresh period is known and has held still: pace at it.
  bool locked() const { return consistent_ >= p_.settle_flips && period_ema_ > 0.0; }
  double period_ns() const { return period_ema_; }
  double band_ns() const { return band_; }
  // Start to swap, as the deadline uses it: the mean plus work_spread deviations, capped.
  double work_ns() const {
    const double extra = work_dev_ * p_.work_spread;
    return work_ema_ + (extra < p_.work_cap_ns ? extra : p_.work_cap_ns);
  }
  double work_mean_ns() const { return work_ema_; }
  double last_lat_ns() const { return last_lat_; }   // the last sample's swap-to-flip time
  double target_ns() const { return target_; }       // the tick the frame now running aims at (diagnostics)
  u64 late() const { return late_; }       // flips taken a tick after the one their frame aimed at (frames that started on time)
  u64 samples() const { return samples_; }

  // After a pause, a speed change or a lost lock: the period, the work and
  // the band are kept (they are the panel's and the machine's); the target
  // and the frames in flight are not.
  void reset() {
    target_ = 0.0;
    for (Start& e : ring_) e = Start{};
    ring_next_ = 0;
    engaging_ = 0;
    last_start_ = 0;
  }

private:
  struct Panel { u64 last_at = 0; };
  struct Start { u64 start = 0, target = 0; bool late = false, engaging = false; };

  // The first tick of `panel` at or after x (its last flip, stepped by the period).
  double tick_after(int panel, double x) const {
    const double period = period_ns();
    const Panel& s = panels_[panel].last_at ? panels_[panel] : panels_[0];
    const double ref = static_cast<double>(s.last_at);
    const double n = std::ceil((x - ref) / period);
    return ref + n * period;
  }

  // Where the swap for target tick t (on panel 0's grid) should land: the
  // band before the earliest panel's tick for that frame.
  double swap_for(double t) const {
    const double period = period_ns();
    double earliest = t;
    for (int i = 1; i < PANELS; ++i) {
      if (!panels_[i].last_at) continue;
      const double ti = tick_after(i, t - period * 0.5);
      if (ti < earliest) earliest = ti;
    }
    return earliest - band_;
  }

  // The panel's period from the gaps between its flips. A gap can span two
  // or more refreshes (nothing was presented, or a frame landed late): a
  // whole multiple of the period is still the panel's clock and keeps the
  // run, only single gaps go into the average, and a gap that is no multiple
  // (a mode change, a stall in the event read) starts the run over.
  void learn_period(u64 gap) {
    const double g = static_cast<double>(gap);
    if (g < 4.0e6) return;   // not two vblanks
    if (period_ema_ <= 0.0 || g < period_ema_ * 0.75) { period_ema_ = g; consistent_ = 1; return; }   // first gap, or the first was a multiple
    const double k = std::round(g / period_ema_);
    const double d = std::fabs(g - k * period_ema_);
    if (d > period_ema_ * p_.period_tol * k) { consistent_ = 0; return; }
    ++consistent_;
    if (k == 1.0) period_ema_ += (g - period_ema_) * p_.period_alpha;
  }

  Params p_;
  Panel panels_[PANELS];
  Start ring_[RING];
  int ring_next_ = 0;
  double period_ema_ = 0.0;
  int consistent_ = 0;
  double band_;
  double work_ema_ = 0.0, work_dev_ = 0.0;
  double emu_ema_ = 0.0;     // emulation time per frame, from back-to-back starts
  u64 last_start_ = 0;
  double target_ = 0.0;      // the tick (panel 0's grid) the frame now starting aims at
  int engaging_ = 0;         // frames still to start before the first target can be trusted
  double last_lat_ = 0.0;
  u64 late_ = 0, samples_ = 0;
};

} // namespace ds::sdl
