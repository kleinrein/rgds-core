// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RG DS: page-flip completions on SDL KMSDRM's DRM fd, read here so that
// each window can present on its own thread.
//
// SDL 2's KMSDRM swap queues a window's page flip with the address of that
// window's waiting_for_flip flag as the event's user data, and its next swap
// first loops poll(fd, -1) + drmHandleEvent until the handler has cleared
// the flag. All windows share the one fd, so two threads in that loop race:
// one reads (and clears) the other's event, and the other polls on for an
// event that has already gone. Here one thread at a time reads the fd and
// clears the flag the way SDL's handler does, and before() holds a window's
// present until its own previous flip has completed, so SDL's loop finds
// nothing to wait for and never polls.
//
// A window's flag address is learned from its first flip: after() waits for
// that event while it is the only unknown one in flight. The caller presents
// every window on one thread until ready() holds for all of them. If a
// window's flips never show up, failed() turns this off and SDL waits for
// every flip itself again (which is only safe on one thread).
#pragma once
#include "core/types.h"

#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace ds::sdl {

class KmsFlips {
public:
  static constexpr int SLOTS = 4;
  // `fd`: SDL's DRM fd (SDL_SysWMinfo). The timeouts: a flip not done after
  // `wait_ns` is a hung GPU, and the present goes ahead without it.
  explicit KmsFlips(int fd, u64 wait_ns = 500'000'000, u64 learn_ns = 200'000'000);

  int add();                  // a window's slot; -1 when all are taken
  void remove(int slot);      // after before(): nothing of it in flight
  bool ready(int slot) const; // its flag is known: it may present beside other windows
  bool failed() const;

  // Around one SDL_RenderPresent of the slot's window, on the thread that
  // presents it. before() returns once its previous flip has completed;
  // after() notes the flip SDL queued (and learns the flag from the first).
  // `tag` rides along to the flip's record (Flip::tag): the frame pacer marks
  // a frame that started late with it.
  void before(int slot);
  void after(int slot, int tag = 0);

  // One line at close: flips, time waited for them, how long they took.
  void report(int slot, const char* name, std::FILE* out) const;

  struct Stats {
    u64 presents = 0, flips = 0, waits = 0, wait_ns = 0, wait_max_ns = 0;
    u64 timed = 0, lat_ns = 0, lat_max_ns = 0, late = 0, timeouts = 0;
  };
  Stats stats(int slot) const;
  u64 strays() const;   // events for no known window (cleared as SDL would)

  // One completed flip: its number for the window (from 1), when it completed
  // (the DRM event's CLOCK_MONOTONIC stamp) and when SDL's swap that queued
  // it returned (0 when that swap was not seen pending). The frame pacer
  // phase-locks to these (phase_lock.h).
  // wait_ns: how long before() held that swap for the window's previous flip.
  // tag: bit 0 (after()'s tag) the frame started late; bit 2 (DrmOut's
  // mailbox) the frame queued behind this flip was dropped at its vblank.
  struct Flip { u64 seq = 0, at_ns = 0, issue_ns = 0; int tag = 0; u64 wait_ns = 0; };
  static constexpr int RING = 8;
  // The slot's flips after number `after`, oldest first, at most `max` (the
  // ring keeps the last RING); returns how many were written.
  int recent(int slot, u64 after, Flip* out, int max) const;

private:
  struct Slot {
    bool used = false;
    volatile int* flag = nullptr;   // SDL's windata->waiting_for_flip (an SDL_bool)
    bool pending = false;           // a flip queued by after() and not yet completed
    int learn_tries = 0;
    u64 seen = 0, seen_before = 0;  // events read for it; the count at before()
    u64 issue_ns = 0, last_ns = 0, period_ns = 0;
    int pending_tag = 0;            // after()'s tag for the pending flip
    u64 last_wait_ns = 0, pending_wait_ns = 0;   // before()'s wait for the swap in progress; for the pending flip
    Flip ring[RING];                // the last RING flips, ring[(seq - 1) % RING]
    Stats st;
  };
  // Reads the fd (one thread at a time) until done() or the timeout.
  template <class Done> bool wait_for(std::unique_lock<std::mutex>& lk, Done done, u64 timeout_ns);
  void dispatch(const char* buf, long n);

  int fd_;
  u64 wait_ns_, learn_ns_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool reading_ = false;
  bool failed_ = false;
  int learning_ = -1;
  u64 strays_ = 0;
  Slot slots_[SLOTS];
};

} // namespace ds::sdl
