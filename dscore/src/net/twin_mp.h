// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Local wireless between two consoles on one machine, decided by game time alone (docs/NETPLAY_PLAN.md).
//
// The two consoles are two processes (the JIT runtime is one per process) that booted together, so their
// scheduler clocks are one timeline. They share a file-backed block: a message ring each way and each side's
// published clock. The rules, which make every answer a function of the two games and never of the machine:
//   - a message sent at game time s reaches the other console at s + kLead;
//   - a console at time c may conclude "nothing yet" only once the other's published clock has passed
//     c - kLead (nothing older can still come), so it waits for that;
//   - the host collects replies to a CMD until they are in or the other's clock has passed the reply window.
// Neither side can wait on the other at once: each waits only for the other to be within kLead of it.
// Otherwise the queue rules are LanMp's, so the DS side behaves as it does over the LAN.
#pragma once

#include "core/io/wifi_transport.h"

#include <functional>
#include <string>

namespace ds::net {

class TwinMp final : public io::MpTransport {
public:
  // `path` names the shared block: the host makes it, the guest opens it (waiting up to `wait_ms` for it).
  // `now` is this console's scheduler clock (NDS::sched.now()).
  TwinMp(const std::string& path, bool host, std::function<u64()> now, int wait_ms = 10000);
  ~TwinMp() override;
  bool ok() const { return shm_ != nullptr; }
  const std::string& error() const { return err_; }
  bool is_host() const { return host_; }

  // Says how far this console has run; the frontend calls it after every frame, so the other is never kept
  // waiting longer than a frame by a console whose radio is off.
  void publish();

  void begin() override {}
  void end() override {}
  void tick() override { publish(); }
  int send_packet(const u8* data, int len, u64 timestamp) override { return send(0, data, len, timestamp); }
  int recv_packet(u8* data, u64* timestamp) override;
  int send_cmd(const u8* data, int len, u64 timestamp) override;
  int send_reply(const u8* data, int len, u64 timestamp, u16 aid) override {
    return send(2 | (u32(aid) << 16), data, len, timestamp);
  }
  int send_ack(const u8* data, int len, u64 timestamp) override { return send(3, data, len, timestamp); }
  int recv_host_packet(u8* data, u64* timestamp) override;
  int peek_host_packet(u8* data, u64* timestamp) override;
  u16 recv_replies(u8* data, u64 timestamp, u16 aidmask) override;

  // Scheduler ticks (ARM9 cycles, 67.03 MHz) a message takes to arrive: 512 us, or DS_TWIN_LEAD_US (both consoles
  // of a session must agree on it). The slack lets the two run side by side: at 128 us they took turns and a MKDS
  // race frame cost 10.6 ms on the RG DS Plus, at 512 us 7.4 ms (the host console's own cost), 2 ms no better.
  static constexpr u64 kDefaultLead = 34318;
  // Spins on the other's clock, for the log.
  unsigned long long waits() const { return waits_; }
  double wait_ms() const { return wait_ns_ / 1e6; }

private:
  struct Block;
  struct Msg;
  int send(u32 type, const u8* data, int len, u64 timestamp);
  // The oldest message from the other console that has arrived by game time `by`, after waiting until no older
  // one can still come; null when there is none.
  const Msg* front(u64 by);
  void pop();
  // Waits until the other's clock is at least `t`.
  void wait_for(u64 t);
  int take(const Msg* m, u8* data, u64* timestamp);

  std::string err_;
  bool host_ = false;
  std::function<u64()> now_;
  Block* shm_ = nullptr;
  size_t shm_size_ = 0;
  u64 kLead = kDefaultLead;
  int me_ = 0; // ring index: 0 = host's sends, 1 = guest's sends
  unsigned long long waits_ = 0;
  double wait_ns_ = 0;
};

} // namespace ds::net
