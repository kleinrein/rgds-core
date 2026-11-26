// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A two-console session (docs/NETPLAY_PLAN.md, net.mode = twin): this handheld runs its own console, shown, and the
// other player's, hidden, as a headless child process (`rgds-twin`). The two talk local wireless through TwinMp;
// only each console's inputs cross the network (InputLink), D frames ahead, so the other handheld's copy of this
// console computes exactly what this one does.
//
// Start: the handhelds meet over LanMp (discovery and its reliable channel), and each sends the other its seat:
// what its console needs to boot identically on both (save, firmware user, MAC suffix, emulation settings). The
// host adds the moment both consoles' clocks start from. Then LanMp goes; nothing of the lockstep protocol runs.
#pragma once

#include "core/bios/freebios.h"
#include "core/input/input_log.h"
#include "core/types.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <sys/types.h>

namespace ds::net {
class TwinMp;
class InputLink;
} // namespace ds::net

namespace ds::sdl {

class TwinSession {
public:
  struct Params {
    std::string rom;         // the same file on both handhelds (its name and size are checked)
    std::string save;        // this console's battery save (may not exist yet)
    std::string fw_override; // this console's firmware Wi-Fi sidecar (may not exist)
    std::string name;        // shown while finding the other handheld
    bios::UserSettings user;
    u32 mac_suffix = 0;
    std::string timing;    // emu.timing as this engine runs it ("" = the core's default)
    std::string idle_skip; // emu.idle_skip ("" = default)
    bool jit = true;
    int delay = 3;            // frames of input delay
    std::string child;        // the headless binary that runs the hidden console
    int child_core = 2;       // the hidden console's core (-1: anywhere)
    std::function<u64()> now; // this console's scheduler clock
  };

  // Finds the other handheld (up to `scan_ms` listening, then hosting and waiting up to `wait_ms` for one),
  // trades seats and starts the hidden console. Null, with `err` set, when there is no session: the game then
  // runs alone.
  static std::unique_ptr<TwinSession> start(const Params& p, int scan_ms, int wait_ms, std::string& err);
  ~TwinSession();

  bool host() const { return host_; }
  const std::string& peer_name() const { return peer_name_; }
  net::TwinMp* link() const { return link_.get(); }
  // The clock both copies of this console start from (Io::start_rtc_clock_at).
  const u8* rtc() const { return rtc_; }

  // Once a frame, before the console runs it: takes this frame's pad reading, sends it on (it applies `delay`
  // frames later) and returns what this frame applies.
  input::Frame frame(const input::Frame& pad);
  // After the frame: this console's clock, for the hidden one (and DS_TWIN_HASH's line).
  void after_frame(const u8* ram, size_t bytes);
  // False once the hidden console has stopped (the other handheld left).
  bool child_running();

private:
  TwinSession() = default;
  bool host_ = false;
  std::string peer_name_;
  std::unique_ptr<net::TwinMp> link_;
  std::unique_ptr<net::InputLink> inputs_;
  std::map<u32, input::Frame> pending_;
  u32 frame_ = 0;
  int delay_ = 3;
  u8 rtc_[7] = {};
  pid_t child_ = -1;
  std::string tmp_; // the session's files (twin block, the other's save and sidecar)
};

} // namespace ds::sdl
