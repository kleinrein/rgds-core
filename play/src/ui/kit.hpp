// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's DSi look for the player's own screens, drawn on a Canvas:
// the touch screen's grid, raised cards and pills (a rim, a soft gradient and
// a highlight, as App::glossy_pill), recessed tracks, meters, icon badges and
// a game's own tiles. Every screen the player draws goes through these, so
// they match the launcher and each other (docs/COMPANION.md, the rules).
//
// Layout is in the launcher's 640x480 units: 24 px side margins, 12 px gaps,
// radius 14 cards, 44 px icon boxes, text at the baked sizes only.
#pragma once
#include "ui/canvas.hpp"
#include "ui/icons.hpp"
#include "ui/look.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace play {

namespace kit {
constexpr int margin = 24, gap = 12, radius = 14, badge = 44, touch = 48;
// Text sizes by role.
constexpr int small = 16, body = 20, title = 24, large = 28, number = 36;
} // namespace kit

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// A game's own picture for an icon: 2bpp Game Boy tiles read from the ROM,
// colour numbers 0-3 (255: see-through).
struct GameArt {
    int w = 0, h = 0;
    std::vector<uint8_t> index;
    bool empty() const { return index.empty(); }
};

// The second screen's ground: the DS touch screen's square tiles (Grid) or
// the launcher's faint ruling (Plain).
void backdrop(Canvas& out, const Look& look);
// A rounded fill at an alpha (shadows, glows).
void soft_round(Canvas& out, int x, int y, int w, int h, int r, Rgb c, int alpha);
// The launcher's glossy pill: rim, gradient body, top highlight; press 0..1 sinks it.
void glossy(Canvas& out, int x, int y, int w, int h, int r, Rgb base, double press = 0, bool highlight = true);
// A glossy pill with its shadow.
void raised(Canvas& out, const Look& look, int x, int y, int w, int h, int r, Rgb fill, double press = 0,
            bool shadow = true, bool highlight = true);
// A settings card: the slot-coloured edge under a panel (accent ring when focused).
void card(Canvas& out, const Look& look, const Box& b, bool focused = false, int r = kit::radius);
// A recessed track: a rim, the background inside, a shadow along its top.
void sunken(Canvas& out, const Look& look, const Box& b, int r);
// A meter: a sunken track filled to `fraction` with a glossy colour.
void meter(Canvas& out, const Look& look, const Box& b, double fraction, Rgb fill);
// A 12x12 glyph centred on cx, cy at about `size` px: whole-pixel steps for
// Simple pixel icons, smoothed for Rich ones.
void icon(Canvas& out, const Look& look, Icon which, int cx, int cy, int size, Rgb ink, int alpha = 255);
// The launcher's icon box: a 44 px slot (accent when lit) with the icon in it.
void icon_badge(Canvas& out, const Look& look, int x, int y, Icon which, bool lit = false, int size = kit::badge);
// A game's own art in a badge, at the largest whole scale that fits, in its
// own four shades (colour 0, the lightest, first) on a little screen of colour 0.
void art_badge(Canvas& out, const Look& look, int x, int y, const GameArt& art, const uint32_t shades[4],
               int size = kit::badge);
// A small rounded label (a chip) at x, centred on cy; returns its width.
int chip(Canvas& out, const Look& look, const Font& font, int x, int cy, const std::string& text, bool accent);
// A whole progress ring around cx, cy (a hold-to-confirm button).
void ring(Canvas& out, int cx, int cy, int r, int thickness, double fraction, Rgb c, int alpha = 255);
// A filled disc (anti-aliased).
void disc(Canvas& out, int cx, int cy, int r, Rgb c, int alpha = 255);

// Something that takes a touch, for the companion lint (targets of at least
// kit::touch a side); drawn screens call it for each, and it is kept while a log is set.
void touch_target(const Box& b);
void set_target_log(std::vector<Box>* log);

// GB 2bpp tiles (16 bytes each, row-major in cols x rows) into art.
GameArt gb_tiles(const std::vector<uint8_t>& bytes, int cols, int rows, int clear = -1);

// Smoothstep from 0 to 1 over a duration; instant when animations are off.
double ease(const Look& look, double t);

} // namespace play
