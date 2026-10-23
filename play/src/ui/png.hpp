// SPDX-License-Identifier: GPL-3.0-or-later
// Just enough PNG for picture frames (RetroArch overlays): 8-bit RGBA, RGB,
// grey with or without alpha and palettes, not interlaced. Pixels come out as
// the canvas has them (bytes R, G, B, A).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace play {

bool read_png(const std::string& path, std::vector<uint32_t>& pixels, int& width, int& height, std::string& error);

} // namespace play
