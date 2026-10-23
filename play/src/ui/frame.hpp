// SPDX-License-Identifier: GPL-3.0-or-later
// What can be drawn around the game when a frame is chosen.
//
// Built in, drawn here from shapes and the launcher's font, with no pictures
// of real consoles: a handheld shell in one of the classic shell colours
// (the game at a whole scale in a dark lens, with its power light, three
// red-green-blue dots and a speaker grille); the black bar (the game across
// the full width at the top, the system's name on a black bar below); and an
// SP-style clamshell (a slim lens, the name under it).
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

enum FrameStyle {
    FrameOff = 0,
    FrameIndigo,
    FrameGlacier,
    FrameBlack,
    FramePlatinum,
    FrameBar,
    FrameSpOnyx,
    FrameSpCobalt,
    FrameSpPlatinum,
    FrameStyles
};

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

// A built-in style by its play.ini name, or -1 for any other name (a picture frame).
int parse_frame(const std::string& name);
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
