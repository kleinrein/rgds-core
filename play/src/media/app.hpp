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
};

struct Context;

class Screen {
public:
    explicit Screen(Context& c) : c(c) {}
    virtual ~Screen() = default;
    virtual void key(Key k, bool repeat) = 0;
    virtual void touch(const TouchEvent& t) = 0; // the lower panel
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
    const std::string& tr(const std::string& s) const { return words(s); }
    bool animations() const { return look.animations; }
};

std::unique_ptr<Screen> make_library(Context& c, int shelf);
std::unique_ptr<Screen> make_watch(Context& c, const Item& item, std::vector<Item> siblings);
std::unique_ptr<Screen> make_read(Context& c, const Item& item);

// ---- Pieces the screens share

// Area-averaged scaling (posters and covers into tiles), once, on the CPU.
void resample(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);
// A picture into a canvas with rounded corners (anti-aliased against what is there).
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
