// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Core frontend requests. All emulator UI is rendered by RGDS.
#pragma once
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
        return live && std::strcmp(live, "1") == 0 && control && *control ? std::string(control) + ".blocked" : std::string();
    }();
    if (blocked.empty()) return false;
    using clock = std::chrono::steady_clock;
    static clock::time_point next{};
    static bool held = false;
    const auto now = clock::now();
    if (now >= next) { next = now + std::chrono::milliseconds(20); held = ::access(blocked.c_str(), F_OK) == 0; }
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
inline std::function<bool(int, bool, int)> graphics_handler;
inline std::function<bool(int)> filter2d_handler;
// Heavy 3D scenes: 1 adaptive (native during heavy stretches), 0 always sharp. Live.
inline std::function<bool(int)> adaptive_handler;
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
    if (scale < 1 || scale > 5 || smooth < 0 || smooth > 1 || skip < 0 || skip > 3 || !rgds::graphics_handler) return 0;
    try { return rgds::graphics_handler(scale, smooth != 0, skip) ? 1 : 0; }
    catch (const std::exception& e) { std::fprintf(stderr, "RG DS graphics: %s\n", e.what()); return 0; }
}

// Called by RGDS on the SDL/emulation thread, between emulated frames.
// 0 native grid, 1 sharp, 2 smooth, 3 crisp; applies to the enhanced (2x-5x) panel image.
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_filter2d(int mode) {
    if (mode < 0 || mode > 3 || !rgds::filter2d_handler) return 0;
    try { return rgds::filter2d_handler(mode) ? 1 : 0; }
    catch (const std::exception& e) { std::fprintf(stderr, "RG DS 2D filter: %s\n", e.what()); return 0; }
}

// Called by RGDS on the SDL/emulation thread. 1 adaptive, 0 always sharp;
// takes effect at the next frame (a running native hold ends at once).
extern "C" __attribute__((used, visibility("default"))) inline int rgds_set_adaptive(int on) {
    if (on < 0 || on > 1 || !rgds::adaptive_handler) return 0;
    try { return rgds::adaptive_handler(on) ? 1 : 0; }
    catch (const std::exception& e) { std::fprintf(stderr, "RG DS heavy 3D scenes: %s\n", e.what()); return 0; }
}
