// SPDX-License-Identifier: GPL-3.0-or-later
// The microphone from capture's bursts to the frames the core spreads it over, and the noise gate.
#include "check.h"
#include "frontend/sdl/mic_queue.h"

#include <cmath>
#include <cstdlib>

using ds::s16;
using ds::sdl::MicBand;
using ds::sdl::MicGate;
using ds::sdl::MicQueue;

namespace {

constexpr size_t PER_FRAME = 547;
constexpr double RATE = 32768;

struct Run {
  size_t empty = 0, shortest = 1000000, longest = 0, dropped = 0;
};

// `period`-sized bursts at the capture rate against frames of `frame_seconds`, from `after` frames on. Samples count up,
// so a gap in what comes out is a drop.
Run run(size_t period, double frame_seconds, int frames, int after) {
  MicQueue queue(PER_FRAME);
  Run r;
  std::vector<s16> in, out;
  double now = 0, next_burst = period / RATE;
  int counter = 0, last = -1;
  for (int f = 0; f < frames; ++f) {
    now += frame_seconds;
    in.clear();
    while (next_burst <= now) {
      for (size_t i = 0; i < period; ++i) in.push_back(static_cast<s16>(counter++ & 0x3FFF));
      next_burst += period / RATE;
    }
    queue.push(in);
    queue.take(out);
    if (f < after) {
      if (!out.empty()) last = out.back();
      continue;
    }
    if (out.empty()) ++r.empty;
    r.shortest = std::min(r.shortest, out.size());
    r.longest = std::max(r.longest, out.size());
    for (s16 v : out) {
      if (last >= 0 && v != ((last + 1) & 0x3FFF)) ++r.dropped;
      last = v;
    }
  }
  return r;
}

void bursts_become_even_frames() {
  // A burst of 25 ms against 16.7 ms frames, at the DS's own frame rate and at the panels' 60 Hz.
  for (double frame : {1 / 59.8261, 1 / 60.0}) {
    const Run r = run(819, frame, 20000, 60);
    CHECK(r.empty == 0);
    CHECK(r.dropped == 0);
    CHECK(r.shortest >= PER_FRAME - 8);
    CHECK(r.longest <= PER_FRAME + 8);
  }
  // A coarser burst than any frame.
  const Run r = run(2048, 1 / 59.8261, 20000, 60);
  CHECK(r.empty == 0);
  CHECK(r.dropped == 0);
}

void a_game_running_slow_keeps_up() {
  // 40 fps: the slices settle at the larger size with nothing left behind.
  const Run r = run(819, 1 / 40.0, 20000, 4000);
  CHECK(r.empty == 0);
  CHECK(r.dropped == 0);
  CHECK(r.shortest >= 800);
  CHECK(r.longest <= 840);
}

void nothing_before_the_cushion_and_after_a_stall_it_recovers() {
  MicQueue queue(PER_FRAME);
  std::vector<s16> in(PER_FRAME * 2), out;
  queue.push(in);
  queue.take(out);
  CHECK(out.empty()); // two frames' worth is under the cushion
  queue.push(in);
  queue.take(out);
  CHECK(!out.empty());

  // Capture stops: frames go silent rather than short once it has run dry.
  std::vector<s16> none;
  for (int i = 0; i < 30; ++i) {
    queue.push(none);
    queue.take(out);
  }
  CHECK(out.empty());

  // A big backlog is cut back, it never becomes latency.
  std::vector<s16> stall(PER_FRAME * 20);
  queue.push(stall);
  queue.take(out);
  CHECK(!out.empty());
  CHECK(out.size() < PER_FRAME * 4);
}

void a_game_that_opens_the_microphone_late_gets_full_frames_at_once() {
  MicQueue queue(PER_FRAME);
  std::vector<s16> none, burst(819), out;
  for (int f = 0; f < 5000; ++f) { // a minute of game before the first read of the microphone
    queue.push(none);
    queue.take(out);
    CHECK(out.empty());
  }
  double now = 0, next_burst = 819 / RATE;
  int full = 0, frames = 0;
  for (int f = 0; f < 240; ++f) {
    now += 1 / 59.8261;
    bool arrived = false;
    while (next_burst <= now) {
      queue.push(burst);
      arrived = true;
      next_burst += 819 / RATE;
    }
    if (!arrived) queue.push(none);
    queue.take(out);
    if (out.empty()) {
      CHECK(frames == 0); // only the cushion filling
      continue;
    }
    ++frames;
    if (out.size() >= PER_FRAME - 8 && out.size() <= PER_FRAME + 8) ++full;
  }
  CHECK(frames > 200);
  CHECK(full == frames);
}

std::vector<s16> tone(int amplitude) {
  std::vector<s16> v(PER_FRAME);
  for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<s16>(i & 1 ? amplitude : -amplitude);
  return v;
}

bool silent(const std::vector<s16>& v) {
  for (s16 s : v)
    if (s) return false;
  return true;
}

void the_gate_stays_open_between_syllables() {
  MicGate gate(2.0);
  for (int i = 0; i < 10; ++i) {
    auto noise = tone(50);
    gate.apply(noise);
    CHECK(silent(noise));
  }
  auto loud = tone(5000);
  gate.apply(loud);
  CHECK(!silent(loud));
  // the dip right after still comes through, for the hold
  for (int i = 0; i < 15; ++i) {
    auto dip = tone(50);
    gate.apply(dip);
    CHECK(!silent(dip));
  }
  auto after = tone(50);
  gate.apply(after);
  CHECK(silent(after));
}

double through_the_band(double hz) {
  MicBand band(RATE);
  double sq = 0;
  const int settle = 32768, count = 32768;
  for (int i = 0; i < settle + count; ++i) {
    const double y = band.apply(10000 * std::sin(2 * 3.14159265358979 * hz * i / RATE));
    if (i >= settle) sq += y * y;
  }
  return std::sqrt(sq / count) / (10000 / std::sqrt(2.0));
}

void the_band_keeps_speech_and_drops_rumble_and_what_would_fold_down() {
  for (double hz : {300.0, 1000.0, 3000.0}) CHECK(through_the_band(hz) > 0.85); // speech: within 1.5 dB
  CHECK(through_the_band(30) < 0.1);                                            // rumble
  CHECK(through_the_band(12000) < 0.25);                                        // would fold to 4 kHz at 8 kHz
  MicBand band(RATE);
  double y = 0;
  for (int i = 0; i < 32768; ++i) y = band.apply(5000); // an offset does not pass
  CHECK(std::abs(y) < 1);
}

void a_gate_of_zero_is_off() {
  MicGate gate(0);
  auto quiet = tone(1);
  gate.apply(quiet);
  CHECK(!silent(quiet));
}

} // namespace

int main() {
  bursts_become_even_frames();
  a_game_running_slow_keeps_up();
  nothing_before_the_cushion_and_after_a_stall_it_recovers();
  a_game_that_opens_the_microphone_late_gets_full_frames_at_once();
  the_gate_stays_open_between_syllables();
  the_band_keeps_speech_and_drops_rumble_and_what_would_fold_down();
  a_gate_of_zero_is_off();
  return 0;
}
