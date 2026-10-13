// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Core frontend requests. All emulator UI is rendered by RGDS.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>

namespace rgds {
inline std::function<bool(int, bool, int)> graphics_handler;
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
