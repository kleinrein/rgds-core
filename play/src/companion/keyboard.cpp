// SPDX-License-Identifier: GPL-3.0-or-later
// The computers' keyboards (ZX Spectrum, Commodore 64) on the second screen, a
// touch for each key: every game of theirs gets it, on the switch beside its
// own views and on its own when it has none. One finger at a time, so the
// shift keys stay down until the next key has been pressed; on the Spectrum
// the keys then show what they mean with that shift. A tap too quick for a
// game to see still holds its key for a few frames. Keys and tray as the
// launcher's keyboard (keyboard_screen.hpp) draws them; the legends are each
// machine's own: a Spectrum's keywords and red symbols, a C64's shifted signs.
// A DOS PC has a third page, its mouse: a touch pad with the mouse's buttons.
#include "companion/view.hpp"
#include "core/core.hpp"
#include "core/libretro/libretro.h"
#include "ui/kit.hpp"
#include "ui/look.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>

namespace play {
namespace {

// Page: code 0 turns to the next page, n to page n - 1. Mouse: a mouse button (code 1 left, 2 right), held down
// while the finger is.
enum Kind { Plain, Sticky, Page, Mouse };

struct Key {
    const char* label;
    unsigned code;
    int span = 1;
    Kind kind = Plain;
    const char* with_symbol = nullptr; // the Spectrum's legends under each shift
    const char* with_caps = nullptr;
    const char* top = nullptr; // printed above the main legend; with_caps when it's null
};
using Rows = std::vector<std::vector<Key>>;

// Labels of one of these characters are drawn as arrows: the cursor keys solid, the keys that print an arrow
// with a stem, as a C64 has both on one page.
constexpr char arrow_left = 1, arrow_up = 2, arrow_down = 3, printed_left = 5, printed_up = 6; // 4: right
const char* const left_key = "\x01";
const char* const up_key = "\x02";
const char* const down_key = "\x03";
const char* const right_key = "\x04";
const char* const left_arrow = "\x05";
const char* const up_arrow = "\x06";

constexpr unsigned caps_shift = RETROK_LSHIFT, symbol_shift = RETROK_LCTRL; // Fuse's own choice of keys for them

const std::vector<Rows>& spectrum() {
    static const std::vector<Rows> pages = {{
        {{"1", RETROK_1, 1, Plain, "!", "EDIT"},
         {"2", RETROK_2, 1, Plain, "@", "LOCK"},
         {"3", RETROK_3, 1, Plain, "#", "TRUE"},
         {"4", RETROK_4, 1, Plain, "$", "INV"},
         {"5", RETROK_5, 1, Plain, "%", left_key},
         {"6", RETROK_6, 1, Plain, "&", down_key},
         {"7", RETROK_7, 1, Plain, "'", up_key},
         {"8", RETROK_8, 1, Plain, "(", right_key},
         {"9", RETROK_9, 1, Plain, ")", "GRPH"},
         {"0", RETROK_0, 1, Plain, "_", "DEL"}},
        {{"Q", RETROK_q, 1, Plain, "<=", nullptr, "PLOT"},
         {"W", RETROK_w, 1, Plain, "<>", nullptr, "DRAW"},
         {"E", RETROK_e, 1, Plain, ">=", nullptr, "REM"},
         {"R", RETROK_r, 1, Plain, "<", nullptr, "RUN"},
         {"T", RETROK_t, 1, Plain, ">", nullptr, "RAND"},
         {"Y", RETROK_y, 1, Plain, "AND", nullptr, "RET"},
         {"U", RETROK_u, 1, Plain, "OR", nullptr, "IF"},
         {"I", RETROK_i, 1, Plain, "AT", nullptr, "INPUT"},
         {"O", RETROK_o, 1, Plain, ";", nullptr, "POKE"},
         {"P", RETROK_p, 1, Plain, "\"", nullptr, "PRINT"}},
        {{"A", RETROK_a, 1, Plain, "STOP", nullptr, "NEW"},
         {"S", RETROK_s, 1, Plain, "NOT", nullptr, "SAVE"},
         {"D", RETROK_d, 1, Plain, "STEP", nullptr, "DIM"},
         {"F", RETROK_f, 1, Plain, "TO", nullptr, "FOR"},
         {"G", RETROK_g, 1, Plain, "THEN", nullptr, "GO TO"},
         {"H", RETROK_h, 1, Plain, up_arrow, nullptr, "GSUB"},
         {"J", RETROK_j, 1, Plain, "-", nullptr, "LOAD"},
         {"K", RETROK_k, 1, Plain, "+", nullptr, "LIST"},
         {"L", RETROK_l, 1, Plain, "=", nullptr, "LET"},
         {"ENTER", RETROK_RETURN, 1, Plain}},
        {{"CAPS", caps_shift, 1, Sticky},
         {"Z", RETROK_z, 1, Plain, ":", nullptr, "COPY"},
         {"X", RETROK_x, 1, Plain, "\xC2\xA3", nullptr, "CLEAR"},
         {"C", RETROK_c, 1, Plain, "?", nullptr, "CONT"},
         {"V", RETROK_v, 1, Plain, "/", nullptr, "CLS"},
         {"B", RETROK_b, 1, Plain, "*", nullptr, "BORD"},
         {"N", RETROK_n, 1, Plain, ",", nullptr, "NEXT"},
         {"M", RETROK_m, 1, Plain, ".", nullptr, "PAUSE"},
         {"SYM", symbol_shift, 1, Sticky},
         {"SPACE", RETROK_SPACE, 1, Plain, nullptr, nullptr, "BREAK"}},
    }};
    return pages;
}

// The C64's 66 keys don't fit at a finger's width: two pages. Its keys under VICE's positional map (sdl_pos.vkm).
const std::vector<Rows>& commodore() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1, 1, Plain, nullptr, nullptr, "!"},
             {"2", RETROK_2, 1, Plain, nullptr, nullptr, "\""},
             {"3", RETROK_3, 1, Plain, nullptr, nullptr, "#"},
             {"4", RETROK_4, 1, Plain, nullptr, nullptr, "$"},
             {"5", RETROK_5, 1, Plain, nullptr, nullptr, "%"},
             {"6", RETROK_6, 1, Plain, nullptr, nullptr, "&"},
             {"7", RETROK_7, 1, Plain, nullptr, nullptr, "'"},
             {"8", RETROK_8, 1, Plain, nullptr, nullptr, "("},
             {"9", RETROK_9, 1, Plain, nullptr, nullptr, ")"},
             {"0", RETROK_0},
             {"DEL", RETROK_BACKSPACE}},
            {{"Q", RETROK_q},
             {"W", RETROK_w},
             {"E", RETROK_e},
             {"R", RETROK_r},
             {"T", RETROK_t},
             {"Y", RETROK_y},
             {"U", RETROK_u},
             {"I", RETROK_i},
             {"O", RETROK_o},
             {"P", RETROK_p},
             {"STOP", RETROK_ESCAPE}},
            {{"A", RETROK_a},
             {"S", RETROK_s},
             {"D", RETROK_d},
             {"F", RETROK_f},
             {"G", RETROK_g},
             {"H", RETROK_h},
             {"J", RETROK_j},
             {"K", RETROK_k},
             {"L", RETROK_l},
             {"RETURN", RETROK_RETURN, 2}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"Z", RETROK_z},
             {"X", RETROK_x},
             {"C", RETROK_c},
             {"V", RETROK_v},
             {"B", RETROK_b},
             {"N", RETROK_n},
             {"M", RETROK_m},
             {",", RETROK_COMMA, 1, Plain, nullptr, nullptr, "<"},
             {".", RETROK_PERIOD, 1, Plain, nullptr, nullptr, ">"}},
            {{"C=", RETROK_LCTRL, 1, Sticky},
             {"CTRL", RETROK_TAB, 1, Sticky},
             {"SPACE", RETROK_SPACE, 7},
             {"F1-F7", 0, 2, Page}},
        },
        {
            {{"F1", RETROK_F1, 2},
             {"F3", RETROK_F3, 2},
             {"F5", RETROK_F5, 2},
             {"F7", RETROK_F7, 2},
             {"HOME", RETROK_HOME, 3}},
            {{"+", RETROK_MINUS},
             {"-", RETROK_EQUALS},
             {"\xC2\xA3", RETROK_END},
             {"@", RETROK_LEFTBRACKET},
             {"*", RETROK_RIGHTBRACKET},
             {up_arrow, RETROK_BACKSLASH},
             {":", RETROK_SEMICOLON, 1, Plain, nullptr, nullptr, "["},
             {";", RETROK_QUOTE, 1, Plain, nullptr, nullptr, "]"},
             {"=", RETROK_PAGEDOWN},
             {"/", RETROK_SLASH, 1, Plain, nullptr, nullptr, "?"},
             {left_arrow, RETROK_BACKQUOTE}},
            {{left_key, RETROK_LEFT, 2},
             {up_key, RETROK_UP, 2},
             {down_key, RETROK_DOWN, 2},
             {right_key, RETROK_RIGHT, 2},
             {"INST", RETROK_INSERT, 3}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"RETURN", RETROK_RETURN, 2},
             {"STOP", RETROK_ESCAPE, 2},
             {"RESTORE", RETROK_PAGEUP, 3},
             {"LOCK", RETROK_CAPSLOCK, 2}},
            {{"C=", RETROK_LCTRL, 1, Sticky},
             {"CTRL", RETROK_TAB, 1, Sticky},
             {"SPACE", RETROK_SPACE, 7},
             {"ABC", 0, 2, Page}},
        },
    };
    return pages;
}

// An MSX's keys as blueMSX reads them (its libretro.c btn_map): GRAPH is Alt, STOP End, SELECT Page Up, HOME/CLS
// Home; the international layout's shifted legends above.
const std::vector<Rows>& msx() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1, 1, Plain, nullptr, nullptr, "!"},
             {"2", RETROK_2, 1, Plain, nullptr, nullptr, "@"},
             {"3", RETROK_3, 1, Plain, nullptr, nullptr, "#"},
             {"4", RETROK_4, 1, Plain, nullptr, nullptr, "$"},
             {"5", RETROK_5, 1, Plain, nullptr, nullptr, "%"},
             {"6", RETROK_6, 1, Plain, nullptr, nullptr, "^"},
             {"7", RETROK_7, 1, Plain, nullptr, nullptr, "&"},
             {"8", RETROK_8, 1, Plain, nullptr, nullptr, "*"},
             {"9", RETROK_9, 1, Plain, nullptr, nullptr, "("},
             {"0", RETROK_0, 1, Plain, nullptr, nullptr, ")"},
             {"BS", RETROK_BACKSPACE}},
            {{"Q", RETROK_q},
             {"W", RETROK_w},
             {"E", RETROK_e},
             {"R", RETROK_r},
             {"T", RETROK_t},
             {"Y", RETROK_y},
             {"U", RETROK_u},
             {"I", RETROK_i},
             {"O", RETROK_o},
             {"P", RETROK_p},
             {"ESC", RETROK_ESCAPE}},
            {{"A", RETROK_a},
             {"S", RETROK_s},
             {"D", RETROK_d},
             {"F", RETROK_f},
             {"G", RETROK_g},
             {"H", RETROK_h},
             {"J", RETROK_j},
             {"K", RETROK_k},
             {"L", RETROK_l},
             {"RETURN", RETROK_RETURN, 2}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"Z", RETROK_z},
             {"X", RETROK_x},
             {"C", RETROK_c},
             {"V", RETROK_v},
             {"B", RETROK_b},
             {"N", RETROK_n},
             {"M", RETROK_m},
             {",", RETROK_COMMA, 1, Plain, nullptr, nullptr, "<"},
             {".", RETROK_PERIOD, 1, Plain, nullptr, nullptr, ">"}},
            {{"CTRL", RETROK_LCTRL, 2, Sticky},
             {"GRAPH", RETROK_LALT, 2, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"F1-F5", 0, 2, Page}},
        },
        {
            {{"F1", RETROK_F1, 2},
             {"F2", RETROK_F2, 2},
             {"F3", RETROK_F3, 2},
             {"F4", RETROK_F4, 2},
             {"F5", RETROK_F5, 3}},
            {{"-", RETROK_MINUS, 1, Plain, nullptr, nullptr, "="},
             {"^", RETROK_EQUALS, 1, Plain, nullptr, nullptr, "~"},
             {"\\", RETROK_BACKSLASH, 1, Plain, nullptr, nullptr, "|"},
             {"@", RETROK_BACKQUOTE, 1, Plain, nullptr, nullptr, "`"},
             {"[", RETROK_LEFTBRACKET, 1, Plain, nullptr, nullptr, "{"},
             {"]", RETROK_RIGHTBRACKET, 1, Plain, nullptr, nullptr, "}"},
             {";", RETROK_SEMICOLON, 1, Plain, nullptr, nullptr, "+"},
             {":", RETROK_QUOTE, 1, Plain, nullptr, nullptr, "*"},
             {"/", RETROK_SLASH, 1, Plain, nullptr, nullptr, "?"},
             {"_", RETROK_UNDERSCORE},
             {"TAB", RETROK_TAB}},
            {{left_key, RETROK_LEFT, 2},
             {up_key, RETROK_UP, 2},
             {down_key, RETROK_DOWN, 2},
             {right_key, RETROK_RIGHT, 2},
             {"HOME", RETROK_HOME, 3}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"STOP", RETROK_END, 2},
             {"SELECT", RETROK_PAGEUP, 2},
             {"INS", RETROK_INSERT, 2},
             {"DEL", RETROK_DELETE, 1},
             {"CAPS", RETROK_CAPSLOCK, 2}},
            {{"CTRL", RETROK_LCTRL, 2, Sticky},
             {"GRAPH", RETROK_LALT, 2, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"ABC", 0, 2, Page}},
        },
    };
    return pages;
}

// An Amstrad CPC 6128's keys as Caprice32 takes them (retro_events.c init_keyboard_table, positional from a US
// keyboard): COPY is Alt, CLR Delete, DEL Backspace, the keypad's f0-f9, f. and ENTER the PC's keypad; the UK
// legends, shifted ones above.
const std::vector<Rows>& cpc() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1, 1, Plain, nullptr, nullptr, "!"},
             {"2", RETROK_2, 1, Plain, nullptr, nullptr, "\""},
             {"3", RETROK_3, 1, Plain, nullptr, nullptr, "#"},
             {"4", RETROK_4, 1, Plain, nullptr, nullptr, "$"},
             {"5", RETROK_5, 1, Plain, nullptr, nullptr, "%"},
             {"6", RETROK_6, 1, Plain, nullptr, nullptr, "&"},
             {"7", RETROK_7, 1, Plain, nullptr, nullptr, "'"},
             {"8", RETROK_8, 1, Plain, nullptr, nullptr, "("},
             {"9", RETROK_9, 1, Plain, nullptr, nullptr, ")"},
             {"0", RETROK_0, 1, Plain, nullptr, nullptr, "_"},
             {"DEL", RETROK_BACKSPACE}},
            {{"Q", RETROK_q},
             {"W", RETROK_w},
             {"E", RETROK_e},
             {"R", RETROK_r},
             {"T", RETROK_t},
             {"Y", RETROK_y},
             {"U", RETROK_u},
             {"I", RETROK_i},
             {"O", RETROK_o},
             {"P", RETROK_p},
             {"ESC", RETROK_ESCAPE}},
            {{"A", RETROK_a},
             {"S", RETROK_s},
             {"D", RETROK_d},
             {"F", RETROK_f},
             {"G", RETROK_g},
             {"H", RETROK_h},
             {"J", RETROK_j},
             {"K", RETROK_k},
             {"L", RETROK_l},
             {"RETURN", RETROK_RETURN, 2}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"Z", RETROK_z},
             {"X", RETROK_x},
             {"C", RETROK_c},
             {"V", RETROK_v},
             {"B", RETROK_b},
             {"N", RETROK_n},
             {"M", RETROK_m},
             {",", RETROK_COMMA, 1, Plain, nullptr, nullptr, "<"},
             {".", RETROK_PERIOD, 1, Plain, nullptr, nullptr, ">"}},
            {{"CTRL", RETROK_LCTRL, 2, Sticky},
             {"COPY", RETROK_LALT, 2},
             {"SPACE", RETROK_SPACE, 5},
             {"f0-f9", 0, 2, Page}},
        },
        {
            {{"f0", RETROK_KP0},
             {"f1", RETROK_KP1},
             {"f2", RETROK_KP2},
             {"f3", RETROK_KP3},
             {"f4", RETROK_KP4},
             {"f5", RETROK_KP5},
             {"f6", RETROK_KP6},
             {"f7", RETROK_KP7},
             {"f8", RETROK_KP8},
             {"f9", RETROK_KP9},
             {"f.", RETROK_KP_PERIOD}},
            {{"-", RETROK_MINUS, 1, Plain, nullptr, nullptr, "="},
             {"^", RETROK_EQUALS, 1, Plain, nullptr, nullptr, "\xC2\xA3"},
             {"@", RETROK_LEFTBRACKET, 1, Plain, nullptr, nullptr, "|"},
             {"[", RETROK_RIGHTBRACKET, 1, Plain, nullptr, nullptr, "{"},
             {":", RETROK_SEMICOLON, 1, Plain, nullptr, nullptr, "*"},
             {";", RETROK_QUOTE, 1, Plain, nullptr, nullptr, "+"},
             {"]", RETROK_BACKSLASH, 1, Plain, nullptr, nullptr, "}"},
             {"/", RETROK_BACKQUOTE, 1, Plain, nullptr, nullptr, "?"},
             {"\\", RETROK_SLASH, 1, Plain, nullptr, nullptr, "`"},
             {"TAB", RETROK_TAB, 2}},
            {{left_key, RETROK_LEFT, 2},
             {up_key, RETROK_UP, 2},
             {down_key, RETROK_DOWN, 2},
             {right_key, RETROK_RIGHT, 2},
             {"ENTER", RETROK_KP_ENTER, 3}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"CAPS", RETROK_CAPSLOCK, 2},
             {"ESC", RETROK_ESCAPE, 2},
             {"CLR", RETROK_DELETE, 2},
             {"DEL", RETROK_BACKSPACE, 3}},
            {{"CTRL", RETROK_LCTRL, 2, Sticky},
             {"COPY", RETROK_LALT, 2},
             {"SPACE", RETROK_SPACE, 5},
             {"ABC", 0, 2, Page}},
        },
    };
    return pages;
}

// An Amiga's keys as PUAE takes them (libretro-mapper.h: positional, US): HELP is Insert, the Amiga keys Page Up and
// Page Down.
const std::vector<Rows>& amiga() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1, 1, Plain, nullptr, nullptr, "!"},
             {"2", RETROK_2, 1, Plain, nullptr, nullptr, "@"},
             {"3", RETROK_3, 1, Plain, nullptr, nullptr, "#"},
             {"4", RETROK_4, 1, Plain, nullptr, nullptr, "$"},
             {"5", RETROK_5, 1, Plain, nullptr, nullptr, "%"},
             {"6", RETROK_6, 1, Plain, nullptr, nullptr, "^"},
             {"7", RETROK_7, 1, Plain, nullptr, nullptr, "&"},
             {"8", RETROK_8, 1, Plain, nullptr, nullptr, "*"},
             {"9", RETROK_9, 1, Plain, nullptr, nullptr, "("},
             {"0", RETROK_0, 1, Plain, nullptr, nullptr, ")"},
             {"BS", RETROK_BACKSPACE}},
            {{"Q", RETROK_q},
             {"W", RETROK_w},
             {"E", RETROK_e},
             {"R", RETROK_r},
             {"T", RETROK_t},
             {"Y", RETROK_y},
             {"U", RETROK_u},
             {"I", RETROK_i},
             {"O", RETROK_o},
             {"P", RETROK_p},
             {"ESC", RETROK_ESCAPE}},
            {{"A", RETROK_a},
             {"S", RETROK_s},
             {"D", RETROK_d},
             {"F", RETROK_f},
             {"G", RETROK_g},
             {"H", RETROK_h},
             {"J", RETROK_j},
             {"K", RETROK_k},
             {"L", RETROK_l},
             {"RETURN", RETROK_RETURN, 2}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"Z", RETROK_z},
             {"X", RETROK_x},
             {"C", RETROK_c},
             {"V", RETROK_v},
             {"B", RETROK_b},
             {"N", RETROK_n},
             {"M", RETROK_m},
             {",", RETROK_COMMA, 1, Plain, nullptr, nullptr, "<"},
             {".", RETROK_PERIOD, 1, Plain, nullptr, nullptr, ">"}},
            {{"CTRL", RETROK_LCTRL, 1, Sticky},
             {"ALT", RETROK_LALT, 1, Sticky},
             {"AMIGA", RETROK_PAGEUP, 2, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"F1-F10", 0, 2, Page}},
        },
        {
            {{"F1", RETROK_F1},
             {"F2", RETROK_F2},
             {"F3", RETROK_F3},
             {"F4", RETROK_F4},
             {"F5", RETROK_F5},
             {"F6", RETROK_F6},
             {"F7", RETROK_F7},
             {"F8", RETROK_F8},
             {"F9", RETROK_F9},
             {"F10", RETROK_F10},
             {"DEL", RETROK_DELETE}},
            {{"`", RETROK_BACKQUOTE, 1, Plain, nullptr, nullptr, "~"},
             {"-", RETROK_MINUS, 1, Plain, nullptr, nullptr, "_"},
             {"=", RETROK_EQUALS, 1, Plain, nullptr, nullptr, "+"},
             {"\\", RETROK_BACKSLASH, 1, Plain, nullptr, nullptr, "|"},
             {"[", RETROK_LEFTBRACKET, 1, Plain, nullptr, nullptr, "{"},
             {"]", RETROK_RIGHTBRACKET, 1, Plain, nullptr, nullptr, "}"},
             {";", RETROK_SEMICOLON, 1, Plain, nullptr, nullptr, ":"},
             {"'", RETROK_QUOTE, 1, Plain, nullptr, nullptr, "\""},
             {"/", RETROK_SLASH, 1, Plain, nullptr, nullptr, "?"},
             {"TAB", RETROK_TAB, 2}},
            {{left_key, RETROK_LEFT, 2},
             {up_key, RETROK_UP, 2},
             {down_key, RETROK_DOWN, 2},
             {right_key, RETROK_RIGHT, 2},
             {"HELP", RETROK_INSERT, 3}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"RETURN", RETROK_RETURN, 2},
             {"CAPS", RETROK_CAPSLOCK, 2},
             {"ESC", RETROK_ESCAPE, 2},
             {"R AMIGA", RETROK_PAGEDOWN, 3, Sticky}},
            {{"CTRL", RETROK_LCTRL, 1, Sticky},
             {"ALT", RETROK_LALT, 1, Sticky},
             {"AMIGA", RETROK_PAGEUP, 2, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"ABC", 0, 2, Page}},
        },
    };
    return pages;
}

// A PC's keys as DOSBox Pure takes them (its keyboard callback: positional, US). The third page is the mouse: no keys
// above its last row, where the touch pad is.
constexpr unsigned to_page_1 = 1, to_page_2 = 2, to_mouse = 3;
const std::vector<Rows>& pc() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1, 1, Plain, nullptr, nullptr, "!"},
             {"2", RETROK_2, 1, Plain, nullptr, nullptr, "@"},
             {"3", RETROK_3, 1, Plain, nullptr, nullptr, "#"},
             {"4", RETROK_4, 1, Plain, nullptr, nullptr, "$"},
             {"5", RETROK_5, 1, Plain, nullptr, nullptr, "%"},
             {"6", RETROK_6, 1, Plain, nullptr, nullptr, "^"},
             {"7", RETROK_7, 1, Plain, nullptr, nullptr, "&"},
             {"8", RETROK_8, 1, Plain, nullptr, nullptr, "*"},
             {"9", RETROK_9, 1, Plain, nullptr, nullptr, "("},
             {"0", RETROK_0, 1, Plain, nullptr, nullptr, ")"},
             {"BS", RETROK_BACKSPACE}},
            {{"Q", RETROK_q},
             {"W", RETROK_w},
             {"E", RETROK_e},
             {"R", RETROK_r},
             {"T", RETROK_t},
             {"Y", RETROK_y},
             {"U", RETROK_u},
             {"I", RETROK_i},
             {"O", RETROK_o},
             {"P", RETROK_p},
             {"ESC", RETROK_ESCAPE}},
            {{"A", RETROK_a},
             {"S", RETROK_s},
             {"D", RETROK_d},
             {"F", RETROK_f},
             {"G", RETROK_g},
             {"H", RETROK_h},
             {"J", RETROK_j},
             {"K", RETROK_k},
             {"L", RETROK_l},
             {"ENTER", RETROK_RETURN, 2}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"Z", RETROK_z},
             {"X", RETROK_x},
             {"C", RETROK_c},
             {"V", RETROK_v},
             {"B", RETROK_b},
             {"N", RETROK_n},
             {"M", RETROK_m},
             {",", RETROK_COMMA, 1, Plain, nullptr, nullptr, "<"},
             {".", RETROK_PERIOD, 1, Plain, nullptr, nullptr, ">"}},
            {{"CTRL", RETROK_LCTRL, 1, Sticky},
             {"ALT", RETROK_LALT, 1, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"F1-F12", to_page_2, 2, Page},
             {"Mouse", to_mouse, 2, Page}},
        },
        {
            {{"ESC", RETROK_ESCAPE},
             {"F1", RETROK_F1},
             {"F2", RETROK_F2},
             {"F3", RETROK_F3},
             {"F4", RETROK_F4},
             {"F5", RETROK_F5},
             {"F6", RETROK_F6},
             {"F7", RETROK_F7},
             {"F8", RETROK_F8},
             {"F9", RETROK_F9},
             {"F10", RETROK_F10}},
            {{"F11", RETROK_F11},
             {"F12", RETROK_F12},
             {"`", RETROK_BACKQUOTE, 1, Plain, nullptr, nullptr, "~"},
             {"-", RETROK_MINUS, 1, Plain, nullptr, nullptr, "_"},
             {"=", RETROK_EQUALS, 1, Plain, nullptr, nullptr, "+"},
             {"[", RETROK_LEFTBRACKET, 1, Plain, nullptr, nullptr, "{"},
             {"]", RETROK_RIGHTBRACKET, 1, Plain, nullptr, nullptr, "}"},
             {"\\", RETROK_BACKSLASH, 1, Plain, nullptr, nullptr, "|"},
             {";", RETROK_SEMICOLON, 1, Plain, nullptr, nullptr, ":"},
             {"'", RETROK_QUOTE, 1, Plain, nullptr, nullptr, "\""},
             {"/", RETROK_SLASH, 1, Plain, nullptr, nullptr, "?"}},
            {{left_key, RETROK_LEFT},
             {up_key, RETROK_UP},
             {down_key, RETROK_DOWN},
             {right_key, RETROK_RIGHT},
             {"HOME", RETROK_HOME},
             {"END", RETROK_END},
             {"PGUP", RETROK_PAGEUP},
             {"PGDN", RETROK_PAGEDOWN},
             {"INS", RETROK_INSERT},
             {"DEL", RETROK_DELETE},
             {"TAB", RETROK_TAB}},
            {{"SHIFT", RETROK_LSHIFT, 2, Sticky},
             {"CAPS", RETROK_CAPSLOCK, 2},
             {"ENTER", RETROK_RETURN, 3},
             {"BS", RETROK_BACKSPACE, 2},
             {"+", RETROK_KP_PLUS},
             {"-", RETROK_KP_MINUS}},
            {{"CTRL", RETROK_LCTRL, 1, Sticky},
             {"ALT", RETROK_LALT, 1, Sticky},
             {"SPACE", RETROK_SPACE, 5},
             {"ABC", to_page_1, 2, Page},
             {"Mouse", to_mouse, 2, Page}},
        },
        {
            {},
            {},
            {},
            {},
            {{"ESC", RETROK_ESCAPE, 2},
             {"Left", 1, 3, Mouse},
             {"Right", 2, 3, Mouse},
             {"ENTER", RETROK_RETURN, 2},
             {"ABC", to_page_1, 2, Page}},
        },
    };
    return pages;
}

// Flash's: the PC's, its touch pad page also holding the arrows and Space, what most Flash games play with besides
// the mouse, so the pad is enough on its own. Flash opens on it.
const std::vector<Rows>& flash_pc() {
    static const std::vector<Rows> pages = [] {
        std::vector<Rows> p = pc();
        p[to_mouse - 1].back() = {{"Left", 1, 2, Mouse},
                                  {"Right", 2, 2, Mouse},
                                  {left_key, RETROK_LEFT},
                                  {up_key, RETROK_UP},
                                  {down_key, RETROK_DOWN},
                                  {right_key, RETROK_RIGHT},
                                  {"SPACE", RETROK_SPACE, 2},
                                  {"ABC", to_page_1, 2, Page}};
        return p;
    }();
    return pages;
}

class Keyboard final : public View {
public:
    enum Machine { Spectrum, Commodore, Msx, Cpc, Amiga, Pc };
    Keyboard(const std::vector<Rows>& pages, Machine machine, int first_page = 0)
        : pages_(pages), spectrum_(machine == Spectrum), commodore_(machine == Commodore), amiga_(machine == Amiga), cpc_(machine == Cpc),
          pc_(machine == Pc) {
        page_ = first_page;
    }

    void update(Core& live, uint64_t) override {
        if (pc_) update_mouse(live);
        disks_ = live.disk_count();
        if (next_disk_ && disks_) live.set_disk((live.disk_index() + 1) % disks_);
        next_disk_ = false;
        disk_ = live.disk_index();
        // The key being held, then a moment up so the same key tapped twice is seen twice.
        if (holding_) {
            if (++frames_ >= hold_frames && released_) {
                live.set_key(active_, false);
                holding_ = false, frames_ = 0;
                for (unsigned code : held_) live.set_key(code, false);
                sent_.clear();
                held_.clear();
            }
            return;
        }
        for (unsigned code : held_)
            if (std::find(sent_.begin(), sent_.end(), code) == sent_.end())
                live.set_key(code, true), sent_.push_back(code);
        for (auto it = sent_.begin(); it != sent_.end();)
            if (std::find(held_.begin(), held_.end(), *it) == held_.end())
                live.set_key(*it, false), it = sent_.erase(it);
            else ++it;
        if (++frames_ < gap_frames || taps_.empty()) return;
        active_ = taps_.front();
        // A tap whose finger is already up holds for the shortest time; the newest may still be down.
        released_ = taps_.size() > 1 || finger_up_;
        taps_.pop_front();
        live.set_key(active_, true);
        holding_ = true, frames_ = 0;
    }

    bool shown() const override { return true; }

    uint64_t key(uint64_t) const override {
        uint64_t h = 1469598103934665603ull ^ uint64_t(page_) ^ (uint64_t(disks_) << 8) ^ (uint64_t(disk_) << 16) ^
                     (uint64_t(button_ + 1) << 24) ^ (uint64_t(dragging_) << 28) ^ (uint64_t(pad_used_) << 29) ^
                     (uint64_t(finger_left_) << 30) ^ (uint64_t(finger_right_) << 31);
        h = (h ^ uint64_t(finger_ + 1)) * 1099511628211ull;
        for (unsigned code : held_) h = (h ^ code) * 1099511628211ull;
        return h;
    }

    bool wants_fingers() const override { return pc_; }

    // Every finger on the panel, beside the one touch() follows: a second finger holds the mouse's button down while
    // the first slides, as a thumb clicks a laptop's pad under the finger that aims. A finger on Left or Right holds
    // that button for as long as it stays; a second finger on the pad, the left one. And when the first finger down is
    // the one on the button, the pad's finger (which touch() never hears of) is followed here.
    bool fingers(const std::vector<std::pair<int, int>>& down) override {
        fingers_at_ = clock_;
        const bool was_left = finger_left_, was_right = finger_right_;
        finger_left_ = finger_right_ = false;
        if (!on_mouse_page()) {
            sliding_ = false;
            return was_left || was_right;
        }
        const Box pad = pad_box();
        const std::pair<int, int>* slide = nullptr;
        int nearest = 0, on_pad = 0;
        bool on_left = false, on_right = false;
        for (const auto& f : down) {
            if (pad.contains(f.first, f.second)) {
                ++on_pad;
                const int far = std::abs(f.first - slide_x_) + std::abs(f.second - slide_y_);
                if (!slide || (sliding_ && far < nearest)) slide = &f, nearest = far;
                continue;
            }
            int column = 0;
            for (const Key& k : rows().back()) {
                if (k.kind == Mouse && mouse_key_box(column, k.span).contains(f.first, f.second))
                    (k.code == 1 ? on_left : on_right) = true;
                column += k.span;
            }
        }
        finger_right_ = on_right;
        finger_left_ = on_left || (!on_right && on_pad >= 2);
        if (finger_left_ || finger_right_) pad_used_ = true;
        // The pad's finger when touch() follows another (the one that went down first, on a button).
        if (slide && !pad_finger_) {
            // A jump is another finger, not a slide.
            if (sliding_ && nearest < 120) slide_by(slide->first - slide_x_, slide->second - slide_y_);
            sliding_ = true, slide_x_ = slide->first, slide_y_ = slide->second;
        } else sliding_ = false;
        return was_left != finger_left_ || was_right != finger_right_;
    }

    bool take_click() override { return std::exchange(click_, false); }

    bool touch(const Touch& t) override {
        if (pad_finger_ || (t.kind == Touch::Down && on_mouse_page() && pad_box().contains(t.x, t.y)))
            return pad_touch(t);
        if (t.kind == Touch::Up && button_ >= 0) button_up_ = true;
        if (on_mouse_page() && t.kind == Touch::Down) {
            int column = 0;
            for (const Key& k : rows().back()) {
                if (mouse_key_box(column, k.span).contains(t.x, t.y))
                    return press(k, encode(int(rows().size()) - 1, column));
                column += k.span;
            }
            return false;
        }
        if (t.kind == Touch::Up) {
            finger_up_ = true;
            if (taps_.empty()) released_ = true;
            const bool was = finger_ >= 0;
            finger_ = -1;
            return was;
        }
        if (t.kind != Touch::Down) return false;
        if (disks_ > 1 && !on_mouse_page() && disk_box().contains(t.x, t.y)) {
            next_disk_ = true;
            return true;
        }
        int index = 0;
        for (const auto& row : rows()) {
            int column = 0;
            for (const Key& k : row) {
                if (box(rows(), index, column, k.span).contains(t.x, t.y)) return press(k, encode(index, column));
                column += k.span;
            }
            ++index;
        }
        return false;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        // The panel's own size (1024x768 on an RG DS Plus): the keys and the pad fill it, and touches are hit there.
        w_ = out.w(), h_ = out.h();
        backdrop(out, look);
        if (on_mouse_page()) {
            draw_mouse_page(out, look, font);
            return;
        }
        // No heading (the keys say what this is): only the machine's mark in the corner.
        badge_stripes(out, kit::margin, kit::margin + kit::badge / 2);
        // A game on several disks: the next one, from here (a game asks for its other side by name, not by key).
        if (disks_ > 1) {
            const Box b = disk_box();
            touch_target(b);
            key_face(out, look, b, look.focus, 0);
            const std::string label =
                look.tr("Disk") + " " + std::to_string(disk_ + 1) + " / " + std::to_string(disks_);
            draw_legend(out, font, font.fit(label, kit::small, b.w - 8), b, look.ink, kit::small);
        }
        // The tray to the screen's foot, notched where the last row stops short of the dock's corner button.
        const Box last = box(rows(), int(rows().size()) - 1, 0, 1);
        const int tray_right = row_right(rows(), int(rows().size()) - 1) + tray;
        out.rounded_rect(left - tray, keys_top - tray, w_ - 2 * left + 2 * tray, last.y - keys_top + 2 * tray, 10,
                         look.slot);
        out.rounded_rect(left - tray, keys_top - tray, tray_right - left + tray, last.y + last.h - keys_top + 2 * tray,
                         10, look.slot);
        const bool caps = is_held(caps_shift), symbol = is_held(symbol_shift);
        int index = 0;
        for (const auto& row : rows()) {
            int column = 0;
            for (const Key& k : row) {
                const Box b = box(rows(), index, column, k.span);
                touch_target(b);
                const bool lit = (k.kind == Sticky && is_held(k.code)) || k.code == RETROK_RETURN || false;
                const bool down = finger_ == encode(index, column);
                const bool plain =
                    k.kind == Plain && k.code != RETROK_RETURN && (k.span == 1 || k.code == RETROK_SPACE);
                const Rgb fill = lit ? look.accent : (down || !plain) ? look.focus : look.panel;
                const int sink = down ? 2 : 0;
                key_face(out, look, b, fill, sink);
                draw_key(out, look, font, k, {b.x, b.y + sink, b.w, b.h}, lit ? look.on_accent : look.ink, caps,
                         symbol);
                column += k.span;
            }
            ++index;
        }
    }

private:
    static constexpr int left = 18, tray = 6, keys_top = 84, key_gap = 5;
    static constexpr int hold_frames = 4, gap_frames = 2;
    // The canvas last drawn (the lower panel's pixels, which touches come in): 640x480 on the RG DS.
    mutable int w_ = 640, h_ = 480;
    int keys_bottom() const { return h_ - left; }
    // The dock's corner button (dock.cpp Item::Button) sits under the last row's right end.
    int corner_x() const { return w_ - kit::margin - 56 - kit::gap; }
    int corner_y() const { return h_ - kit::margin - 56; }

    const Rows& rows() const { return pages_[size_t(page_)]; }
    // At the top, clear of the switch.
    Box disk_box() const { return {w_ - kit::margin - 56 - kit::gap - 120, kit::margin + 4, 120, 48}; }
    static int encode(int row, int column) { return row * 16 + column; }
    bool is_held(unsigned code) const { return std::find(held_.begin(), held_.end(), code) != held_.end(); }

    static int units(const Rows& rows) {
        int units = 0;
        for (const auto& r : rows) units = std::max(units, row_units(r));
        return units;
    }
    static int row_units(const std::vector<Key>& r) {
        int n = 0;
        for (const Key& k : r) n += k.span;
        return n;
    }
    // A row's keys as left and right edges on the grid, as wide as the longest row's keys.
    std::vector<std::pair<int, int>> grid_edges(const Rows& rows, int row) const {
        const double step = double(w_ - 2 * left + key_gap) / units(rows);
        std::vector<std::pair<int, int>> edges;
        int column = 0;
        for (const Key& k : rows[size_t(row)]) {
            edges.push_back(
                {left + int(std::lround(column * step)), left + int(std::lround((column + k.span) * step)) - key_gap});
            column += k.span;
        }
        return edges;
    }
    // Room the last row's wide keys (space bar, page keys) can give and keep a finger's width.
    int wide_slack(const Rows& rows) const {
        const int last = int(rows.size()) - 1;
        const auto edges = grid_edges(rows, last);
        int slack = 0;
        for (size_t i = 0; i < edges.size(); ++i)
            if (rows[size_t(last)][i].span > 1) slack += std::max(0, edges[i].second - edges[i].first - kit::touch);
        return slack;
    }
    int overhang(const Rows& rows) const { return grid_edges(rows, int(rows.size()) - 1).back().second - corner_x(); }
    // Down to the screen's foot when the last row can stop short of the dock's corner button; a row of single keys
    // (the Spectrum's, whose words need every key's width) can't, so that board stops above the button.
    int bottom(const Rows& rows) const {
        return overhang(rows) <= 0 || wide_slack(rows) >= overhang(rows) ? keys_bottom() : corner_y() - key_gap;
    }
    int row_high(const Rows& rows) const { return (bottom(rows) - keys_top + key_gap) / int(rows.size()); }
    // The last row beside the corner button gives up the room from its wide keys, the space bar most.
    std::vector<std::pair<int, int>> row_edges(const Rows& rows, int row) const {
        auto edges = grid_edges(rows, row);
        const int over = overhang(rows), slack = wide_slack(rows);
        if (row != int(rows.size()) - 1 || over <= 0 || slack < over) return edges;
        int x = left, given = 0, owed = 0;
        for (size_t i = 0; i < edges.size(); ++i) {
            auto& e = edges[i];
            const int spare = rows[size_t(row)][i].span > 1 ? std::max(0, e.second - e.first - kit::touch) : 0;
            given += spare;
            const int cut = int(std::lround(double(given) * over / slack)) - owed;
            owed += cut;
            const int w = e.second - e.first - cut;
            e = {x, x + w};
            x += w + key_gap;
        }
        return edges;
    }
    int row_right(const Rows& rows, int row) const { return row_edges(rows, row).back().second; }
    // Every row across the same grid, as wide as the longest row's keys; rows share the height, down to the foot.
    Box box(const Rows& rows, int row, int column, int) const {
        const auto edges = row_edges(rows, row);
        int at = 0, index = 0;
        for (const Key& k : rows[size_t(row)]) {
            if (at == column) break;
            at += k.span, ++index;
        }
        const auto& e = edges[size_t(std::min(index, int(edges.size()) - 1))];
        const int high = row_high(rows);
        return {e.first, keys_top + row * high, e.second - e.first, high - key_gap};
    }

    bool press(const Key& k, int place) {
        if (k.kind == Page) {
            page_ = k.code ? int(k.code - 1) % int(pages_.size()) : (page_ + 1) % int(pages_.size());
            return true;
        }
        if (k.kind == Mouse) {
            button_ = int(k.code), button_frames_ = 0, button_up_ = false;
            finger_ = place;
            return true;
        }
        if (k.kind == Sticky) {
            auto it = std::find(held_.begin(), held_.end(), k.code);
            if (it == held_.end()) held_.push_back(k.code);
            else held_.erase(it);
            return true;
        }
        taps_.push_back(k.code);
        finger_ = place;
        finger_up_ = false;
        return true;
    }

    // The launcher's key: an edge of line colour two pixels down, the face above it (sunk onto the edge when pressed).
    static void key_face(Canvas& out, const Look& look, const Box& b, Rgb fill, int sink) {
        out.rounded_rect(b.x, b.y + 2, b.w, b.h, 7, look.line);
        out.rounded_rect(b.x, b.y + sink, b.w, b.h, 7, fill);
    }

    // The PC's mouse page: the touch pad over the rows the keys had, its buttons on the last.
    // The pad over the whole screen but a slim row of buttons, which stops short of the dock's corner button.
    bool on_mouse_page() const { return pc_ && page_ == int(to_mouse - 1); }
    static constexpr int pad_edge = 12, mouse_row_h = kit::touch;
    int mouse_row_y() const { return h_ - 8 - mouse_row_h; }
    int mouse_row_right() const { return w_ - 92; } // short of the dock's corner button
    Box pad_box() const { return {pad_edge, pad_edge, w_ - 2 * pad_edge, mouse_row_y() - key_gap - pad_edge}; }
    Box mouse_key_box(int column, int span) const {
        int units = 0;
        for (const Key& k : rows().back()) units += k.span;
        const double step = double(mouse_row_right() - pad_edge + key_gap) / units;
        const int x = pad_edge + int(std::lround(column * step));
        const int right = pad_edge + int(std::lround((column + span) * step)) - key_gap;
        return {x, mouse_row_y(), right - x, mouse_row_h};
    }

    void draw_mouse_page(Canvas& out, const Look& look, const Font& font) const {
        draw_pad(out, look);
        const int row = int(rows().size()) - 1;
        int column = 0;
        for (const Key& k : rows().back()) {
            const Box b = mouse_key_box(column, k.span);
            touch_target(b);
            const bool lit =
                k.kind == Mouse ? button_ == int(k.code) || ((dragging_ || finger_left_) && k.code == 1) ||
                                      (finger_right_ && k.code == 2)
                                : k.code == RETROK_RETURN;
            const bool down = finger_ == encode(row, column);
            const int sink = down ? 2 : 0;
            key_face(out, look, b, lit ? look.accent : look.focus, sink);
            draw_legend(out, font, k.kind == Page || k.kind == Mouse ? look.tr(k.label) : k.label,
                        {b.x, b.y + sink, b.w, b.h}, lit ? look.on_accent : look.ink, kit::small);
            column += k.span;
        }
    }

    // A laptop's touch pad: a finger's slide moves the mouse (a quick one further), a tap clicks the left button.
    // The button is held as a pad that clicks holds it: a finger pressed and kept where it landed takes the button
    // (update_mouse, with a tick from the motor) and keeps it, sliding or not, until it lifts. A tap and then a slide
    // holds it too, as Pure's own touchpad mode does.
    bool pad_touch(const Touch& t) {
        const int frame = clock_;
        if (t.kind == Touch::Down) {
            pad_finger_ = true, pad_moved_ = pad_slid_ = false;
            pad_x_ = pad_x0_ = t.x, pad_y_ = pad_y0_ = t.y, pad_travel_ = 0, pad_down_at_ = frame;
            const bool first = !pad_used_;
            pad_used_ = true;                                   // the pad's mouse drawing has said what it is
            dragging_ = frame - tapped_at_ <= tap_again_frames; // the second touch of a tap and drag
            tapped_at_ = -1000;
            return dragging_ || first;
        }
        if (!pad_finger_) return false;
        const int dx = t.x - pad_x_, dy = t.y - pad_y_;
        pad_x_ = t.x, pad_y_ = t.y;
        pad_travel_ += std::abs(dx) + std::abs(dy);
        if (pad_travel_ > tap_travel) pad_moved_ = true;
        // Away from where it landed: a slide, which never takes the button (a finger resting on its aim must not).
        if (std::abs(t.x - pad_x0_) + std::abs(t.y - pad_y0_) > press_travel) pad_slid_ = true;
        slide_by(dx, dy);
        if (t.kind != Touch::Up) return false;
        pad_finger_ = false;
        const bool was_dragging = dragging_;
        dragging_ = false;
        if (!pad_moved_ && !was_dragging && frame - pad_down_at_ <= tap_frames) {
            click_frames_ = hold_frames; // a tap: the left button, briefly
            tapped_at_ = frame;
        }
        return was_dragging;
    }

    // The finger's motion, gathered over the frame: how fast it went is judged a frame at a time (update_mouse), not a
    // report at a time, so a touch screen that reports more often (the RG DS Plus's) doesn't lose the quick slides.
    void slide_by(int dx, int dy) { pad_rx_ += dx, pad_ry_ += dy; }

    // Slow slides land on a pixel, quick ones cross the screen and more: the pad is smaller than the picture it aims
    // at, so a flick has to carry the pointer across it in one stroke, as a laptop's pad does.
    void accelerate() {
        const double speed = std::hypot(pad_rx_, pad_ry_);
        const double gain = speed <= 4 ? 1.0 : std::min(3.0, 1.0 + (speed - 4) * 0.1);
        pad_dx_ += pad_rx_ * gain, pad_dy_ += pad_ry_ * gain;
        pad_rx_ = pad_ry_ = 0;
    }

    void update_mouse(Core& live) {
        ++clock_;
        // No word of the fingers for a moment (another screen is shown, the menu is up): none holds a button.
        if (clock_ - fingers_at_ > 3) finger_left_ = finger_right_ = sliding_ = false;
        // Pressed and kept in place: the button is this finger's until it lifts.
        if (pad_finger_ && !pad_slid_ && !dragging_ && clock_ - pad_down_at_ >= press_frames)
            dragging_ = true, click_ = true;
        accelerate();
        const int dx = int(pad_dx_), dy = int(pad_dy_); // the whole pixels; the rest waits for the next frame
        pad_dx_ -= dx, pad_dy_ -= dy;
        if (button_ >= 0 && ++button_frames_ >= hold_frames && button_up_) button_ = -1, finger_ = -1;
        const bool left = dragging_ || click_frames_ > 0 || button_ == 1 || finger_left_,
                   right = button_ == 2 || finger_right_;
        if (click_frames_ > 0) --click_frames_;
        live.move_pad_mouse(dx, dy, left, right);
    }

    void draw_pad(Canvas& out, const Look& look) const {
        const Box b = pad_box();
        touch_target(b);
        out.rounded_rect(b.x, b.y + 2, b.w, b.h, 12, look.line);
        out.rounded_rect(b.x, b.y, b.w, b.h, 12, dragging_ || finger_left_ ? look.focus : look.panel);
        // A laptop pad's faint dot grid, and a mouse in the middle until the pad is first touched.
        for (int y = b.y + 20; y < b.y + b.h - 12; y += 24)
            for (int x = b.x + 20; x < b.x + b.w - 12; x += 24) out.fill_rect(x, y, 2, 2, look.line);
        if (pad_used_) return;
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        out.rounded_rect(cx - 22, cy - 34, 44, 68, 20, look.muted);
        out.rounded_rect(cx - 19, cy - 31, 38, 62, 17, dragging_ ? look.focus : look.panel);
        out.fill_rect(cx - 1, cy - 31, 2, 24, look.muted);
        out.fill_rect(cx - 19, cy - 8, 38, 2, look.muted);
    }

    // Each machine's mark in the top corner: the Spectrum's four slanted bands, the C64's five stripes, an MSX's
    // function keys, the CPC's three bands, the Amiga's Boing Ball, a PC's DOS prompt.
    void badge_stripes(Canvas& out, int x, int cy) const {
        if (spectrum_) {
            static constexpr Rgb bands[] = {{216, 40, 40}, {240, 200, 40}, {60, 180, 70}, {40, 170, 220}};
            for (int row = 0; row < 20; ++row)
                for (int i = 0; i < 4; ++i) out.fill_rect(x + i * 8 + (19 - row), cy - 10 + row, 8, 1, bands[i]);
        } else if (commodore_) {
            static constexpr Rgb bands[] = {
                {204, 52, 44}, {232, 124, 44}, {240, 200, 60}, {84, 168, 76}, {58, 108, 188}};
            for (int i = 0; i < 5; ++i) out.fill_rect(x, cy - 10 + i * 4, 44, 4, bands[i]);
        } else if (pc_) {
            // C:\> on a black screen, its cursor green.
            out.rounded_rect(x, cy - 10, 34, 20, 3, Rgb{24, 24, 28});
            out.fill_rect(x + 5, cy - 4, 2, 8, Rgb{200, 200, 200});
            out.fill_rect(x + 9, cy + 2, 2, 2, Rgb{200, 200, 200});
            out.fill_rect(x + 13, cy - 4, 2, 2, Rgb{200, 200, 200});
            out.fill_rect(x + 15, cy - 2, 2, 2, Rgb{200, 200, 200});
            out.fill_rect(x + 13, cy, 2, 2, Rgb{200, 200, 200});
            out.fill_rect(x + 20, cy + 2, 8, 2, Rgb{76, 217, 100});
        } else if (cpc_) {
            // The CPC's red, green and blue bands, stepped as on its case.
            static constexpr Rgb bands[] = {{216, 48, 44}, {60, 172, 80}, {44, 108, 200}};
            for (int i = 0; i < 3; ++i) out.fill_rect(x + i * 6, cy - 9 + i * 6, 30, 5, bands[i]);
        } else if (amiga_) {
            // The Boing Ball's red and white checks.
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    out.fill_rect(x + col * 5, cy - 10 + row * 5, 5, 5,
                                  (row + col) % 2 ? Rgb{240, 240, 240} : Rgb{208, 40, 40});
        } else {
            // An MSX's function keys: a row of five grey bars.
            for (int i = 0; i < 5; ++i) out.rounded_rect(x + i * 10, cy - 5, 8, 10, 2, Rgb{150, 150, 160});
        }
    }

    // A Spectrum's red, deeper on a light key.
    static Rgb symbol_ink(const Look& look) {
        if (look.high_contrast) return look.ink;
        return look.dark ? Rgb{255, 150, 140} : Rgb{150, 20, 20};
    }

    void draw_key(Canvas& out, const Look& look, const Font& font, const Key& k, const Box& b, Rgb ink, bool caps,
                  bool symbol) const {
        // A shift down: what each key means now, alone on the key, as the Spectrum's cursor would.
        if (spectrum_ && symbol != caps && k.kind == Plain) {
            if (const char* now = symbol ? k.with_symbol : k.with_caps) {
                const bool lit = k.code == RETROK_RETURN;
                draw_legend(out, font, now, b, symbol && !lit ? symbol_ink(look) : ink);
                return;
            }
        }
        if (k.kind == Sticky && spectrum_) { // CAPS SHIFT and SYMBOL SHIFT, in two lines as on the machine
            const bool lit = is_held(k.code);
            const Rgb word = !lit && k.code == symbol_shift ? symbol_ink(look) : ink;
            centred_text(out, font, k.code == symbol_shift ? "SYM" : "CAPS", b, b.y + b.h / 2 - 11, word);
            centred_text(out, font, "SHIFT", b, b.y + b.h / 2 + 11, word);
            return;
        }
        if (commodore_ && k.code == RETROK_LCTRL) { // the Commodore key's C= in the logo's blue and red
            const int size = kit::body, width = font.measure("C=", size), x = b.x + (b.w - width) / 2;
            const int y = font.centred(size, b.y + b.h / 2);
            const bool lit = is_held(k.code), dark = look.dark;
            font.draw(out, x, y, "C", size,
                      lit || look.high_contrast ? ink
                      : dark                    ? Rgb{160, 195, 255}
                                                : Rgb{26, 62, 140});
            font.draw(out, x + font.measure("C", size), y, "=", size, lit ? ink : symbol_ink(look));
            return;
        }
        const char* top = k.top ? k.top : k.with_caps;
        const bool below = spectrum_ && k.with_symbol;
        if (!top && !below) {
            draw_legend(out, font, k.kind == Page || k.kind == Mouse ? look.tr(k.label) : k.label, b, ink);
            return;
        }
        // The small legends in the key's top and bottom thirds, its own in the middle.
        const Rgb faint = k.code == RETROK_RETURN ? ink : look.muted;
        if (top) small_legend(out, font, top, b, b.y + 13, faint);
        if (below) small_legend(out, font, k.with_symbol, b, b.y + b.h - 13, symbol_ink(look));
        const int middle = !top ? b.y + b.h / 2 - 6 : !below ? b.y + b.h / 2 + 8 : b.y + b.h / 2;
        draw_legend(out, font, k.label, {b.x, middle - b.h / 2, b.w, b.h}, ink);
    }

    static void centred_text(Canvas& out, const Font& font, const std::string& text, const Box& b, int cy, Rgb ink) {
        const std::string shown =
            font.measure(text, kit::small) <= b.w - 2 ? text : font.fit(text, kit::small, b.w - 2);
        font.draw(out, b.x + (b.w - font.measure(shown, kit::small)) / 2, font.centred(kit::small, cy), shown,
                  kit::small, ink);
    }

    static void small_legend(Canvas& out, const Font& font, const std::string& text, const Box& b, int cy, Rgb ink) {
        if (text.size() == 1 && text[0] >= arrow_left && text[0] <= printed_up)
            arrow(out, text[0], b.x + b.w / 2, cy, ink);
        else centred_text(out, font, text, b, cy, ink);
    }

    static void draw_legend(Canvas& out, const Font& font, const std::string& text, const Box& b, Rgb ink,
                            int forced = 0) {
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (text.size() == 1 && text[0] >= arrow_left && text[0] <= printed_up) {
            arrow(out, text[0], cx, cy, ink);
            return;
        }
        // One character (the pound sign is two bytes) large, words small.
        const bool one = text.size() == 1 || (text.size() == 2 && uint8_t(text[0]) >= 0x80);
        const int size = forced ? forced : one ? kit::title : kit::small;
        const std::string shown = font.fit(text, size, b.w - 4);
        font.draw(out, cx - font.measure(shown, size) / 2, font.centred(size, cy), shown, size, ink);
    }

    // A solid triangle pointing its way, behind it a stem on the keys that print an arrow.
    static void arrow(Canvas& out, char way, int cx, int cy, Rgb ink) {
        constexpr int n = 9, stem = 12;
        for (int i = 0; i < n; ++i) {
            switch (way) {
            case arrow_up: out.fill_rect(cx - i, cy - n / 2 + i, 2 * i + 1, 1, ink); break;
            case printed_up: out.fill_rect(cx - i, cy - n + i, 2 * i + 1, 1, ink); break;
            case arrow_down: out.fill_rect(cx - i, cy + n / 2 - i, 2 * i + 1, 1, ink); break;
            case arrow_left: out.fill_rect(cx - n / 2 + i, cy - i, 1, 2 * i + 1, ink); break;
            case printed_left: out.fill_rect(cx - n + i, cy - i, 1, 2 * i + 1, ink); break;
            default: out.fill_rect(cx + n / 2 - i, cy - i, 1, 2 * i + 1, ink); break;
            }
        }
        if (way == printed_up) out.fill_rect(cx - 1, cy - 1, 3, stem, ink);
        if (way == printed_left) out.fill_rect(cx - 1, cy - 1, stem, 3, ink);
    }

    const std::vector<Rows>& pages_;
    const bool spectrum_, commodore_, amiga_, cpc_, pc_;
    static constexpr int tap_frames = 15, tap_again_frames = 15, tap_travel = 8;
    static constexpr int press_frames = 13, press_travel = 10; // a press: this long within this far of its landing
    int clock_ = 0;                       // frames, for the pad's taps
    int button_ = -1, button_frames_ = 0; // a mouse button key down (1 left, 2 right)
    bool button_up_ = false;
    bool pad_finger_ = false, pad_moved_ = false, dragging_ = false, pad_used_ = false;
    int pad_x_ = 0, pad_y_ = 0, pad_travel_ = 0, pad_down_at_ = 0, tapped_at_ = -1000, click_frames_ = 0;
    double pad_dx_ = 0, pad_dy_ = 0;
    int pad_rx_ = 0, pad_ry_ = 0; // this frame's finger motion, before acceleration
    // From fingers(): a second finger holding a button, and the pad's finger followed when touch() has another.
    bool finger_left_ = false, finger_right_ = false, sliding_ = false;
    bool pad_slid_ = false, click_ = false;
    int pad_x0_ = 0, pad_y0_ = 0;
    int slide_x_ = 0, slide_y_ = 0, fingers_at_ = -1000;
    int page_ = 0, disks_ = 0, disk_ = 0;
    bool next_disk_ = false;
    int finger_ = -1; // the key under the finger, for drawing
    bool finger_up_ = true;
    std::deque<unsigned> taps_;
    unsigned active_ = 0;
    bool holding_ = false, released_ = false;
    int frames_ = gap_frames;
    std::vector<unsigned> held_, sent_; // the shifts down, as chosen and as the core has been told
};

} // namespace

std::unique_ptr<View> make_keyboard(const std::string& system) {
    if (system == "zx") return std::make_unique<Keyboard>(spectrum(), Keyboard::Spectrum);
    if (system == "c64") return std::make_unique<Keyboard>(commodore(), Keyboard::Commodore);
    if (system == "msx") return std::make_unique<Keyboard>(msx(), Keyboard::Msx);
    if (system == "cpc") return std::make_unique<Keyboard>(cpc(), Keyboard::Cpc);
    if (system == "amiga") return std::make_unique<Keyboard>(amiga(), Keyboard::Amiga);
    if (system == "dos") return std::make_unique<Keyboard>(pc(), Keyboard::Pc);
    // Flash: the touch pad first, a laptop's, with the arrows and Space beside its buttons (the game itself is touched
    // on the top screen); a PC's keys a tap away for name entry and keyboard games.
    if (system == "flash") return std::make_unique<Keyboard>(flash_pc(), Keyboard::Pc, int(to_mouse - 1));
    return nullptr;
}

} // namespace play
