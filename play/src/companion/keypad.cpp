// SPDX-License-Identifier: GPL-3.0-or-later
// The controllers the touch screen can be: the ColecoVision's and the
// Intellivision's twelve-key keypad, the Sega Pico's storyware book and
// drawing pad under its pen, the Atari 2600's console switches, and the
// Dreamcast controller's memory unit (VMU) with its little screen. Each is a second screen every game of its system
// gets, on the switch beside its own views and on its own when it has none.
//
// A keypad key is held as long as the finger is on it (games read held keys:
// a ColecoVision game's skill level, an Intellivision game's fire on 5), and
// a tap too quick for a game to see still holds it for a few frames. Sliding
// onto another key moves the press there, as a thumb on the real pad does.
//
// The Atari's panel holds Game Select and Game Reset while touched, and
// throws the difficulty and TV type switches as the console's levers do
// (Stella reads them as L/L2, R/R2 and L3/R3; they start at B and colour).
//
// The Pico's book shows the page it is open at when the owner keeps its
// scans beside the game (PicoDrive's own names: Game_0.png ... Game_6.png,
// Game_pad.png), else a blank page that still takes the pen. A finger puts
// the pen on the page or the pad; lifting it lifts the pen. A, as on the
// Pico, is the pen's red button; L and R turn the pages, as the buttons
// under the book do.
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/png.hpp"

#include <algorithm>
#include <array>
#include <filesystem>

namespace play {
namespace {

constexpr int W = 640;

// The keys as the pad has them, read across: bit i of the core's mask.
struct Pad {
    std::array<const char*, 12> legends;
    Rgb mark; // the maker's colour beside the heading
};
const Pad coleco_pad = {{"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"}, {200, 40, 40}};
const Pad intellivision_pad = {{"1", "2", "3", "4", "5", "6", "7", "8", "9", "CLEAR", "0", "ENTER"}, {201, 163, 78}};

class Keypad final : public View {
public:
    explicit Keypad(const Pad& pad) : pad_(pad) {}

    void update(Core& live, uint64_t) override {
        // A key is down for at least hold_frames, then up once the finger has left it, or at once when the next
        // press waits; a waiting press follows a frame later, so the same key tapped twice reaches the game twice.
        if (pressed_ >= 0) ++held_frames_;
        const bool release = pressed_ >= 0 && held_frames_ >= hold_frames && (finger_ != pressed_ || queued_ >= 0);
        if (release) pressed_ = -1;
        else if (pressed_ < 0 && queued_ >= 0 && !released_) {
            pressed_ = queued_;
            queued_ = -1;
            held_frames_ = 0;
        }
        released_ = release;
        const unsigned mask = pressed_ >= 0 ? 1u << pressed_ : 0;
        if (mask != sent_) live.set_keypad(mask);
        sent_ = mask;
    }

    bool shown() const override { return true; }

    uint64_t key(uint64_t) const override { return 1469598103934665603ull ^ uint64_t(pressed_ + 1) ^ (uint64_t(finger_ + 1) << 8); }

    bool touch(const Touch& t) override {
        const int was = finger_;
        int under = -1;
        if (t.kind != Touch::Up)
            for (int i = 0; i < 12; ++i)
                if (box(i).contains(t.x, t.y)) under = i;
        // Lifted, or slid off the keys: the key goes up after its shortest hold.
        finger_ = under;
        if (under < 0 || (t.kind == Touch::Move && under == was)) return under != was;
        if (pressed_ < 0 && queued_ < 0) {
            pressed_ = under;
            held_frames_ = 0;
        } else if (pressed_ >= 0 && under != pressed_ && held_frames_ >= hold_frames && queued_ < 0) {
            pressed_ = under; // slid onto the next key: the press moves with the thumb
            held_frames_ = 0;
        } else {
            queued_ = under; // the same key again, or one pressed before the last was seen
        }
        return true;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const std::string heading = look.tr("Keypad");
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), heading, kit::title, look.ink);
        out.rounded_rect(kit::margin + font.measure(heading, kit::title) + kit::gap, kit::margin + kit::badge / 2 - 4, 44, 8,
                         4, pad_.mark);
        const Box tray = tray_box();
        out.rounded_rect(tray.x, tray.y, tray.w, tray.h, 12, look.slot);
        for (int i = 0; i < 12; ++i) {
            const Box b = box(i);
            touch_target(b);
            const bool down = pressed_ == i;
            const int sink = down ? 2 : 0;
            out.rounded_rect(b.x, b.y + 2, b.w, b.h, 9, look.line);
            out.rounded_rect(b.x, b.y + sink, b.w, b.h, 9, down ? look.accent : i >= 9 && i != 10 ? look.focus : look.panel);
            const std::string legend = pad_.legends[size_t(i)];
            const int size = legend.size() == 1 ? kit::number : kit::small;
            const std::string shown = font.fit(legend, size, b.w - 8);
            font.draw(out, b.x + (b.w - font.measure(shown, size)) / 2, font.centred(size, b.y + sink + b.h / 2), shown, size,
                      down ? look.on_accent : look.ink);
        }
    }

private:
    static constexpr int keys_top = 84, keys_bottom = 452, key_gap = 8, hold_frames = 4;

    // Three keys across, as wide as a thumb's reach, centred clear of the dock on the right.
    Box tray_box() const {
        const int h = keys_bottom - keys_top, w = std::min(W - 2 * kit::margin, h * 3 / 4 + 120);
        return {(W - w) / 2, keys_top, w, h};
    }
    Box box(int i) const {
        const Box t = tray_box();
        const int inset = 8, kw = (t.w - 2 * inset - 2 * key_gap) / 3, kh = (t.h - 2 * inset - 3 * key_gap) / 4;
        return {t.x + inset + (i % 3) * (kw + key_gap), t.y + inset + (i / 3) * (kh + key_gap), kw, kh};
    }

    const Pad& pad_;
    int finger_ = -1, pressed_ = -1, queued_ = -1, held_frames_ = 0;
    bool released_ = false;
    unsigned sent_ = 0;
};

// A picture scaled to fill w x h (nearest pixel; a storyware scan is a page, not pixel art, but this screen is
// near its size).
std::vector<uint32_t> scaled(const std::vector<uint32_t>& px, int pw, int ph, int w, int h) {
    std::vector<uint32_t> out(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out[size_t(y) * w + x] = px[size_t(y * ph / h) * pw + x * pw / w] | 0xFF000000u;
    return out;
}

class Storyware final : public View {
public:
    void update(Core& live, uint64_t) override {
        if (stem_.empty()) {
            std::filesystem::path file = live.game_file();
            stem_ = (file.parent_path() / file.stem()).string();
        }
        if (turn_) live.set_pico_page(std::clamp(live.pico_page() + turn_, 0, last_page));
        turn_ = 0;
        const int page = live.pico_page();
        if (page != page_) page_ = page, picture_ready_ = false;
        if (!picture_ready_) load_picture();
        if (pen_changed_) live.set_pico_pen(pen_x_, pen_y_, pen_down_, !on_pad_);
        pen_changed_ = false;
    }

    bool shown() const override { return true; }

    uint64_t key(uint64_t) const override {
        return 1469598103934665603ull ^ uint64_t(page_ + 1) ^ (uint64_t(on_pad_) << 8) ^ (uint64_t(pen_down_) << 9) ^
               (uint64_t(pen_x_) << 16) ^ (uint64_t(pen_y_) << 32) ^ (uint64_t(picture_ready_) << 48);
    }

    bool touch(const Touch& t) override {
        const Box page = page_box();
        if (t.kind == Touch::Down && !page.contains(t.x, t.y)) {
            if (back_box().contains(t.x, t.y)) return turn(-1);
            if (forward_box().contains(t.x, t.y)) return turn(1);
            if (switch_box().contains(t.x, t.y)) {
                on_pad_ = !on_pad_;
                picture_ready_ = false;
                return true;
            }
            return false;
        }
        if (t.kind == Touch::Up) {
            if (!pen_down_) return false;
            pen_down_ = false;
            pen_changed_ = true;
            return true;
        }
        if (t.kind == Touch::Move && !pen_down_) return false;
        pen_x_ = std::clamp((t.x - page.x) * 320 / page.w, 0, 319);
        pen_y_ = std::clamp((t.y - page.y) * 224 / page.h, 0, 223);
        pen_down_ = pen_changed_ = true;
        return true;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const std::string heading = look.tr(on_pad_ ? "Drawing pad" : "Storyware");
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), heading, kit::title, look.ink);
        const Box page = page_box();
        out.rounded_rect(page.x - 6, page.y - 6, page.w + 12, page.h + 12, 12, look.slot);
        if (!picture_.empty()) out.blit(picture_.data(), page.w, page.h, page.x, page.y, 1);
        else {
            out.rounded_rect(page.x, page.y, page.w, page.h, 8, look.panel);
            // A blank page names itself; the pad stays blank, as the real one is.
            const std::string blank = on_pad_ ? "" : look.tr("Page") + " " + std::to_string(std::max(page_, 0) + 1);
            if (!blank.empty())
                font.draw(out, page.x + (page.w - font.measure(blank, kit::large)) / 2,
                          font.centred(kit::large, page.y + page.h / 2), blank, kit::large, look.muted);
        }
        touch_target(page);
        // The pen where it last touched: a ring, filled while it is down.
        if (pen_x_ >= 0) {
            const int cx = page.x + pen_x_ * page.w / 320, cy = page.y + pen_y_ * page.h / 224;
            disc(out, cx, cy, 9, look.accent, pen_down_ ? 255 : 0);
            ring(out, cx, cy, 10, 3, 1.0, look.accent);
        }
        if (!on_pad_) {
            pill(out, look, font, back_box(), "<", page_ <= 0);
            pill(out, look, font, forward_box(), ">", page_ >= last_page);
            const std::string at = look.tr("Page") + " " + std::to_string(std::max(page_, 0) + 1);
            const Box b = back_box();
            font.draw(out, b.x + b.w + kit::gap, font.centred(kit::body, b.y + b.h / 2), at, kit::body, look.ink);
        }
        pill(out, look, font, switch_box(), look.tr(on_pad_ ? "Storyware" : "Drawing pad"), false);
    }

private:
    static constexpr int last_page = 6; // seven pages; PicoDrive's eighth is its own test page

    // The book's page is twice as wide as it is high, as the storyware is; the pad shares the place.
    static Box page_box() { return {kit::margin + 6, 84, W - 2 * kit::margin - 12, 296}; }
    static Box back_box() { return {kit::margin, 404, 56, kit::touch}; }
    static Box forward_box() { return {kit::margin + 56 + kit::gap + 96 + kit::gap, 404, 56, kit::touch}; }
    static Box switch_box() { return {W - kit::margin - 200 - 96, 404, 200, kit::touch}; }

    bool turn(int by) {
        turn_ = by;
        return true;
    }

    static void pill(Canvas& out, const Look& look, const Font& font, const Box& b, const std::string& label, bool off) {
        touch_target(b);
        out.rounded_rect(b.x, b.y + 2, b.w, b.h, b.h / 2, look.line);
        out.rounded_rect(b.x, b.y, b.w, b.h, b.h / 2, look.focus);
        const std::string shown = font.fit(label, kit::body, b.w - 16);
        font.draw(out, b.x + (b.w - font.measure(shown, kit::body)) / 2, font.centred(kit::body, b.y + b.h / 2), shown,
                  kit::body, off ? look.muted : look.ink);
    }

    void load_picture() {
        picture_ready_ = true;
        picture_.clear();
        if (stem_.empty() || (!on_pad_ && page_ < 0)) return;
        const std::string suffix = on_pad_ ? "_pad" : "_" + std::to_string(page_);
        for (const char* ext : {".png", ".PNG"}) {
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            std::string error;
            if (!read_png(stem_ + suffix + ext, px, w, h, error) || w <= 0 || h <= 0) continue;
            const Box page = page_box();
            picture_ = scaled(px, w, h, page.w, page.h);
            return;
        }
    }

    std::string stem_;
    int page_ = -1, turn_ = 0;
    bool on_pad_ = false, picture_ready_ = false;
    std::vector<uint32_t> picture_;
    int pen_x_ = -1, pen_y_ = -1;
    bool pen_down_ = false, pen_changed_ = false;
};

class ConsolePanel final : public View {
public:
    void update(Core& live, uint64_t) override {
        uint32_t mask = 0;
        for (int i = 0; i < count; ++i) {
            Hold& h = holds_[i];
            if (h.frames > 0) {
                --h.frames;
                mask |= 1u << ids[i];
            } else if (h.finger) {
                mask |= 1u << ids[i];
            }
        }
        if (mask != sent_) live.set_panel_buttons(mask);
        sent_ = mask;
    }

    bool shown() const override { return true; }

    uint64_t key(uint64_t) const override {
        uint64_t k = 1469598103934665603ull ^ uint64_t(left_a_) ^ (uint64_t(right_a_) << 1) ^ (uint64_t(colour_) << 2);
        for (int i = 0; i < count; ++i) k ^= uint64_t(holds_[i].finger || holds_[i].frames > 0) << (8 + i);
        return k;
    }

    bool touch(const Touch& t) override {
        if (t.kind == Touch::Up) {
            bool any = false;
            for (auto& h : holds_) any |= h.finger, h.finger = false;
            return any;
        }
        if (t.kind != Touch::Down) return false;
        for (int i = 0; i < count; ++i) {
            if (!box(i).contains(t.x, t.y)) continue;
            holds_[i].frames = hold_frames;
            holds_[i].finger = i == select || i == reset; // the switches are thrown, the buttons held
            if (i == left_a || i == left_b) left_a_ = i == left_a;
            if (i == right_a || i == right_b) right_a_ = i == right_a;
            if (i == colour || i == black_white) colour_ = i == colour;
            return true;
        }
        return false;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const std::string heading = look.tr("Console switches");
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), heading, kit::title, look.ink);
        // The console's wood grain beside the heading.
        const int mx = kit::margin + font.measure(heading, kit::title) + kit::gap;
        out.rounded_rect(mx, kit::margin + kit::badge / 2 - 6, 44, 12, 4, Rgb{110, 70, 38});
        out.fill_rect(mx + 4, kit::margin + kit::badge / 2 - 2, 36, 1, Rgb{140, 96, 56});
        out.fill_rect(mx + 4, kit::margin + kit::badge / 2 + 2, 36, 1, Rgb{90, 56, 28});
        const bool lit[count] = {false, false, left_a_, !left_a_, right_a_, !right_a_, colour_, !colour_};
        static const char* const captions[] = {"LEFT DIFFICULTY", "RIGHT DIFFICULTY", "TV TYPE"};
        for (int row = 0; row < 3; ++row) {
            const Box b = box(2 + row * 2);
            font.draw(out, kit::margin, font.centred(kit::small, b.y + b.h / 2), captions[row], kit::small, look.muted);
        }
        for (int i = 0; i < count; ++i) {
            const Box b = box(i);
            touch_target(b);
            const bool down = holds_[i].finger || (holds_[i].frames > 0 && (i == select || i == reset));
            const bool on = lit[i] || down;
            const int sink = down ? 2 : 0;
            out.rounded_rect(b.x, b.y + 2, b.w, b.h, 10, look.line);
            out.rounded_rect(b.x, b.y + sink, b.w, b.h, 10, on ? look.accent : look.panel);
            const int size = i <= reset ? kit::title : kit::body;
            const std::string shown = font.fit(labels[i], size, b.w - 12);
            font.draw(out, b.x + (b.w - font.measure(shown, size)) / 2, font.centred(size, b.y + sink + b.h / 2), shown, size,
                      on ? look.on_accent : look.ink);
        }
    }

private:
    enum { select, reset, left_a, left_b, right_a, right_b, colour, black_white, count };
    static constexpr int hold_frames = 6;
    // RETRO_DEVICE_ID_JOYPAD_: SELECT, START, L, L2, R, R2, L3, R3
    static constexpr unsigned ids[count] = {2, 3, 10, 12, 11, 13, 14, 15};
    static constexpr const char* labels[count] = {"GAME SELECT", "GAME RESET", "A", "B", "A", "B", "COLOR", "B/W"};

    // Two big buttons on top, then three rows of two-way switches beside their names.
    static Box box(int i) {
        if (i <= reset) return {kit::margin + i * (296 + kit::gap), 84, 292 - kit::gap / 2, 96};
        const int row = (i - left_a) / 2, side = (i - left_a) % 2, y = 204 + row * 84;
        const int x0 = 268, w = (W - kit::margin - 96 - x0 - kit::gap) / 2;
        return {x0 + side * (w + kit::gap), y, w, 64};
    }

    struct Hold {
        int frames = 0;
        bool finger = false;
    };
    Hold holds_[count];
    bool left_a_ = false, right_a_ = false, colour_ = true;
    uint32_t sent_ = 0;
};

// The Dreamcast's VMU as it sits in the controller, its 48 x 32 screen large: what games put there (a life bar, a
// map, a character's face) is the second screen the Dreamcast always had. Shown once the game has drawn on it; until
// then the second screen keeps its other views (the info screen).
class Vmu final : public View {
public:
    void update(Core& live, uint64_t) override {
        const uint64_t drawn = live.vmu_screen(nullptr);
        if (drawn == drawn_) return;
        live.vmu_screen(dots_.data());
        drawn_ = drawn;
        ++revision_;
    }

    bool shown() const override { return drawn_ != 0; }

    uint64_t key(uint64_t) const override { return 0x9E3779B97F4A7C15ull ^ revision_; }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        font.draw(out, kit::margin, font.centred(kit::title, kit::margin + kit::badge / 2), look.tr("VMU screen"),
                  kit::title, look.ink);
        // The unit's shell (the white of the first VMUs), the screen's frame, then the LCD: a dark dot as the panel
        // draws it, an unlit one a faint ghost of it, as on the real glass.
        constexpr int scale = 8, lcd_w = 48 * scale, lcd_h = 32 * scale;
        const int shell_w = lcd_w + 64, shell_h = lcd_h + 64, x0 = (W - shell_w) / 2, y0 = 92;
        out.rounded_rect(x0, y0 + 4, shell_w, shell_h, 28, look.shadow(), 120);
        out.rounded_rect(x0, y0, shell_w, shell_h, 28, Rgb{236, 236, 232});
        out.rounded_rect(x0 + 20, y0 + 20, lcd_w + 24, lcd_h + 24, 10, Rgb{58, 62, 70});
        const int lx = x0 + 32, ly = y0 + 32;
        const Rgb glass{164, 196, 178}, ghost{150, 182, 165}, dot{28, 40, 56};
        out.fill_rect(lx, ly, lcd_w, lcd_h, glass);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 48; ++x)
                out.fill_rect(lx + x * scale, ly + y * scale, scale - 1, scale - 1,
                              dots_[size_t(y * 48 + x)] ? dot : ghost);
    }

private:
    std::array<uint8_t, 48 * 32> dots_{};
    uint64_t drawn_ = 0, revision_ = 0;
};

} // namespace

std::unique_ptr<View> make_controller(const std::string& system) {
    if (system == "dc") return std::make_unique<Vmu>();
    if (system == "atari") return std::make_unique<ConsolePanel>();
    if (system == "coleco") return std::make_unique<Keypad>(coleco_pad);
    if (system == "intellivision") return std::make_unique<Keypad>(intellivision_pad);
    if (system == "pico") return std::make_unique<Storyware>();
    return nullptr;
}

} // namespace play
