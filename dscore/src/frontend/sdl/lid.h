// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// Host lid switch (evdev SW_LID; SDL has no switch events). Hosts without a
// switch get a synthetic pulse: a BOOTTIME/MONOTONIC clock jump after resume
// closes the lid, mimicking the real hinge sequence. The lid stays closed
// until the firmware has gone to sleep (its lid IRQ then wakes it) or, for a
// game that ignores the hinge, for PULSE_FRAMES: an open edge before the
// sleep leaves the console asleep with nothing to wake it (panels off, saves
// refused). DS_LID_PULSE=0 turns the pulse off for a host that freezes the
// emulator over its own sleep and wants the game to carry on where it was.
class Lid {
public:
  static constexpr int PULSE_FRAMES = 120;   // two seconds at 60 Hz

  Lid();
  void open();
  void close();
  // Call once a frame with whether the console is asleep (nds.sleeping).
  // Returns true if state changed, stores it in `closed`.
  bool poll(bool& closed, bool asleep = false);
  bool has_switch() const { return fd_ >= 0; }
  // The host came back from suspend: start the synthetic pulse (if enabled).
  void resumed();
  bool pulsing() const { return pulse_ > 0; }
  void enable_pulse(bool on) { pulse_enabled_ = on; }

private:
  int  fd_ = -1;
  bool closed_ = false;
  bool last_ = false;          // what the previous poll() reported
  bool pulse_enabled_ = true;
  int  pulse_ = 0;             // frames left of a suspend-detected close
  s64  skew_ns_ = 0;           // last boottime - monotonic
};

} // namespace ds::sdl
