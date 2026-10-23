// SPDX-License-Identifier: GPL-3.0-or-later
// A second screen the player draws itself from what the game keeps in memory
// (and on its cartridge), for games whose own screens have nothing to show
// there: an explored-area map for Metroid II, a monster party, a farm diary.
// A profile's [view] names the format and the addresses it reads.
#pragma once
#include "companion/profile.hpp"
#include "ui/canvas.hpp"

#include <memory>
#include <string>

namespace play {

class Core;
struct Look;

class View {
public:
    virtual ~View() = default;
    // After every live frame.
    virtual void update(Core& live, uint64_t frame) = 0;
    // Something to show (in play, with what the view needs read).
    virtual bool shown() const = 0;
    // Changes whenever draw() would draw something different.
    virtual uint64_t key(uint64_t frame) const = 0;
    virtual void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const = 0;
    // What the view has learnt that the game itself doesn't keep (the explored
    // map), kept next to the save. Formats without any ignore these.
    virtual bool dirty() const { return false; }
    virtual bool save(std::string& bytes) const {
        (void)bytes;
        return false;
    }
    virtual void load(const std::string& bytes) { (void)bytes; }
};

// The view for a profile's [view], or null for a format this build doesn't
// know or addresses that don't parse (`error` says which).
std::unique_ptr<View> make_view(const ViewSpec& spec, std::string& error);

} // namespace play
