// SPDX-License-Identifier: GPL-3.0-or-later
// Cheats for the systems RG DS Play runs: a libretro cheat file (.cht, the format RetroArch and its
// libretro-database use) read for the game, switched on and off from the shared in-game menu, and remembered.
//
// Where the file is looked for, first found wins:
//   <root>/cheats/<system>/<game>.cht    the player's own folder (userdata/play)
//   <game's folder>/<game>.cht           beside the ROM
//   <root>/cheats/<game>.cht
// <game> is the ROM's file name without its extension. Which codes are on is kept by name in
// <root>/cheats/<system>/<game>.on, so editing the file keeps the choices as long as the names stay.
#pragma once
#include "core/core.hpp"

#include <string>
#include <vector>

namespace play {

// The codes of a .cht file's text: "cheats = N", cheatN_desc, cheatN_code, cheatN_enable.
std::vector<CheatCode> parse_cht(const std::string& text);

class CheatBook {
public:
    // Reads the game's file and its remembered choices; false (and no codes) when there is none.
    bool load(const std::string& root, const std::string& system, const std::string& rom_path);
    const std::vector<CheatCode>& codes() const { return codes_; }
    const std::string& path() const { return path_; }
    // Flips one code and remembers the choice; false for a number out of range.
    bool set(size_t index, bool on);

private:
    void remember() const;
    std::vector<CheatCode> codes_;
    std::string path_, memory_;
};

} // namespace play
