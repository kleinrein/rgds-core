#pragma once
// The cartridge drawn for a game without its own icon: one original shell per system (GBA wide with a clipped
// corner, GB tall with two clipped corners, GBC tall with one), a label for the badge and a connector edge. Plain
// shapes in a unit square, so game_icon can draw them at any size. Included once, from render_assets.hpp.

struct CartShape {
    int x, y, w, h, radius; // in 0..100 of the icon's side
    Color colour;
    bool label = false; // the badge goes on this one
};

struct CartridgeArt {
    std::vector<CartShape> shapes;
    SDL_Rect label{}; // 0..100 box for the badge
    Color ink{};      // the badge's ink
};

inline Color cart_shade(Color c, int amount) { // -100 darker .. +100 lighter
    auto mix = [&](unsigned char v) {
        return static_cast<unsigned char>(amount < 0 ? v * (100 + amount) / 100 : v + (255 - v) * amount / 100);
    };
    return {mix(c.r), mix(c.g), mix(c.b)};
}

inline CartridgeArt cartridge_art(const std::string& system, Color accent) {
    CartridgeArt art;
    auto add = [&](int x, int y, int w, int h, int radius, Color colour, bool label = false) {
        art.shapes.push_back({x, y, w, h, radius, colour, label});
    };
    if (system == "gba") {
        // Wide shell, purple-grey, the corner at the top left clipped, a grip of ridges at the top right.
        const Color shell{128, 124, 150}, dark{78, 74, 100}, pad{196, 194, 208};
        add(16, 24, 78, 14, 4, dark);
        add(6, 34, 88, 48, 6, dark);
        add(16, 22, 78, 16, 4, shell); // the clipped corner: a shorter first step
        add(6, 32, 88, 48, 6, shell);
        for (int i = 0; i < 4; i++) add(66 + i * 6, 25, 3, 7, 1, cart_shade(shell, -28));
        add(12, 36, 76, 36, 4, cart_shade(shell, -12));
        add(14, 38, 72, 32, 3, pad, true);
        add(14, 38, 72, 5, 0, accent);
        add(18, 74, 64, 6, 0, dark); // connector edge
        art.label = {14, 44, 72, 26};
        art.ink = {52, 48, 76};
    } else if (system == "gbc") {
        // Tall shell in clear teal plastic: one clipped corner, a colour stripe on the label.
        const Color shell{60, 168, 160}, dark{26, 102, 100}, pad{226, 240, 238};
        add(30, 10, 48, 16, 3, dark);
        add(22, 20, 56, 74, 6, dark);
        add(30, 8, 48, 18, 3, shell); // one clipped corner (top left)
        add(22, 18, 56, 74, 6, shell);
        for (int i = 0; i < 3; i++) add(38, 12 + i * 3, 34, 1, 0, cart_shade(shell, 22));
        add(28, 24, 44, 52, 4, cart_shade(shell, -14));
        add(30, 26, 40, 48, 3, pad, true);
        const Color stripe[4] = {{232, 70, 70}, {244, 190, 52}, {86, 190, 96}, {70, 130, 232}};
        for (int i = 0; i < 4; i++) add(30 + i * 10, 26, 10, 5, 0, stripe[i]);
        add(28, 82, 44, 8, 0, dark);
        art.label = {30, 34, 40, 38};
        art.ink = {22, 84, 84};
    } else if (system == "gb") {
        // The classic grey brick: both top corners clipped, a groove under the shoulders.
        const Color shell{172, 170, 160}, dark{108, 106, 98}, pad{222, 220, 206};
        add(30, 10, 40, 16, 3, dark);
        add(22, 20, 56, 74, 6, dark);
        add(30, 8, 40, 18, 3, shell); // both shoulders clipped
        add(22, 18, 56, 74, 6, shell);
        add(26, 16, 48, 2, 0, cart_shade(shell, -22));
        add(28, 24, 44, 52, 4, cart_shade(shell, -16));
        add(30, 26, 40, 48, 3, pad, true);
        add(30, 26, 40, 5, 0, accent);
        add(28, 82, 44, 8, 0, dark);
        art.label = {30, 34, 40, 38};
        art.ink = {64, 62, 54};
    } else {
        // Any other system: a plain cartridge in the interface's accent colour.
        const Color shell = cart_shade(accent, -10), pad{232, 232, 236};
        add(20, 6, 60, 88, 6, cart_shade(shell, -40));
        add(20, 4, 60, 88, 6, shell);
        add(28, 24, 44, 52, 4, cart_shade(shell, -18));
        add(30, 26, 40, 48, 3, pad, true);
        add(28, 82, 44, 8, 0, cart_shade(shell, -40));
        art.label = {30, 34, 40, 38};
        art.ink = {60, 60, 70};
    }
    return art;
}
