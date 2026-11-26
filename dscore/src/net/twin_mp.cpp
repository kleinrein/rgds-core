// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "net/twin_mp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ds::net {

namespace {
constexpr u32 kMagic = 0x4E495754; // "TWIN"
constexpr u32 kSlots = 512;
constexpr u32 kMaxLen = 2048 + 64;
constexpr u32 kDropped = 0xFFFFFFFFu; // a slot the reader has thrown away in place
constexpr u64 kGone = ~u64(0);        // a clock that will never hold anyone up again
constexpr double kTicksPerUs = 67.027964;

int popcount16(u16 v) {
  int n = 0;
  for (; v; v &= static_cast<u16>(v - 1)) ++n;
  return n;
}
} // namespace

struct TwinMp::Msg {
  u64 at = 0;        // the sender's game time when it was sent
  u64 timestamp = 0; // the DS's own microsecond stamp, passed through
  u32 type = 0;      // LanMp's: 0 frame, 1 CMD, 2 | aid << 16 reply, 3 ack
  u32 len = 0;
  u8 data[kMaxLen];
};

struct TwinMp::Block {
  std::atomic<u32> magic;
  std::atomic<u64> clock[2]; // each side's promise: no message from it will carry an earlier `at`
  struct Ring {
    std::atomic<u64> head; // next to read (the reader's)
    std::atomic<u64> tail; // next to write (the writer's)
    Msg slot[kSlots];
  } ring[2]; // ring[i] carries side i's sends
};

TwinMp::TwinMp(const std::string& path, bool host, std::function<u64()> now, int wait_ms)
    : host_(host), now_(std::move(now)), me_(host ? 0 : 1) {
  if (const char* e = std::getenv("DS_TWIN_LEAD_US"); e && std::atoi(e) > 0)
    kLead = static_cast<u64>(std::atoi(e) * kTicksPerUs);
  shm_size_ = sizeof(Block);
  int fd = -1;
  if (host) {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(shm_size_)) != 0) {
      err_ = "cannot make " + path;
      if (fd >= 0) ::close(fd);
      return;
    }
  } else {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
      struct stat st{};
      fd = ::open(path.c_str(), O_RDWR);
      if (fd >= 0 && ::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) >= shm_size_) break;
      if (fd >= 0) ::close(fd);
      fd = -1;
      if (std::chrono::steady_clock::now() >= deadline) {
        err_ = "no twin host at " + path;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  void* p = ::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    err_ = "cannot map " + path;
    return;
  }
  auto* b = static_cast<Block*>(p);
  if (host) {
    for (int i = 0; i < 2; ++i) {
      b->clock[i].store(0, std::memory_order_relaxed);
      b->ring[i].head.store(0, std::memory_order_relaxed);
      b->ring[i].tail.store(0, std::memory_order_relaxed);
    }
    b->magic.store(kMagic, std::memory_order_release);
  } else {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (b->magic.load(std::memory_order_acquire) != kMagic) {
      if (std::chrono::steady_clock::now() >= deadline) {
        ::munmap(p, shm_size_);
        err_ = "twin block at " + path + " never became ready";
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  shm_ = b;
}

TwinMp::~TwinMp() {
  if (!shm_) return;
  shm_->clock[me_].store(kGone, std::memory_order_release); // the other must not wait for a console that left
  ::munmap(shm_, shm_size_);
}

void TwinMp::publish() {
  if (!shm_) return;
  const u64 t = now_();
  u64 cur = shm_->clock[me_].load(std::memory_order_relaxed);
  if (t > cur) shm_->clock[me_].store(t, std::memory_order_release);
}

void TwinMp::wait_for(u64 t) {
  const int other = 1 - me_;
  if (shm_->clock[other].load(std::memory_order_acquire) >= t) return;
  ++waits_;
  const auto t0 = std::chrono::steady_clock::now();
  for (unsigned spin = 0; shm_->clock[other].load(std::memory_order_acquire) < t; ++spin) {
    if (spin < 4000) continue;
    std::this_thread::yield();
  }
  wait_ns_ += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
}

int TwinMp::send(u32 type, const u8* data, int len, u64 timestamp) {
  if (!shm_ || len < 0) return 0;
  publish();
  const u64 at = now_();
  // A promise made in recv_replies covers the reply window; a send inside it would break determinism.
  if (at < shm_->clock[me_].load(std::memory_order_relaxed)) {
    static bool said = false;
    if (!said) std::fprintf(stderr, "twin: a send at %llu inside a promised window\n", (unsigned long long)at);
    said = true;
  }
  auto& r = shm_->ring[me_];
  const u64 tail = r.tail.load(std::memory_order_relaxed);
  while (tail - r.head.load(std::memory_order_acquire) >= kSlots) std::this_thread::yield(); // full: rare
  Msg& m = r.slot[tail % kSlots];
  m.at = at;
  m.timestamp = timestamp;
  m.type = type;
  m.len = static_cast<u32>(std::min<int>(len, kMaxLen));
  if (m.len) std::memcpy(m.data, data, m.len);
  r.tail.store(tail + 1, std::memory_order_release);
  return len;
}

const TwinMp::Msg* TwinMp::front(u64 by) {
  auto& r = shm_->ring[1 - me_];
  for (;;) {
    const u64 head = r.head.load(std::memory_order_relaxed);
    if (head != r.tail.load(std::memory_order_acquire)) {
      const Msg& m = r.slot[head % kSlots];
      if (m.type == kDropped) {
        r.head.store(head + 1, std::memory_order_release);
        continue;
      }
      return m.at + kLead <= by ? &m : nullptr; // in order: a later one arrives later still
    }
    // Empty for now: final only once nothing sent before by - kLead can still come.
    if (by < kLead || shm_->clock[1 - me_].load(std::memory_order_acquire) > by - kLead) return nullptr;
    wait_for(by - kLead + 1);
  }
}

void TwinMp::pop() {
  auto& r = shm_->ring[1 - me_];
  r.head.store(r.head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

int TwinMp::take(const Msg* m, u8* data, u64* timestamp) {
  u32 len = std::min<u32>(m->len, 2048);
  if (len) std::memcpy(data, m->data, len);
  if (timestamp) *timestamp = m->timestamp;
  pop();
  return static_cast<int>(len);
}

// LanMp's rules, each answer as of game time now: a frame not regular at the front is dropped, then whatever
// is next is taken.
int TwinMp::recv_packet(u8* data, u64* timestamp) {
  if (!shm_) return 0;
  publish();
  const u64 now = now_();
  const Msg* m = front(now);
  if (m && m->type != 0) {
    pop();
    m = front(now);
  }
  return m ? take(m, data, timestamp) : 0;
}

int TwinMp::peek_host_packet(u8* data, u64* timestamp) {
  if (!shm_) return 0;
  publish();
  const Msg* m = front(now_());
  if (!m || m->type == 0) return 0; // a regular frame is left for the client's own poll
  return take(m, data, timestamp);
}

int TwinMp::recv_host_packet(u8* data, u64* timestamp) {
  if (!shm_) return 0;
  publish();
  const Msg* m = front(now_());
  return m ? take(m, data, timestamp) : 0;
}

int TwinMp::send_cmd(const u8* data, int len, u64 timestamp) {
  if (!shm_) return 0;
  publish();
  // Replies that have arrived by now are stale: drop them, as LanMp does, before they pass for this CMD's.
  const u64 now = now_();
  if (now >= kLead && shm_->clock[1 - me_].load(std::memory_order_acquire) <= now - kLead) wait_for(now - kLead + 1);
  auto& r = shm_->ring[1 - me_];
  const u64 tail = r.tail.load(std::memory_order_acquire);
  for (u64 i = r.head.load(std::memory_order_relaxed); i != tail; ++i) {
    Msg& m = r.slot[i % kSlots];
    if (m.at + kLead > now) break;
    if (m.type != kDropped && (m.type & 0xFFFF) == 2) m.type = kDropped;
  }
  return send(1, data, len, timestamp);
}

// Called as the CMD finishes going out. The replies come in the window that follows (on the hardware, within
// it); this console sends nothing until the window ends, so it promises that much, and the guest can run
// through the window to answer.
u16 TwinMp::recv_replies(u8* packets, u64 timestamp, u16 aidmask) {
  if (!shm_) return 0;
  publish();
  const u64 now = now_();
  // The last CMD this side sent says how long its window is: 112 us plus a slot per client.
  u64 window_us = 112 + 10 * 15;
  {
    auto& mine = shm_->ring[me_];
    const u64 tail = mine.tail.load(std::memory_order_relaxed);
    for (u64 i = tail; i-- > 0 && tail - i <= kSlots;) {
      const Msg& m = mine.slot[i % kSlots];
      if (m.type != 1) continue;
      if (m.len >= 12 + 28) {
        u16 clienttime, clientmask;
        std::memcpy(&clienttime, m.data + 12 + 24, 2);
        std::memcpy(&clientmask, m.data + 12 + 26, 2);
        window_us = 112 + (u64(clienttime) + 10) * static_cast<u64>(std::max(1, popcount16(clientmask & 0xFFFE)));
      }
      break;
    }
  }
  const u64 until = now + static_cast<u64>(static_cast<double>(window_us) * kTicksPerUs);
  {
    const u64 cur = shm_->clock[me_].load(std::memory_order_relaxed);
    if (until > cur) shm_->clock[me_].store(until, std::memory_order_release);
  }
  u16 ret = 0;
  auto& r = shm_->ring[1 - me_];
  for (;;) {
    const u64 head = r.head.load(std::memory_order_relaxed);
    if (head == r.tail.load(std::memory_order_acquire)) {
      if (shm_->clock[1 - me_].load(std::memory_order_acquire) >= until) return ret; // the window closed
      // Wait for a message or the other's clock past the window, whichever comes first.
      ++waits_;
      const auto t0 = std::chrono::steady_clock::now();
      for (unsigned spin = 0; r.head.load(std::memory_order_relaxed) == r.tail.load(std::memory_order_acquire) &&
                              shm_->clock[1 - me_].load(std::memory_order_acquire) < until;
           ++spin)
        if (spin >= 4000) std::this_thread::yield();
      wait_ns_ += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
      continue;
    }
    Msg& m = r.slot[head % kSlots];
    if (m.type == kDropped) {
      pop();
      continue;
    }
    if (m.at >= until) return ret; // sent after the window: not this CMD's
    const bool good = (m.type & 0xFFFF) == 2 && m.timestamp >= timestamp - 32;
    if (good) {
      const u32 aid = m.type >> 16;
      if (aid >= 1 && aid <= 15 && m.len) {
        std::memcpy(&packets[(aid - 1) * 1024], m.data, std::min<u32>(m.len, 1024));
        ret |= static_cast<u16>(1 << aid);
      }
      pop();
      return ret; // the one other console has answered
    }
    pop(); // anything else in the window is dropped, as LanMp does
    (void)aidmask;
  }
}

} // namespace ds::net
