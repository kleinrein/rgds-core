// SPDX-License-Identifier: GPL-3.0-or-later
// A book, comic or document, in one of three orientations (the reader's menu switches between them, Select steps
// through them):
//
// Sideways (book view): the handheld is held sideways, like a book. Each panel is a 480 x 640 portrait page, the
// hinge is the spine: a two-page spread is open across both screens, a page wider than tall (a scanned double page)
// spans both. Manga reads right to left. Held in the left hand the upper screen is the left page; in the right hand,
// the right. Everything on the lower panel (the menu, the contents) is drawn turned too, upright for the reader.
//
// Upright (two screens): held normally, the book runs down both screens: a reflowing book shows two pages, the
// upper one read first; a fixed page (a PDF, a comic) is one page across both screens.
//
// With controls (page view): held normally, the page on the upper screen, and on the lower one the place in the
// book: the page number, a slider through the whole book, the chapter, and buttons.
//
// Both touch screens are pages: a page follows the finger when dragged and settles with a spring; a tap near an
// outer edge turns, a tap in the middle opens the menu. The menu shows the place in the book with a slider, the
// three orientations as pictures of the handheld, the hand it is held in, the paper, the text size, the reading
// direction, the contents and Close. Every change shows at once behind it.
//
// Pages are rendered by MuPDF on its own thread (book.cpp) at exactly the size they are shown, then only moved.
#include "launcher/kinetics.hpp"
#include "media/app.hpp"
#include "media/book.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace media {
using play::kit::margin;

namespace {

constexpr int RW = 480, RH = 640; // a page in book view (reader's frame)
const int text_sizes_em[] = {8, 9, 10, 11, 12, 13, 14, 16, 18, 21};
constexpr int text_size_count = int(sizeof text_sizes_em / sizeof text_sizes_em[0]);

std::map<std::string, std::string> parse_extra(const std::string& s) {
    std::map<std::string, std::string> m;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ';')) {
        auto eq = part.find('=');
        if (eq != std::string::npos) m[part.substr(0, eq)] = part.substr(eq + 1);
    }
    return m;
}

enum class View { Book, Stack, Page };

// The page view's buttons along the foot of the lower screen.
enum Chip { CContents, CPaper, CText, CDirection, CView, CCount };

// What the menu holds, in reading order.
enum MenuItem {
    MSlider,
    MBook,
    MStack,
    MPage,
    MHandLeft,
    MHandRight,
    MWhite,
    MSepia,
    MNight,
    MSmaller,
    MLarger,
    MLtr,
    MRtl,
    MContents,
    MClose,
    MCount
};

class Read : public Screen {
public:
    Read(Context& c, const Item& item) : Screen(c), item_(item) {
        std::string error;
        if (!book_.open(item.path, error)) {
            error_ = error;
            return;
        }
        Place p = c.places.get(item.key);
        auto ex = parse_extra(p.extra);
        rtl_ = ex.count("dir") ? ex["dir"] == "rtl" : looks_like_manga();
        view_ = ex["mode"] == "page" ? View::Page : ex["mode"] == "stack" ? View::Stack : View::Book;
        paper_ = ex["paper"] == "night" ? Paper::Night : ex["paper"] == "sepia" ? Paper::Sepia : Paper::White;
        em_ = ex.count("em") ? std::clamp(std::atoi(ex["em"].c_str()), 8, 21) : 11;
        right_hand_ = ex["hold"] == "right";
        page_ = std::clamp(int(p.position), 0, std::max(0, book_.count() - 1));
        if (book_.reflows()) {
            // Places in a reflowing book are kept as a share of the book: the page count moves with the text size.
            double share = p.length > 0 ? p.position / p.length : 0;
            relayout();
            page_ = std::clamp(int(std::lround(share * book_.count())), 0, std::max(0, book_.count() - 1));
        }
        page_ = align(page_);
        outline_ = book_.outline();
        stale_ = true;
        focus_ = view_item();
    }
    ~Read() override { book_.close(); }
    const char* name() const override { return "read"; }
    bool upper_touch() const override { return error_.empty(); }

    bool looks_like_manga() const {
        std::string p = item_.path;
        for (auto& ch : p) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return p.find("manga") != std::string::npos;
    }

    void leave() override {
        if (!error_.empty()) return;
        Place p;
        p.position = page_;
        p.length = book_.count();
        if (page_ + spread(page_).size() >= size_t(book_.count())) p.position = p.length; // the last spread: finished
        std::ostringstream ex;
        ex << "dir=" << (rtl_ ? "rtl" : "ltr") << ";mode="
           << (view_ == View::Book    ? "book"
               : view_ == View::Stack ? "stack"
                                      : "page")
           << ";paper="
           << (paper_ == Paper::Night   ? "night"
               : paper_ == Paper::Sepia ? "sepia"
                                        : "white")
           << ";em=" << em_ << ";hold=" << (right_hand_ ? "right" : "left");
        p.extra = ex.str();
        c.places.set(item_.key, p);
        c.places.save();
    }

    // ---- Layout of pages

    bool book_view() const { return view_ == View::Book; }
    // Upright on both screens, a fixed page is one tall page across both.
    bool tall() const { return view_ == View::Stack && !book_.reflows(); }
    // The box a page is laid out in (reflowing books).
    int box_w() const { return book_view() ? RW : PW; }
    int box_h() const { return book_view() ? RH : PH; }
    // The box page i is rendered into.
    void page_box(int i, int& w, int& h) const {
        if (book_view()) w = wide(i) ? 2 * RW : RW, h = RH;
        else if (tall()) w = PW, h = 2 * PH;
        else w = PW, h = PH;
    }
    // A page box in the panel's own pixels: pages are rendered that big and drawn at their layout size, so they are
    // as sharp as the panel (1.6x on the RG DS Plus).
    int px(int v) const { return int(std::lround(v * c.scale)); }
    // How far apart two spreads lie along x.
    double span() const { return book_view() ? 2 * RW : PW; }
    bool wide(int i) const {
        double w = 0, h = 0;
        return const_cast<Book&>(book_).shape(i, w, h) && w > h * 1.15;
    }
    bool cover_alone() const { return !book_.reflows() && book_.count() > 2; }
    // The pages open now, in reading order (one or two).
    std::vector<int> spread(int p) const {
        const int n = book_.count();
        if (p < 0 || p >= n) return {};
        if (view_ == View::Page || tall()) return {p};
        if (view_ == View::Stack) return p + 1 < n ? std::vector<int>{p, p + 1} : std::vector<int>{p};
        if (wide(p) || (p == 0 && cover_alone()) || p + 1 >= n || wide(p + 1)) return {p};
        return {p, p + 1};
    }
    int next_of(int p) const { return p + int(std::max<size_t>(1, spread(p).size())); }
    int prev_of(int p) const {
        if (p <= 0) return -1;
        if (p >= 2 && next_of(p - 2) == p) return p - 2;
        return p - 1;
    }
    // The first page of the spread holding page p.
    int align(int p) const {
        if (view_ == View::Page || tall()) return p;
        int q = 0;
        while (q < book_.count()) {
            int n = next_of(q);
            if (n > p) return q;
            q = n;
        }
        return std::max(0, book_.count() - 1);
    }

    void relayout() {
        if (!book_.reflows()) return;
        // Laid out in points at 2/3 of the box: the text is drawn 1.5x, the same scale as the launcher's text.
        int keep = page_;
        page_ = book_.relayout(box_w() * 2 / 3, box_h() * 2 / 3, em_, keep);
        clear_textures();
    }

    // ---- Turning

    // Positive goes forward in the book.
    void turn(int d) {
        if (!error_.empty()) return;
        int to = d > 0 ? next_of(page_) : prev_of(page_);
        if (to < 0 || to >= book_.count()) {
            // At an end: a small give, and back.
            shift_.velocity += -d * fwd() * 1.4;
            return;
        }
        page_ = to;
        shift_.value += d * fwd() * span();
        shift_.target = 0;
        if (!c.animations()) shift_.snap(0);
        stale_ = true;
        menu_stale_ = true;
        leave();
    }
    double fwd() const { return rtl_ ? -1 : 1; } // the next spread lies this way along x
    void go(int page) {
        page_ = align(std::clamp(page, 0, std::max(0, book_.count() - 1)));
        shift_.snap(0);
        stale_ = true;
        menu_stale_ = true;
        leave();
    }

    // ---- Changing how the book is shown

    MenuItem view_item() const {
        return view_ == View::Book ? MBook : view_ == View::Stack ? MStack : MPage;
    }
    void set_view(View v) {
        if (v == view_) return;
        if (c.crossfade && c.animations()) c.crossfade();
        view_ = v;
        shift_.snap(0);
        dragging_ = sliding_ = false;
        clear_textures();
        relayout();
        page_ = align(page_);
        number_.reset();
        menu_tex_.reset();
        menu_stale_ = stale_ = true;
        if (sheet_.open) sheet_.hide(), sheet_.rise.snap(0);
        leave();
    }
    void set_hand(bool right) {
        if (right == right_hand_) return;
        if (c.crossfade && c.animations()) c.crossfade();
        right_hand_ = right;
        clear_textures();
        number_.reset();
        menu_stale_ = true;
        leave();
    }
    void set_paper(Paper p) {
        if (p == paper_) return;
        paper_ = p;
        clear_textures();
        menu_stale_ = true;
        leave();
    }
    void set_text(int step) {
        if (!book_.reflows()) return;
        int at = 0;
        for (int i = 0; i < text_size_count; ++i)
            if (text_sizes_em[i] == em_) at = i;
        int to = std::clamp(at + step, 0, text_size_count - 1);
        if (to == at) return;
        em_ = text_sizes_em[to];
        relayout();
        page_ = align(page_);
        menu_stale_ = stale_ = true;
        leave();
    }
    void set_rtl(bool r) {
        if (r == rtl_) return;
        rtl_ = r;
        menu_stale_ = stale_ = true;
        leave();
    }

    // ---- Input

    void key(Key k, bool repeat) override {
        if (!error_.empty()) {
            if (k == KB || k == KA) c.pop();
            return;
        }
        // Held sideways, the D-pad turns with the reader: its Up points left.
        const Key t = book_view() ? turn_key(k) : k;
        if (sheet_.open) {
            sheet_.key(t);
            return;
        }
        if (menu_open_) {
            menu_key(t, repeat);
            return;
        }
        switch (t) {
        case KRight: turn(rtl_ ? -1 : 1); break;
        case KLeft: turn(rtl_ ? 1 : -1); break;
        case KDown: turn(1); break;
        case KUp: turn(-1); break;
        case KA:
        case KR: turn(1); break;
        case KL: turn(-1); break;
        case KL2: go(page_ - 10); break;
        case KR2: go(page_ + 10); break;
        case KX:
        case KStart:
            if (!repeat) open_menu(view_item());
            break;
        case KY:
            if (!repeat) open_contents();
            break;
        case KSelect: // the next orientation
            if (!repeat)
                set_view(view_ == View::Book    ? View::Stack
                         : view_ == View::Stack ? View::Page
                                                : View::Book);
            break;
        case KB:
            if (!repeat) {
                leave();
                c.pop();
            }
            break;
        default: break;
        }
    }
    // The D-pad as the reader sees it, held sideways.
    Key turn_key(Key k) const {
        // Left hand (turned anticlockwise): the pad's Up points left, Right points up.
        if (!right_hand_) {
            if (k == KUp) return KLeft;
            if (k == KDown) return KRight;
            if (k == KLeft) return KDown;
            if (k == KRight) return KUp;
        } else {
            if (k == KUp) return KRight;
            if (k == KDown) return KLeft;
            if (k == KLeft) return KUp;
            if (k == KRight) return KDown;
        }
        return k;
    }

    // Which panel holds the left page in book view.
    bool lower_is_right() const { return !right_hand_; }
    // A panel's pixels -> the book view's reader frame (x across the whole 960 spread, y down the 640 page). Both
    // panels are turned the same way; the one holding the right page starts at x = 480.
    void to_reader(bool upper, double px, double py, double& xr, double& yr) const {
        if (!right_hand_) xr = py, yr = PW - px;
        else xr = PH - py, yr = px;
        if (upper != lower_is_right()) xr += RW;
    }

    void touch(const TouchEvent& ev) override {
        if (!error_.empty()) {
            if (ev.kind == TouchEvent::Up && !ev.upper) c.pop();
            return;
        }
        // The lower panel as the menu and the contents see it: turned upright for the reader in book view.
        TouchEvent local = ev;
        if (book_view() && !ev.upper) {
            double xr, yr;
            to_reader(false, ev.x, ev.y, xr, yr);
            local.x = xr - (lower_is_right() ? RW : 0);
            local.y = yr;
        }
        if (sheet_.open || menu_open_) {
            if (ev.upper) {
                // A tap on the other screen puts the menu away.
                if (ev.kind == TouchEvent::Down) {
                    if (sheet_.open) sheet_.hide();
                    else close_menu();
                }
                return;
            }
            if (sheet_.open) sheet_.touch(local);
            else menu_touch(local);
            return;
        }
        if (view_ == View::Page && !ev.upper) {
            touch_page(ev);
            return;
        }
        // A point in the frame the pages are laid out in: the spread (book view), both screens down (upright), or
        // the upper screen (page view).
        double fx = ev.x, fy = ev.y;
        if (book_view()) to_reader(ev.upper, ev.x, ev.y, fx, fy);
        else if (view_ == View::Stack && !ev.upper) fy += PH;
        touch_pages(ev, fx);
    }

    void touch_pages(const TouchEvent& t, double fx) {
        if (t.kind == TouchEvent::Down) {
            down_x_ = fx;
            moved_ = false;
            grab_shift_ = shift_.value;
            finger_.reset();
            finger_.add(t.at, fx);
            dragging_ = true;
        } else if (t.kind == TouchEvent::Move && dragging_) {
            finger_.add(t.at, fx);
            if (std::abs(fx - down_x_) > 10) moved_ = true;
            if (moved_) {
                double s = grab_shift_ + (fx - down_x_);
                // Past the first or last spread the page gives, but only a little.
                bool can = (s * fwd() < 0 && next_of(page_) < book_.count()) || (s * fwd() > 0 && page_ > 0);
                shift_.snap(can ? s : rgds::ScrollMotion::band(s) * .6);
                c.dirty = true;
            }
        } else if (t.kind == TouchEvent::Up && dragging_) {
            dragging_ = false;
            if (!moved_) {
                // Outer parts turn, the middle (around the spine in book view) opens the menu.
                const double width = book_view() ? 2 * RW : PW;
                const double edge = book_view() ? RW * .6 / width : .3;
                const double f = fx / width;
                if (f < edge) turn(rtl_ ? 1 : -1);
                else if (f > 1 - edge) turn(rtl_ ? -1 : 1);
                else open_menu(view_item());
                return;
            }
            const double v = finger_.estimate(t.at); // px/ms along x
            const double s = shift_.value;
            if (s * fwd() < -110 || v * fwd() < -.45) turn_from_drag(1);
            else if (s * fwd() > 110 || v * fwd() > .45) turn_from_drag(-1);
            else shift_.target = 0; // back where it was
            shift_.velocity = v;
        }
    }
    void turn_from_drag(int d) {
        int to = d > 0 ? next_of(page_) : prev_of(page_);
        if (to < 0 || to >= book_.count()) {
            shift_.target = 0;
            return;
        }
        page_ = to;
        shift_.value += d * fwd() * span();
        shift_.target = 0;
        stale_ = true;
        menu_stale_ = true;
        leave();
    }

    // ---- Page view's lower screen

    Box slider_box() const { return {margin + 10, 236, PW - 2 * margin - 20, 40}; }
    Box button_box(int i) const { // 0 previous, 1 next
        return i == 0 ? Box{margin, 296, (PW - 2 * margin - 14) / 2, 64}
                      : Box{PW / 2 + 7, 296, (PW - 2 * margin - 14) / 2, 64};
    }
    Box chip_box(int i) const {
        const int n = CCount, gap = 10, w = (PW - 2 * margin - (n - 1) * gap) / n;
        return {margin + i * (w + gap), 392, w, 64};
    }

    void touch_page(const TouchEvent& t) {
        if (t.kind == TouchEvent::Down) {
            pressed_ = -1;
            Box s = slider_box();
            if (t.y > s.y - 20 && t.y < s.y + s.h + 20) {
                sliding_ = true;
                slide_to(s, t.x);
                return;
            }
            for (int i = 0; i < 2; ++i)
                if (button_box(i).contains(int(t.x), int(t.y))) pressed_ = 100 + i;
            for (int i = 0; i < CCount; ++i)
                if (chip_box(i).contains(int(t.x), int(t.y))) pressed_ = i;
            if (pressed_ >= 0) c.ripples.start(t.x, t.y, 60, t.at);
            stale_ = true;
        } else if (t.kind == TouchEvent::Move) {
            if (sliding_) slide_to(slider_box(), t.x);
        } else {
            c.ripples.release(t.at);
            if (sliding_) {
                sliding_ = false;
                go(page_);
            } else if (pressed_ >= 100) {
                turn(pressed_ == 100 ? -1 : 1);
            } else if (pressed_ == CContents) {
                open_contents();
            } else if (pressed_ >= 0) {
                const MenuItem at[CCount] = {MContents, MenuItem(MWhite + int(paper_)), MLarger, rtl_ ? MRtl : MLtr,
                                             view_item()};
                open_menu(at[pressed_]);
            }
            pressed_ = -1;
            stale_ = true;
        }
    }
    // A slider through the book: the page follows the finger (rendered as it is reached).
    void slide_to(const Box& s, double x) {
        double f = std::clamp((x - s.x) / s.w, 0.0, 1.0);
        if (rtl_ && view_ != View::Page) f = 1 - f;
        int n = std::max(1, book_.count());
        int to = align(std::clamp(int(f * (n - 1) + .5), 0, n - 1));
        if (to != page_) {
            page_ = to;
            shift_.snap(0);
        }
        stale_ = true;
        menu_stale_ = true;
    }

    // ---- The menu

    // The frame it is drawn in: upright for the reader, so a portrait page in book view.
    int menu_w() const { return book_view() ? RW : PW; }
    int menu_h() const { return book_view() ? RH : PH; }

    struct MenuLayout {
        Box panel, title;
        Box item[MCount];
        bool shown[MCount] = {};
        Box orientation_caption;
        Box paper_label, text_label, direction_label, text_value;
        std::vector<std::vector<MenuItem>> rows; // for the D-pad
    };

    MenuLayout menu_layout() const {
        MenuLayout L;
        const int W = menu_w(), H = menu_h();
        const bool portrait = book_view();
        const int m = 20;
        const int content = portrait ? 596 : 356;
        L.panel = {10, H - content, W - 20, content + 24}; // runs off the bottom
        const int x0 = L.panel.x + m, w = L.panel.w - 2 * m, top = L.panel.y;
        auto put = [&](MenuItem i, Box b) {
            L.item[i] = b;
            L.shown[i] = true;
        };
        L.title = {x0, top + 18, w, 48};
        if (portrait) {
            put(MSlider, {x0, top + 84, w, 36});
            L.orientation_caption = {x0, top + 156, w, 20};
        } else {
            put(MSlider, {x0, top + 70, w, 36});
            L.orientation_caption = {x0, top + 114, w, 20};
        }
        // Three pictures of the handheld.
        const int tile_y = L.orientation_caption.y + 24, tile_h = portrait ? 100 : 88, tgap = 10;
        const int tw = (w - 2 * tgap) / 3;
        for (int i = 0; i < 3; ++i) put(MenuItem(MBook + i), {x0 + i * (tw + tgap), tile_y, tw, tile_h});
        int y = tile_y + tile_h + 14;
        const int sw = 44, sgap = 12; // paper swatches
        auto swatches = [&](int right, int cy) {
            for (int i = 0; i < 3; ++i)
                put(MenuItem(MWhite + i), {right - (3 - i) * sw - (2 - i) * sgap, cy - sw / 2, sw, sw});
        };
        auto stepper = [&](int right, int cy) {
            const int bw = 52, vw = 70, bh = 44;
            put(MLarger, {right - bw, cy - bh / 2, bw, bh});
            L.text_value = {right - bw - vw, cy - bh / 2, vw, bh};
            put(MSmaller, {right - 2 * bw - vw, cy - bh / 2, bw, bh});
        };
        if (portrait) {
            const int hw = (w - tgap) / 2;
            put(MHandLeft, {x0, y, hw, 44});
            put(MHandRight, {x0 + hw + tgap, y, w - hw - tgap, 44});
            y += 44 + 16;
            L.paper_label = {x0, y, 140, 52};
            swatches(x0 + w, y + 26);
            y += 52 + 8;
            L.text_label = {x0, y, 140, 52};
            stepper(x0 + w, y + 26);
            y += 52 + 8;
            L.direction_label = {x0, y, 140, 52};
            const int segw = 280;
            put(MLtr, {x0 + w - segw, y + 4, segw / 2, 44});
            put(MRtl, {x0 + w - segw / 2, y + 4, segw / 2, 44});
            y += 52 + 16;
            const int bw = (w - tgap) / 2;
            put(MContents, {x0, y, bw, 52});
            put(MClose, {x0 + bw + tgap, y, w - bw - tgap, 52});
            L.rows = {{MSlider},          {MBook, MStack, MPage}, {MHandLeft, MHandRight}, {MWhite, MSepia, MNight},
                      {MSmaller, MLarger}, {MLtr, MRtl},           {MContents, MClose}};
        } else {
            const int hw = (w - 20) / 2;
            L.paper_label = {x0, y, hw, 52};
            swatches(x0 + hw, y + 26);
            L.text_label = {x0 + hw + 20, y, hw, 52};
            stepper(x0 + w, y + 26);
            y += 52 + 10;
            put(MLtr, {x0, y, hw / 2, 48});
            put(MRtl, {x0 + hw / 2, y, hw - hw / 2, 48});
            const int bw = (hw - tgap) / 2;
            put(MContents, {x0 + hw + 20, y, bw, 48});
            put(MClose, {x0 + hw + 20 + bw + tgap, y, hw - bw - tgap, 48});
            L.rows = {{MSlider},
                      {MBook, MStack, MPage},
                      {MWhite, MSepia, MNight, MSmaller, MLarger},
                      {MLtr, MRtl, MContents, MClose}};
        }
        return L;
    }

    void open_menu(MenuItem focus) {
        menu_open_ = true;
        menu_rise_.target = 1;
        if (!c.animations()) menu_rise_.snap(1);
        focus_ = focus;
        menu_pressed_ = -1;
        menu_stale_ = true;
        c.dirty = true;
    }
    void close_menu() {
        menu_open_ = false;
        menu_rise_.target = 0;
        if (!c.animations()) menu_rise_.snap(0);
        menu_sliding_ = false;
        menu_pressed_ = -1;
        c.dirty = true;
    }

    void activate(MenuItem i) {
        switch (i) {
        case MSlider: break;
        case MBook: set_view(View::Book); break;
        case MStack: set_view(View::Stack); break;
        case MPage: set_view(View::Page); break;
        case MHandLeft: set_hand(false); break;
        case MHandRight: set_hand(true); break;
        case MWhite: set_paper(Paper::White); break;
        case MSepia: set_paper(Paper::Sepia); break;
        case MNight: set_paper(Paper::Night); break;
        case MSmaller: set_text(-1); break;
        case MLarger: set_text(1); break;
        case MLtr: set_rtl(false); break;
        case MRtl: set_rtl(true); break;
        case MContents:
            close_menu();
            menu_rise_.snap(0);
            open_contents();
            break;
        case MClose:
            leave();
            c.pop();
            break;
        default: break;
        }
        menu_stale_ = true;
    }

    void menu_key(Key k, bool repeat) {
        MenuLayout L = menu_layout();
        int row = 0, col = 0;
        for (size_t r = 0; r < L.rows.size(); ++r)
            for (size_t i = 0; i < L.rows[r].size(); ++i)
                if (L.rows[r][i] == focus_) row = int(r), col = int(i);
        auto centre = [&](MenuItem i) { return L.item[i].x + L.item[i].w / 2; };
        auto to_row = [&](int r) {
            r = std::clamp(r, 0, int(L.rows.size()) - 1);
            // The item nearest across.
            const int x = centre(focus_);
            MenuItem best = L.rows[size_t(r)][0];
            for (MenuItem i : L.rows[size_t(r)])
                if (std::abs(centre(i) - x) < std::abs(centre(best) - x)) best = i;
            focus_ = best;
        };
        switch (k) {
        case KUp: to_row(row - 1); break;
        case KDown: to_row(row + 1); break;
        case KLeft:
        case KRight: {
            const int d = k == KRight ? 1 : -1;
            if (focus_ == MSlider) {
                turn(rtl_ && view_ != View::Page ? -d : d);
                break;
            }
            if (repeat) break;
            const auto& r = L.rows[size_t(row)];
            focus_ = r[size_t(std::clamp(col + d, 0, int(r.size()) - 1))];
            break;
        }
        case KL: turn(-1); break;
        case KR: turn(1); break;
        case KL2: go(page_ - 10); break;
        case KR2: go(page_ + 10); break;
        case KA:
            if (!repeat) activate(focus_);
            break;
        case KY:
            if (!repeat) activate(MContents);
            break;
        case KSelect:
            if (!repeat) {
                set_view(view_ == View::Book    ? View::Stack
                         : view_ == View::Stack ? View::Page
                                                : View::Book);
                focus_ = view_item();
            }
            break;
        case KB:
        case KX:
        case KStart:
            if (!repeat) close_menu();
            break;
        default: break;
        }
        menu_stale_ = true;
    }

    void menu_touch(const TouchEvent& t) {
        MenuLayout L = menu_layout();
        const int x = int(t.x), y = int(t.y);
        if (t.kind == TouchEvent::Down) {
            menu_pressed_ = -1;
            if (!L.panel.contains(x, y)) {
                close_menu(); // a tap above the menu puts it away
                return;
            }
            Box s = L.item[MSlider];
            if (y > s.y - 14 && y < s.y + s.h + 14) {
                menu_sliding_ = true;
                focus_ = MSlider;
                slide_to(s, t.x);
                return;
            }
            for (int i = 0; i < MCount; ++i)
                if (L.shown[i] && L.item[i].contains(x, y)) menu_pressed_ = i;
            menu_stale_ = true;
        } else if (t.kind == TouchEvent::Move) {
            if (menu_sliding_) slide_to(L.item[MSlider], t.x);
            else if (menu_pressed_ >= 0 && !L.item[menu_pressed_].contains(x, y)) {
                menu_pressed_ = -1;
                menu_stale_ = true;
            }
        } else {
            if (menu_sliding_) {
                menu_sliding_ = false;
                go(page_);
            } else if (menu_pressed_ >= 0) {
                MenuItem i = MenuItem(menu_pressed_);
                menu_pressed_ = -1;
                focus_ = i;
                activate(i);
            }
            menu_stale_ = true;
        }
    }

    void open_contents() {
        std::vector<Sheet::Row> rows;
        if (outline_.empty()) {
            rows.push_back({c.tr("This book has no contents list"), "", false});
            show(c.tr("Contents"), rows, 0, [](int) {});
            return;
        }
        int at = 0;
        for (int i = 0; i < int(outline_.size()); ++i) {
            const Heading& h = outline_[size_t(i)];
            if (h.page <= page_) at = i;
            rows.push_back({h.title, std::to_string(h.page + 1), false, h.level});
        }
        if (at < int(rows.size())) rows[size_t(at)].chosen = true;
        show(c.tr("Contents"), rows, at, [this](int i) { go(outline_[size_t(i)].page); });
    }

    void show(const std::string& title, std::vector<Sheet::Row> rows, int focus, std::function<void(int)> f) {
        sheet_.w = book_view() ? RW : PW;
        sheet_.h = book_view() ? RH : PH;
        sheet_.tex.reset();
        sheet_.show(title, std::move(rows), focus, std::move(f), c.animations());
    }

    std::string chapter() const {
        std::string out;
        for (const auto& h : outline_)
            if (h.page <= page_) out = h.title;
        return out;
    }
    std::string where() const {
        auto sp = spread(page_);
        std::string s = sp.size() == 2 ? std::to_string(sp[0] + 1) + "–" + std::to_string(sp[1] + 1)
                                       : std::to_string(page_ + 1);
        return s + " / " + std::to_string(book_.count());
    }
    double progress() const { return book_.count() > 1 ? double(page_) / (book_.count() - 1) : 1; }

    // The handheld drawn small: two panels in the orientation a tile stands for.
    void handheld(Canvas& cv, MenuItem which, int cx, int cy, Rgb ink, Rgb ground) {
        auto screen = [&](int x, int y, int w, int h, bool text, bool controls) {
            cv.rounded_rect(x, y, w, h, 4, ink);
            cv.rounded_rect(x + 2, y + 2, w - 4, h - 4, 3, ground);
            if (text) {
                const bool across = w > h; // lines run along the long side as read
                const int lines = across ? (h - 8) / 5 : (h - 10) / 5;
                for (int i = 0; i < lines; ++i) {
                    int lw = (i % 3 == 2 ? 60 : 85) * (w - 12) / 100;
                    cv.fill_rect(x + 6, y + 6 + i * 5, lw, 2, ink, 170);
                }
            }
            if (controls) {
                cv.rounded_rect(x + 6, y + h / 2 - 6, w - 12, 4, 2, ink, 170);
                for (int i = 0; i < 3; ++i) cv.rounded_rect(x + 6 + i * (w - 12) / 3, y + h - 12, (w - 24) / 3, 6, 2, ink, 140);
            }
        };
        if (which == MBook) {
            // Held sideways: two portrait screens side by side, a page on each, the spine between.
            const int w = 30, h = 42;
            screen(cx - w - 3, cy - h / 2, w, h, true, false);
            screen(cx + 3, cy - h / 2, w, h, true, false);
        } else {
            const int w = 50, h = 26;
            screen(cx - w / 2, cy - h - 3, w, h, true, false);
            screen(cx - w / 2, cy + 3, w, h, which == MStack, which == MPage);
        }
    }

    void draw_menu_canvas(SDL_Renderer* r) {
        const int W = menu_w(), H = menu_h();
        const MenuLayout L = menu_layout();
        const auto& K = c.look;
        FontScale fs_(c.font, c.scale);
        Canvas cv(W, H, c.scale);
        std::fill(cv.data(), cv.data() + size_t(cv.pixel_w()) * cv.pixel_h(), 0x00000000u);
        const Box& p = L.panel;
        play::soft_round(cv, p.x, p.y - 4, p.w, p.h, 24, K.shadow(), K.dark ? 120 : 60);
        play::glossy(cv, p.x, p.y, p.w, p.h, 24, K.panel, 0, true);
        cv.rounded_rect(W / 2 - 22, p.y + 8, 44, 5, 2, play::mix(K.panel, K.muted, 120));
        const int small = play::kit::small, body = play::kit::body;
        // Title, and where we are.
        const std::string place = where() + "  ·  " + std::to_string(int(std::lround(progress() * 100))) + "%";
        const int place_w = c.font.measure(place, small);
        const std::string name = book_.title().empty() ? item_.name : book_.title();
        c.font.draw(cv, L.title.x, L.title.y + 20, c.font.fit(name, play::kit::title, L.title.w - place_w - 16),
                    play::kit::title, K.ink);
        c.font.draw(cv, L.title.x + L.title.w - place_w, L.title.y + 18, place, small, K.muted);
        std::string under = chapter();
        if (under.empty()) under = book_.author();
        if (!under.empty())
            c.font.draw(cv, L.title.x, L.title.y + 44, c.font.fit(under, small, L.title.w), small, K.muted);
        // Focus: an accent ring around what the D-pad is on.
        auto ring_around = [&](const Box& b, int radius) {
            cv.rounded_rect(b.x - 4, b.y - 4, b.w + 8, b.h + 8, radius + 4, K.accent);
            cv.rounded_rect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, radius + 2, K.panel);
        };
        // The slider through the book, chapters as ticks; reversed for a book read right to left.
        {
            const Box s = L.item[MSlider];
            const bool flip = rtl_ && view_ != View::Page;
            const double f = flip ? 1 - progress() : progress();
            if (focus_ == MSlider) ring_around({s.x - 6, s.y, s.w + 12, s.h}, 18);
            const int cy = s.y + s.h / 2;
            play::sunken(cv, K, {s.x, cy - 5, s.w, 10}, 5);
            const int kx = s.x + int(f * s.w);
            if (flip) cv.rounded_rect(kx, cy - 5, s.x + s.w - kx, 10, 5, K.accent);
            else cv.rounded_rect(s.x, cy - 5, std::max(10, kx - s.x), 10, 5, K.accent);
            for (const auto& h : outline_)
                if (h.level == 0 && book_.count() > 1) {
                    double hf = double(h.page) / (book_.count() - 1);
                    if (flip) hf = 1 - hf;
                    cv.fill_rect(s.x + int(hf * s.w), cy - 11, 2, 4, K.muted);
                }
            play::disc(cv, kx, cy + 1, 15, K.shadow(), 60);
            play::disc(cv, kx, cy, 14, K.accent);
            play::disc(cv, kx, cy, 6, Rgb{255, 255, 255});
        }
        // Orientation.
        c.font.draw(cv, L.orientation_caption.x, L.orientation_caption.y + 15, c.tr("Orientation"), small, K.muted);
        const char* tile_words[3] = {"Sideways", "Upright", "With controls"};
        for (int i = 0; i < 3; ++i) {
            const MenuItem it = MenuItem(MBook + i);
            const Box b = L.item[it];
            const bool lit = view_item() == it, pr = menu_pressed_ == it;
            if (focus_ == it) ring_around(b, 16);
            const Rgb fill = lit ? K.accent : play::mix(K.panel, K.background, 90);
            play::raised(cv, K, b.x, b.y, b.w, b.h, 16, fill, pr ? 1 : 0, !lit);
            const Rgb ink = lit ? K.on_accent : K.ink;
            const int press = pr ? 2 : 0;
            handheld(cv, it, b.x + b.w / 2, b.y + (b.h - 26) / 2 + press, ink, fill);
            std::string word = c.font.fit(c.tr(tile_words[i]), small, b.w - 10);
            c.font.draw(cv, b.x + (b.w - c.font.measure(word, small)) / 2, b.y + b.h - 10 + press, word, small, ink);
        }
        // Segmented choices: a sunken track, the chosen one raised in the accent.
        auto segment = [&](MenuItem a, MenuItem b, bool second, const std::string& wa, const std::string& wb) {
            Box ba = L.item[a], bb = L.item[b];
            Box track{ba.x, ba.y, bb.x + bb.w - ba.x, ba.h};
            play::sunken(cv, K, track, track.h / 2);
            for (int k = 0; k < 2; ++k) {
                const MenuItem it = k == 0 ? a : b;
                const Box bx = L.item[it];
                const bool lit = (k == 1) == second;
                if (focus_ == it) ring_around({bx.x + 2, bx.y + 2, bx.w - 4, bx.h - 4}, (bx.h - 4) / 2);
                if (lit)
                    play::raised(cv, K, bx.x + 3, bx.y + 3, bx.w - 6, bx.h - 6, (bx.h - 6) / 2, K.accent,
                                 menu_pressed_ == it ? 1 : 0, false);
                const std::string w = c.font.fit(k == 0 ? wa : wb, small, bx.w - 14);
                c.font.draw(cv, bx.x + (bx.w - c.font.measure(w, small)) / 2, c.font.centred(small, bx.y + bx.h / 2),
                            w, small, lit ? K.on_accent : K.ink);
            }
        };
        if (L.shown[MHandLeft]) segment(MHandLeft, MHandRight, right_hand_, c.tr("Left hand"), c.tr("Right hand"));
        // Paper: three swatches of the paper itself.
        c.font.draw(cv, L.paper_label.x, c.font.centred(body, L.paper_label.y + L.paper_label.h / 2), c.tr("Paper"),
                    body, K.ink);
        const Rgb grounds[3] = {{255, 255, 255}, {0xF4, 0xEC, 0xD8}, {0x15, 0x17, 0x1C}};
        const Rgb inks[3] = {{20, 20, 20}, {0x4A, 0x3A, 0x28}, {0xD8, 0xDC, 0xE2}};
        for (int i = 0; i < 3; ++i) {
            const MenuItem it = MenuItem(MWhite + i);
            const Box b = L.item[it];
            const int cx = b.x + b.w / 2, cy = b.y + b.h / 2, rad = b.w / 2 - (menu_pressed_ == it ? 2 : 0);
            const bool lit = int(paper_) == i;
            if (focus_ == it) play::disc(cv, cx, cy, rad + 7, K.accent);
            if (focus_ == it) play::disc(cv, cx, cy, rad + 5, K.panel);
            if (lit) play::disc(cv, cx, cy, rad + 3, K.accent);
            play::disc(cv, cx, cy, rad, play::mix(grounds[i], Rgb{0, 0, 0}, 40));
            play::disc(cv, cx, cy, rad - 1, grounds[i]);
            const std::string aa = "Aa";
            c.font.draw(cv, cx - c.font.measure(aa, small) / 2, c.font.centred(small, cy), aa, small, inks[i]);
        }
        // Text size: smaller, the size, larger (only where the text reflows).
        const bool reflows = book_.reflows();
        c.font.draw(cv, L.text_label.x, c.font.centred(body, L.text_label.y + L.text_label.h / 2), c.tr("Text size"),
                    body, reflows ? K.ink : K.muted);
        for (int k = 0; k < 2; ++k) {
            const MenuItem it = k == 0 ? MSmaller : MLarger;
            const Box b = L.item[it];
            if (focus_ == it) ring_around(b, 14);
            play::raised(cv, K, b.x, b.y, b.w, b.h, 14, K.panel, menu_pressed_ == it ? 1 : 0);
            const int size = k == 0 ? small : play::kit::large;
            const Rgb ink = reflows ? K.ink : K.muted;
            const int press = menu_pressed_ == it ? 2 : 0;
            c.font.draw(cv, b.x + (b.w - c.font.measure("A", size)) / 2, c.font.centred(size, b.y + b.h / 2) + press,
                        "A", size, ink);
        }
        {
            const std::string v = reflows ? std::to_string(em_) + " pt" : c.tr("fixed");
            const Box b = L.text_value;
            const std::string fv = c.font.fit(v, small, b.w);
            c.font.draw(cv, b.x + (b.w - c.font.measure(fv, small)) / 2, c.font.centred(small, b.y + b.h / 2), fv, small,
                        reflows ? K.ink : K.muted);
        }
        // Reading direction.
        if (L.direction_label.w > 0)
            c.font.draw(cv, L.direction_label.x, c.font.centred(body, L.direction_label.y + L.direction_label.h / 2),
                        c.tr("Direction"), body, K.ink);
        segment(MLtr, MRtl, rtl_, c.tr("Left to right"), c.tr("Right to left"));
        // Contents and Close.
        for (int k = 0; k < 2; ++k) {
            const MenuItem it = k == 0 ? MContents : MClose;
            const Box b = L.item[it];
            if (focus_ == it) ring_around(b, 16);
            const bool pr = menu_pressed_ == it;
            play::raised(cv, K, b.x, b.y, b.w, b.h, 16, K.panel, pr ? 1 : 0);
            const Glyph g = k == 0 ? Glyph::Contents : Glyph::Close;
            // Narrow (upright, beside Contents): just Close.
            const char* word = k == 0 ? "Contents" : book_view() ? "Close the book" : "Close";
            const std::string w = c.font.fit(c.tr(word), small, b.w - 52);
            const int total = 22 + 10 + c.font.measure(w, small);
            const int gx = b.x + (b.w - total) / 2, press = pr ? 2 : 0;
            glyph(cv, g, gx + 11, b.y + b.h / 2 + press, 20, K.ink, K.panel);
            c.font.draw(cv, gx + 32, c.font.centred(small, b.y + b.h / 2) + press, w, small, K.ink);
        }
        for (int i = 0; i < MCount; ++i)
            if (L.shown[i]) play::touch_target(L.item[i]);
        upload(r, menu_tex_, cv, true, false);
        menu_stale_ = false;
    }

    // ---- Time

    bool update(uint32_t now) override {
        const double dt = last_ ? double(now - last_) : 16;
        last_ = now;
        if (!error_.empty()) return false;
        bool moving = false;
        if (!dragging_) moving = shift_.step(dt, .0034, .92) || moving;
        moving = sheet_.update(dt) || moving;
        moving = menu_rise_.step(dt, .0040, .80) || moving;
        // Ask for what is open and what comes next.
        Paper p = paper_;
        auto want = [&](int i) {
            int w = 0, h = 0;
            page_box(i, w, h);
            book_.want(i, px(w), px(h), 100, p);
        };
        for (int i : spread(page_)) want(i);
        std::vector<int> ahead;
        int n = next_of(page_), b = prev_of(page_);
        for (int i : spread(n)) ahead.push_back(i);
        for (int i : spread(b)) ahead.push_back(i);
        int nn = next_of(n);
        for (int i : spread(nn)) ahead.push_back(i);
        for (int i : ahead) want(i);
        if (book_.arrived()) {
            // A page's shape may have changed the spreads (a wide page found): keep the open page in view.
            int a = align(page_);
            if (a != page_ && std::abs(shift_.value) < 1) page_ = a;
            moving = true;
            stale_ = true;
        }
        if (now - bar_at_ > 20000) bar_at_ = now, stale_ = true;
        return moving;
    }

    // ---- Drawing pages

    struct Cached {
        std::weak_ptr<const Page> page;
        Tex tex;
        uint32_t used = 0;
    };
    const Tex* texture(SDL_Renderer* r, const std::shared_ptr<const Page>& p) {
        auto key = std::make_pair(r, p.get());
        auto it = textures_.find(key);
        if (it != textures_.end() && !it->second.page.expired()) {
            it->second.used = c.now;
            return &it->second.tex;
        }
        Cached& e = textures_[key];
        e.page = p;
        e.used = c.now;
        upload(r, e.tex, p->px.data(), p->w, p->h, false, true);
        e.tex.k = c.scale; // rendered at the panel's pixels (px())
        // Keep at most 16: the open spread on both panels, and the ones either side.
        while (textures_.size() > 16) {
            auto old = textures_.begin();
            for (auto i = textures_.begin(); i != textures_.end(); ++i)
                if (i->second.used < old->second.used) old = i;
            textures_.erase(old);
        }
        return &e.tex;
    }
    void clear_textures() {
        textures_.clear();
        spine_.clear();
        stale_ = true;
    }

    Rgb paper_ground() const {
        return paper_ == Paper::Night   ? Rgb{0x15, 0x17, 0x1C}
               : paper_ == Paper::Sepia ? Rgb{0xF4, 0xEC, 0xD8}
                                        : Rgb{255, 255, 255};
    }
    Rgb desk() const { return paper_ == Paper::Night ? Rgb{8, 9, 12} : Rgb{40, 42, 48}; }

    // Draws one spread with its left edge at x (reader frame) onto a panel showing reader x in [x0, x0 + RW).
    void draw_spread_book(SDL_Renderer* r, int first, double x, double x0) {
        std::vector<int> pages = spread(first);
        if (pages.empty()) return;
        // In reading order; laid out left to right (reversed for manga).
        std::vector<int> lr = pages;
        if (rtl_) std::reverse(lr.begin(), lr.end());
        auto place = [&](int index, double left, double width) {
            const bool w = wide(index);
            auto pg = book_.get(index, px(w ? 2 * RW : RW), px(RH), 100, paper_);
            if (!pg || pg->failed || pg->px.empty()) {
                // Paper while the page is on its way.
                blit_reader(r, nullptr, left, 0, width, RH, x0, paper_ground());
                return;
            }
            const Tex* t = texture(r, pg);
            // Pages sit against the spine: a left page right-aligned, a right one left-aligned.
            double pw = t->lw(), ph = t->lh();
            double px = left + (width - pw) / 2, py = (RH - ph) / 2;
            if (!w && lr.size() == 2) px = index == lr[0] ? left + width - pw : left;
            blit_reader(r, nullptr, left, 0, width, RH, x0, paper_ground());
            blit_reader(r, t, px, py, pw, ph, x0, {});
        };
        if (lr.size() == 1) {
            const bool w = wide(lr[0]);
            if (w) place(lr[0], x, 2 * RW);
            else {
                // A cover alone sits where a printed book has it: the right (left for manga).
                const bool right_side = !rtl_;
                place(lr[0], x + (right_side ? RW : 0), RW);
                // The other half: the inside of the cover.
                blit_reader(r, nullptr, x + (right_side ? 0 : RW), 0, RW, RH, x0,
                            play::mix(paper_ground(), Rgb{0, 0, 0}, 30));
            }
        } else {
            place(lr[0], x, RW);
            place(lr[1], x + RW, RW);
        }
        // The spine's shade, on both sides of the fold.
        if (!wide(lr[0])) {
            blit_reader(r, &spine(r), x + RW - 28, 0, 28, RH, x0, {});
            blit_reader_flip(r, &spine(r), x + RW, 0, 28, RH, x0);
        }
    }

    const Tex& spine(SDL_Renderer* r) {
        Tex& t = spine_[r];
        if (!t) {
            std::vector<uint32_t> px(28 * 4);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 28; ++x) {
                    double f = x / 27.0;
                    int a = int(std::pow(f, 2.4) * (paper_ == Paper::Night ? 160 : 70));
                    px[size_t(y) * 28 + x] = uint32_t(a) << 24;
                }
            upload(r, t, px.data(), 28, 4, true, true);
        }
        return t;
    }

    // Reader frame -> panel: the centre of a reader-frame rect on the panel showing [x0, x0 + RW), and the turn.
    void turned(double cx, double cy, double& pcx, double& pcy, double& angle) const {
        if (!right_hand_) pcx = PW - cy, pcy = cx, angle = 90;
        else pcx = cy, pcy = PH - cx, angle = -90;
    }

    // A rect of the reader's frame (x across the spread, 0..960) onto the panel showing [x0, x0 + RW): turned.
    void blit_reader(SDL_Renderer* r, const Tex* t, double x, double y, double w, double h, double x0, Rgb ground) {
        double lx = x - x0;
        if (lx + w <= 0 || lx >= RW) return;
        double pcx, pcy, angle;
        turned(lx + w / 2, y + h / 2, pcx, pcy, angle);
        if (!t) {
            // A fill: the turned rect is h wide and w tall on the panel.
            fill(r, pcx - h / 2, pcy - w / 2, h, w, ground);
            return;
        }
        draw_turned(r, *t, pcx, pcy, w, h, angle);
    }
    void blit_reader_flip(SDL_Renderer* r, const Tex* t, double x, double y, double w, double h, double x0) {
        double lx = x - x0;
        if (lx + w <= 0 || lx >= RW) return;
        double pcx, pcy, angle;
        turned(lx + w / 2, y + h / 2, pcx, pcy, angle);
        SDL_FRect d{float(pcx - w / 2), float(pcy - h / 2), float(w), float(h)};
        SDL_SetTextureAlphaMod(t->t, 255);
        SDL_RenderCopyExF(r, t->t, nullptr, &d, angle, nullptr, SDL_FLIP_HORIZONTAL);
    }

    void draw_book_panel(SDL_Renderer* r, bool lower) {
        fill(r, 0, 0, PW, PH, desk());
        const bool right_panel = lower == lower_is_right();
        const double x0 = right_panel ? RW : 0;
        const double s = shift_.value, sp = span();
        draw_spread_book(r, page_, s, x0);
        // The neighbour coming into view.
        if (s * fwd() < -1) {
            int n = next_of(page_);
            if (n < book_.count()) draw_spread_book(r, n, s + fwd() * sp, x0);
        } else if (s * fwd() > 1) {
            int b = prev_of(page_);
            if (b >= 0) draw_spread_book(r, b, s - fwd() * sp, x0);
        }
        // A soft shadow on the moving edge while a spread slides over the hinge.
        if (std::abs(s) > 1) {
            double edge = s > 0 ? s : s + sp;
            blit_reader(r, &spine(r), edge - 28, 0, 28, RH, x0, {});
        }
        if (lower) {
            draw_page_number(r);
            draw_overlays(r);
        }
    }

    // ---- Upright on both screens: a frame 640 wide and 960 tall, the upper panel its top half.

    void draw_spread_stack(SDL_Renderer* r, int first, double x, double y0) {
        std::vector<int> pages = spread(first);
        if (pages.empty()) return;
        auto place = [&](int index, double top, double h) {
            if (top + h <= y0 || top >= y0 + PH) return;
            int bw = 0, bh = 0;
            page_box(index, bw, bh);
            auto pg = book_.get(index, px(bw), px(bh), 100, paper_);
            if (!pg || pg->failed || pg->px.empty()) {
                if (tall()) fill(r, x + PW / 2.0 - 220, top - y0, 440, h, paper_ground());
                else fill(r, x, top - y0, PW, h, paper_ground());
                return;
            }
            if (!tall()) fill(r, x, top - y0, PW, h, paper_ground());
            const Tex* t = texture(r, pg);
            draw(r, *t, x + (PW - t->lw()) / 2.0, top - y0 + (h - t->lh()) / 2.0);
        };
        if (tall()) place(pages[0], 0, 2 * PH);
        else {
            place(pages[0], 0, PH);
            if (pages.size() > 1) place(pages[1], PH, PH);
            else if (y0 >= PH) fill(r, x, 0, PW, PH, play::mix(paper_ground(), Rgb{0, 0, 0}, 30)); // the end
        }
    }

    void draw_stack_panel(SDL_Renderer* r, bool lower) {
        fill(r, 0, 0, PW, PH, desk());
        const double y0 = lower ? PH : 0, s = shift_.value, sp = span();
        draw_spread_stack(r, page_, s, y0);
        if (s * fwd() < -1) {
            int n = next_of(page_);
            if (n < book_.count()) draw_spread_stack(r, n, s + fwd() * sp, y0);
        } else if (s * fwd() > 1) {
            int b = prev_of(page_);
            if (b >= 0) draw_spread_stack(r, b, s - fwd() * sp, y0);
        }
        if (lower) {
            draw_page_number(r);
            draw_overlays(r);
        }
    }

    void draw_page_number(SDL_Renderer* r) {
        // A small chip at the foot of the lower page: where we are.
        if (!number_ || number_for_ != page_ || number_count_ != book_.count()) {
            std::string s = where();
            FontScale fs_(c.font, c.scale);
            Canvas cv(c.font.measure(s, 16) + 24, 28, c.scale);
            std::fill(cv.data(), cv.data() + size_t(cv.pixel_w()) * cv.pixel_h(), 0u);
            cv.rounded_rect(0, 0, cv.w(), cv.h(), 14, Rgb{0, 0, 0}, 110);
            c.font.draw(cv, 12, c.font.centred(16, 14), s, 16, Rgb{255, 255, 255});
            upload(r, number_, cv, true, true);
            number_for_ = page_;
            number_count_ = book_.count();
            number_shown_at_ = c.now;
        }
        // It shows after a turn and fades after two seconds (and while the menu is up, the menu says it).
        double t = (c.now - number_shown_at_) / 2000.0;
        int a = t < 1 ? 255 : int(255 * std::max(0.0, 1 - (t - 1) * 3));
        a = int(a * (1 - std::clamp(menu_rise_.value, 0.0, 1.0)));
        if (a <= 0) return;
        if (!book_view()) {
            draw(r, number_, (PW - number_.lw()) / 2.0, PH - 30 - number_.lh() / 2.0, a);
            return;
        }
        // Reader frame: on the lower page, near its foot.
        double pcx, pcy, angle;
        turned(RW / 2.0, RH - 30, pcx, pcy, angle);
        draw_turned(r, number_, pcx, pcy, number_.lw(), number_.lh(), angle, a);
    }

    // The menu or the contents over the lower panel, upright for the reader.
    void draw_overlays(SDL_Renderer* r) {
        auto put = [&](const Tex& tex, double v, int w, int h) {
            fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, int(110 * v));
            const int alpha = int(255 * std::min(1.0, v * 1.5));
            if (!book_view()) {
                draw(r, tex, 0, (1 - v) * h * .6, alpha);
                return;
            }
            double pcx, pcy, angle;
            turned(w / 2.0, h / 2.0 + (1 - v) * h * .6, pcx, pcy, angle);
            draw_turned(r, tex, pcx, pcy, w, h, angle, alpha);
        };
        if (menu_open_ || menu_rise_.value > .01) {
            if (menu_stale_ || !menu_tex_ || int(std::lround(menu_tex_.lw())) != menu_w() || menu_tex_.k != c.scale)
                draw_menu_canvas(r);
            put(menu_tex_, std::clamp(menu_rise_.value, 0.0, 1.0), menu_w(), menu_h());
        }
        if (sheet_.visible()) {
            sheet_.draw(r, c);
            put(sheet_.tex, std::clamp(sheet_.rise.value, 0.0, 1.0), sheet_.w, sheet_.h);
        }
    }

    // ---- Page view

    void draw_page_upper(SDL_Renderer* r) {
        fill(r, 0, 0, PW, PH, desk());
        auto one = [&](int index, double x) {
            if (index < 0 || index >= book_.count()) return;
            auto pg = book_.get(index, px(PW), px(PH), 100, paper_);
            if (!pg || pg->failed || pg->px.empty()) {
                fill(r, x + PW / 2.0 - 180, 0, 360, PH, paper_ground());
                return;
            }
            const Tex* t = texture(r, pg);
            draw(r, *t, x + (PW - t->lw()) / 2.0, (PH - t->lh()) / 2.0);
        };
        const double s = shift_.value;
        one(page_, s);
        if (s * fwd() < -1) one(page_ + 1, s + fwd() * PW);
        if (s * fwd() > 1) one(page_ - 1, s - fwd() * PW);
    }

    void page_canvas(SDL_Renderer* r) {
        FontScale fs_(c.font, c.scale);
        Canvas cv(PW, PH, c.scale);
        play::backdrop(cv, c.look);
        Canvas bar(PW, 36, c.scale);
        c.power.read(c.now);
        status_bar(bar, c.look, c.font, c.power, "");
        cv.paste(bar, 0, 0);
        const auto& L = c.look;
        c.font.draw(cv, margin, 76, c.font.fit(book_.title().empty() ? item_.name : book_.title(), 24, PW - 2 * margin),
                    24, L.ink);
        if (!book_.author().empty())
            c.font.draw(cv, margin, 100, c.font.fit(book_.author(), 16, PW - 2 * margin), 16, L.muted);
        // Where we are: the page in large figures, and the chapter.
        std::string n = std::to_string(page_ + 1), of = " / " + std::to_string(book_.count());
        int nx = margin;
        c.font.draw(cv, nx, 168, n, 36, L.accent);
        c.font.draw(cv, nx + c.font.measure(n, 36) + 4, 168, of, 24, L.muted);
        const std::string ch = chapter();
        if (!ch.empty()) c.font.draw(cv, margin, 200, c.font.fit(ch, 20, PW - 2 * margin), 20, L.ink);
        int pct = int(std::lround(progress() * 100));
        std::string p = std::to_string(pct) + "%";
        c.font.draw(cv, PW - margin - c.font.measure(p, 24), 168, p, 24, L.muted);
        // The slider through the book.
        Box s = slider_box();
        play::meter(cv, L, {s.x, s.y + s.h / 2 - 6, s.w, 12}, progress(), L.accent);
        for (const auto& h : outline_) // chapters as ticks
            if (h.level == 0 && book_.count() > 1)
                cv.fill_rect(s.x + int(double(h.page) / (book_.count() - 1) * s.w), s.y + s.h / 2 - 10, 2, 4, L.muted);
        int kx = s.x + int(progress() * s.w);
        play::disc(cv, kx, s.y + s.h / 2 + 1, 15, L.shadow(), 60);
        play::disc(cv, kx, s.y + s.h / 2, 14, L.accent);
        play::disc(cv, kx, s.y + s.h / 2, 6, Rgb{255, 255, 255});
        // Previous and next.
        for (int i = 0; i < 2; ++i) {
            Box b = button_box(i);
            double press = pressed_ == 100 + i ? 1 : 0;
            play::raised(cv, L, b.x, b.y, b.w, b.h, 18, L.panel, press);
            glyph(cv, i == 0 ? Glyph::Previous : Glyph::Next, b.x + b.w / 2, b.y + b.h / 2 + int(2 * press), 26, L.ink,
                  L.panel);
        }
        const Glyph g[CCount] = {Glyph::Contents, Glyph::Paper, Glyph::TextSize, Glyph::Direction, Glyph::Book};
        const char* words[CCount] = {"Contents", "Paper", "Text size", "Direction", "Orientation"};
        for (int i = 0; i < CCount; ++i) {
            Box b = chip_box(i);
            bool lit = (i == CPaper && paper_ != Paper::White) || (i == CDirection && rtl_);
            double press = pressed_ == i ? 1 : 0;
            play::raised(cv, L, b.x, b.y, b.w, b.h, 14, lit ? L.accent : L.panel, press);
            Rgb ink = lit ? L.on_accent : (i == CText && !book_.reflows() ? L.muted : L.ink);
            glyph(cv, g[i], b.x + b.w / 2, b.y + 22 + int(2 * press), 22, ink, lit ? L.accent : L.panel);
            std::string w = c.font.fit(c.tr(words[i]), 16, b.w - 6);
            c.font.draw(cv, b.x + (b.w - c.font.measure(w, 16)) / 2, b.y + 52 + int(2 * press), w, 16, ink);
        }
        upload(r, lower_tex_, cv);
        stale_ = false;
    }

    void draw_upper(SDL_Renderer* r) override {
        if (!error_.empty()) {
            fill(r, 0, 0, PW, PH, c.look.background);
            return;
        }
        if (book_view()) draw_book_panel(r, false);
        else if (view_ == View::Stack) draw_stack_panel(r, false);
        else draw_page_upper(r);
    }

    void draw_lower(SDL_Renderer* r) override {
        if (!error_.empty()) {
            FontScale fs_(c.font, c.scale);
            Canvas cv(PW, PH, c.scale);
            play::backdrop(cv, c.look);
            play::raised(cv, c.look, margin, 120, PW - 2 * margin, 200, 18, c.look.panel);
            c.font.draw(cv, margin + 24, 170, c.tr("This book cannot be opened"), 24, c.look.ink);
            auto lines = wrap(c.font, item_.name + ": " + error_, 20, PW - 2 * margin - 48, 3);
            for (size_t i = 0; i < lines.size(); ++i)
                c.font.draw(cv, margin + 24, 210 + int(i) * 26, lines[i], 20, c.look.muted);
            upload(r, lower_tex_, cv);
            draw(r, lower_tex_, 0, 0);
            return;
        }
        if (book_view()) {
            draw_book_panel(r, true);
            return;
        }
        if (view_ == View::Stack) {
            draw_stack_panel(r, true);
            return;
        }
        if (stale_ || !lower_tex_) page_canvas(r);
        draw(r, lower_tex_, 0, 0);
        draw_overlays(r);
    }

private:
    Item item_;
    Book book_;
    std::string error_;
    std::vector<Heading> outline_;
    int page_ = 0, em_ = 11, pressed_ = -1;
    bool rtl_ = false, right_hand_ = false;
    View view_ = View::Book;
    Paper paper_ = Paper::White;
    Spring shift_;
    Sheet sheet_;
    rgds::FingerSpeed finger_;
    bool dragging_ = false, moved_ = false, sliding_ = false, stale_ = true;
    double down_x_ = 0, grab_shift_ = 0;
    // The menu.
    bool menu_open_ = false, menu_stale_ = true, menu_sliding_ = false;
    Spring menu_rise_; // 0 hidden .. 1 shown
    MenuItem focus_ = MBook;
    int menu_pressed_ = -1;
    Tex menu_tex_;
    std::map<std::pair<SDL_Renderer*, const Page*>, Cached> textures_;
    std::map<SDL_Renderer*, Tex> spine_;
    Tex number_, lower_tex_;
    int number_for_ = -1, number_count_ = -1;
    uint32_t number_shown_at_ = 0, last_ = 0, bar_at_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_read(Context& c, const Item& item) { return std::make_unique<Read>(c, item); }

} // namespace media
