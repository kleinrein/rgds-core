// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "net/input_link.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ds::net {

namespace {
constexpr u32 kFrames = 0x4E494E31; // "1NIN": frames, sender to receiver
constexpr u32 kAsk = 0x4B534131;    // "1ASK": send again from frame n, receiver to sender
constexpr u32 kBye = 0x45594231;    // "1BYE": the sender has left, sender to receiver
constexpr size_t kRecord = 13;      // buttons u16, x, y, flags (pen down, lid), 8 mic samples

void put(u8* p, const input::Frame& f) {
  std::memcpy(p, &f.buttons, 2);
  p[2] = f.x;
  p[3] = f.y;
  p[4] = static_cast<u8>((f.down ? 1 : 0) | (f.lid ? 2 : 0));
  std::memcpy(p + 5, f.mic, 8);
}
input::Frame get_rec(const u8* p) {
  input::Frame f;
  std::memcpy(&f.buttons, p, 2);
  f.x = p[2];
  f.y = p[3];
  f.down = (p[4] & 1) != 0;
  f.lid = (p[4] & 2) != 0;
  std::memcpy(f.mic, p + 5, 8);
  return f;
}
} // namespace

InputLink::~InputLink() {
  if (fd_ >= 0 && sender_ && have_peer_) { // so the other handheld's copy of this console ends now, not later
    u8 buf[12] = {};
    std::memcpy(buf, &kBye, 4);
    for (int k = 0; k < 3; ++k) ::sendto(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(peer_), peer_len_);
  }
  if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<InputLink> InputLink::sender(const std::string& peer) {
  std::unique_ptr<InputLink> lp(new InputLink);
  InputLink& l = *lp;
  l.sender_ = true;
  const auto colon = peer.rfind(':');
  if (colon == std::string::npos) {
    l.err_ = "want host:port, got " + peer;
    return lp;
  }
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(peer.substr(0, colon).c_str(), peer.substr(colon + 1).c_str(), &hints, &res) != 0 || !res) {
    l.err_ = "cannot resolve " + peer;
    return lp;
  }
  l.fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  std::memcpy(l.peer_, res->ai_addr, res->ai_addrlen);
  l.peer_len_ = res->ai_addrlen;
  l.have_peer_ = true;
  freeaddrinfo(res);
  if (l.fd_ < 0) l.err_ = "no socket";
  else ::fcntl(l.fd_, F_SETFL, O_NONBLOCK);
  return lp;
}

std::unique_ptr<InputLink> InputLink::receiver(int port) {
  std::unique_ptr<InputLink> lp(new InputLink);
  InputLink& l = *lp;
  l.fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u16>(port));
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (l.fd_ < 0 || ::bind(l.fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    l.err_ = "cannot listen on port " + std::to_string(port);
    if (l.fd_ >= 0) ::close(l.fd_);
    l.fd_ = -1;
    return lp;
  }
  ::fcntl(l.fd_, F_SETFL, O_NONBLOCK);
  return lp;
}

void InputLink::send_from(u32 first) {
  if (!have_peer_ || next_ == 0) return;
  u8 buf[16 + 64 * kRecord];
  u32 count = 0;
  for (u32 n = first; n < next_ && count < 64; ++n) {
    auto it = sent_.find(n);
    if (it == sent_.end()) break;
    put(buf + 12 + count * kRecord, it->second);
    ++count;
  }
  if (!count) return;
  std::memcpy(buf, &kFrames, 4);
  std::memcpy(buf + 4, &first, 4);
  std::memcpy(buf + 8, &count, 4);
  ::sendto(fd_, buf, 12 + count * kRecord, 0, reinterpret_cast<sockaddr*>(peer_), peer_len_);
}

void InputLink::drain() {
  u8 buf[2048];
  for (;;) {
    sockaddr_storage from{};
    socklen_t fl = sizeof from;
    const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 12) return;
    u32 magic, first, count;
    std::memcpy(&magic, buf, 4);
    std::memcpy(&first, buf + 4, 4);
    std::memcpy(&count, buf + 8, 4);
    if (!sender_ && magic == kBye) {
      bye_ = true;
    } else if (sender_ && magic == kAsk) {
      for (auto it = sent_.begin(); it != sent_.end() && it->first + 64 < first;) it = sent_.erase(it);
      send_from(first);
    } else if (!sender_ && magic == kFrames && count <= 64 && static_cast<size_t>(n) >= 12 + count * kRecord) {
      std::memcpy(peer_, &from, fl); // where asks go
      peer_len_ = fl;
      have_peer_ = true;
      for (u32 k = 0; k < count; ++k)
        if (first + k >= need_) got_.emplace(first + k, get_rec(buf + 12 + k * kRecord));
    }
  }
}

void InputLink::push(u32 n, const input::Frame& f) {
  if (fd_ < 0) return;
  sent_[n] = f;
  next_ = n + 1;
  drain(); // asks
  send_from(next_ > kRepeat ? next_ - kRepeat : 0);
  // Keep enough to answer an ask from the last few seconds.
  while (!sent_.empty() && sent_.begin()->first + 600 < next_) sent_.erase(sent_.begin());
}

bool InputLink::get(u32 n, input::Frame& f, int give_up_ms) {
  if (fd_ < 0) return false;
  drain();
  auto it = got_.find(n);
  if (it == got_.end()) {
    ++waits_;
    const auto t0 = std::chrono::steady_clock::now();
    auto last_ask = t0;
    for (;;) {
      pollfd p{fd_, POLLIN, 0};
      ::poll(&p, 1, 5);
      drain();
      it = got_.find(n);
      if (it != got_.end()) break;
      if (bye_) return false;
      const auto now = std::chrono::steady_clock::now();
      if (now - t0 > std::chrono::milliseconds(give_up_ms)) return false;
      if (have_peer_ && now - last_ask > std::chrono::milliseconds(kAskMs)) {
        u8 buf[12];
        const u32 zero = 0;
        std::memcpy(buf, &kAsk, 4);
        std::memcpy(buf + 4, &n, 4);
        std::memcpy(buf + 8, &zero, 4);
        ::sendto(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(peer_), peer_len_);
        last_ask = now;
      }
    }
    wait_ns_ += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
  }
  f = it->second;
  got_.erase(got_.begin(), std::next(it)); // the frames before n are behind us
  need_ = n + 1;
  return true;
}

u64 ram_hash(const u8* ram, size_t bytes) {
  u64 h = 1469598103934665603ull;
  for (size_t i = 0; i + 8 <= bytes; i += 8) {
    u64 w;
    std::memcpy(&w, ram + i, 8);
    h = (h ^ w) * 1099511628211ull;
  }
  return h;
}

void log_ram_hash(u32 frame, const u8* ram, size_t bytes, std::FILE* to) {
  static const bool on = !std::getenv("DS_TWIN_HASH") || std::strcmp(std::getenv("DS_TWIN_HASH"), "0") != 0;
  if (!on || frame % 60) return;
  const auto h = static_cast<unsigned long long>(ram_hash(ram, bytes));
  std::fprintf(stderr, "[twin-hash] frame %u %016llx\n", frame, h);
  if (to) std::fprintf(to, "[twin-hash] frame %u %016llx\n", frame, h), std::fflush(to);
}

} // namespace ds::net
