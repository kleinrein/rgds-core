#pragma once
// Snake's screen state (snake_screen.hpp draws and drives it; the game is snake_game.hpp). Kept apart from App so
// main.cpp only carries one member for it.
#include "launcher/snake_game.hpp"
#include "platform/sdl_dynamic.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace rgds {

namespace snake_layout {
// The upper screen: the board fills it from the status bar down, edge to edge. 27 x 18 cells of 24 px are 648 x 432:
// the outer columns run 4 px past each side, so the grid reaches the edges with no margin.
constexpr int cell = 24;
constexpr int board_x = -4, board_y = 48;
constexpr int board_w = snake::cols * cell, board_h = snake::rows * cell;
constexpr int view_x = 0, view_w = 640; // what of the board the panel shows
// The lower screen: score card, speeds, two switches, the footer.
constexpr int card_y = 92, card_h = 112;
constexpr int speeds_y = 250, speed_w = 56, speed_h = 48;
inline int speed_x(int i) { return 36 + i * 64; } // 1 to 9
constexpr int switches_y = 322, switch_w = 288, switch_h = 56;
constexpr int footer_y = 432, footer_w = 296;
constexpr std::array<int, 2> footer_x = {16, 328}; // Back, Start
} // namespace snake_layout

struct SnakeUi {
    bool open = false, loaded = false;
    enum class State { Ready, Playing, Paused, Over } state = State::Ready;
    snake::Game game;
    int level = 5;
    bool walls = false, classic = false;
    std::array<int, 2> best{}; // edges wrap, walls
    bool new_best = false, card_shown = true;
    uint32_t step_at = 0, over_at = 0, ate_at = 0;
    // The way in: five quick taps on the battery on Home's upper screen.
    int taps = 0;
    uint32_t tap_at = 0;
    // A game that is paused remembers how far into its step it was, so the glide carries on from there.
    uint32_t paused_into = 0;
    // The ready screen's attract mode: the snake plays by itself behind the card.
    uint32_t demo_step_at = 0;
    // Effects that wait for the glide: an apple is eaten when the head reaches it on screen, a step after the game
    // ate it, and the sound, the buzz, the crumbs and the score bubble come then.
    struct Pending {
        uint32_t at;
        int kind; // 0 apple, 1 critter, 2 a piece of the snake bursting at the end
        float x, y;
        int value;
        bool quiet; // the attract mode makes crumbs, never a sound
    };
    std::vector<Pending> pending;
    struct Particle {
        float x, y, vx, vy, size;
        uint8_t r, g, b;
        uint32_t born;
        int life;
    };
    std::vector<Particle> particles;
    struct Pop {
        float x, y;
        int value;
        uint32_t born;
    };
    std::vector<Pop> pops;
    uint32_t food_at = 0; // when the apple on the board appeared (it pops in)
    // What was eaten on the last step, still drawn shrinking into the mouth while the head glides up to it.
    int eaten_step = -1;
    bool eaten_bonus = false, eaten_facing_left = false;
    snake::Cell eaten_cell{};
    // Textures baked per renderer (a soft disc, the board, the LCD filter); dropped with the renderers.
    std::map<std::tuple<SDL_Renderer*, int, uint32_t>, SDL_Texture*> textures;
    // Touch. A swipe on either screen steers; on the lower one a tap may press a button instead.
    int button = -1, anchor_x = 0, anchor_y = 0;
    bool swiped = false;
    bool top_down = false, top_swiped = false;
    int top_x = 0, top_y = 0;
};

} // namespace rgds
