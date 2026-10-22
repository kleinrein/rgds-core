// SPDX-License-Identifier: GPL-3.0-or-later
// The synthetic hinge pulse after a host suspend (frontend/sdl/lid.h): the lid
// stays closed until the firmware is asleep, so the open edge's IRQ wakes it.
#include "frontend/sdl/lid.h"
#include "check.h"

using ds::sdl::Lid;

int main() {
  bool closed = true;
  Lid lid; // no open(): no switch, as on the RG DS (its hall sensor is a key, not SW_LID)
  CHECK(!lid.poll(closed) && !closed);
  lid.resumed();
  CHECK(lid.pulsing());
  CHECK(lid.poll(closed) && closed);                               // the close edge
  for (int i = 0; i < 30; ++i) CHECK(!lid.poll(closed) && closed); // held while the game is still awake
  CHECK(lid.poll(closed, true) && !closed);                        // asleep now: open, which raises the lid IRQ
  CHECK(!lid.pulsing());
  CHECK(!lid.poll(closed, true) && !closed); // an asleep console with the lid open is left to the game

  Lid ignored; // a game that never sleeps on the hinge sees the lid open again after PULSE_FRAMES
  ignored.resumed();
  int frames = 0;
  while (ignored.poll(closed), closed) {
    ++frames;
    CHECK(frames <= Lid::PULSE_FRAMES);
  }
  CHECK(frames == Lid::PULSE_FRAMES - 1);
  CHECK(!ignored.pulsing());

  Lid off; // DS_LID_PULSE=0: the host froze the emulator over its sleep; nothing happens
  off.enable_pulse(false);
  off.resumed();
  CHECK(!off.pulsing());
  CHECK(!off.poll(closed) && !closed);
  return 0;
}
