// SPDX-License-Identifier: GPL-3.0-or-later
// Laying a game picture onto a panel, and writing panels to image files.
#pragma once
#include "core/core.hpp"
#include "ui/canvas.hpp"

#include <string>

namespace play {

enum class Fit { Integer, Fill };

// The game picture centred on the panel: the largest whole multiple, or as
// large as fits (sharp: whole pixels, the fraction spread evenly).
void draw_game(Canvas& panel, const Picture& game, Fit fit);
bool write_bmp(const std::string& path, const Canvas& top, const Canvas* bottom = nullptr);
uint64_t picture_hash(const Picture& p);

} // namespace play
