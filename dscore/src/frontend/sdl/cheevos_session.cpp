// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos_session.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <dlfcn.h>

#include "cheevos/cheevos_hash.h"
#include "core/nds.h"
#include "config.h"

namespace ds::sdl {
namespace {

// The RG DS bridge's notice (src/platform/emulator_bridge.cpp): kind 0 an
// unlock, 1 news, 2 a problem. Absent without the bridge.
using ToastHook = void (*)(int kind, const char* title, const char* detail, int points);
ToastHook toast_hook() {
  static const ToastHook hook = reinterpret_cast<ToastHook>(::dlsym(RTLD_DEFAULT, "rgds_toast"));
  return hook;
}

} // namespace

bool CheevosSession::begin(const Config& cfg) {
  on_ = false;
  if (!cfg.flag("cheevos.enabled", false)) return false;
  toasts_ = cfg.flag("cheevos.toasts", true);

  const std::string helper = cfg.str("cheevos.helper");
  if (!helper.empty()) {
    cheevos::HelperOptions h;
    h.script = helper;
    h.badges = cfg.str("cheevos.badges");
    if (h.badges.empty()) h.badges = Config::dir() + "/cheevos-badges";
    client_.set_helper(h);
  }
  client_.set_badges_dir(cfg.str("cheevos.badges"));
  client_.set_list_path(cfg.str("cheevos.list"));

  std::string err;
  if (!client_.start(err)) {
    std::fprintf(stderr, "cheevos: %s\n", client_.unavailable_reason().c_str());
    return false;
  }
  // Before anything loads: rcheevos reads encore at game load only.
  client_.set_encore(cfg.flag("cheevos.encore", false));

  // A password is never asked for or kept; an expired token fails, and the
  // player signs in again in RG DS.
  cheevos::Credentials creds;
  const std::string token_file = cfg.str("cheevos.token_file");
  if (!token_file.empty()) {
    if (!cheevos::read_token_file(token_file, creds, err)) {
      std::fprintf(stderr, "cheevos: %s\n", err.c_str());
    } else {
      if (creds.username.empty()) creds.username = cfg.str("cheevos.username");
      if (creds.username.empty()) {
        std::fprintf(stderr, "cheevos: %s has a token but no username; pass --cheevos-user or set cheevos.username\n",
                     token_file.c_str());
        creds = cheevos::Credentials{};
      } else {
        std::fprintf(stderr, "cheevos: using the token from %s\n", token_file.c_str());
      }
    }
  }
  if (creds.empty() && token_file.empty() && !cheevos::load_credentials(Config::dir(), creds, err) && !err.empty())
    std::fprintf(stderr, "cheevos: %s\n", err.c_str());
  if (creds.empty() && token_file.empty() && cfg.flag("cheevos.use_system_login", true)) {
    std::string from;
    if (cheevos::import_cfw_credentials(creds, from))
      std::fprintf(stderr, "cheevos: using the sign-in from %s\n", from.c_str());
  }
  if (creds.empty()) {
    std::fprintf(stderr, "cheevos: not signed in\n");
    client_.shutdown();
    return false;
  }
  client_.sign_in_with_token(creds.username, creds.token);
  std::fprintf(stderr, "cheevos: transport %s\n", client_.transport_name());
  on_ = true;
  return true;
}

void CheevosSession::identify(NDS& nds, const std::string& name, const std::string& dsi_hash) {
  if (!on_) return;
  hash_.clear();
  set_asked_ = false;
  if (!nds.cart) {
    hash_ = dsi_hash;
    return;
  }
  std::string err;
  // Safe here: read_unpatched reads past the secure-area rewrite the cart has done.
  if (!cheevos::rom_hash(nds.cart->source(), name, hash_, err, nds.dsi)) {
    std::fprintf(stderr, "cheevos: cannot identify this ROM: %s\n", err.c_str());
    hash_.clear();
  }
}

// The set is asked for when the (asynchronous) sign-in has completed.
void CheevosSession::catch_up(NDS& nds) {
  if (set_asked_ || hash_.empty() || client_.state() != cheevos::State::SignedIn) return;
  set_asked_ = true;
  client_.load_game(nds, hash_);
}

void CheevosSession::frame(NDS& nds) {
  if (!on_) return;
  client_.frame();
  catch_up(nds);
  show();
}

void CheevosSession::idle(NDS& nds) {
  if (!on_) return;
  client_.idle();
  catch_up(nds);
  show();
}

void CheevosSession::end() {
  if (!on_) return;
  show();
  client_.shutdown();
  on_ = false;
}

// Every message goes to the log. On the panel: unlocks, the set's count when
// it loads, a finished set, and what stops achievements from counting. The
// rest (signed in, no set for this dump) stays in the log.
void CheevosSession::show() {
  using Kind = cheevos::Message::Kind;
  for (const cheevos::Message& m : client_.take_messages()) {
    std::fprintf(stderr, "cheevos: %s%s%s\n", m.text.c_str(), m.detail.empty() ? "" : " -- ", m.detail.c_str());
    if (!toasts_ || !toast_hook()) continue;
    // The notice's header already says RetroAchievements.
    std::string title = m.text;
    if (title.rfind("RetroAchievements ", 0) == 0) {
      title.erase(0, std::strlen("RetroAchievements "));
      if (!title.empty() && title[0] >= 'a' && title[0] <= 'z') title[0] = static_cast<char>(title[0] - 'a' + 'A');
    }
    if (m.kind == Kind::Unlock) {
      toast_hook()(0, m.text.c_str(), "", static_cast<int>(m.points));
    } else if (m.text == "All achievements earned") {
      toast_hook()(1, m.text.c_str(), m.detail.c_str(), 0);
    } else if (m.kind == Kind::Info && m.detail == "achievements active") {
      const cheevos::Client::Summary s = client_.summary();
      char line[64];
      std::snprintf(line, sizeof line, "%u of %u achievements", s.unlocked, s.total);
      toast_hook()(1, line, "", 0);
    } else if (m.text == "RetroAchievements offline") {
      if (!offline_shown_) toast_hook()(2, title.c_str(), "Unlocks are sent when it is back", 0);
      offline_shown_ = true;
    } else if (m.text == "RetroAchievements reconnected") {
      if (offline_shown_) toast_hook()(1, title.c_str(), "", 0);
      offline_shown_ = false;
    } else if (m.kind == Kind::Problem && m.text != "No achievements for this ROM") {
      toast_hook()(2, title.c_str(), m.detail.c_str(), 0);
    }
  }
}

} // namespace ds::sdl
