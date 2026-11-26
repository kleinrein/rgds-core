// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS Media's frame: two panels, the keys and touches, the screens (library, a film, a book) and what they share.
#pragma once
#include "media/gpu.hpp"
#include "media/library.hpp"
#include "media/sound_out.hpp"
#include "ui/canvas.hpp"
#include "ui/look.hpp"
#include "ui/words.hpp"

#include <SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace media {

constexpr int PW = 640, PH = 480; // each panel

struct Panel {
    SDL_Window* window = nullptr;
    SDL_Renderer* r = nullptr;
    SDL_GLContext gl = nullptr;
    SDL_Rect viewport{0, 0, PW, PH}; // one desktop window holds both panels: the lower one is a viewport
    bool owns_window = false;
};

enum Key { KUp, KDown, KLeft, KRight, KA, KB, KX, KY, KL, KR, KL2, KR2, KStart, KSelect, KHome, KCount };

struct TouchEvent {
    enum Kind { Down, Move, Up } kind = Down;
    double x = 0, y = 0; // panel pixels
    uint32_t at = 0;
    bool upper = false; // the upper panel's touch screen (only for screens that ask for it: upper_touch())
};

struct Context;

class Screen {
public:
    explicit Screen(Context& c) : c(c) {}
    virtual ~Screen() = default;
    virtual void key(Key k, bool repeat) = 0;
    virtual void touch(const TouchEvent& t) = 0; // the lower panel, and the upper one when upper_touch()
    // Whether touches on the upper panel come here too (the reader: both screens are pages).
    virtual bool upper_touch() const { return false; }
    // Advances time; true while something moves (a frame is due).
    virtual bool update(uint32_t now) = 0;
    virtual void draw_upper(SDL_Renderer* r) = 0;
    virtual void draw_lower(SDL_Renderer* r) = 0;
    // A film plays: every frame is drawn.
    virtual bool live() const { return false; }
    // Saves where it is (leaving, the app closing).
    virtual void leave() {}
    // Back on top after the screen over it closed.
    virtual void resumed() {}
    // Its buttons are on the upper panel (a film with the screens swapped): touch answers are drawn there.
    virtual bool controls_upper() const { return false; }
    virtual const char* name() const = 0;

protected:
    Context& c;
};

struct Context {
    play::Look look;
    play::Font font;
    play::Words words;
    Panel upper, lower;
    Power power;
    Ripples ripples; // touches on the lower panel
    SoundOut sound;  // the output in use and its equaliser curve (the music player's)
    Places places;
    Posters posters;
    std::string state_dir;
    std::vector<std::string> video_dirs, book_dirs;
    uint32_t now = 0;
    bool music_tab = false; // --music-tab: a third shelf, Music, that hands back to the launcher's player
    int exit_code = 0;      // 42: open the music player
    bool dirty = true;      // something asks for a frame
    // The navigation the screens ask for.
    std::function<void(std::unique_ptr<Screen>)> push;
    std::function<void()> pop;
    std::function<void()> quit;
    // Panel pixels a layout unit: 1 on the RG DS, 1.6 on the RG DS Plus's 1024x768 (the layout stays 640x480).
    // Canvases made at this scale (Canvas(w, h, c.scale)) are drawn at the panel's own pixels, so they are sharp.
    float scale = 1;
    // The volume and brightness (percent; -1: not known or not adjustable here), as runtime/media.py reports
    // them, and how to ask for another level: set_level("volume" | "brightness", percent). While
    // levels_shown, a screen shows the volume itself and the volume keys' pill stays away.
    int volume = -1, brightness = -1;
    bool levels_shown = false;
    // Whether the upper panel takes touches. False on the RG DS Plus: only its lower panel is a touch screen, so a
    // screen that moves its buttons up (a film with the screens swapped) reads them from the lower panel instead.
    bool upper_touch = true;
    std::function<void(const std::string&, int)> set_level;
    // Fades from what both panels show now to what they show next (a reader turned to another orientation): call
    // it before changing anything.
    std::function<void()> crossfade;
    const std::string& tr(const std::string& s) const { return words(s); }
    bool animations() const { return look.animations; }
};

std::unique_ptr<Screen> make_library(Context& c, int shelf);
std::unique_ptr<Screen> make_watch(Context& c, const Item& item, std::vector<Item> siblings);
std::unique_ptr<Screen> make_read(Context& c, const Item& item);

// ---- Pieces the screens share

// Text measured for canvases at the panel's scale while it lives (Font::set_scale), back to 1 after: the other
// screens' canvases are still made at the layout's size.
struct FontScale {
    play::Font& font;
    FontScale(play::Font& f, float k) : font(f) { font.set_scale(k); }
    ~FontScale() { font.set_scale(1); }
    FontScale(const FontScale&) = delete;
    FontScale& operator=(const FontScale&) = delete;
};

// Area-averaged scaling (posters and covers into tiles), once, on the CPU.
void resample(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);
// A picture (w x h of the canvas's own pixels: Canvas::to_px of its layout size) into a canvas at layout x, y with
// corners of radius r (layout), anti-aliased against what is there.
void blit_rounded(Canvas& c, const uint32_t* px, int w, int h, int x, int y, int r);
// Text broken into lines that fit `width` (spaces, then anywhere), at most max_lines (the last one ellipsed).
std::vector<std::string> wrap(const play::Font& font, const std::string& text, int size, int width, int max_lines);

// A list that slides up over the lower panel: Audio, Subtitles, Contents, Paper...
struct Sheet {
    struct Row {
        std::string label, value;
        bool chosen = false;
        int indent = 0;
    };
    std::string title;
    std::vector<Row> rows;
    int focus = 0;
    int scroll = 0; // first row shown
    bool open = false;
    Spring rise; // 0 hidden .. 1 shown
    int pressed = -1;
    std::function<void(int)> chosen; // the row picked
    Tex tex;
    bool stale = true;
    int w = PW, h = PH; // the frame it is drawn in (book mode: 480 x 640)

    void show(const std::string& t, std::vector<Row> r, int focus_row, std::function<void(int)> on_pick, bool animate);
    void hide();
    bool visible() const { return open || rise.value > .01; }
    void key(Key k);
    void touch(const TouchEvent& t);
    bool update(double dt_ms);
    void draw(SDL_Renderer* r, Context& c);
    int rows_shown() const;
    Box row_box(int i) const;
    Box panel_box() const;
};

} // namespace media
