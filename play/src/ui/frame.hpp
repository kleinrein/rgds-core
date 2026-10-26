// SPDX-License-Identifier: GPL-3.0-or-later
// What can be drawn around the game when a frame is chosen.
//
// Built in (ui/frame_styles.hpp, one list per system), drawn here from shapes
// and the launcher's font, with no pictures of real consoles: the face of the
// system's own handheld around its screen (the Game Boy, the Pocket, the
// Game Boy Color, the Game Boy Advance and its SP, in their shell colours),
// and the black bar (the game across the full width at the top, the system's
// name on a black bar below).
//
// Picture frames are PNGs the owner puts in userdata/frames/<system>/, laid
// over the game the way RetroArch overlays are (640x480 or any size, scaled
// to the panel): the game fills their see-through part, left fully clear (a
// grid an overlay draws there is dropped; the shader decides the screen).
#pragma once
#include "ui/canvas.hpp"

#include <string>
#include <vector>

namespace play {

// A style is its index in frame_styles; 0 is off.
constexpr int FrameOff = 0;

// Where the game sits: whole means a whole-number scale (drawn without
// smoothing); otherwise it is scaled like sharp fill.
struct FrameLayout {
    int x = 0, y = 0, w = 0, h = 0;
    bool whole = true;
};
FrameLayout frame_layout(int panel_w, int panel_h, int game_w, int game_h, int style);

// The whole panel for a built-in style; the game is drawn over the layout after.
// `system` is what the bar says ("GAME BOY ADVANCE").
void draw_frame(Canvas& out, const FrameLayout& screen, int style, const Font& font, const std::string& system);

// A built-in style for `system` by its play.ini name, or -1 for any other name (a picture frame) and for
// another system's style.
int parse_frame(const std::string& name, const std::string& system);
const char* frame_name(int style);

struct PictureFrame {
    std::vector<uint32_t> px; // bytes R, G, B, A
    int w = 0, h = 0;
    struct {
        int x = 0, y = 0, w = 0, h = 0;
    } hole; // the see-through part, in the picture's own pixels
};
bool load_picture_frame(const std::string& path, PictureFrame& frame, std::string& error);
// The game in the hole, scaled to the panel, as large as fits keeping its shape.
FrameLayout picture_layout(const PictureFrame& frame, int panel_w, int panel_h, int game_w, int game_h);
// The picture scaled to the canvas, its alpha kept (the canvas becomes a see-through texture).
void draw_picture_frame(Canvas& out, const PictureFrame& frame);

} // namespace play
