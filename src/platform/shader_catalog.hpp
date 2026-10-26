#pragma once
#include <cstring>

namespace rgds {
// Only sharp bilinear presets use the destination size to reconstruct pixels.
inline constexpr bool shader_uses_target_size(int id) { return id == 1 || (id >= 9 && id <= 11); }
// Append only: these IDs are persisted in per-game preferences.
inline constexpr const char* shader_files[] = {"off",
                                               "sharp_bilinear",
                                               "lcd1x",
                                               "zfast_lcd",
                                               "nds_color",
                                               "natural_vision",
                                               "lcd1x+nds_color",
                                               "lcd1x+natural_vision",
                                               "lcd1x+nds_color+natural_vision",
                                               "sharp_bilinear+nds_color",
                                               "sharp_bilinear+natural_vision",
                                               "sharp_bilinear+nds_color+natural_vision",
                                               "zfast_lcd+nds_color",
                                               "zfast_lcd+natural_vision",
                                               "zfast_lcd+nds_color+natural_vision",
                                               "soft_scanlines",
                                               "warm_paper",
                                               "cool_crisp",
                                               "pocket_green",
                                               "amber",
                                               "soft_lcd_grid",
                                               "ds_classic",
                                               "ds_lite",
                                               "natural_glow",
                                               "vivid_lcd",
                                               "natural_smooth",
                                               "vivid_smooth",
                                               "pixel_beads",
                                               "glossy_tiles",
                                               "gba_color",
                                               "gba_screen",
                                               "gb_screen",
                                               "gbc_screen"};
inline constexpr int shader_count = sizeof(shader_files) / sizeof(shader_files[0]);
// The community lcd1x and zfast grids were tuned for 1080p, where one DS pixel
// spans 5.6 display pixels. On the 640x480 panels it spans 2.5, so only every
// fifth column and row lands on a grid line and the upstream attenuation terms
// read as plain dimming. These strengths keep the lines clearly visible on the
// handheld (about a quarter of the brightness on a flat image) while the mean
// brightness stays above four fifths. Colour-only and RGDS presets ignore them.
inline constexpr bool shader_uses_lcd1x_grid(int id) { return id == 2 || (id >= 6 && id <= 8); }
inline constexpr bool shader_uses_zfast_grid(int id) { return id == 3 || (id >= 12 && id <= 14); }
inline constexpr const char* shader_grid_defines(int id) {
    return shader_uses_lcd1x_grid(id)   ? "#define BRIGHTEN_SCANLINES 24.0\n#define BRIGHTEN_LCD 6.0\n"
           : shader_uses_zfast_grid(id) ? "#define BORDERMULT 16.0\n"
                                        : "";
}
// Picker order, grouped from plain pixels through DS screens to pixel art.
// Every preset appears exactly once; IDs keep their persisted meaning.
// A family named "... screens" is one handheld's own screen (shader_system_screens).
inline constexpr const char* shader_family_names[] = {"Original", "DS screens", "GBA screens", "Natural",   "Vivid",
                                                      "Pixel art", "Moods",     "GB screens",  "GBC screens"};
inline constexpr int shader_family_count = sizeof(shader_family_names) / sizeof(shader_family_names[0]);
struct ShaderPlace {
    int id, family;
};
inline constexpr ShaderPlace shader_order[] = {{0, 0},  {1, 0},  {21, 1}, {22, 1}, {4, 1},  {6, 1},  {9, 1},  {12, 1},
                                               {8, 1},  {11, 1}, {14, 1}, {30, 2}, {29, 2}, {23, 3}, {25, 3}, {5, 3},
                                               {7, 3},  {10, 3}, {13, 3}, {20, 3}, {15, 3}, {24, 4}, {26, 4}, {17, 4},
                                               {27, 5}, {28, 5}, {2, 5},  {3, 5},  {16, 6}, {18, 6}, {19, 6},
                                               {31, 7}, {32, 8}};
// The family of a system's own screens ("nds", "gba", "gb", "gbc"), or -1.
inline int shader_system_screens(const char* system) {
    const char* systems[] = {"nds", "gba", "gb", "gbc"};
    const int families[] = {1, 2, 7, 8};
    for (int i = 0; i < 4; ++i)
        if (std::strcmp(system, systems[i]) == 0) return families[i];
    return -1;
}
// What a game of another system shows when its own choice is Default: that handheld's screen
// (the GBA's colour and pixel gaps, the Game Boy's dot matrix, the Game Boy Color's faint grid).
// DS games use the default from Settings (-1).
inline int system_default_shader(const char* system) {
    const char* systems[] = {"gba", "gb", "gbc"};
    const int shaders[] = {30, 31, 32};
    for (int i = 0; i < 3; ++i)
        if (std::strcmp(system, systems[i]) == 0) return shaders[i];
    return -1;
}
// What Default draws for a game of `system`: that handheld's own screen, or for a DS game the
// library shader from Settings.
inline int default_shader_for(const char* system, int library) {
    const int own = system_default_shader(system);
    return own >= 0 ? own : library;
}
static_assert(sizeof(shader_order) / sizeof(shader_order[0]) == shader_count, "Every shader needs one picker place");
inline constexpr int shader_family(int id) {
    for (auto place : shader_order)
        if (place.id == id) return place.family;
    return 0;
}
}
