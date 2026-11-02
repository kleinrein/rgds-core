// SPDX-License-Identifier: GPL-3.0-or-later
// The computers' keyboards (ZX Spectrum, Commodore 64) on the second screen, a
// touch for each key: every game of theirs gets it, on the switch beside its
// own views and on its own when it has none. One finger at a time, so the
// shift keys stay down until the next key has been pressed; on the Spectrum
// the keys then show what they mean with that shift. A tap too quick for a
// game to see still holds its key for a few frames.
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
         {"9", RETROK_9, 1, Plain, ")", "GRAPH"},
         {"0", RETROK_0, 1, Plain, "_", "DEL"}},
        {{"Q", RETROK_q, 1, Plain, "<="},
         {"W", RETROK_w, 1, Plain, "<>"},
         {"E", RETROK_e, 1, Plain, ">="},
         {"R", RETROK_r, 1, Plain, "<"},
         {"T", RETROK_t, 1, Plain, ">"},
         {"Y", RETROK_y, 1, Plain, "AND"},
         {"U", RETROK_u, 1, Plain, "OR"},
         {"I", RETROK_i, 1, Plain, "AT"},
         {"O", RETROK_o, 1, Plain, ";"},
         {"P", RETROK_p, 1, Plain, "\""}},
        {{"A", RETROK_a, 1, Plain, "STOP"},
         {"S", RETROK_s, 1, Plain, "NOT"},
         {"D", RETROK_d, 1, Plain, "STEP"},
         {"F", RETROK_f, 1, Plain, "TO"},
         {"G", RETROK_g, 1, Plain, "THEN"},
         {"H", RETROK_h, 1, Plain, up_arrow},
         {"J", RETROK_j, 1, Plain, "-"},
         {"K", RETROK_k, 1, Plain, "+"},
         {"L", RETROK_l, 1, Plain, "="},
         {"ENTER", RETROK_RETURN}},
        {{"CAPS", caps_shift, 1, Sticky},
         {"Z", RETROK_z, 1, Plain, ":"},
         {"X", RETROK_x, 1, Plain, "\xC2\xA3"},
         {"C", RETROK_c, 1, Plain, "?"},
         {"V", RETROK_v, 1, Plain, "/"},
         {"B", RETROK_b, 1, Plain, "*"},
         {"N", RETROK_n, 1, Plain, ","},
         {"M", RETROK_m, 1, Plain, "."},
         {"SYM", symbol_shift, 1, Sticky},
         {"SPC", RETROK_SPACE, 1, Plain, nullptr, "BRK"}},
    }};
    return pages;
}

// The C64's 66 keys don't fit at a finger's width: two pages. Its keys under VICE's positional map (sdl_pos.vkm).
const std::vector<Rows>& commodore() {
    static const std::vector<Rows> pages = {
        {
            {{"1", RETROK_1},
             {"2", RETROK_2},
             {"3", RETROK_3},
             {"4", RETROK_4},
             {"5", RETROK_5},
             {"6", RETROK_6},
             {"7", RETROK_7},
             {"8", RETROK_8},
             {"9", RETROK_9},
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
             {",", RETROK_COMMA},
             {".", RETROK_PERIOD}},
            {{"C=", RETROK_LCTRL, 1, Sticky},
             {"CTRL", RETROK_TAB, 1, Sticky},
             {"SPACE", RETROK_SPACE, 7},
             {"More", 0, 2, Page}},
        },
        {
            {{"F1", RETROK_F1, 2}, {"F3", RETROK_F3, 2}, {"F5", RETROK_F5, 2}, {"F7", RETROK_F7, 2}, {"HOME", RETROK_HOME, 3}},
            {{"+", RETROK_MINUS},
             {"-", RETROK_EQUALS},
             {"\xC2\xA3", RETROK_END},
             {"@", RETROK_LEFTBRACKET},
             {"*", RETROK_RIGHTBRACKET},
             {up_arrow, RETROK_BACKSLASH},
             {":", RETROK_SEMICOLON},
             {";", RETROK_QUOTE},
             {"=", RETROK_PAGEDOWN},
             {"/", RETROK_SLASH},
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
             {"Back", 0, 2, Page}},
        },
    };
    return pages;
}

class Keyboard final : public View {
public:
    explicit Keyboard(const std::vector<Rows>& pages) : pages_(pages) {}

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
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), look.tr("Keyboard"), kit::title,
                  look.ink);
        // A game on several disks: the next one, from here (a game asks for its other side by name, not by key).
        if (disks_ > 1) {
            const Box b = disk_box();
            touch_target(b);
            raised(out, look, b.x, b.y, b.w, b.h, kit::radius, look.panel);
            const std::string label = look.tr("Disk") + " " + std::to_string(disk_ + 1) + " / " + std::to_string(disks_);
            draw_legend(out, font, font.fit(label, kit::small, b.w - 8), b, look.ink);
        }
        const bool caps = is_held(caps_shift), symbol = is_held(symbol_shift);
        int index = 0;
        for (const auto& row : rows()) {
            int column = 0;
            for (const Key& k : row) {
                const Box b = box(rows(), index, column, k.span);
                touch_target(b);
                const bool lit = k.kind == Sticky && is_held(k.code);
                const Rgb fill = lit ? look.accent : look.panel;
                raised(out, look, b.x, b.y, b.w, b.h, kit::radius, fill, finger_ == encode(index, column) ? 1 : 0, true, !lit);
                const char* legend = k.kind == Page ? nullptr : k.label;
                if (pages_.size() == 1 && symbol && !caps && k.with_symbol) legend = k.with_symbol;
                if (pages_.size() == 1 && caps && !symbol && k.with_caps) legend = k.with_caps;
                const std::string text = legend ? legend : look.tr(k.label);
                draw_legend(out, font, text, b, lit ? Look::ink_on(look.accent) : look.ink);
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
    static constexpr int W = 640, keys_top = 88, keys_bottom = 384, key_gap = 6;
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
        const double step = double(W - 2 * kit::margin + key_gap) / units;
        const int x = kit::margin + int(std::lround(column * step));
        const int right = kit::margin + int(std::lround((column + span) * step)) - key_gap;
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

    static void draw_legend(Canvas& out, const Font& font, const std::string& text, const Box& b, Rgb ink) {
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (text.size() == 1 && text[0] >= arrow_left && text[0] <= printed_up) {
            arrow(out, text[0], cx, cy, ink);
            return;
        }
        // One character (the pound sign is two bytes) large, words small.
        const bool one = text.size() == 1 || (text.size() == 2 && uint8_t(text[0]) >= 0x80);
        const int size = one ? kit::title : kit::small;
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
    if (system == "zx") return std::make_unique<Keyboard>(spectrum());
    if (system == "c64") return std::make_unique<Keyboard>(commodore());
    return nullptr;
}

} // namespace play
