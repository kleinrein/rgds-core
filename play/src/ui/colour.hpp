// SPDX-License-Identifier: GPL-3.0-or-later
// How a Game Boy picture is coloured on its way to the panel.
//
// Graphics > Screen colours, one setting whose choices depend on the game.
// Neither is a shader (they look at one pixel at a time and never at its
// neighbours, so they work on the direct present too):
//   palette: an original Game Boy game draws four greys; the palette is the
//            handheld it is shown on (the first Game Boy's green, a Pocket's
//            grey, the Light's teal). "Game Boy Color" is the colour handheld's
//            own colours for the game: the core draws those (core/mgba_gb.h),
//            then they pass the Game Boy Color screen below.
//   correction: a game that runs on the Game Boy Color (a .gbc, or a .gb made
//            for both) has raw RGB555 colours, which a modern panel shows far
//            more saturated than the handheld's washed-out screen did.
// The game is not touched: the palette runs on a copy of the picture just
// before it is drawn, so the companion's probes and the HUD split still see
// the emulator's own greys.
#pragma once
#include "core/core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace play {

int palette_count();
int correction_count();
const char* palette_slug(int index); // "dmg", "pocket", ...
const char* correction_slug(int index);
int parse_palette(const std::string& slug);    // 0 for an unknown name
int parse_correction(const std::string& slug); // 0 for an unknown name

class Colour {
public:
    // system: "gb", "gbc" or "gba" (which colours nothing: the GBA screens are the shader's);
    // colour_hardware: a "gb" game that runs on the Game Boy Color, which then takes the correction.
    void set(const std::string& system, bool colour_hardware, int palette, int correction);
    // The same for core's game, and the core's own part: the Game Boy Color's colours for an
    // original Game Boy game when that is the palette.
    void set(Core& core, int palette, int correction);
    bool active() const { return mode_ != Mode::None; }
    int palette() const { return palette_; }
    int correction() const { return correction_; }
    // The picture to draw: `in` itself when nothing is chosen, else a coloured copy.
    Picture apply(const Picture& in);

private:
    enum class Mode { None, Palette, Correction };
    Mode mode_ = Mode::None;
    int palette_ = 0, correction_ = 0;
    uint32_t shades_[4] = {};
    std::vector<uint32_t> lut_; // 32768 entries, keyed by the pixel's top five bits per channel
    std::vector<uint32_t> out_;
};

} // namespace play
