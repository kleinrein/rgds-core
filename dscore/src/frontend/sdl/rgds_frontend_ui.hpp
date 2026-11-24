// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Core frontend requests. All emulator UI is rendered by RGDS.
#pragma once
#include "core/gpu/rgds_sample_plan.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>

namespace rgds {
// RGDS_MENU_LIVE=1 (RG DS runs this engine on KMS, with no compositor for a
// paused-menu window): RG DS draws its menu into the shared overlay frame
// and the bridge composites it over each present. True while RG DS blocks
// game input for that menu (its control file's ".blocked" flag), polled at
// most every 20 ms. The game holds still meanwhile; see the main loop.
inline bool menu_hold() {
  static const std::string blocked = [] {
    const char* live = std::getenv("RGDS_MENU_LIVE");
    const char* control = std::getenv("RGDS_CONTROL_FILE");
    return live && std::strcmp(live, "1") == 0 && control && *control ? std::string(control) + ".blocked"
                                                                      : std::string();
  }();
  if (blocked.empty()) return false;
  using clock = std::chrono::steady_clock;
  static clock::time_point next{};
  static bool held = false;
  const auto now = clock::now();
  if (now >= next) {
    next = now + std::chrono::milliseconds(20);
    held = ::access(blocked.c_str(), F_OK) == 0;
  }
  return held;
}
// The overlay frame's publish counter (platform/overlay_frame.hpp: magic,
// width, height, sequence...), so a held engine presents again when the menu
// changes. 0 until the menu helper has created the frame.
inline uint32_t overlay_sequence() {
  static int fd = -1;
  if (fd < 0) {
    const char* path = std::getenv("RGDS_OVERLAY_FILE");
    if (!path || !*path) return 0;
    fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
  }
  uint32_t sequence = 0;
  return ::pread(fd, &sequence, sizeof sequence, 12) == static_cast<ssize_t>(sizeof sequence) ? sequence : 0;
}
// RG DS direct present (Display::draw_scanout_async): on a scanout tier the
// engine composes each panel's finished frame on a canvas of its own and,
// before copying it into the buffer that is page-flipped, hands it to this
// hook of the RG DS bridge (LD_PRELOAD): the bridge blends its overlay (pause
// menu, HUD) into it, serves the launcher's capture requests from it and
// times the presentation for the FPS counter and the benchmark tool -- what
// it did inside its SDL_RenderPresent hook on the renderer tier. `display` is
// the panel's SDL display index (the bridge's panel index), the pixels are
// 0xAARRGGBB rows, `w` x `h` at `pitch` words. Null without the bridge.
using PresentHook = void (*)(int display, uint32_t* px, int w, int h, int pitch);
inline PresentHook present_hook() {
  static const PresentHook hook = reinterpret_cast<PresentHook>(::dlsym(RTLD_DEFAULT, "rgds_present_canvas"));
  return hook;
}
// The DS Rumble Pak: in the slot when RG DS Core's Game rumble is on
// (RGDS_RUMBLE, its strength, 0 off), or DS_RUMBLE_PAK=1 on its own.
inline bool rumble_pak() {
  const char* rgds = std::getenv("RGDS_RUMBLE");
  const char* own = std::getenv("DS_RUMBLE_PAK");
  return (rgds && std::atoi(rgds) > 0) || (own && std::strcmp(own, "1") == 0);
}
// A frame's Rumble Pak moves to the RG DS motor through the bridge's
// rgds_rumble (0-100; the bridge applies the strength and lets the motor stop
// by itself if frames stop coming). A move is felt for about a frame, so a game
// that moves it every frame holds it on; one call more when it stops.
inline void rumble_frame(uint32_t moves) {
  using Hook = void (*)(int);
  static const Hook hook = reinterpret_cast<Hook>(::dlsym(RTLD_DEFAULT, "rgds_rumble"));
  static bool on = false;
  if (!hook || (!moves && !on)) return;
  on = moves != 0;
  hook(on ? 100 : 0);
}
inline std::function<bool(int, bool, int)> graphics_handler;
inline std::function<bool(int)> filter2d_handler;
// Heavy 3D scenes: 1 adaptive (native during heavy stretches), 0 always sharp. Live.
inline std::function<bool(int)> adaptive_handler;
// Online play: 0 off, 1 Wiimmfi, 2 WiiLink WFC. Live: the emulated access point always beacons, and this
// swaps the host network behind it. False when local wireless holds the radio or the stack cannot start.
inline std::function<bool(int)> online_handler;
// Cheats: the Action Replay codes loaded for the cart, listed and toggled from
// the RG DS game menu. Text pointers are the engine's own strings; they stay
// valid until the cart changes (the loader cart's reset), and the bridge copies
// them within the same call anyway.
struct CheatEntry {
  const char* name = "";
  const char* description = "";
  int group = -1;
  int state = 0;
}; // state: -1 a note without code words, 0 off, 1 on
struct CheatGroup {
  const char* name = "";
  int exclusive = 0;
};
inline std::function<int()> cheat_count_handler, cheat_group_count_handler;
inline std::function<bool(int, CheatEntry&)> cheat_entry_handler;
inline std::function<bool(int, CheatGroup&)> cheat_group_handler;
inline std::function<bool(int, bool)> cheat_set_handler;
// Rewind (docs/TIME_TRAVEL_PLAN.md in RG DS Core): the moments the engine keeps, listed for RG DS's game menu, which
// draws the Rewind screen. open marks a moment now and returns its id (0: none, e.g. during a replay or a network
// session); show loads a moment and runs one silent frame, handing back its screens (bytes R, G, B, X; screens * w *
// h pixels) for the menu to show; jump plays from a moment (play) or returns to now (the menu's Back), and ends the
// previews. All on the emulation thread, between frames.
struct RewindMoment {
  unsigned id = 0, parent = 0;
  unsigned long long position = 0;
  int w = 0, h = 0;
  const uint32_t* px = nullptr;
};
inline std::function<unsigned()> rewind_open_handler;
inline std::function<int()> rewind_count_handler;
inline std::function<bool(int, RewindMoment&)> rewind_moment_handler;
inline std::function<double()> rewind_rate_handler;
inline std::function<bool(unsigned, int&, int&, int&, const uint32_t*&)> rewind_show_handler;
inline std::function<bool(unsigned, bool)> rewind_jump_handler;
inline bool request_menu() {
  const char* control = std::getenv("RGDS_CONTROL_FILE");
  if (!control || !*control) {
    std::fprintf(stderr, "RG DS menu unavailable: start this engine from RG DS Core.\n");
    return false;
  }
  const std::string destination = std::string(control) + ".game-menu";
  const std::string temporary = destination + ".engine-" + std::to_string(getpid());
  std::ofstream request(temporary, std::ios::trunc);
  request << "RGDS\n";
  request.close();
  if (!request || std::rename(temporary.c_str(), destination.c_str()) != 0) {
    std::remove(temporary.c_str());
    std::fprintf(stderr, "RG DS menu request failed.\n");
    return false;
  }
  return true;
}
}

// Called by RGDS on the SDL/emulation thread, between emulated frames.
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_graphics(int scale, int smooth, int skip) {
  if (!rgds::valid_scale(scale) || smooth < 0 || smooth > 1 || skip < 0 || skip > 3 || !rgds::graphics_handler)
    return 0;
  try {
    return rgds::graphics_handler(scale, smooth != 0, skip) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS graphics: %s\n", e.what());
    return 0;
  }
}

// Called by RGDS on the SDL/emulation thread, between emulated frames.
// 0 native grid, 1 sharp, 2 smooth, 3 crisp (the 3D screen at 2x-5x); 4 HD (both screens, every scale).
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_filter2d(int mode) {
  if (mode < 0 || mode > 5 || !rgds::filter2d_handler) return 0;
  try {
    return rgds::filter2d_handler(mode) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS 2D filter: %s\n", e.what());
    return 0;
  }
}

// Called by RGDS on the SDL/emulation thread. 1 adaptive, 0 always sharp;
// takes effect at the next frame (a running native hold ends at once).
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_adaptive(int on) {
  if (on < 0 || on > 1 || !rgds::adaptive_handler) return 0;
  try {
    return rgds::adaptive_handler(on) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS heavy 3D scenes: %s\n", e.what());
    return 0;
  }
}

// Called by RGDS on the SDL/emulation thread between emulated frames: 0 off, 1 Wiimmfi, 2 WiiLink WFC.
// 0 when it could not apply; RGDS then relaunches the game from a checkpoint with the new setting.
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_online(int mode) {
  if (mode < 0 || mode > 2 || !rgds::online_handler) return 0;
  try {
    return rgds::online_handler(mode) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS online play: %s\n", e.what());
    return 0;
  }
}

// Cheats, called by RGDS on the SDL/emulation thread between emulated frames.
// Counts are 0 without a cart or a database entry for it.
extern "C" __attribute__((used, visibility("default"))) inline int rgds_cheat_count() {
  return rgds::cheat_count_handler ? rgds::cheat_count_handler() : 0;
}
extern "C" __attribute__((used, visibility("default"))) inline int rgds_cheat_group_count() {
  return rgds::cheat_group_count_handler ? rgds::cheat_group_count_handler() : 0;
}
// 1 with the fields filled in, 0 for an index out of range.
extern "C" __attribute__((used, visibility("default"))) inline int
rgds_cheat_entry(int index, const char** name, const char** description, int* group, int* state) {
  rgds::CheatEntry entry;
  if (!rgds::cheat_entry_handler || !rgds::cheat_entry_handler(index, entry)) return 0;
  if (name) *name = entry.name;
  if (description) *description = entry.description;
  if (group) *group = entry.group;
  if (state) *state = entry.state;
  return 1;
}
extern "C" __attribute__((used, visibility("default"))) inline int rgds_cheat_group(int index, const char** name,
                                                                                    int* exclusive) {
  rgds::CheatGroup group;
  if (!rgds::cheat_group_handler || !rgds::cheat_group_handler(index, group)) return 0;
  if (name) *name = group.name;
  if (exclusive) *exclusive = group.exclusive;
  return 1;
}
// Turns a code on (1) or off (0); 0 for a note, an index out of range or no cart.
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_cheat(int index, int on) {
  if (on < 0 || on > 1 || !rgds::cheat_set_handler) return 0;
  try {
    return rgds::cheat_set_handler(index, on != 0) ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS cheat: %s\n", e.what());
    return 0;
  }
}

// Rewind, called by RGDS on the SDL/emulation thread between emulated frames (the RewindMoment notes above). Nothing
// thrown gets past these (a moment is ~6 MB: out of memory is the likely one): the menu hears "no", the game goes on.
namespace rgds {
template <class F, class R> inline R rewind_safely(F&& f, R failed) {
  try {
    return f();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "RG DS rewind: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "RG DS rewind: failed\n");
  }
  return failed;
}
} // namespace rgds
extern "C" __attribute__((used, visibility("default"))) inline unsigned rgds_rewind_open() {
  return rgds::rewind_safely([] { return rgds::rewind_open_handler ? rgds::rewind_open_handler() : 0u; }, 0u);
}
extern "C" __attribute__((used, visibility("default"))) inline int rgds_rewind_count() {
  return rgds::rewind_count_handler ? rgds::rewind_count_handler() : 0;
}
extern "C" __attribute__((used, visibility("default"))) inline int
rgds_rewind_moment(int index, unsigned* id, unsigned* parent, unsigned long long* position, int* w, int* h,
                   const uint32_t** px) {
  rgds::RewindMoment m;
  if (!rgds::rewind_moment_handler ||
      !rgds::rewind_safely([&] { return rgds::rewind_moment_handler(index, m); }, false))
    return 0;
  if (id) *id = m.id;
  if (parent) *parent = m.parent;
  if (position) *position = m.position;
  if (w) *w = m.w;
  if (h) *h = m.h;
  if (px) *px = m.px;
  return 1;
}
extern "C" __attribute__((used, visibility("default"))) inline double rgds_rewind_rate() {
  return rgds::rewind_rate_handler ? rgds::rewind_rate_handler() : 60.0;
}
extern "C" __attribute__((used, visibility("default"))) inline int rgds_rewind_show(unsigned id, int* screens, int* w,
                                                                                    int* h, const uint32_t** px) {
  int s = 0, pw = 0, ph = 0;
  const uint32_t* p = nullptr;
  if (!rgds::rewind_show_handler ||
      !rgds::rewind_safely([&] { return rgds::rewind_show_handler(id, s, pw, ph, p); }, false))
    return 0;
  if (screens) *screens = s;
  if (w) *w = pw;
  if (h) *h = ph;
  if (px) *px = p;
  return 1;
}
extern "C" __attribute__((used, visibility("default"))) inline int rgds_rewind_jump(unsigned id, int play) {
  return rgds::rewind_jump_handler &&
                 rgds::rewind_safely([&] { return rgds::rewind_jump_handler(id, play != 0); }, false)
             ? 1
             : 0;
}
