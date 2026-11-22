// SPDX-License-Identifier: GPL-3.0-or-later
// The shelves: Videos and Books. The upper screen is the picked item, large (its picture over a soft blur of
// itself, the title, how long, where it was left); the lower screen is the shelf, a grid of posters or covers that
// scrolls under a finger with the launcher's own flick and rubber band.
//
// Every tile is its own texture, drawn once when its poster arrives: scrolling and the focus gliding only move
// textures. The hero is drawn once per pick and cross-fades from the last one.
#include "launcher/kinetics.hpp"
#include "media/app.hpp"
#include "media/video.hpp"
#include "reader/pages.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

namespace media {
namespace fs = std::filesystem;
using play::kit::margin;

namespace {

constexpr int header_h = 64; // tabs
constexpr int grid_top = header_h;

struct Layout {
    int cols, tile_w, tile_h, pic_w, pic_h, gap;
};
Layout layout_for(bool books) {
    if (books) return {4, 134, 226, 134, 190, 14};
    return {3, 186, 148, 186, 105, 13};
}

struct Tile {
    Tex tex;
    bool have_poster = false, asked = false;
    Poster poster;
};

class Library : public Screen {
public:
    Library(Context& c, int shelf) : Screen(c), shelf_(shelf) {
        tab_spring_.snap(shelf_);
        open_root();
    }
    const char* name() const override { return "library"; }

    void open_root() {
        path_.clear();
        load(shelf_dirs().empty() ? "" : shelf_dirs().front(), true);
    }

    const std::vector<std::string>& shelf_dirs() const { return shelf_ == 0 ? c.video_dirs : c.book_dirs; }
    bool books() const { return shelf_ == 1; }

    // The items of a folder; at the root, every folder of the shelf together.
    void load(const std::string& dir, bool root) {
        items_.clear();
        tiles_.clear();
        if (root) {
            for (const auto& d : shelf_dirs()) {
                std::error_code ec;
                fs::create_directories(d, ec);
                auto part = list_folder(d, books());
                items_.insert(items_.end(), part.begin(), part.end());
            }
        } else {
            items_ = list_folder(dir, books());
        }
        // Started, not finished, newest first, ahead of the rest: "Continue" comes first.
        std::stable_sort(items_.begin(), items_.end(), [&](const Item& a, const Item& b) {
            Place pa = c.places.get(a.key), pb = c.places.get(b.key);
            bool ca = pa.started() && !pa.finished(), cb = pb.started() && !pb.finished();
            if (ca != cb) return ca;
            if (ca && cb) return pa.when > pb.when;
            return false;
        });
        tiles_.resize(items_.size());
        focus_ = 0;
        scroll_ = 0;
        motion_.stop();
        hero_stale_ = true;
        header_stale_ = true;
        bar_stale_ = true;
        focus_x_.snap(0), focus_y_.snap(0), focus_w_.snap(0), focus_h_.snap(0);
        place_focus(true);
        c.dirty = true;
    }

    // ---- Geometry

    Box tile_box(int i) const {
        Layout l = layout_for(books());
        int col = i % l.cols, row = i / l.cols;
        int total = l.cols * l.tile_w + (l.cols - 1) * l.gap;
        int x0 = (PW - total) / 2;
        return {x0 + col * (l.tile_w + l.gap), grid_top + 12 + row * (l.tile_h + l.gap), l.tile_w, l.tile_h};
    }
    int content_h() const {
        if (items_.empty()) return 0;
        Box last = tile_box(int(items_.size()) - 1);
        return last.y + last.h + 16 - grid_top;
    }
    int max_scroll() const { return std::max(0, content_h() - (PH - grid_top)); }
    double drawn_scroll() const { return motion_.dragging || motion_.active() ? motion_.position : double(scroll_); }

    void place_focus(bool snap) {
        if (items_.empty()) return;
        Box b = tile_box(focus_);
        // Keep the focused tile in view with a row of air: the grid glides there.
        int top = b.y - grid_top - 12, bottom = b.y + b.h - PH + 16;
        int want = scroll_;
        if (top < scroll_) want = std::max(0, top);
        if (bottom > scroll_) want = std::min(max_scroll(), bottom);
        if (want != scroll_) {
            motion_.glide(c.now, scroll_, want, c.animations() && !snap);
            scroll_ = want;
        }
        auto set = [&](Spring& s, double v) { snap || !c.animations() ? s.snap(v) : void(s.target = v); };
        set(focus_x_, b.x), set(focus_y_, b.y), set(focus_w_, b.w), set(focus_h_, b.h);
        hero_stale_ = true;
        c.dirty = true;
    }

    // ---- Input

    void key(Key k, bool repeat) override {
        Layout l = layout_for(books());
        const int n = int(items_.size());
        int f = focus_;
        switch (k) {
        case KLeft: f = std::max(0, f - 1); break;
        case KRight: f = std::min(n - 1, f + 1); break;
        case KUp: f = f - l.cols >= 0 ? f - l.cols : f; break;
        case KDown: f = f + l.cols < n ? f + l.cols : (f / l.cols < (n - 1) / l.cols ? n - 1 : f); break;
        case KL: switch_shelf(0); return;
        case KR: switch_shelf(shelf_ == 0 ? 1 : c.music_tab ? 2 : 1); return;
        case KA:
            if (!repeat) activate(focus_);
            return;
        case KB:
            if (!repeat) back();
            return;
        case KStart:
            if (!repeat) resume_latest();
            return;
        default: return;
        }
        if (n && f != focus_) {
            focus_ = f;
            place_focus(false);
        }
    }

    void touch(const TouchEvent& t) override {
        if (t.kind == TouchEvent::Down) {
            down_x_ = t.x, down_y_ = t.y;
            moved_ = false;
            pressed_ = -1;
            press_tab_ = -1;
            if (t.y < header_h) {
                press_tab_ = tab_at(t.x);
                if (press_tab_ >= 0) c.ripples.start(t.x, t.y, 60, t.at);
                header_stale_ = true;
                return;
            }
            motion_.begin(t.at, scroll_);
            grab_ = scroll_;
            pressed_ = hit(t.x, t.y);
            if (pressed_ >= 0) {
                press_at_ = t.at;
                c.ripples.start(t.x, t.y, 90, t.at);
            }
        } else if (t.kind == TouchEvent::Move) {
            if (std::hypot(t.x - down_x_, t.y - down_y_) > 10) {
                if (!moved_) c.ripples.release(t.at);
                moved_ = true;
                pressed_ = -1;
            }
            if (press_tab_ < 0 && motion_.dragging)
                scroll_ = motion_.drag(t.at, grab_ + (down_y_ - t.y), 0, max_scroll());
        } else {
            c.ripples.release(t.at);
            if (press_tab_ >= 0) {
                if (!moved_ && tab_at(t.x) == press_tab_) switch_shelf(press_tab_);
                press_tab_ = -1;
                header_stale_ = true;
            } else {
                motion_.release(t.at, 0, max_scroll());
                if (!moved_ && pressed_ >= 0) {
                    focus_ = pressed_;
                    place_focus(false);
                    activate(pressed_);
                }
            }
            pressed_ = -1;
        }
        c.dirty = true;
    }

    int hit(double x, double y) const {
        double sy = y + drawn_scroll();
        for (int i = 0; i < int(items_.size()); ++i)
            if (tile_box(i).contains(int(x), int(sy))) return i;
        return -1;
    }
    int tabs() const { return c.music_tab ? 3 : 2; }
    int tab_at(double x) const {
        for (int i = 0; i < tabs(); ++i)
            if (tab_box(i).contains(int(x), 20)) return i;
        if (!path_.empty() && x < 140) return 9; // the back pill
        return -1;
    }
    Box tab_box(int i) const {
        const int w = c.music_tab ? 128 : 150, x0 = PW - margin - tabs() * w - (tabs() - 1) * 6;
        return {x0 + i * (w + 6), 10, w, 44};
    }

    void switch_shelf(int s) {
        if (s == 9) {
            back();
            return;
        }
        if (s == 2) { // Music: the launcher's own player, which this program leaves for
            c.exit_code = 42;
            c.quit();
            return;
        }
        if (s == shelf_ && path_.empty()) return;
        shelf_ = s;
        tab_spring_.target = s;
        if (!c.animations()) tab_spring_.snap(s);
        slide_.snap(s == 1 ? 1 : -1); // the new shelf comes in from its side
        slide_.target = 0;
        if (!c.animations()) slide_.snap(0);
        open_root();
    }

    void back() {
        if (!path_.empty()) {
            std::string parent = path_.size() > 1 ? path_[path_.size() - 2] : "";
            std::string left = path_.back();
            path_.pop_back();
            load(parent.empty() ? (shelf_dirs().empty() ? "" : shelf_dirs().front()) : parent, parent.empty());
            for (int i = 0; i < int(items_.size()); ++i)
                if (items_[size_t(i)].path == left) focus_ = i;
            place_focus(true);
            slide_.snap(-1), slide_.target = 0;
            if (!c.animations()) slide_.snap(0);
            return;
        }
        c.quit();
    }

    void activate(int i) {
        if (i < 0 || i >= int(items_.size())) return;
        const Item it = items_[size_t(i)]; // a copy: opening a folder replaces items_
        if (it.kind == Kind::Folder) {
            path_.push_back(it.path);
            load(it.path, false);
            slide_.snap(1), slide_.target = 0;
            if (!c.animations()) slide_.snap(0);
            return;
        }
        if (it.kind == Kind::Video) {
            std::vector<Item> films;
            for (const auto& o : items_)
                if (o.kind == Kind::Video) films.push_back(o);
            // In file order, for "next episode".
            std::sort(films.begin(), films.end(),
                      [](const Item& a, const Item& b) { return play::natural_less(a.path, b.path); });
            c.push(make_watch(c, it, films));
        } else {
            c.push(make_read(c, it));
        }
    }

    void resume_latest() {
        for (int i = 0; i < int(items_.size()); ++i) {
            Place p = c.places.get(items_[size_t(i)].key);
            if (p.started() && !p.finished()) {
                focus_ = i;
                place_focus(false);
                activate(i);
                return;
            }
        }
    }

    // ---- Time

    bool update(uint32_t now) override {
        const double dt = last_ ? double(now - last_) : 16;
        last_ = now;
        bool moving = false;
        int s = scroll_;
        if (motion_.step(now, s, 0, max_scroll())) {
            scroll_ = s;
            moving = true;
        }
        for (Spring* sp : {&focus_x_, &focus_y_, &focus_w_, &focus_h_}) moving = sp->step(dt) || moving;
        moving = tab_spring_.step(dt, .0030, .85) || moving;
        moving = slide_.step(dt, .0032, .9) || moving;
        if (hero_fade_ < 1) {
            hero_fade_ = std::min(1.0, hero_fade_ + dt / 180.0);
            moving = true;
        }
        // Posters: ask for the ones in view (and a screen ahead), take what is done.
        const double top = drawn_scroll() + grid_top - PH / 2.0, bottom = drawn_scroll() + PH * 1.5;
        for (int i = int(items_.size()) - 1; i >= 0; --i) {
            Box b = tile_box(i);
            if (b.y + b.h < top || b.y > bottom || tiles_[size_t(i)].asked) continue;
            tiles_[size_t(i)].asked = true;
            if (c.posters.cached(items_[size_t(i)], tiles_[size_t(i)].poster)) {
                tiles_[size_t(i)].have_poster = true;
                tiles_[size_t(i)].tex.reset();
            } else {
                c.posters.want(items_[size_t(i)]);
            }
        }
        for (auto& [key, poster] : c.posters.take())
            for (size_t i = 0; i < items_.size(); ++i)
                if (items_[i].key == key) {
                    tiles_[i].poster = poster;
                    tiles_[i].have_poster = !poster.failed;
                    tiles_[i].tex.reset();
                    if (int(i) == focus_) hero_stale_ = true;
                    moving = true;
                }
        if (now - clock_at_ > 15000) clock_at_ = now, bar_stale_ = true, moving = true;
        return moving;
    }

    // ---- Drawing

    void draw_tile(SDL_Renderer* r, int i) {
        Tile& t = tiles_[size_t(i)];
        const Item& it = items_[size_t(i)];
        Layout l = layout_for(books());
        if (!t.tex) {
            Canvas cv(l.tile_w, l.tile_h);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
            const int pw = l.pic_w, ph = l.pic_h;
            const Place place = c.places.get(it.key);
            // The picture, or a card in its place.
            if (t.have_poster && !t.poster.px.empty()) {
                // Fill the frame (crop the overflow), area-averaged.
                double s = std::max(double(pw) / t.poster.w, double(ph) / t.poster.h);
                int sw = std::max(1, int(pw / s)), sh = std::max(1, int(ph / s));
                int sx = (t.poster.w - sw) / 2, sy = (t.poster.h - sh) / 2;
                std::vector<uint32_t> crop(size_t(sw) * sh), out(size_t(pw) * ph);
                for (int y = 0; y < sh; ++y)
                    std::copy(t.poster.px.begin() + (size_t(sy + y) * t.poster.w + sx),
                              t.poster.px.begin() + (size_t(sy + y) * t.poster.w + sx + sw),
                              crop.begin() + size_t(y) * sw);
                resample(crop.data(), sw, sh, out.data(), pw, ph);
                play::soft_round(cv, 1, 3, pw - 2, ph - 1, 10, c.look.shadow(), c.look.dark ? 110 : 45);
                blit_rounded(cv, out.data(), pw, ph, 0, 0, 10);
            } else {
                play::soft_round(cv, 1, 3, pw - 2, ph - 1, 10, c.look.shadow(), c.look.dark ? 110 : 45);
                play::glossy(cv, 0, 0, pw, ph, 10, it.kind == Kind::Folder ? c.look.focus : c.look.panel);
                glyph(cv,
                      it.kind == Kind::Folder ? Glyph::Folder
                      : books()               ? Glyph::Book
                                              : Glyph::Film,
                      pw / 2, ph / 2 - (it.kind == Kind::Folder ? 0 : 0), books() ? 40 : 34,
                      play::mix(c.look.panel, c.look.muted, 160), c.look.panel);
            }
            if (it.kind == Kind::Folder) {
                // A folder: a tab on the picture and how much is inside.
                cv.rounded_rect(8, 8, 34, 24, 8, Rgb{0, 0, 0}, 150);
                glyph(cv, Glyph::Folder, 25, 20, 16, Rgb{255, 255, 255}, Rgb{0, 0, 0});
                std::string n = std::to_string(it.children);
                int cw = c.font.measure(n, 16) + 14;
                cv.rounded_rect(pw - cw - 8, 8, cw, 24, 8, Rgb{0, 0, 0}, 150);
                c.font.draw(cv, pw - cw - 1, c.font.centred(16, 20), n, 16, Rgb{255, 255, 255});
            } else {
                // How long, in a chip on the picture's corner.
                double length = place.length > 0 ? place.length : t.poster.length;
                std::string chip = length <= 0 ? "" : books() ? std::to_string(int(length)) + " p" : clock_text(length);
                if (!chip.empty()) {
                    int cw = c.font.measure(chip, 16) + 14;
                    cv.rounded_rect(pw - cw - 6, ph - 30, cw, 24, 8, Rgb{0, 0, 0}, 160);
                    c.font.draw(cv, pw - cw + 1, c.font.centred(16, ph - 18), chip, 16, Rgb{255, 255, 255});
                }
                // Where it was left: a bar along the picture's foot.
                if (place.started()) {
                    double f = place.finished() ? 1.0 : place.fraction();
                    cv.fill_rect(10, ph - 5, pw - 20, 3, Rgb{0, 0, 0}, 140);
                    cv.fill_rect(10, ph - 5, std::max(3, int((pw - 20) * f)), 3,
                                 place.finished() ? c.look.muted : c.look.accent);
                }
            }
            // The title under the picture.
            const int ty = ph + 8;
            auto lines = wrap(c.font, it.name, 16, l.tile_w - 4, books() ? 1 : 2);
            for (size_t k = 0; k < lines.size(); ++k)
                c.font.draw(cv, 2, ty + 14 + int(k) * 19, lines[k], 16, k == 0 ? c.look.ink : c.look.muted);
            upload(r, t.tex, cv, true, true);
        }
        Box b = tile_box(i);
        double y = b.y - drawn_scroll() + slide_.value * 0;
        double x = b.x + slide_.value * PW * .35;
        // A pressed tile sinks a little, toward its centre.
        double press = (i == pressed_) ? std::min(1.0, (c.now - press_at_) / 90.0) : 0;
        double s = 1 - .04 * press;
        int alpha = int(255 * (1 - std::min(1.0, std::abs(slide_.value))));
        draw(r, t.tex, x + b.w * (1 - s) / 2, y + b.h * (1 - s) / 2, b.w * s, b.h * s, alpha);
    }

    void draw_lower(SDL_Renderer* r) override {
        if (!backdrop_) {
            Canvas bg(PW, PH);
            play::backdrop(bg, c.look);
            upload(r, backdrop_, bg);
        }
        draw(r, backdrop_, 0, 0);
        if (items_.empty()) {
            draw_empty(r);
        } else {
            Box clip{0, grid_top, PW, PH - grid_top};
            SDL_Rect cr{clip.x, clip.y, clip.w, clip.h};
            SDL_RenderSetClipRect(r, &cr);
            const double sc = drawn_scroll();
            for (int i = 0; i < int(items_.size()); ++i) {
                Box b = tile_box(i);
                if (b.y + b.h - sc < grid_top || b.y - sc > PH) continue;
                draw_tile(r, i);
            }
            // The focus corners glide from tile to tile (the picture part, as the launcher frames a cover).
            Layout l = layout_for(books());
            if (!motion_.dragging && std::abs(slide_.value) < .3)
                focus_corners(r, focus_x_.value + slide_.value * PW * .35, focus_y_.value - sc, focus_w_.value,
                              std::min(focus_h_.value, double(l.pic_h)), c.look.accent);
            SDL_RenderSetClipRect(r, nullptr);
            draw_scrollbar(r, sc);
        }
        draw_header(r);
    }

    void draw_scrollbar(SDL_Renderer* r, double sc) {
        if (max_scroll() <= 0) return;
        const double view = PH - grid_top, total = content_h();
        double h = std::max(36.0, view * view / total);
        double y = grid_top + (view - h) * std::clamp(sc / max_scroll(), 0.0, 1.0);
        int alpha = motion_.dragging || motion_.active() ? 180 : 70;
        fill(r, PW - 6, y + 4, 3, h - 8, c.look.muted, alpha);
    }

    void draw_empty(SDL_Renderer* r) {
        if (!empty_) {
            Canvas cv(PW, PH - grid_top);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
            Box card{margin, 40, PW - 2 * margin, 230};
            play::raised(cv, c.look, card.x, card.y, card.w, card.h, 18, c.look.panel);
            glyph(cv, books() ? Glyph::Book : Glyph::Film, PW / 2, card.y + 58, 52, c.look.accent, c.look.panel);
            std::string head = books() ? c.tr("No books yet") : c.tr("No videos yet");
            c.font.draw(cv, (PW - c.font.measure(head, 24)) / 2, card.y + 124, head, 24, c.look.ink);
            std::string folder = shelf_dirs().empty() ? "" : fs::path(shelf_dirs().front()).filename().string();
            std::string hint = books() ? c.tr("Copy PDF, EPUB, CBZ or folders of pages into the books folder.")
                                       : c.tr("Copy films and series into the videos folder.");
            auto lines = wrap(c.font, hint, 20, card.w - 48, 3);
            for (size_t i = 0; i < lines.size(); ++i)
                c.font.draw(cv, (PW - c.font.measure(lines[i], 20)) / 2, card.y + 160 + int(i) * 26, lines[i], 20,
                            c.look.muted);
            upload(r, empty_, cv, true);
        }
        draw(r, empty_, 0, grid_top);
    }

    void draw_header(SDL_Renderer* r) {
        if (header_stale_ || !header_) {
            Canvas cv(PW, header_h);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
            // A strip of the panel colour, as the launcher's bars, fading into the grid below.
            cv.fill_rect(0, 0, PW, header_h - 8, c.look.panel, 235);
            for (int i = 0; i < 8; ++i) cv.fill_rect(0, header_h - 8 + i, PW, 1, c.look.panel, 200 - i * 25);
            if (!path_.empty()) {
                Box b{margin - 8, 10, 120, 44};
                play::raised(cv, c.look, b.x, b.y, b.w, b.h, 14, c.look.panel, press_tab_ == 9 ? 1 : 0);
                play::icon(cv, c.look, play::Icon::Back, b.x + 24, b.y + 22, 20, c.look.ink);
                std::string folder = fs::path(path_.back()).filename().string();
                c.font.draw(cv, b.x + 42, c.font.centred(20, b.y + 22), c.font.fit(folder, 20, 70), 20, c.look.ink);
            } else {
                c.font.draw(cv, margin, c.font.centred(28, 32), c.tr("Media"), 28, c.look.ink);
            }
            // The segmented control: the lit half slides (drawn per frame below), the words stay.
            for (int i = 0; i < tabs(); ++i) play::sunken(cv, c.look, tab_box(i), 14);
            upload(r, header_, cv, true);
            header_stale_ = false;
        }
        draw(r, header_, 0, 0);
        // The lit tab: a glossy pill that slides between the two.
        Box a = tab_box(0), b = tab_box(1);
        double x = a.x + (b.x - a.x) * tab_spring_.value;
        if (!pill_) {
            Canvas cv(a.w, a.h + 4);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
            play::raised(cv, c.look, 2, 1, a.w - 4, a.h - 4, 12, c.look.accent);
            upload(r, pill_, cv, true, true);
        }
        draw(r, pill_, x, a.y + 1);
        if (!tab_words_) {
            Canvas cv(PW, header_h);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
            const char* names[3] = {"Videos", "Books", "Music"};
            const Glyph gl[3] = {Glyph::Film, Glyph::Book, Glyph::Audio};
            for (int i = 0; i < tabs(); ++i) {
                Box t = tab_box(i);
                std::string w = c.tr(names[i]);
                w = c.font.fit(w, 20, t.w - 40);
                int tw = c.font.measure(w, 20) + 28;
                int x0 = t.x + (t.w - tw) / 2;
                glyph(cv, gl[i], x0 + 10, t.y + t.h / 2 - 1, 20, Rgb{255, 255, 255}, Rgb{0, 0, 0});
                c.font.draw(cv, x0 + 28, c.font.centred(20, t.y + t.h / 2 - 1), w, 20, Rgb{255, 255, 255});
            }
            upload(r, tab_words_, cv, true);
        }
        // The words are white; on the unlit tab they take the ink colour (texture colour mod).
        for (int i = 0; i < tabs(); ++i) {
            Box t = tab_box(i);
            double lit = 1 - std::min(1.0, std::abs(tab_spring_.value - i));
            Rgb ink = play::mix(c.look.ink, c.look.on_accent, int(256 * lit));
            SDL_SetTextureColorMod(tab_words_.t, ink.r, ink.g, ink.b);
            SDL_Rect src{t.x, 0, t.w, header_h};
            draw_part(r, tab_words_, src, t.x, 0, t.w, header_h);
        }
        SDL_SetTextureColorMod(tab_words_.t, 255, 255, 255);
    }

    // ---- The upper screen

    void make_hero(SDL_Renderer* r) {
        blur_shade_prev_ = blur_shade_;
        hero_prev_ = std::move(hero_);
        blur_prev_ = std::move(blur_);
        hero_fade_ = c.animations() ? 0 : 1;
        Canvas cv(PW, PH - 36);
        std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0x00000000u);
        if (items_.empty()) {
            upload(r, hero_, cv, true);
            hero_stale_ = false;
            return;
        }
        const Item& it = items_[size_t(focus_)];
        const Tile& t = tiles_[size_t(focus_)];
        const Place place = c.places.get(it.key);
        const bool picture = t.have_poster && !t.poster.px.empty();
        // A blur of the picture behind everything: 24 px wide, scaled up smooth by the GPU.
        if (picture) {
            int bw = 24, bh = std::max(2, 24 * t.poster.h / std::max(1, t.poster.w));
            std::vector<uint32_t> small(size_t(bw) * bh);
            resample(t.poster.px.data(), t.poster.w, t.poster.h, small.data(), bw, bh);
            upload(r, blur_, small.data(), bw, bh, false, true);
            // How light the picture is: a pale cover needs a deeper shade behind white words.
            double sum = 0;
            for (uint32_t p : small) sum += (p & 255) * .3 + ((p >> 8) & 255) * .59 + ((p >> 16) & 255) * .11;
            const double lum = sum / std::max<size_t>(1, small.size());
            // Shade so the ground ends near 55 of 255 whatever the picture was.
            blur_shade_ = std::clamp(int(255 * (1 - 55.0 / std::max(55.0, lum))), 110, 225);
        } else {
            blur_shade_ = 120;
        }
        // The picture itself, as large as the screen lets it be beside the words.
        const bool tall = books() || (picture && t.poster.h > t.poster.w);
        int pw, ph, px, py;
        if (tall) {
            ph = 300, pw = picture ? std::min(240, ph * t.poster.w / std::max(1, t.poster.h)) : 212;
            px = margin + 8, py = 30;
        } else {
            pw = 440, ph = picture ? pw * t.poster.h / std::max(1, t.poster.w) : 248;
            ph = std::min(ph, 250);
            px = (PW - pw) / 2, py = 18;
        }
        play::soft_round(cv, px - 2, py + 6, pw + 4, ph + 4, 16, Rgb{0, 0, 0}, 120);
        if (picture) {
            std::vector<uint32_t> big(size_t(pw) * ph);
            // Fill the frame, cropping the overflow.
            double s = std::max(double(pw) / t.poster.w, double(ph) / t.poster.h);
            int sw = std::max(1, int(pw / s)), sh = std::max(1, int(ph / s));
            int sx = (t.poster.w - sw) / 2, sy = (t.poster.h - sh) / 2;
            std::vector<uint32_t> crop(size_t(sw) * sh);
            for (int y = 0; y < sh; ++y)
                std::copy(t.poster.px.begin() + (size_t(sy + y) * t.poster.w + sx),
                          t.poster.px.begin() + (size_t(sy + y) * t.poster.w + sx + sw), crop.begin() + size_t(y) * sw);
            resample(crop.data(), sw, sh, big.data(), pw, ph);
            blit_rounded(cv, big.data(), pw, ph, px, py, 14);
        } else {
            play::glossy(cv, px, py, pw, ph, 14, c.look.panel);
            glyph(cv,
                  it.kind == Kind::Folder ? Glyph::Folder
                  : books()               ? Glyph::Book
                                          : Glyph::Film,
                  px + pw / 2, py + ph / 2, 64, play::mix(c.look.panel, c.look.muted, 160), c.look.panel);
        }
        // The words: white on the dark blur, so they read over any picture.
        const Rgb white{255, 255, 255}, soft{214, 222, 232};
        int tx = tall ? px + pw + 28 : margin + 8, ty = tall ? py + 30 : py + ph + 44;
        int tw = PW - tx - margin - 8;
        auto title = wrap(c.font, it.name, 28, tw, tall ? 4 : 2);
        for (auto& line : title) {
            c.font.draw(cv, tx, ty, line, 28, white);
            ty += 34;
        }
        ty += 4;
        std::string meta;
        double length = place.length > 0 ? place.length : t.poster.length;
        if (it.kind == Kind::Folder) {
            meta = std::to_string(it.children) + " " + c.tr(it.children == 1 ? "item" : "items");
        } else if (books()) {
            if (length > 0) meta = std::to_string(int(length)) + " " + c.tr("pages");
            std::string ext = it.ext.empty() ? "" : it.ext.substr(1);
            for (auto& ch : ext) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (!ext.empty()) meta += (meta.empty() ? "" : "  ·  ") + ext;
        } else {
            if (length > 0) meta = clock_text(length);
            if (it.size >= 1000000) {
                char sz[32];
                std::snprintf(sz, sizeof sz, "%.1f GB", it.size / 1e9);
                if (it.size < 1e9) std::snprintf(sz, sizeof sz, "%d MB", int(it.size / 1e6));
                meta += (meta.empty() ? "" : "  ·  ") + std::string(sz);
            }
        }
        if (!meta.empty()) {
            c.font.draw(cv, tx, ty, meta, 20, soft);
            ty += 28;
        }
        if (place.started() && it.kind != Kind::Folder) {
            std::string where;
            if (place.finished()) where = c.tr("Finished");
            else if (books())
                where = c.tr("Page") + " " + std::to_string(int(place.position) + 1) +
                        (place.length > 0 ? " / " + std::to_string(int(place.length)) : "");
            else
                where = c.tr("Resume at") + " " + clock_text(place.position) + "  ·  " +
                        clock_text(std::max(0.0, place.length - place.position)) + " " + c.tr("left");
            c.font.draw(cv, tx, ty + 4, where, 20, white);
            ty += 18;
            int bw = std::min(tw, 360);
            cv.rounded_rect(tx, ty, bw, 6, 3, Rgb{255, 255, 255}, 60);
            cv.rounded_rect(tx, ty, std::max(6, int(bw * (place.finished() ? 1.0 : place.fraction()))), 6, 3,
                            c.look.accent);
            ty += 18;
        }
        // What A does, as the launcher's key tips.
        std::string a = it.kind == Kind::Folder                ? c.tr("Open")
                        : place.started() && !place.finished() ? (books() ? c.tr("Read on") : c.tr("Resume"))
                        : books()                              ? c.tr("Read")
                                                               : c.tr("Play");
        int cx = tx, cy = tall ? std::max(ty + 26, py + ph - 20) : std::min(PH - 36 - 26, ty + 22);
        cv.rounded_rect(cx, cy - 16, 32, 32, 16, c.look.accent);
        c.font.draw(cv, cx + (32 - c.font.measure("A", 20)) / 2, c.font.centred(20, cy), "A", 20, c.look.on_accent);
        c.font.draw(cv, cx + 42, c.font.centred(20, cy), a, 20, white);
        upload(r, hero_, cv, true);
        hero_stale_ = false;
    }

    void draw_upper(SDL_Renderer* r) override {
        if (hero_stale_) make_hero(r);
        // The ground: the picture's blur, darkened; without one, the theme's dark panel.
        fill(r, 0, 0, PW, PH, Rgb{12, 14, 20});
        const double f = ease_out(hero_fade_);
        if (blur_prev_ && f < 1) draw(r, blur_prev_, -40, -30, PW + 80, PH + 60, int(255 * (1 - f)));
        if (blur_) draw(r, blur_, -40, -30, PW + 80, PH + 60, int(255 * f));
        fill(r, 0, 0, PW, PH, Rgb{6, 8, 12}, int(blur_shade_prev_ + (blur_shade_ - blur_shade_prev_) * f));
        // The hero glides up a little as it fades in.
        if (hero_prev_ && f < 1) draw(r, hero_prev_, 0, 36 - 6 * f, int(255 * (1 - f)));
        if (hero_) draw(r, hero_, 0, 36 + 10 * (1 - f), int(255 * f));
        if (bar_stale_ || !bar_) {
            Canvas bar(PW, 36);
            std::string where =
                path_.empty() ? c.tr(books() ? "Books" : "Videos") : fs::path(path_.back()).filename().string();
            status_bar(bar, c.look, c.font, c.power, where);
            upload(r, bar_, bar);
            bar_stale_ = false;
        }
        draw(r, bar_, 0, 0);
    }

    void leave() override {}
    void resumed() override {
        // Back from a film or book: its place changed; its tile and the hero say so.
        for (auto& t : tiles_) t.tex.reset();
        hero_stale_ = true;
        bar_stale_ = true;
    }

private:
    int shelf_ = 0;
    std::vector<std::string> path_; // folders opened, below the shelf's root
    std::vector<Item> items_;
    std::vector<Tile> tiles_;
    int focus_ = 0, scroll_ = 0, grab_ = 0;
    rgds::ScrollMotion motion_;
    Spring focus_x_, focus_y_, focus_w_, focus_h_, tab_spring_, slide_;
    double down_x_ = 0, down_y_ = 0;
    bool moved_ = false;
    int pressed_ = -1, press_tab_ = -1;
    uint32_t press_at_ = 0, last_ = 0, clock_at_ = 0;
    Tex backdrop_, header_, pill_, tab_words_, empty_, hero_, hero_prev_, blur_, blur_prev_, bar_;
    bool hero_stale_ = true, header_stale_ = true, bar_stale_ = true;
    double hero_fade_ = 1;
    int blur_shade_ = 120, blur_shade_prev_ = 120;
};

} // namespace

std::unique_ptr<Screen> make_library(Context& c, int shelf) { return std::make_unique<Library>(c, shelf); }

} // namespace media
