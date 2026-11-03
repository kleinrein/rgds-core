// SPDX-License-Identifier: GPL-3.0-or-later
// PhaseLock against a simulated panel and present pipeline: vblanks every
// `period` per panel, a frame that emulates for `work`, a present thread
// that uploads for `draw` and cannot swap before the panel's previous flip
// has completed (SDL KMSDRM's wait, kms_flips.h's before()), a swap that is
// taken by the first vblank after the GPU's share, and flips that are read
// by the emulation thread a frame late, as they are. Checks the arithmetic
// (period learning, work learning, the deadline on the tick grid, a long
// frame costing one repeat and not a run, two panels, a wandering period)
// without a clock or a display.
#include "check.h"
#include "frontend/sdl/phase_lock.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

using ds::u64;
using ds::sdl::PhaseLock;

namespace {

constexpr double MS = 1e6;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Panel {
  double period, phase;
  double last_flip = 0.0;
  double tick_after(double t) const { return phase + std::ceil((t - phase) / period) * period; }
};

// One simulated run. Flips complete on the panels' grids and are handed to
// the lock only once the emulation thread's next frame has started (the
// read at the end of a frame sees the flips completed by then).
struct Sim {
  PhaseLock& lock;
  std::vector<Panel> panels;
  double work = 10.0 * MS, draw = 5.0 * MS, gpu = 0.3 * MS;
  double free_period = 1e9 / 60.0; // the pacer's own clock before the lock holds
  double now = 100.0 * MS;         // the current frame's hand-off time
  // The direct KMS tier's mailbox (display_drm.cpp): a frame held for the
  // previous flip longer than `margin` is dropped at that flip's vblank and
  // has no flip at all (margin < 0: every held frame is flipped, as SDL's
  // swap does). `report_hold`: the tier tells the lock how long the swap
  // was held (KmsFlips::Flip::wait_ns); false is the record DrmOut kept
  // before 19 October (the issue time at the vblank, the hold unknown).
  double margin = -1.0;
  bool report_hold = true;
  struct Pending {
    int panel;
    double at, swap, wait;
    int frame = 0;
    bool dropped_behind = false;
  };
  std::deque<Pending> pending;
  // The last frame's results, per panel.
  std::vector<double> lat, wait, gap;
  int repeats = 0, late_starts = 0, dropped = 0;

  Sim(PhaseLock& l, std::vector<Panel> p)
      : lock(l), panels(std::move(p)), lat(panels.size()), wait(panels.size()), gap(panels.size()) {}

  void deliver(double upto) {
    while (!pending.empty() && pending.front().at <= upto) {
      const Pending& f = pending.front();
      lock.flip(f.panel, static_cast<u64>(f.at), static_cast<u64>(f.swap), static_cast<u64>(f.wait),
                report_hold && f.dropped_behind, f.frame);
      pending.pop_front();
    }
  }

  void frame() {
    deliver(now);
    double d = lock.locked() ? static_cast<double>(lock.deadline(static_cast<u64>(now))) : now + free_period - work;
    if (!lock.locked() && d < now) d = now;
    const bool late = lock.locked() && d <= now;
    const double start = d <= now ? now : d;
    if (late) ++late_starts;
    const int number = lock.started(static_cast<u64>(start), late);
    const double done = start + work;
    for (size_t i = 0; i < panels.size(); ++i) {
      Panel& p = panels[i];
      const double ready = done + draw;
      const double swap = ready > p.last_flip ? ready : p.last_flip; // held for the previous flip
      wait[i] = swap - ready;
      if (margin >= 0.0 && wait[i] > margin) {
        // The mailbox: dropped at the pending flip's vblank, no flip of its
        // own; that flip's record says so (it is still to be delivered: the
        // decision falls at its completion, after this frame's hand-off).
        ++dropped;
        for (auto it = pending.rbegin(); it != pending.rend(); ++it)
          if (it->panel == static_cast<int>(i)) {
            it->dropped_behind = true;
            break;
          }
        continue;
      }
      const double at = p.tick_after(swap + gpu);
      lat[i] = at - swap;
      gap[i] = p.last_flip ? at - p.last_flip : p.period;
      if (p.last_flip && gap[i] > 1.5 * p.period) ++repeats;
      p.last_flip = at;
      pending.push_back({static_cast<int>(i), at, swap, report_hold ? wait[i] : 0.0, number});
    }
    now = done;
  }
  void run(int n) {
    for (int i = 0; i < n; ++i) frame();
  }
};

} // namespace

int main() {
  const double panel_period = 1e9 / 59.7; // the RG DS panels, give or take their wander

  // 1. Period learning: after settle_flips consistent gaps the lock holds and
  //    the period is the panel's; a gap of two refreshes is still the panel's
  //    clock, a gap that is no multiple starts the run over.
  {
    PhaseLock lock;
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 7; ++i) {
      lock.flip(0, t, 0);
      t += static_cast<u64>(panel_period);
    }
    CHECK(!lock.locked()); // 6 gaps < 8
    for (int i = 0; i < 3; ++i) {
      lock.flip(0, t, 0);
      t += static_cast<u64>(panel_period);
    }
    CHECK(lock.locked());
    CHECK(near(lock.period_ns(), panel_period, 2'000.0));
    t += static_cast<u64>(panel_period); // one refresh with no flip
    lock.flip(0, t, 0);
    CHECK(lock.locked());
    CHECK(near(lock.period_ns(), panel_period, 2'000.0));
    t += static_cast<u64>(panel_period * 1.3);
    lock.flip(0, t, 0);
    CHECK(!lock.locked());
    for (int i = 0; i < 9; ++i) {
      t += static_cast<u64>(panel_period);
      lock.flip(0, t, 0);
    }
    CHECK(lock.locked());
    CHECK(lock.samples() == 0); // no swap times: nothing learned about the work
  }

  // 2. Convergence on one panel: from a free-running 60 Hz pacer the lock
  //    learns the start-to-swap time (work + draw) and puts every swap `band`
  //    before its tick; no frame waits for the previous flip and no tick is
  //    missed once settled.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 3.7 * MS}});
    sim.run(60);
    CHECK(lock.locked());
    std::fprintf(
        stderr,
        "phase lock: after 60 frames work mean %.3f ms (used %.3f), %llu samples, band %.2f, late %llu, repeats %d, lat %.3f wait %.3f\n",
        lock.work_mean_ns() / MS, lock.work_ns() / MS, static_cast<unsigned long long>(lock.samples()),
        lock.band_ns() / MS, static_cast<unsigned long long>(lock.late()), sim.repeats, sim.lat[0] / MS,
        sim.wait[0] / MS);
    CHECK(near(lock.work_mean_ns(), sim.work + sim.draw, 50'000.0));
    CHECK(lock.late() == 0); // the frames held while engaging are not misses
    CHECK(near(lock.band_ns(), PhaseLock::Params{}.band_ns, 0.1 * MS));
    const int repeats0 = sim.repeats;
    const u64 late0 = lock.late();
    double worst = 0.0, worst_wait = 0.0;
    for (int i = 0; i < 600; ++i) {
      sim.frame();
      worst = std::max(worst, std::fabs(sim.lat[0] - lock.band_ns()));
      worst_wait = std::max(worst_wait, sim.wait[0]);
    }
    std::fprintf(
        stderr,
        "phase lock: one panel, worst |lat - band| after settling %.3f ms, worst hold for the previous flip %.3f ms, band %.2f ms, work %.2f ms, period %.4f ms\n",
        worst / MS, worst_wait / MS, lock.band_ns() / MS, lock.work_ns() / MS, lock.period_ns() / MS);
    CHECK(worst < 0.1 * MS); // the spread allowance, learned from the engagement frames, decays towards zero
    CHECK(worst_wait == 0.0);
    CHECK(sim.repeats == repeats0);
    CHECK(lock.late() == late0);
    CHECK(near(lock.period_ns(), panel_period, 2'000.0));
  }

  // 3. Any starting phase converges the same way.
  for (double phase = 0.0; phase < panel_period; phase += panel_period / 7.0) {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, phase}});
    sim.run(200);
    const int repeats0 = sim.repeats;
    sim.run(100);
    CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
    CHECK(sim.repeats == repeats0);
  }

  // 4. One long frame: it misses its tick (one repeated frame), the band
  //    widens a step, and the frame after it makes its own tick: no run of
  //    late frames, no second repeat.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 0.0}});
    sim.run(300);
    const int repeats0 = sim.repeats;
    const u64 late0 = lock.late();
    const double band0 = lock.band_ns();
    const double work0 = sim.work;
    sim.work = work0 + 6.0 * MS; // over its slot
    sim.frame();
    sim.work = work0;
    CHECK(sim.repeats == repeats0 + 1);
    for (int i = 0; i < 8; ++i) {
      sim.frame();
      CHECK(sim.repeats == repeats0 + 1);
    } // the two frames behind it are held, none repeats
    CHECK(lock.late() == late0 + 1);
    CHECK(near(lock.band_ns(), band0 + PhaseLock::Params{}.band_bump_ns, 0.01 * MS));
    sim.run(100);
    CHECK(sim.repeats == repeats0 + 1);
    CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
  }

  // 5. Two panels, ticks 6 ms apart: the swap sits `band` before the earlier
  //    one, the other sees it 6 ms sooner than that, and neither is missed.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 0.0}, Panel{panel_period, 6.0 * MS}});
    sim.run(400);
    const int repeats0 = sim.repeats;
    const u64 late0 = lock.late();
    sim.run(100);
    const double lo = std::min(sim.lat[0], sim.lat[1]), hi = std::max(sim.lat[0], sim.lat[1]);
    std::fprintf(
        stderr,
        "phase lock: two panels, lat %.3f / %.3f ms, wait %.3f / %.3f, band %.2f, work %.2f (mean %.2f), late %llu, repeats %d\n",
        sim.lat[0] / MS, sim.lat[1] / MS, sim.wait[0] / MS, sim.wait[1] / MS, lock.band_ns() / MS, lock.work_ns() / MS,
        lock.work_mean_ns() / MS, static_cast<unsigned long long>(lock.late()), sim.repeats);
    CHECK(near(lo, lock.band_ns(), 0.1 * MS));
    CHECK(near(hi - lo, 6.0 * MS, 0.05 * MS));
    CHECK(sim.repeats == repeats0);
    CHECK(lock.late() == late0);
  }

  // 6. The panel's period wanders (as the RG DS's does, by 1.5 % over tens of
  //    seconds): the lock follows it within a few frames and few ticks are lost.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{1e9 / 60.3, 2.0 * MS}});
    sim.run(300);
    const int repeats0 = sim.repeats;
    Panel& p = sim.panels[0];
    p.phase = p.last_flip;
    p.period = 1e9 / 59.2; // the grid continues from the last flip at the new period
    sim.run(200);
    std::fprintf(stderr,
                 "phase lock: period wander, period %.4f ms, lat %.3f, band %.2f, work %.2f, repeats +%d, late %llu\n",
                 lock.period_ns() / MS, sim.lat[0] / MS, lock.band_ns() / MS, lock.work_ns() / MS,
                 sim.repeats - repeats0, static_cast<unsigned long long>(lock.late()));
    CHECK(near(lock.period_ns(), 1e9 / 59.2, 20'000.0));
    CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
    CHECK(sim.repeats - repeats0 <= 2);
  }

  // 7. Emulation that cannot keep up (work over the period): every frame
  //    starts late, the target skips ahead rather than falling further
  //    behind, deadlines stay sane and no missed tick is blamed on the band.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 0.0}});
    sim.run(100);
    sim.work = panel_period * 1.1;
    const double band0 = lock.band_ns();
    const u64 late0 = lock.late();
    for (int i = 0; i < 100; ++i) {
      sim.frame();
      CHECK(lock.locked());
    }
    std::fprintf(stderr,
                 "phase lock: cpu-bound, late starts %d, late %llu (was %llu), band %.2f (was %.2f), repeats %d\n",
                 sim.late_starts, static_cast<unsigned long long>(lock.late()), static_cast<unsigned long long>(late0),
                 lock.band_ns() / MS, band0 / MS, sim.repeats);
    CHECK(sim.late_starts >= 90);
    CHECK(lock.late() <= late0 + 1);
    CHECK(lock.band_ns() <= band0 + PhaseLock::Params{}.band_bump_ns);
    CHECK(sim.repeats <= 14); // 100 frames of 1.1 refresh each need 110 ticks: about 10 repeats, no more than that
    const u64 d = lock.deadline(static_cast<u64>(sim.now));
    CHECK(static_cast<double>(d) < sim.now + panel_period);
  }

  // 8. reset() keeps what belongs to the panel and the machine, drops the
  //    target: the next deadline is re-anchored from now.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 1.0 * MS}});
    sim.run(200);
    const double period = lock.period_ns(), band = lock.band_ns(), work = lock.work_ns();
    lock.reset();
    CHECK(lock.locked() && lock.period_ns() == period && lock.band_ns() == band && lock.work_ns() == work);
    const double now = sim.now + 500.0 * MS;
    const double d = static_cast<double>(lock.deadline(static_cast<u64>(now)));
    CHECK(d >= now && d < now + panel_period);
  }

  // 9. The direct KMS tier as a mailbox (display_drm.cpp, RG DS 19 October):
  //    the present is 2 ms, a frame ready while the panel's previous flip is
  //    pending is issued at that flip's vblank (the record's issue time is
  //    the vblank, wait_ns the hold) or dropped there when it waited longer
  //    than the margin, and has no flip then. Free-running at 60 Hz against
  //    a 59.7 Hz panel about half the flips are held ones, so the lock must
  //    learn the work from the unheld half, settle like the SDL tier, and
  //    take one long frame as one repeat plus the drop of the frame behind
  //    it (held `band` > margin), nothing more.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 2.0 * MS}});
    sim.draw = 2.0 * MS;
    sim.margin = 1.0 * MS; // below any band: a frame held behind a late one is always dropped
    sim.run(300);
    CHECK(lock.locked());
    std::fprintf(
        stderr,
        "phase lock: mailbox, after 300 frames work mean %.3f ms (used %.3f), band %.2f, late %llu, repeats %d, dropped %d, lat %.3f\n",
        lock.work_mean_ns() / MS, lock.work_ns() / MS, lock.band_ns() / MS,
        static_cast<unsigned long long>(lock.late()), sim.repeats, sim.dropped, sim.lat[0] / MS);
    CHECK(near(lock.work_mean_ns(), sim.work + sim.draw, 50'000.0));
    const int repeats0 = sim.repeats, dropped0 = sim.dropped;
    const u64 late0 = lock.late();
    for (int i = 0; i < 300; ++i) {
      sim.frame();
      CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
    }
    CHECK(sim.repeats == repeats0 && sim.dropped == dropped0 && lock.late() == late0);
    const double work0 = sim.work;
    sim.work = work0 + 6.0 * MS; // over its slot
    sim.frame();
    sim.work = work0;
    for (int i = 0; i < 8; ++i) sim.frame();
    std::fprintf(stderr, "phase lock: mailbox, one long frame: repeats +%d, dropped +%d, late +%llu, band %.2f\n",
                 sim.repeats - repeats0, sim.dropped - dropped0, static_cast<unsigned long long>(lock.late() - late0),
                 lock.band_ns() / MS);
    CHECK(sim.repeats == repeats0 + 1); // the tick the long frame missed
    CHECK(sim.dropped == dropped0 + 1); // the frame behind it, held past the margin
    CHECK(lock.late() == late0 + 1);
    sim.run(100);
    CHECK(sim.repeats == repeats0 + 1 && sim.dropped == dropped0 + 1);
    CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
  }
  // 9b. The same mailbox with the band under its margin (DS_PACE_BAND 1.6 ms,
  //     margin 2 ms): the frame behind a late one is held, not dropped, and
  //     flipped one tick late with its hold in the record; so is the frame
  //     behind that one, until the lock aims the next frame past them. One
  //     long frame costs one repeat and no drop, and the lock settles again.
  {
    PhaseLock::Params p;
    p.band_ns = 1.6 * MS;
    PhaseLock lock(p);
    Sim sim(lock, {Panel{panel_period, 2.0 * MS}});
    sim.draw = 2.0 * MS;
    sim.margin = 2.0 * MS;
    sim.run(300);
    const int repeats0 = sim.repeats, dropped0 = sim.dropped;
    const u64 late0 = lock.late();
    for (int i = 0; i < 100; ++i) {
      sim.frame();
      CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
    }
    const double work0 = sim.work;
    sim.work = work0 + 6.0 * MS;
    sim.frame();
    sim.work = work0;
    int held = 0;
    for (int i = 0; i < 12; ++i) {
      sim.frame();
      if (sim.wait[0] > 0.0) ++held;
    }
    std::fprintf(
        stderr,
        "phase lock: mailbox under the margin, one long frame: repeats +%d, dropped +%d, late +%llu, %d of the next 12 frames held, band %.2f, lat %.3f\n",
        sim.repeats - repeats0, sim.dropped - dropped0, static_cast<unsigned long long>(lock.late() - late0), held,
        lock.band_ns() / MS, sim.lat[0] / MS);
    CHECK(sim.repeats == repeats0 + 1);
    CHECK(sim.dropped == dropped0);
    CHECK(held <= 3);
    sim.run(100);
    CHECK(sim.repeats == repeats0 + 1 && sim.dropped == dropped0);
    CHECK(near(sim.lat[0], lock.band_ns(), 0.1 * MS));
  }

  // The same tier with the hold unreported (DrmOut's record before 19 Oct):
  // a held flip's issue time is the vblank, and the lock learns the hold as
  // work. Printed for the record; the device measurements are in
  // docs/PERFORMANCE.md.
  {
    PhaseLock lock;
    Sim sim(lock, {Panel{panel_period, 2.0 * MS}});
    sim.draw = 2.0 * MS;
    sim.margin = 2.0 * MS;
    sim.report_hold = false;
    sim.run(300);
    const int repeats0 = sim.repeats, dropped0 = sim.dropped;
    const u64 late0 = lock.late();
    sim.run(300);
    std::fprintf(
        stderr,
        "phase lock: mailbox, hold unreported: work mean %.3f ms (real %.3f), band %.2f, over 300 settled frames late +%llu, repeats +%d, dropped +%d, lat %.3f\n",
        lock.work_mean_ns() / MS, (sim.work + sim.draw) / MS, lock.band_ns() / MS,
        static_cast<unsigned long long>(lock.late() - late0), sim.repeats - repeats0, sim.dropped - dropped0,
        sim.lat[0] / MS);
  }

  // Free-running frames on the renderer tier: a frame held for the previous
  // flip keeps its present lane busy, and the next frame waits to be handed
  // to it (Display::take_lane_wait_ns()). That wait, reported through
  // queued(), is not work: the lock learns the frame's own 10 ms, not the 16
  // its swap came after, so its lead fits a refresh (HD smooth at 2x read
  // 25-28 ms of lead and never locked, 30 October 2026).
  {
    PhaseLock lock;
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 10; ++i, t += static_cast<u64>(panel_period)) lock.flip(0, t, 0);
    CHECK(lock.locked());
    for (int i = 0; i < 60; ++i) {
      const u64 start = t - static_cast<u64>(17.0 * MS); // swapped 1 ms before its flip, 16 ms after its start
      const int frame = lock.started(start);
      lock.queued(static_cast<u64>(6.0 * MS));
      lock.flip(0, t, start + static_cast<u64>(16.0 * MS), 0, false, frame);
      t += static_cast<u64>(panel_period);
    }
    std::fprintf(stderr, "phase lock: queued hand-off, work mean %.3f ms (frame 10, queued 6)\n",
                 lock.work_mean_ns() / MS);
    CHECK(near(lock.work_mean_ns(), 10.0 * MS, 50'000.0));
  }

  // A band widened by misses relaxes while the lock is off as well, or the
  // lead it is tested against (main.cpp: free over 2 periods, locked again
  // under 1.8) would keep the lock out for the rest of the session.
  {
    PhaseLock::Params p;
    p.band_ns = 6.0 * MS;
    PhaseLock lock(p);
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 10; ++i, t += static_cast<u64>(panel_period)) lock.flip(0, t, 0);
    for (int i = 0; i < 60 * 60; ++i, t += static_cast<u64>(panel_period)) {
      const u64 start = t - static_cast<u64>(12.0 * MS);
      const int frame = lock.started(start); // no deadline asked: free-running
      lock.flip(0, t, start + static_cast<u64>(9.0 * MS), 0, false, frame);
    }
    std::fprintf(stderr, "phase lock: free for a minute, band 6.00 -> %.2f ms\n", lock.band_ns() / MS);
    CHECK(lock.band_ns() < 3.0 * MS);
  }

  // Free pacing settles with nearly every swap held for the previous flip
  // (13 ms at 2x). Those frames still teach the work, or the estimate stays
  // what the last heavy scene left and the lead never fits again (frames back
  // at 10 ms, lock off for the rest of the run, 30 October 2026).
  {
    PhaseLock lock;
    const u64 period = static_cast<u64>(panel_period);
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 10; ++i, t += period) lock.flip(0, t, 0);
    for (int i = 0; i < 120; ++i, t += period) { // a heavy scene, swaps not held, the work swinging 14 to 20 ms
      const u64 work = static_cast<u64>((i % 2 ? 14.0 : 20.0) * MS);
      const u64 start = t - work - static_cast<u64>(2.0 * MS);
      const int frame = lock.started(start);
      lock.flip(0, t, start + work, 0, false, frame);
    }
    const double heavy = lock.work_ns();
    CHECK(heavy > 17.0 * MS);
    for (int i = 0; i < 300; ++i, t += period) { // light again: ready after 13 ms, held 13 more, flipped 3 ms on
      const u64 start = t - static_cast<u64>(29.0 * MS);
      const int frame = lock.started(start);
      lock.flip(0, t, start + static_cast<u64>(26.0 * MS), static_cast<u64>(13.0 * MS), false, frame);
    }
    std::fprintf(stderr, "phase lock: free with held swaps, work %.2f -> %.2f ms (frame 13)\n", heavy / MS,
                 lock.work_ns() / MS);
    CHECK(near(lock.work_mean_ns(), 13.0 * MS, 100'000.0));
    CHECK(lock.work_ns() + 2.5 * MS < lock.period_ns() * 1.8); // main.cpp's relock test
  }

  // The menu's held picture and fast forward flip with no frame the lock knows of, and those flips teach it nothing;
  // the estimate holds over a pause and the lead still fits (one menu used to read as 1.5 s of work, and the frames
  // after it then kept the estimate at a whole number of refreshes: lock off until quit, 2 November 2026).
  {
    PhaseLock lock;
    const u64 period = static_cast<u64>(1e9 / 60.0);
    u64 t = 1'000'000'000ull;
    const auto free_frames = [&](int n) {
      for (int i = 0; i < n; ++i, t += period) {
        const u64 start = t - static_cast<u64>(29.0 * MS);
        const int frame = lock.started(start);
        lock.flip(0, t, start + static_cast<u64>(26.0 * MS), static_cast<u64>(13.0 * MS), false, frame);
      }
    };
    for (int i = 0; i < 10; ++i, t += period) lock.flip(0, t, 0);
    free_frames(300);
    CHECK(near(lock.work_mean_ns(), 13.0 * MS, 100'000.0));
    t += static_cast<u64>(3500.0 * MS);
    for (int i = 0; i < 8; ++i, t += period * 2) lock.flip(0, t, t - static_cast<u64>(3.0 * MS));
    std::fprintf(stderr, "phase lock: after a held picture, work %.2f ms (frame 13)\n", lock.work_mean_ns() / MS);
    CHECK(near(lock.work_mean_ns(), 13.0 * MS, 100'000.0));
    CHECK(lock.locked());
    free_frames(60);
    CHECK(near(lock.work_mean_ns(), 13.0 * MS, 100'000.0));
    CHECK(lock.work_ns() + lock.band_ns() < lock.period_ns() * 1.8); // main.cpp's relock test
  }

  // One real stall reads as what it was, and is forgotten: the estimate has no resting place above the real work.
  {
    PhaseLock lock;
    const u64 period = static_cast<u64>(1e9 / 60.0);
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 10; ++i, t += period) lock.flip(0, t, 0);
    int stalled = 0;
    for (int i = 0; i < 300; ++i, t += period) {
      const bool stall = i == 100;
      const u64 start = stall ? t - static_cast<u64>(1000.0 * MS) : t - static_cast<u64>(29.0 * MS);
      const int frame = lock.started(start);
      lock.flip(0, t, start + static_cast<u64>((stall ? 990.0 : 26.0) * MS), stall ? 0 : static_cast<u64>(13.0 * MS),
                false, frame);
      if (stall) stalled = static_cast<int>(lock.work_mean_ns() / MS);
    }
    std::fprintf(stderr, "phase lock: a 990 ms frame, work %d ms, 200 frames on %.2f ms (frame 13)\n", stalled,
                 lock.work_mean_ns() / MS);
    CHECK(stalled > 50);
    CHECK(near(lock.work_mean_ns(), 13.0 * MS, 100'000.0));
  }

  // A flip whose frame number has left the ring, or that the lock never gave, is no sample.
  {
    PhaseLock lock;
    const u64 period = static_cast<u64>(1e9 / 60.0);
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 10; ++i, t += period) lock.flip(0, t, 0);
    const int first = lock.started(t - static_cast<u64>(12.0 * MS));
    CHECK(first != 0);
    for (int i = 0; i < PhaseLock::RING; ++i) CHECK(lock.started(t - static_cast<u64>(12.0 * MS)) != first);
    const u64 samples = lock.samples();
    lock.flip(0, t, t - static_cast<u64>(2.0 * MS), 0, false, first);
    lock.flip(0, t + period, t + period - static_cast<u64>(2.0 * MS), 0, false, 0);
    CHECK(lock.samples() == samples);
  }

  std::fprintf(stderr, "phase_lock: ok\n");
  return 0;
}
