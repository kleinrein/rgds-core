// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "kms_flips.h"
#include "drm_uapi.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace ds::sdl {

namespace {
// DRM stamps flip events with CLOCK_MONOTONIC.
u64 now_ns() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<u64>(t.tv_sec) * 1'000'000'000ull + static_cast<u64>(t.tv_nsec);
}
constexpr int kLearnTries = 3;
double ms(u64 ns) { return static_cast<double>(ns) / 1e6; }
} // namespace

KmsFlips::KmsFlips(int fd, u64 wait_ns, u64 learn_ns) : fd_(fd), wait_ns_(wait_ns), learn_ns_(learn_ns) {
  // Reads follow a poll() that saw data and nothing else reads the fd once
  // this is in use, but a read that lost a race must not block a present.
  const int fl = fcntl(fd_, F_GETFL);
  if (fl >= 0) fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
}

int KmsFlips::add() {
  std::lock_guard<std::mutex> lk(m_);
  for (int i = 0; i < SLOTS; ++i)
    if (!slots_[i].used) {
      slots_[i] = Slot{};
      slots_[i].used = true;
      return i;
    }
  return -1;
}

void KmsFlips::remove(int slot) {
  if (slot < 0) return;
  std::lock_guard<std::mutex> lk(m_);
  slots_[slot] = Slot{};
}

bool KmsFlips::ready(int slot) const {
  if (slot < 0) return false;
  std::lock_guard<std::mutex> lk(m_);
  return !failed_ && slots_[slot].flag != nullptr;
}

bool KmsFlips::failed() const {
  std::lock_guard<std::mutex> lk(m_);
  return failed_;
}

template <class Done> bool KmsFlips::wait_for(std::unique_lock<std::mutex>& lk, Done done, u64 timeout_ns) {
  const u64 deadline = now_ns() + timeout_ns;
  while (!done()) {
    const u64 t = now_ns();
    if (t >= deadline) return false;
    if (reading_) { // another thread reads for everyone and wakes us after each read
      cv_.wait_for(lk, std::chrono::nanoseconds(deadline - t));
      continue;
    }
    reading_ = true;
    lk.unlock();
    pollfd p = {fd_, POLLIN, 0};
    const int r = poll(&p, 1, static_cast<int>((deadline - t + 999'999) / 1'000'000));
    alignas(8) char buf[1024];
    long n = 0;
    if (r > 0 && (p.revents & POLLIN)) n = static_cast<long>(read(fd_, buf, sizeof buf));
    const bool broken = (r < 0 && errno != EINTR) || (r > 0 && !(p.revents & POLLIN));
    lk.lock();
    reading_ = false;
    if (n > 0) dispatch(buf, n);
    cv_.notify_all();
    if (broken) return done();
  }
  return true;
}

// Called with m_ held. Each event is SDL's: a flip queued with the address of
// its window's waiting_for_flip.
void KmsFlips::dispatch(const char* buf, long n) {
  for (long off = 0; off + static_cast<long>(sizeof(drmu::event)) <= n;) {
    drmu::event e;
    std::memcpy(&e, buf + off, sizeof e);
    if (e.length < sizeof e || off + static_cast<long>(e.length) > n) break;
    if (e.type == drmu::EVENT_FLIP_COMPLETE && e.length >= sizeof(drmu::event_vblank)) {
      drmu::event_vblank v;
      std::memcpy(&v, buf + off, sizeof v);
      auto* flag = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(v.user_data));
      Slot* s = nullptr;
      for (Slot& c : slots_)
        if (c.used && c.flag && c.flag == flag) {
          s = &c;
          break;
        }
      if (!s && flag && learning_ >= 0 && !slots_[learning_].flag) {
        s = &slots_[learning_];
        s->flag = flag;
      }
      if (s) {
        const u64 at = static_cast<u64>(v.tv_sec) * 1'000'000'000ull + static_cast<u64>(v.tv_usec) * 1000ull;
        // The panel's period: the shortest gap between two of its flips.
        if (s->last_ns && at > s->last_ns + 4'000'000 && (!s->period_ns || at - s->last_ns < s->period_ns))
          s->period_ns = at - s->last_ns;
        s->last_ns = at;
        Flip& rec = s->ring[s->st.flips % RING];
        rec = Flip{s->st.flips + 1, at, 0, 0, 0};
        if (s->pending && at > s->issue_ns) {
          const u64 lat = at - s->issue_ns;
          rec.issue_ns = s->issue_ns;
          rec.tag = s->pending_tag;
          rec.wait_ns = s->pending_wait_ns;
          ++s->st.timed;
          s->st.lat_ns += lat;
          if (lat > s->st.lat_max_ns) s->st.lat_max_ns = lat;
          // Past the first vblank after the swap: the GPU had not finished the frame by then.
          if (s->period_ns && lat > s->period_ns + s->period_ns / 20) ++s->st.late;
        }
        ++s->seen;
        ++s->st.flips;
        s->pending = false;
      } else {
        ++strays_;
      }
      if (flag) *flag = 0; // what SDL's KMSDRM_FlipHandler does with every flip event
    }
    off += static_cast<long>(e.length);
  }
}

void KmsFlips::before(int slot) {
  if (slot < 0) return;
  std::unique_lock<std::mutex> lk(m_);
  Slot& s = slots_[slot];
  s.last_wait_ns = 0;
  if (!failed_ && s.flag) {
    if (s.pending) {
      const u64 t0 = now_ns();
      if (!wait_for(lk, [&] { return !s.pending; }, wait_ns_)) {
        // Present anyway: SDL's flip may then fail (EBUSY) and drop a
        // frame, where polling for it could block this thread for good.
        s.pending = false;
        ++s.st.timeouts;
        std::fprintf(stderr, "kms: no page flip completion in %.0f ms\n", ms(wait_ns_));
      }
      const u64 ns = now_ns() - t0;
      s.last_wait_ns = ns;
      ++s.st.waits;
      s.st.wait_ns += ns;
      if (ns > s.st.wait_max_ns) s.st.wait_max_ns = ns;
    }
    *s.flag = 0; // a flag SDL set after its event was read here already
  }
  s.seen_before = s.seen;
}

void KmsFlips::after(int slot, int tag) {
  if (slot < 0) return;
  std::unique_lock<std::mutex> lk(m_);
  Slot& s = slots_[slot];
  ++s.st.presents;
  if (failed_) return;
  if (s.flag) {
    if (s.seen != s.seen_before) *s.flag = 0; // its flip has completed already
    else if (*s.flag) {
      s.pending = true;
      s.issue_ns = now_ns();
      s.pending_tag = tag;
      s.pending_wait_ns = s.last_wait_ns;
    }
    return;
  }
  // SDL's first swap of a window sets the mode: no flip to learn from.
  if (s.st.presents == 1) return;
  learning_ = slot;
  const bool ok = wait_for(lk, [&] { return s.flag != nullptr; }, learn_ns_);
  learning_ = -1;
  if (ok || ++s.learn_tries < kLearnTries) return;
  failed_ = true;
  std::fprintf(stderr, "kms: no page flip events for a window; SDL waits for them itself, on one present thread\n");
}

void KmsFlips::report(int slot, const char* name, std::FILE* out) const {
  if (slot < 0) return;
  const Stats st = stats(slot);
  if (!st.presents) return;
  std::lock_guard<std::mutex> lk(m_);
  const Slot& s = slots_[slot];
  std::fprintf(
      out,
      "kms flips (%s): %llu presents, %llu flips, waited %llu times %.3f ms mean %.3f ms max, flip %.3f ms mean %.3f ms max after the swap, "
      "%llu late (period %.3f ms), %llu timeouts, %llu stray%s\n",
      name ? name : "?", static_cast<unsigned long long>(st.presents), static_cast<unsigned long long>(st.flips),
      static_cast<unsigned long long>(st.waits), st.waits ? ms(st.wait_ns) / static_cast<double>(st.waits) : 0.0,
      ms(st.wait_max_ns), st.timed ? ms(st.lat_ns) / static_cast<double>(st.timed) : 0.0, ms(st.lat_max_ns),
      static_cast<unsigned long long>(st.late), ms(s.period_ns), static_cast<unsigned long long>(st.timeouts),
      static_cast<unsigned long long>(strays_),
      failed_  ? ", off (SDL waits)"
      : s.flag ? ""
               : ", never matched");
}

KmsFlips::Stats KmsFlips::stats(int slot) const {
  std::lock_guard<std::mutex> lk(m_);
  return slot < 0 ? Stats{} : slots_[slot].st;
}

u64 KmsFlips::strays() const {
  std::lock_guard<std::mutex> lk(m_);
  return strays_;
}

int KmsFlips::recent(int slot, u64 after, Flip* out, int max) const {
  if (slot < 0 || max <= 0) return 0;
  std::lock_guard<std::mutex> lk(m_);
  const Slot& s = slots_[slot];
  const u64 newest = s.st.flips;
  if (newest <= after) return 0;
  u64 first = after + 1;
  if (newest - first + 1 > static_cast<u64>(RING)) first = newest - RING + 1; // the older ones are gone
  if (newest - first + 1 > static_cast<u64>(max)) first = newest - max + 1;
  int n = 0;
  for (u64 q = first; q <= newest; ++q) out[n++] = s.ring[(q - 1) % RING];
  return n;
}

} // namespace ds::sdl
