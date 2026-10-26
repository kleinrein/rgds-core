// SPDX-License-Identifier: GPL-3.0-or-later
// The Game Boy screens the menu offers (Graphics > Screen colours), in the
// menu's order. One list for the player (ui/colour.cpp) and the launcher's
// pickers, so a name, its swatches and its colours cannot drift apart; the
// slugs are what play.ini keeps and runtime/play_options.py repeats (a test
// compares).
#pragma once
#include <cstdint>

namespace play {

// An original Game Boy game draws four shades; each handheld showed them in
// its own colours. Index 0 is the default: the screen the game was made for.
struct GbPalette {
    const char* slug;
    const char* name;
    uint32_t shade[4]; // lightest to darkest, 0xRRGGBB (the swatches; "color" shows its default set)
};

// The handhelds' shades after SameBoy's measured palettes (MIT). "color" is
// the Game Boy Color's own colours for the game, chosen by the core
// (core/mgba_gb.h); "greys" is the emulator's plain four greys.
inline constexpr GbPalette gb_palettes[] = {
    {"dmg", "Game Boy", {0xC6DE8C, 0x84A563, 0x396139, 0x081810}},
    {"pocket", "Game Boy Pocket", {0xC2CE93, 0x818D66, 0x3A4C3A, 0x07100E}},
    {"light", "Game Boy Light", {0x7FE2C3, 0x56B495, 0x357862, 0x0A1C15}},
    {"color", "Game Boy Color", {0xFFFFFF, 0x7BFF31, 0x0063C5, 0x000000}},
    {"greys", "Black and white", {0xF8F8F8, 0xA8A8A8, 0x505050, 0x000000}},
};
inline constexpr int gb_palette_count = int(sizeof(gb_palettes) / sizeof(gb_palettes[0]));
inline constexpr int gb_palette_gbc = 3, gb_palette_greys = 4;

// A game that runs on the Game Boy Color (its own colours): how its screen
// shows them. Index 0 is the default, the handheld's own washed-out screen.
struct GbCorrection {
    const char* slug;
    const char* name;
};

inline constexpr GbCorrection gb_corrections[] = {
    {"gbc", "Game Boy Color"},
    {"gbc-dim", "Game Boy Color, darker"},
    {"vivid", "Full colour"},
};
inline constexpr int gb_correction_count = int(sizeof(gb_corrections) / sizeof(gb_corrections[0]));
inline constexpr int gb_correction_vivid = 2;

} // namespace play
