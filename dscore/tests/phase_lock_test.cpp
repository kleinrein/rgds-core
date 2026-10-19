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

using ds::sdl::PhaseLock;
using ds::u64;

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
  double free_period = 1e9 / 60.0;   // the pacer's own clock before the lock holds
  double now = 100.0 * MS;           // the current frame's hand-off time
  struct Pending { int panel; double at, swap, wait; };
  std::deque<Pending> pending;
  // The last frame's results, per panel.
  std::vector<double> lat, wait, gap;
  int repeats = 0, late_starts = 0;

  Sim(PhaseLock& l, std::vector<Panel> p) : lock(l), panels(std::move(p)), lat(panels.size()), wait(panels.size()), gap(panels.size()) {}

  void deliver(double upto) {
    while (!pending.empty() && pending.front().at <= upto) {
      const Pending& f = pending.front();
      lock.flip(f.panel, static_cast<u64>(f.at), static_cast<u64>(f.swap), static_cast<u64>(f.wait));
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
    lock.started(static_cast<u64>(start), late);
    const double done = start + work;
    for (size_t i = 0; i < panels.size(); ++i) {
      Panel& p = panels[i];
      const double ready = done + draw;
      const double swap = ready > p.last_flip ? ready : p.last_flip;   // held for the previous flip
      wait[i] = swap - ready;
      const double at = p.tick_after(swap + gpu);
      lat[i] = at - swap;
      gap[i] = p.last_flip ? at - p.last_flip : p.period;
      if (p.last_flip && gap[i] > 1.5 * p.period) ++repeats;
      p.last_flip = at;
      pending.push_back({static_cast<int>(i), at, swap, wait[i]});
    }
    now = done;
  }
  void run(int n) { for (int i = 0; i < n; ++i) frame(); }
};

} // namespace

int main() {
  const double panel_period = 1e9 / 59.7;   // the RG DS panels, give or take their wander

  // 1. Period learning: after settle_flips consistent gaps the lock holds and
  //    the period is the panel's; a gap of two refreshes is still the panel's
  //    clock, a gap that is no multiple starts the run over.
  {
    PhaseLock lock;
    u64 t = 1'000'000'000ull;
    for (int i = 0; i < 7; ++i) { lock.flip(0, t, 0); t += static_cast<u64>(panel_period); }
    CHECK(!lock.locked());   // 6 gaps < 8
    for (int i = 0; i < 3; ++i) { lock.flip(0, t, 0); t += static_cast<u64>(panel_period); }
    CHECK(lock.locked());
    CHECK(near(lock.period_ns(), panel_period, 2'000.0));
    t += static_cast<u64>(panel_period);   // one refresh with no flip
    lock.flip(0, t, 0);
    CHECK(lock.locked());
    CHECK(near(lock.period_ns(), panel_period, 2'000.0));
    t += static_cast<u64>(panel_period * 1.3);
    lock.flip(0, t, 0);
    CHECK(!lock.locked());
    for (int i = 0; i < 9; ++i) { t += static_cast<u64>(panel_period); lock.flip(0, t, 0); }
    CHECK(lock.locked());
    CHECK(lock.samples() == 0);   // no swap times: nothing learned about the work
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
    std::fprintf(stderr, "phase lock: after 60 frames work mean %.3f ms (used %.3f), %llu samples, band %.2f, late %llu, repeats %d, lat %.3f wait %.3f\n",
                 lock.work_mean_ns() / MS, lock.work_ns() / MS, static_cast<unsigned long long>(lock.samples()), lock.band_ns() / MS,
                 static_cast<unsigned long long>(lock.late()), sim.repeats, sim.lat[0] / MS, sim.wait[0] / MS);
    CHECK(near(lock.work_mean_ns(), sim.work + sim.draw, 50'000.0));
    CHECK(lock.late() == 0);   // the frames held while engaging are not misses
    CHECK(near(lock.band_ns(), PhaseLock::Params{}.band_ns, 0.1 * MS));
    const int repeats0 = sim.repeats;
    const u64 late0 = lock.late();
    double worst = 0.0, worst_wait = 0.0;
    for (int i = 0; i < 600; ++i) {
      sim.frame();
      worst = std::max(worst, std::fabs(sim.lat[0] - lock.band_ns()));
      worst_wait = std::max(worst_wait, sim.wait[0]);
    }
    std::fprintf(stderr, "phase lock: one panel, worst |lat - band| after settling %.3f ms, worst hold for the previous flip %.3f ms, band %.2f ms, work %.2f ms, period %.4f ms\n",
                 worst / MS, worst_wait / MS, lock.band_ns() / MS, lock.work_ns() / MS, lock.period_ns() / MS);
    CHECK(worst < 0.1 * MS);   // the spread allowance, learned from the engagement frames, decays towards zero
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
    sim.work = work0 + 6.0 * MS;   // over its slot
    sim.frame();
    sim.work = work0;
    CHECK(sim.repeats == repeats0 + 1);
    for (int i = 0; i < 8; ++i) { sim.frame(); CHECK(sim.repeats == repeats0 + 1); }   // the two frames behind it are held, none repeats
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
    std::fprintf(stderr, "phase lock: two panels, lat %.3f / %.3f ms, wait %.3f / %.3f, band %.2f, work %.2f (mean %.2f), late %llu, repeats %d\n",
                 sim.lat[0] / MS, sim.lat[1] / MS, sim.wait[0] / MS, sim.wait[1] / MS, lock.band_ns() / MS, lock.work_ns() / MS, lock.work_mean_ns() / MS,
                 static_cast<unsigned long long>(lock.late()), sim.repeats);
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
    p.phase = p.last_flip; p.period = 1e9 / 59.2;   // the grid continues from the last flip at the new period
    sim.run(200);
    std::fprintf(stderr, "phase lock: period wander, period %.4f ms, lat %.3f, band %.2f, work %.2f, repeats +%d, late %llu\n",
                 lock.period_ns() / MS, sim.lat[0] / MS, lock.band_ns() / MS, lock.work_ns() / MS, sim.repeats - repeats0, static_cast<unsigned long long>(lock.late()));
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
    std::fprintf(stderr, "phase lock: cpu-bound, late starts %d, late %llu (was %llu), band %.2f (was %.2f), repeats %d\n", sim.late_starts,
                 static_cast<unsigned long long>(lock.late()), static_cast<unsigned long long>(late0), lock.band_ns() / MS, band0 / MS, sim.repeats);
    CHECK(sim.late_starts >= 90);
    CHECK(lock.late() <= late0 + 1);
    CHECK(lock.band_ns() <= band0 + PhaseLock::Params{}.band_bump_ns);
    CHECK(sim.repeats <= 14);   // 100 frames of 1.1 refresh each need 110 ticks: about 10 repeats, no more than that
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

  std::fprintf(stderr, "phase_lock: ok\n");
  return 0;
}
