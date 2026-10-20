// SPDX-License-Identifier: GPL-3.0-or-later
#include "lid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

namespace ds::sdl {

namespace {
s64 clock_ns(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<s64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
#ifdef __linux__
bool switch_state(int fd, bool& closed) {
  unsigned long bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
  if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0) return false;
  closed = (bits[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1;
  return true;
}
#endif
}

Lid::Lid() : skew_ns_(clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC)) {}

void Lid::open() {
  skew_ns_ = clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC);
  if (const char* v = std::getenv("DS_LID_PULSE")) pulse_enabled_ = std::atoi(v) != 0;
#ifdef __linux__
  DIR* d = opendir("/dev/input");
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + e->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw) >= 0 && ((sw[SW_LID / (8 * sizeof(long))] >> (SW_LID % (8 * sizeof(long)))) & 1)) {
      char name[64] = "?";
      ioctl(fd, EVIOCGNAME(sizeof name), name);
      fd_ = fd;
      switch_state(fd_, closed_);
      std::fprintf(stderr, "lid: %s (%s), %s\n", path.c_str(), name, closed_ ? "closed" : "open");
      break;
    }
    ::close(fd);
  }
  closedir(d);
#endif
}

void Lid::close() {
#ifdef __linux__
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
}

void Lid::resumed() {
  if (fd_ >= 0) return;   // the switch reports the real hinge
  if (!pulse_enabled_) { std::fprintf(stderr, "lid: host resumed from suspend; the lid stays as it is\n"); return; }
  pulse_ = PULSE_FRAMES;
  std::fprintf(stderr, "lid: host resumed from suspend; closing the lid until the firmware sleeps\n");
}

bool Lid::poll(bool& closed, bool asleep) {
#ifdef __linux__
  if (fd_ >= 0) {
    input_event ev;
    bool any = false;
    while (read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) any = true;
    if (any) switch_state(fd_, closed_);
  }
#endif
  // No switch (or host froze before the event): clocks drifted >1s apart.
  const s64 skew = clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC);
  if (skew - skew_ns_ > 1000000000) resumed();
  skew_ns_ = skew;
  // Open once the firmware is asleep (the open edge's IRQ wakes it), or when
  // a game has ignored the hinge for the whole pulse.
  if (pulse_ > 0 && (asleep || --pulse_ == 0)) pulse_ = 0;
  closed = closed_ || pulse_ > 0;
  const bool changed = closed != last_;
  last_ = closed;
  return changed;
}

} // namespace ds::sdl
