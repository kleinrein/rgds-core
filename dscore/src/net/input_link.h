// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One console's inputs between the two handhelds of a two-console session (docs/NETPLAY_PLAN.md). Each console
// runs on both handhelds: on its player's, it reads the pad and sends what it applies (sender); on the other's it
// applies exactly that (receiver). The sender applies a pad reading D frames later than it was read, so the
// receiver gets frame n's input while it is still D frames away.
//
// UDP. Each packet repeats the last kRepeat frames, so a lost packet costs nothing; a receiver still missing a
// frame after kAskMs asks the sender to send again from it.
#pragma once

#include "core/input/input_log.h"

#include <map>
#include <memory>
#include <string>

namespace ds::net {

class InputLink {
public:
  // Sender: `peer` is "host:port" of the receiving handheld. Receiver: `peer` is empty, `port` is ours.
  static std::unique_ptr<InputLink> sender(const std::string& peer);
  static std::unique_ptr<InputLink> receiver(int port);
  ~InputLink();
  InputLink(const InputLink&) = delete;
  InputLink& operator=(const InputLink&) = delete;
  bool ok() const { return fd_ >= 0; }
  const std::string& error() const { return err_; }

  // Sender: frame n's input (n counts up from 0, one call per frame).
  void push(u32 n, const input::Frame& f);
  // Receiver: frame n's input, waiting for it (up to `give_up_ms`); false if it never came.
  bool get(u32 n, input::Frame& f, int give_up_ms = 15000);

  double wait_ms() const { return wait_ns_ / 1e6; }
  unsigned waits() const { return waits_; }

  static constexpr u32 kRepeat = 8;
  static constexpr int kAskMs = 50;

private:
  InputLink() = default;
  void send_from(u32 first);
  void drain(); // reads what has come in, without waiting

  int fd_ = -1;
  bool sender_ = false;
  std::string err_;
  // Sender: what was pushed, by frame (frames below the receiver's ask are kept until it asks no more).
  std::map<u32, input::Frame> sent_;
  u32 next_ = 0; // frames pushed so far
  // Receiver: what came, by frame, and where replies to an ask go.
  std::map<u32, input::Frame> got_;
  u32 need_ = 0; // frames below this are used: late copies of them are ignored
  bool have_peer_ = false;
  unsigned char peer_[128] = {}; // sockaddr_storage, kept opaque here
  unsigned peer_len_ = 0;
  double wait_ns_ = 0;
  unsigned waits_ = 0;
};

// A quick hash of a console's main RAM, logged every 60 frames of a session by both frontends (DS_TWIN_HASH=0: not),
// so the two copies of a console can be compared after a session ("[twin-hash] frame N H"). ~1 ms a second.
u64 ram_hash(const u8* ram, size_t bytes);
void log_ram_hash(u32 frame, const u8* ram, size_t bytes);

} // namespace ds::net
