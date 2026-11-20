// SPDX-License-Identifier: GPL-3.0-or-later
// A game's scanned manual on the second screen (docs/MANUALS_PLAN.md): one
// more screen behind the companion's switch. Pages are read and decoded on a
// thread of their own, the one shown first and its neighbours next, so the
// game's frame never waits for a JPEG. A two-page spread is shown a half at a
// time. One finger: swipe or tap an edge to turn, drag to pan when zoomed,
// double tap to cycle fit page, fit width and the scan's own pixels.
#pragma once
#include "companion/view.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace play {

// Two fingers on the second screen (the panel's own reader, beside the one-finger touches): every finger down
// now, in panel pixels, after each poll. True when what is drawn changed.
class Pinch {
public:
    virtual ~Pinch() = default;
    virtual bool fingers(const std::vector<std::pair<int, int>>& down) = 0;
};

// The manuals for a game, best first: <rom stem>.pdf / .cbz / .manual/ beside the ROM (a zipped ROM by the zip's
// name), then manual.pdf / .cbz / manual/ in its library folder (`art_dir`, may be empty).
std::vector<std::string> find_manuals(const std::string& rom, const std::string& art_dir);

// The reader for `path`; the page each manual was left on is kept in `place_file` (may be empty).
std::unique_ptr<View> make_manual(const std::string& path, const std::string& place_file);

} // namespace play
