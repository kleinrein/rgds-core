// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's appearance (Settings > Theme & display) for the player's own
// screens, so the second screen reads as part of RG DS: colour mode, accent,
// high contrast, the DS grid, the typeface, the icon style and animations come
// from the same preferences files, through the launcher's own code.
#pragma once
#include "ui/canvas.hpp"

#include <string>

namespace play {

struct Look {
    // The launcher's dark theme (launcher/model.hpp) until preferences are read.
    Rgb background{12, 17, 26}, panel{23, 32, 46}, ink{233, 244, 255}, muted{145, 167, 190}, accent{78, 187, 255},
        line{45, 67, 90}, slot{17, 26, 39}, on_accent{7, 21, 36}, focus{26, 61, 89};
    bool dark = true, high_contrast = false;
    bool grid = true;        // Background: Grid (the DS touch screen's tiles), else Plain
    bool pixel_text = false; // Typeface: Pixel classic
    bool rich_icons = true;  // Icon style: Rich, else Simple pixel
    bool animations = true;  // Animations: off is instant, everywhere
    std::string language = "en-US";
    bool twelve_hour = false; // Settings > System > Time format
    // The launcher's translations, for words a screen shows (null: English).
    const class Words* words = nullptr;
    const std::string& tr(const std::string& text) const;
    // A corner the screen leaves free for the dock's button (bottom right), 0 when none.
    int reserve_w = 0, reserve_h = 0;
    // And a corner at the top right for the companion's switch.
    int reserve_top_w = 0, reserve_top_h = 0;
    // The game's own four shades as the top screen shows them (the Game Boy
    // palette chosen in the menu), lightest first: a game's art is drawn in them.
    uint32_t game_shades[4] = {0xFFF8F8F8u, 0xFFA8A8A8u, 0xFF505050u, 0xFF000000u};
    // Shadows under raised things.
    Rgb shadow() const { return dark ? Rgb{0, 0, 0} : Rgb{24, 40, 60}; }
    // Ink that reads on a fill (the launcher's tile_ink).
    static Rgb ink_on(Rgb fill);
};

// Theme & display as the launcher would draw it with these choices.
Look make_look(bool dark, int accent, bool high_contrast);
// userdata/preferences.ini and ui.ini; the defaults when they are missing.
Look load_look(const std::string& preferences_file);

// WCAG contrast of two colours (1..21).
double contrast(Rgb a, Rgb b);

} // namespace play
