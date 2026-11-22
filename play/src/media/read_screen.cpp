// SPDX-License-Identifier: GPL-3.0-or-later
// A book, comic or document.
//
// Book view: the handheld is held sideways, like a book. Each panel is a 480 x 640 portrait page, the hinge is the
// spine: a two-page spread is open across both screens, a page wider than tall (a scanned double page) spans both.
// Manga reads right to left. Held in the left hand the upper screen is the left page; in the right hand, the right.
// Pages follow the finger when dragged and settle with a spring; a tap on the outer third turns, the middle opens the
// menu. The menu and everything else on the lower panel is drawn turned too, upright for the reader.
//
// Page view: held normally, the page on the upper screen, and on the lower one the place in the book: the page
// number, a slider through the whole book, the chapter, and the menu's buttons.
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

enum Chip { CContents, CPaper, CText, CDirection, CView, CCount };

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
        book_view_ = ex.count("mode") ? ex["mode"] == "book" : true;
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
    }
    ~Read() override { book_.close(); }
    const char* name() const override { return "read"; }

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
        ex << "dir=" << (rtl_ ? "rtl" : "ltr") << ";mode=" << (book_view_ ? "book" : "page") << ";paper="
           << (paper_ == Paper::Night   ? "night"
               : paper_ == Paper::Sepia ? "sepia"
                                        : "white")
           << ";em=" << em_ << ";hold=" << (right_hand_ ? "right" : "left");
        p.extra = ex.str();
        c.places.set(item_.key, p);
        c.places.save();
    }

    // ---- Layout of pages

    // The box a page is rendered into.
    int box_w() const { return book_view_ ? RW : PW; }
    int box_h() const { return book_view_ ? RH : PH; }
    bool wide(int i) const {
        double w = 0, h = 0;
        return const_cast<Book&>(book_).shape(i, w, h) && w > h * 1.15;
    }
    bool cover_alone() const { return !book_.reflows() && book_.count() > 2; }
    // The pages open now, in reading order (one or two).
    std::vector<int> spread(int p) const {
        const int n = book_.count();
        if (p < 0 || p >= n) return {};
        if (!book_view_) return {p};
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
        if (!book_view_) return p;
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
        const double span = book_view_ ? 2 * RW : PW;
        page_ = to;
        shift_.value += d * fwd() * span;
        shift_.target = 0;
        if (!c.animations()) shift_.snap(0);
        stale_ = true;
        leave();
    }
    double fwd() const { return rtl_ ? -1 : 1; } // the next spread lies this way along x
    void go(int page) {
        page_ = align(std::clamp(page, 0, std::max(0, book_.count() - 1)));
        shift_.snap(0);
        stale_ = true;
        leave();
    }

    // ---- Input

    void key(Key k, bool repeat) override {
        if (!error_.empty()) {
            if (k == KB || k == KA) c.pop();
            return;
        }
        if (sheet_.open) {
            // Held sideways, the D-pad turns with the reader: its Up points left.
            sheet_.key(book_view_ ? turn_key(k) : k);
            return;
        }
        Key t = book_view_ ? turn_key(k) : k;
        switch (t) {
        case KRight: turn(rtl_ ? -1 : 1); break;
        case KLeft: turn(rtl_ ? 1 : -1); break;
        case KDown:
            if (!book_view_) turn(1);
            break;
        case KUp:
            if (!book_view_) turn(-1);
            break;
        case KA:
        case KR: turn(1); break;
        case KL: turn(-1); break;
        case KL2: go(page_ - 10); break;
        case KR2: go(page_ + 10); break;
        case KX:
        case KStart:
            if (!repeat) open_menu();
            break;
        case KY:
            if (!repeat) open_sheet(CContents);
            break;
        case KSelect:
            if (!repeat) open_sheet(CView);
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

    // Which panel holds the left page, and where a reader-frame point lands on a panel.
    bool lower_is_right() const { return !right_hand_; }
    // Reader frame (x across the whole 960 spread) -> lower panel pixels and back.
    void to_reader(double px, double py, double& xr, double& yr) const {
        if (!right_hand_) xr = py, yr = PW - px;
        else xr = PH - py, yr = px;
        if (lower_is_right()) xr += RW;
    }

    void touch(const TouchEvent& ev) override {
        if (!error_.empty()) {
            if (ev.kind == TouchEvent::Up) c.pop();
            return;
        }
        TouchEvent t = ev;
        double xr = t.x, yr = t.y;
        if (book_view_) {
            to_reader(t.x, t.y, xr, yr);
            t.x = xr - (lower_is_right() ? RW : 0);
            t.y = yr;
        }
        if (sheet_.open) {
            sheet_.touch(t);
            return;
        }
        if (book_view_) touch_book(t, xr);
        else touch_page(ev);
    }

    void touch_book(const TouchEvent& t, double xr) {
        if (t.kind == TouchEvent::Down) {
            down_x_ = xr, down_y_ = t.y;
            moved_ = false;
            grab_shift_ = shift_.value;
            finger_.reset();
            finger_.add(t.at, xr);
            dragging_ = true;
        } else if (t.kind == TouchEvent::Move && dragging_) {
            finger_.add(t.at, xr);
            if (std::abs(xr - down_x_) > 10) moved_ = true;
            if (moved_) {
                double s = grab_shift_ + (xr - down_x_);
                // Past the first or last spread the page gives, but only a little.
                bool can = (s * fwd() < 0 && next_of(page_) < book_.count()) || (s * fwd() > 0 && page_ > 0);
                shift_.snap(can ? s : rgds::ScrollMotion::band(s) * .6);
                c.dirty = true;
            }
        } else if (t.kind == TouchEvent::Up && dragging_) {
            dragging_ = false;
            if (!moved_) {
                // Outer thirds turn, the middle opens the menu. The lower panel is one page of two: its outer
                // edge is the book's.
                const double local = t.x; // 0..480 on this page
                const bool outer = lower_is_right() ? local > RW * .55 : local < RW * .45;
                const bool inner = lower_is_right() ? local < RW * .2 : local > RW * .8;
                if (outer) turn(lower_is_right() == !rtl_ ? 1 : -1);
                else if (inner) turn(lower_is_right() == !rtl_ ? -1 : 1);
                else open_menu();
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
        shift_.value += d * fwd() * (book_view_ ? 2 * RW : PW);
        shift_.target = 0;
        stale_ = true;
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
            down_x_ = t.x, down_y_ = t.y;
            Box s = slider_box();
            if (t.y > s.y - 20 && t.y < s.y + s.h + 20) {
                sliding_ = true;
                slide_to(t.x);
                return;
            }
            for (int i = 0; i < 2; ++i)
                if (button_box(i).contains(int(t.x), int(t.y))) pressed_ = 100 + i;
            for (int i = 0; i < CCount; ++i)
                if (chip_box(i).contains(int(t.x), int(t.y))) pressed_ = i;
            if (pressed_ >= 0) c.ripples.start(t.x, t.y, 60, t.at);
            stale_ = true;
        } else if (t.kind == TouchEvent::Move) {
            if (sliding_) slide_to(t.x);
        } else {
            c.ripples.release(t.at);
            if (sliding_) {
                sliding_ = false;
                go(slide_page_);
            } else if (pressed_ >= 100) {
                turn(pressed_ == 100 ? -1 : 1);
            } else if (pressed_ >= 0) {
                open_sheet(pressed_);
            }
            pressed_ = -1;
            stale_ = true;
        }
    }
    void slide_to(double x) {
        Box s = slider_box();
        double f = std::clamp((x - s.x) / s.w, 0.0, 1.0);
        int n = std::max(1, book_.count());
        slide_page_ = std::clamp(int(f * (n - 1) + .5), 0, n - 1);
        // The page above follows (rendered as it is reached).
        if (slide_page_ != page_) {
            page_ = slide_page_;
            shift_.snap(0);
        }
        stale_ = true;
    }

    // ---- Menus

    void open_menu() {
        std::vector<Sheet::Row> rows = {
            {c.tr("Contents"), outline_.empty() ? c.tr("none") : std::to_string(outline_.size()), false},
            {c.tr("Paper"), paper_name(), false},
            {c.tr("Text size"), book_.reflows() ? std::to_string(em_) + " pt" : c.tr("fixed"), false},
            {c.tr("Reading direction"), rtl_ ? c.tr("Right to left") : c.tr("Left to right"), false},
            {c.tr("View"), book_view_ ? c.tr("Book, held sideways") : c.tr("Page"), false},
            {c.tr("Close the book"), "", false},
        };
        if (book_view_)
            rows.insert(rows.begin() + 5,
                        {c.tr("Hold in"), right_hand_ ? c.tr("right hand") : c.tr("left hand"), false});
        const bool bv = book_view_;
        show(c.tr("Book"), rows, 0, [this, bv](int i) {
            if (bv && i == 5) {
                right_hand_ = !right_hand_;
                clear_textures();
                leave();
                return;
            }
            if (i == (bv ? 6 : 5)) {
                leave();
                c.pop();
                return;
            }
            open_sheet(i);
        });
    }

    std::string paper_name() const {
        return paper_ == Paper::Night ? c.tr("Night") : paper_ == Paper::Sepia ? c.tr("Sepia") : c.tr("White");
    }

    void show(const std::string& title, std::vector<Sheet::Row> rows, int focus, std::function<void(int)> f) {
        sheet_.w = book_view_ ? RW : PW;
        sheet_.h = book_view_ ? RH : PH;
        sheet_.tex.reset();
        sheet_.show(title, std::move(rows), focus, std::move(f), c.animations());
    }

    void open_sheet(int which) {
        std::vector<Sheet::Row> rows;
        switch (which) {
        case CContents: {
            if (outline_.empty()) {
                rows.push_back({c.tr("This book has no contents list"), "", false});
                show(c.tr("Contents"), rows, 0, [](int) {});
                break;
            }
            int at = 0;
            for (int i = 0; i < int(outline_.size()); ++i) {
                const Heading& h = outline_[size_t(i)];
                if (h.page <= page_) at = i;
                rows.push_back({h.title, std::to_string(h.page + 1), false, h.level});
            }
            if (at < int(rows.size())) rows[size_t(at)].chosen = true;
            show(c.tr("Contents"), rows, at, [this](int i) { go(outline_[size_t(i)].page); });
            break;
        }
        case CPaper:
            rows = {{c.tr("White"), "", paper_ == Paper::White},
                    {c.tr("Sepia"), c.tr("warm, for long reading"), paper_ == Paper::Sepia},
                    {c.tr("Night"), c.tr("light text on dark"), paper_ == Paper::Night}};
            show(c.tr("Paper"), rows, int(paper_), [this](int i) {
                paper_ = Paper(i);
                clear_textures();
                leave();
            });
            break;
        case CText: {
            if (!book_.reflows()) {
                rows.push_back({c.tr("The pages of this file are fixed"), "", false});
                show(c.tr("Text size"), rows, 0, [](int) {});
                break;
            }
            int at = 0;
            for (int i = 0; i < 10; ++i) {
                if (text_sizes_em[i] == em_) at = i;
                rows.push_back(
                    {std::to_string(text_sizes_em[i]) + " pt", i == 3 ? c.tr("Normal") : "", text_sizes_em[i] == em_});
            }
            show(c.tr("Text size"), rows, at, [this](int i) {
                em_ = text_sizes_em[i];
                relayout();
                page_ = align(page_);
                leave();
                stale_ = true;
            });
            break;
        }
        case CDirection:
            rows = {{c.tr("Left to right"), c.tr("books, comics"), !rtl_},
                    {c.tr("Right to left"), c.tr("manga"), rtl_}};
            show(c.tr("Reading direction"), rows, rtl_ ? 1 : 0, [this](int i) {
                rtl_ = i == 1;
                leave();
            });
            break;
        case CView:
            rows = {{c.tr("Book, held sideways"), c.tr("two pages"), book_view_},
                    {c.tr("Page"), c.tr("one page, held normally"), !book_view_}};
            show(c.tr("View"), rows, book_view_ ? 0 : 1, [this](int i) {
                bool b = i == 0;
                if (b == book_view_) return;
                book_view_ = b;
                clear_textures();
                relayout();
                page_ = align(page_);
                stale_ = true;
                leave();
            });
            break;
        }
    }

    // ---- Time

    bool update(uint32_t now) override {
        const double dt = last_ ? double(now - last_) : 16;
        last_ = now;
        if (!error_.empty()) return false;
        bool moving = false;
        if (!dragging_) moving = shift_.step(dt, .0034, .92) || moving;
        moving = sheet_.update(dt) || moving;
        // Ask for what is open and what comes next.
        Paper p = paper_;
        std::vector<int> want = spread(page_);
        for (int i : want) book_.want(i, wide(i) && book_view_ ? 2 * RW : box_w(), box_h(), 100, p);
        std::vector<int> ahead;
        int n = next_of(page_), b = prev_of(page_);
        for (int i : spread(n)) ahead.push_back(i);
        for (int i : spread(b)) ahead.push_back(i);
        int nn = next_of(n);
        for (int i : spread(nn)) ahead.push_back(i);
        for (int i : ahead) book_.want(i, wide(i) && book_view_ ? 2 * RW : box_w(), box_h(), 100, p);
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

    // Draws one spread with its left edge at x (reader frame) onto a panel showing reader x in [x0, x0 + RW).
    void draw_spread_book(SDL_Renderer* r, int first, double x, double x0) {
        std::vector<int> pages = spread(first);
        if (pages.empty()) return;
        // In reading order; laid out left to right (reversed for manga).
        std::vector<int> lr = pages;
        if (rtl_) std::reverse(lr.begin(), lr.end());
        auto place = [&](int index, double left, double width) {
            const bool w = wide(index);
            auto pg = book_.get(index, w ? 2 * RW : RW, RH, 100, paper_);
            if (!pg || pg->failed || pg->px.empty()) {
                // Paper while the page is on its way, with its number faint.
                blit_reader(r, nullptr, left, 0, width, RH, x0, paper_ground());
                return;
            }
            const Tex* t = texture(r, pg);
            // Pages sit against the spine: a left page right-aligned, a right one left-aligned.
            double pw = pg->w, ph = pg->h;
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
                bool right_side = !rtl_;
                if (!(first == 0 && cover_alone())) right_side = rtl_ ? false : true;
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

    // A rect of the reader's frame (x across the spread, 0..960) onto the panel showing [x0, x0 + RW): turned.
    void blit_reader(SDL_Renderer* r, const Tex* t, double x, double y, double w, double h, double x0, Rgb ground) {
        double lx = x - x0;
        if (lx + w <= 0 || lx >= RW) return;
        double cx = lx + w / 2, cy = y + h / 2;
        double pcx, pcy, angle;
        if (!right_hand_) pcx = PW - cy, pcy = cx, angle = 90;
        else pcx = cy, pcy = PH - cx, angle = -90;
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
        double cx = lx + w / 2, cy = y + h / 2;
        double pcx, pcy, angle;
        if (!right_hand_) pcx = PW - cy, pcy = cx, angle = 90;
        else pcx = cy, pcy = PH - cx, angle = -90;
        SDL_FRect d{float(pcx - w / 2), float(pcy - h / 2), float(w), float(h)};
        SDL_SetTextureAlphaMod(t->t, 255);
        SDL_RenderCopyExF(r, t->t, nullptr, &d, angle, nullptr, SDL_FLIP_HORIZONTAL);
    }

    void draw_book_panel(SDL_Renderer* r, bool lower) {
        fill(r, 0, 0, PW, PH, paper_ == Paper::Night ? Rgb{8, 9, 12} : Rgb{40, 42, 48});
        const bool right_panel = lower == lower_is_right();
        const double x0 = right_panel ? RW : 0;
        const double s = shift_.value, span = 2 * RW;
        draw_spread_book(r, page_, s, x0);
        // The neighbour coming into view.
        if (s * fwd() < -1) {
            int n = next_of(page_);
            if (n < book_.count()) draw_spread_book(r, n, s + fwd() * span, x0);
        } else if (s * fwd() > 1) {
            int b = prev_of(page_);
            if (b >= 0) draw_spread_book(r, b, s - fwd() * span, x0);
        }
        // A soft shadow on the moving edge while a spread slides over the hinge.
        if (std::abs(s) > 1) {
            double edge = s > 0 ? s : s + span;
            blit_reader(r, &spine(r), edge - 28, 0, 28, RH, x0, {});
        }
        if (lower) {
            draw_page_number(r);
            draw_sheet_turned(r);
        }
    }

    void draw_page_number(SDL_Renderer* r) {
        // A small chip at the foot of the lower page: where we are.
        if (!number_ || number_for_ != page_ || number_count_ != book_.count()) {
            auto sp = spread(page_);
            std::string s = sp.size() == 2 ? std::to_string(sp[0] + 1) + "–" + std::to_string(sp[1] + 1)
                                           : std::to_string(page_ + 1);
            s += " / " + std::to_string(book_.count());
            Canvas cv(c.font.measure(s, 16) + 24, 28);
            std::fill(cv.data(), cv.data() + size_t(cv.w()) * cv.h(), 0u);
            cv.rounded_rect(0, 0, cv.w(), cv.h(), 14, Rgb{0, 0, 0}, 110);
            c.font.draw(cv, 12, c.font.centred(16, 14), s, 16, Rgb{255, 255, 255});
            upload(r, number_, cv, true, true);
            number_for_ = page_;
            number_count_ = book_.count();
            number_shown_at_ = c.now;
        }
        // It shows after a turn and fades after two seconds.
        double t = (c.now - number_shown_at_) / 2000.0;
        int a = t < 1 ? 255 : int(255 * std::max(0.0, 1 - (t - 1) * 3));
        if (a <= 0) return;
        double x = (lower_is_right() ? RW : 0) + (RW - number_.w) / 2.0;
        // Reader frame: on the lower page, near its foot.
        double lx = x - (lower_is_right() ? RW : 0);
        double cx = lx + number_.w / 2.0, cy = RH - 30;
        double pcx, pcy, angle;
        if (!right_hand_) pcx = PW - cy, pcy = cx, angle = 90;
        else pcx = cy, pcy = PH - cx, angle = -90;
        draw_turned(r, number_, pcx, pcy, number_.w, number_.h, angle, a);
    }

    void draw_sheet_turned(SDL_Renderer* r) {
        if (!sheet_.visible()) return;
        sheet_.draw(r, c);
        const double v = std::clamp(sheet_.rise.value, 0.0, 1.0);
        fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, int(110 * v));
        double cx = RW / 2.0, cy = RH / 2.0 + (1 - v) * RH * .6;
        double pcx, pcy, angle;
        if (!right_hand_) pcx = PW - cy, pcy = cx, angle = 90;
        else pcx = cy, pcy = PH - cx, angle = -90;
        draw_turned(r, sheet_.tex, pcx, pcy, RW, RH, angle, int(255 * std::min(1.0, v * 1.5)));
    }

    // ---- Page view

    void draw_page_upper(SDL_Renderer* r) {
        fill(r, 0, 0, PW, PH, paper_ == Paper::Night ? Rgb{8, 9, 12} : Rgb{40, 42, 48});
        auto one = [&](int index, double x) {
            if (index < 0 || index >= book_.count()) return;
            auto pg = book_.get(index, PW, PH, 100, paper_);
            if (!pg || pg->failed || pg->px.empty()) {
                fill(r, x + PW / 2.0 - 180, 0, 360, PH, paper_ground());
                return;
            }
            const Tex* t = texture(r, pg);
            draw(r, *t, x + (PW - pg->w) / 2.0, (PH - pg->h) / 2.0, pg->w, pg->h);
        };
        const double s = shift_.value;
        one(page_, s);
        if (s < -1) one(page_ + 1, s + PW);
        if (s > 1) one(page_ - 1, s - PW);
    }

    void page_canvas(SDL_Renderer* r) {
        Canvas cv(PW, PH);
        play::backdrop(cv, c.look);
        Canvas bar(PW, 36);
        c.power.read(c.now);
        status_bar(bar, c.look, c.font, c.power, "");
        for (int y = 0; y < 36; ++y) std::copy(bar.data() + y * PW, bar.data() + (y + 1) * PW, cv.data() + y * PW);
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
        std::string chapter;
        for (const auto& h : outline_)
            if (h.page <= page_) chapter = h.title;
        if (!chapter.empty()) c.font.draw(cv, margin, 200, c.font.fit(chapter, 20, PW - 2 * margin), 20, L.ink);
        int pct = book_.count() > 1 ? page_ * 100 / (book_.count() - 1) : 100;
        std::string p = std::to_string(pct) + "%";
        c.font.draw(cv, PW - margin - c.font.measure(p, 24), 168, p, 24, L.muted);
        // The slider through the book.
        Box s = slider_box();
        play::meter(cv, L, {s.x, s.y + s.h / 2 - 6, s.w, 12},
                    book_.count() > 1 ? double(page_) / (book_.count() - 1) : 1, L.accent);
        for (const auto& h : outline_) // chapters as ticks
            if (h.level == 0 && book_.count() > 1)
                cv.fill_rect(s.x + int(double(h.page) / (book_.count() - 1) * s.w), s.y + s.h / 2 - 10, 2, 4, L.muted);
        int kx = s.x + int((book_.count() > 1 ? double(page_) / (book_.count() - 1) : 1) * s.w);
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
        const char* words[CCount] = {"Contents", "Paper", "Text size", "Direction", "Book view"};
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
        if (book_view_) draw_book_panel(r, false);
        else draw_page_upper(r);
    }

    void draw_lower(SDL_Renderer* r) override {
        if (!error_.empty()) {
            Canvas cv(PW, PH);
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
        if (book_view_) {
            draw_book_panel(r, true);
            return;
        }
        if (stale_ || !lower_tex_) page_canvas(r);
        draw(r, lower_tex_, 0, 0);
        if (sheet_.visible()) {
            sheet_.draw(r, c);
            const double v = std::clamp(sheet_.rise.value, 0.0, 1.0);
            fill(r, 0, 0, PW, PH, Rgb{0, 0, 0}, int(110 * v));
            draw(r, sheet_.tex, 0, (1 - v) * PH * .6, int(255 * std::min(1.0, v * 1.5)));
        }
    }

private:
    Item item_;
    Book book_;
    std::string error_;
    std::vector<Heading> outline_;
    int page_ = 0, em_ = 11, pressed_ = -1, slide_page_ = 0;
    bool rtl_ = false, book_view_ = true, right_hand_ = false;
    Paper paper_ = Paper::White;
    Spring shift_;
    Sheet sheet_;
    rgds::FingerSpeed finger_;
    bool dragging_ = false, moved_ = false, sliding_ = false, stale_ = true;
    double down_x_ = 0, down_y_ = 0, grab_shift_ = 0;
    std::map<std::pair<SDL_Renderer*, const Page*>, Cached> textures_;
    std::map<SDL_Renderer*, Tex> spine_;
    Tex number_, lower_tex_;
    int number_for_ = -1, number_count_ = -1;
    uint32_t number_shown_at_ = 0, last_ = 0, bar_at_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_read(Context& c, const Item& item) { return std::make_unique<Read>(c, item); }

} // namespace media
