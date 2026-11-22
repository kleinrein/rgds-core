// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace play {

// A finger on the second screen, in its pixels.
struct Touch {
    enum Kind { Down, Move, Up } kind = Down;
    int x = 0, y = 0;
};

} // namespace play
