// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RetroAchievements for the SDL frontend: [cheevos] settings, a token-only
// sign-in, the game's identity, the per-frame calls and the notices. Notices
// go to stderr and, under RG DS, to the bridge's rgds_toast, which draws them
// on the lower (touch) panel: this engine draws no UI of its own.
#pragma once

#include <string>

#include "cheevos/cheevos_client.h" // also declares ds::NDS

namespace ds::sdl {

class Config;

class CheevosSession {
public:
  // Reads [cheevos]; false (and nothing started) when it is off for this run
  // or there is no transport. Never asks for a password: the sign-in is the
  // token file's, the one stored beside the config, or the CFW's.
  bool begin(const Config& cfg);
  bool on() const { return on_; }

  // The running game's identity; `dsi_hash` stands in when the title runs
  // from the NAND (no cart). Asked for once the sign-in completes.
  void identify(NDS& nds, const std::string& name, const std::string& dsi_hash);

  void frame(NDS& nds); // exactly once per emulated frame, right after run_frame()
  void idle(NDS& nds);  // instead of frame() while the game is held
  void end();

  cheevos::Client& client() { return client_; }

private:
  void catch_up(NDS& nds);
  void show();

  cheevos::Client client_;
  bool on_ = false, toasts_ = true, set_asked_ = false, offline_shown_ = false;
  std::string hash_;
};

} // namespace ds::sdl
