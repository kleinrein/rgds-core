// SPDX-License-Identifier: GPL-3.0-or-later
// The computers' keyboards (ZX Spectrum, Commodore 64) on the second screen, a
// touch for each key: every game of theirs gets it, on the switch beside its
// own views and on its own when it has none. One finger at a time, so the
// shift keys stay down until the next key has been pressed; on the Spectrum
// the keys then show what they mean with that shift. A tap too quick for a
// game to see still holds its key for a few frames. Keys and tray as the
// launcher's keyboard (keyboard_screen.hpp) draws them; the legends are each
// machine's own: a Spectrum's keywords and red symbols, a C64's shifted signs.
#include "companion/view.hpp"
#include "core/core.hpp"
#include "core/libretro/libretro.h"
#include "ui/kit.hpp"
#include "ui/look.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

namespace play {
namespace {

enum Kind { Plain, Sticky, Page };

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
            {{"F1", RETROK_F1, 2}, {"F3", RETROK_F3, 2}, {"F5", RETROK_F5, 2}, {"F7", RETROK_F7, 2}, {"HOME", RETROK_HOME, 3}},
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

class Keyboard final : public View {
public:
    Keyboard(const std::vector<Rows>& pages, bool spectrum) : pages_(pages), spectrum_(spectrum) {}

    void update(Core& live, uint64_t) override {
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
            if (std::find(sent_.begin(), sent_.end(), code) == sent_.end()) live.set_key(code, true), sent_.push_back(code);
        for (auto it = sent_.begin(); it != sent_.end();)
            if (std::find(held_.begin(), held_.end(), *it) == held_.end()) live.set_key(*it, false), it = sent_.erase(it);
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
        uint64_t h = 1469598103934665603ull ^ uint64_t(page_) ^ (uint64_t(disks_) << 8) ^ (uint64_t(disk_) << 16);
        h = (h ^ uint64_t(finger_ + 1)) * 1099511628211ull;
        for (unsigned code : held_) h = (h ^ code) * 1099511628211ull;
        return h;
    }

    bool touch(const Touch& t) override {
        if (t.kind == Touch::Up) {
            finger_up_ = true;
            if (taps_.empty()) released_ = true;
            const bool was = finger_ >= 0;
            finger_ = -1;
            return was;
        }
        if (t.kind != Touch::Down) return false;
        if (disks_ > 1 && disk_box().contains(t.x, t.y)) {
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
        backdrop(out, look);
        const std::string heading = look.tr("Keyboard");
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), heading, kit::title, look.ink);
        badge_stripes(out, kit::margin + font.measure(heading, kit::title) + kit::gap, kit::margin + kit::badge / 2);
        // A game on several disks: the next one, from here (a game asks for its other side by name, not by key).
        if (disks_ > 1) {
            const Box b = disk_box();
            touch_target(b);
            key_face(out, look, b, look.focus, 0);
            const std::string label = look.tr("Disk") + " " + std::to_string(disk_ + 1) + " / " + std::to_string(disks_);
            draw_legend(out, font, font.fit(label, kit::small, b.w - 8), b, look.ink, kit::small);
        }
        out.rounded_rect(left - tray, keys_top - tray, W - 2 * left + 2 * tray, keys_bottom - keys_top + 2 * tray, 10,
                         look.slot);
        const bool caps = is_held(caps_shift), symbol = is_held(symbol_shift);
        int index = 0;
        for (const auto& row : rows()) {
            int column = 0;
            for (const Key& k : row) {
                const Box b = box(rows(), index, column, k.span);
                touch_target(b);
                const bool lit = (k.kind == Sticky && is_held(k.code)) || k.code == RETROK_RETURN;
                const bool down = finger_ == encode(index, column);
                const bool plain = k.kind == Plain && k.code != RETROK_RETURN && (k.span == 1 || k.code == RETROK_SPACE);
                const Rgb fill = lit ? look.accent : (down || !plain) ? look.focus : look.panel;
                const int sink = down ? 2 : 0;
                key_face(out, look, b, fill, sink);
                draw_key(out, look, font, k, {b.x, b.y + sink, b.w, b.h}, lit ? look.on_accent : look.ink, caps, symbol);
                column += k.span;
            }
            ++index;
        }
        // Under the keys, beside the dock's corner.
        const int right = look.reserve_w ? out.w() - look.reserve_w - kit::gap : out.w() - kit::margin;
        font.draw(out, kit::margin, font.centred(kit::small, keys_bottom + (out.h() - keys_bottom) / 2),
                  font.fit(look.tr("Shift keys stay down until the next key"), kit::small, right - kit::margin), kit::small,
                  look.muted);
    }

private:
    static constexpr int W = 640, left = 18, tray = 6, keys_top = 84, keys_bottom = 388, key_gap = 5;
    static constexpr int hold_frames = 4, gap_frames = 2;

    const Rows& rows() const { return pages_[size_t(page_)]; }
    // Beside the heading, clear of the switch.
    static Box disk_box() { return {W - kit::margin - 56 - kit::gap - 120, kit::margin + 4, 120, 48}; }
    static int encode(int row, int column) { return row * 16 + column; }
    bool is_held(unsigned code) const { return std::find(held_.begin(), held_.end(), code) != held_.end(); }

    // Every row across the same grid, as wide as the longest row's keys; rows share the height.
    static Box box(const Rows& rows, int row, int column, int span) {
        int units = 0;
        for (const auto& r : rows) {
            int n = 0;
            for (const Key& k : r) n += k.span;
            units = std::max(units, n);
        }
        const double step = double(W - 2 * left + key_gap) / units;
        const int x = left + int(std::lround(column * step));
        const int right = left + int(std::lround((column + span) * step)) - key_gap;
        const int count = int(rows.size());
        const int high = (keys_bottom - keys_top + key_gap) / count;
        return {x, keys_top + row * high, right - x, high - key_gap};
    }

    bool press(const Key& k, int place) {
        if (k.kind == Page) {
            page_ = (page_ + 1) % int(pages_.size());
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

    // Each machine's mark beside the heading: the Spectrum's four slanted bands, the C64's five stripes.
    void badge_stripes(Canvas& out, int x, int cy) const {
        if (spectrum_) {
            static constexpr Rgb bands[] = {{216, 40, 40}, {240, 200, 40}, {60, 180, 70}, {40, 170, 220}};
            for (int row = 0; row < 20; ++row)
                for (int i = 0; i < 4; ++i) out.fill_rect(x + i * 8 + (19 - row), cy - 10 + row, 8, 1, bands[i]);
        } else {
            static constexpr Rgb bands[] = {{204, 52, 44}, {232, 124, 44}, {240, 200, 60}, {84, 168, 76}, {58, 108, 188}};
            for (int i = 0; i < 5; ++i) out.fill_rect(x, cy - 10 + i * 4, 44, 4, bands[i]);
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
        if (!spectrum_ && k.code == RETROK_LCTRL) { // the Commodore key's C= in the logo's blue and red
            const int size = kit::body, width = font.measure("C=", size), x = b.x + (b.w - width) / 2;
            const int y = font.centred(size, b.y + b.h / 2);
            const bool lit = is_held(k.code), dark = look.dark;
            font.draw(out, x, y, "C", size, lit || look.high_contrast ? ink : dark ? Rgb{160, 195, 255} : Rgb{26, 62, 140});
            font.draw(out, x + font.measure("C", size), y, "=", size, lit ? ink : symbol_ink(look));
            return;
        }
        const char* top = k.top ? k.top : k.with_caps;
        const bool below = spectrum_ && k.with_symbol;
        if (!top && !below) {
            draw_legend(out, font, k.kind == Page ? look.tr(k.label) : k.label, b, ink);
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
        const std::string shown = font.measure(text, kit::small) <= b.w - 2 ? text : font.fit(text, kit::small, b.w - 2);
        font.draw(out, b.x + (b.w - font.measure(shown, kit::small)) / 2, font.centred(kit::small, cy), shown, kit::small, ink);
    }

    static void small_legend(Canvas& out, const Font& font, const std::string& text, const Box& b, int cy, Rgb ink) {
        if (text.size() == 1 && text[0] >= arrow_left && text[0] <= printed_up) arrow(out, text[0], b.x + b.w / 2, cy, ink);
        else centred_text(out, font, text, b, cy, ink);
    }

    static void draw_legend(Canvas& out, const Font& font, const std::string& text, const Box& b, Rgb ink, int forced = 0) {
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
    const bool spectrum_;
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
    if (system == "zx") return std::make_unique<Keyboard>(spectrum(), true);
    if (system == "c64") return std::make_unique<Keyboard>(commodore(), false);
    return nullptr;
}

} // namespace play
