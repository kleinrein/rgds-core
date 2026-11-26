// SPDX-License-Identifier: GPL-3.0-or-later
#include "reader/manual.hpp"

#include "reader/pages.hpp"
#include "ui/kit.hpp"
#include "ui/words.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace play {

namespace {

namespace fs = std::filesystem;

uint64_t now_ms() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Level {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

// One decoded page: the scan and its halvings down to about the panel's size, so a page shown small is
// filtered from the nearest one rather than skipping most of the scan's pixels.
struct Decoded {
    std::vector<Level> levels;
    std::string error;
};

Level halve(const Level& in) {
    Level out;
    out.w = std::max(1, in.w / 2);
    out.h = std::max(1, in.h / 2);
    out.px.resize(size_t(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        const uint32_t* a = in.px.data() + size_t(std::min(2 * y, in.h - 1)) * in.w;
        const uint32_t* b = in.px.data() + size_t(std::min(2 * y + 1, in.h - 1)) * in.w;
        for (int x = 0; x < out.w; ++x) {
            const int x0 = std::min(2 * x, in.w - 1), x1 = std::min(2 * x + 1, in.w - 1);
            const uint32_t p = a[x0], q = a[x1], r = b[x0], s = b[x1];
            const uint32_t rb =
                (((p & 0xFF00FF) + (q & 0xFF00FF) + (r & 0xFF00FF) + (s & 0xFF00FF) + 0x020002) >> 2) & 0xFF00FF;
            const uint32_t g = (((p & 0xFF00) + (q & 0xFF00) + (r & 0xFF00) + (s & 0xFF00) + 0x200) >> 2) & 0xFF00;
            out.px[size_t(y) * out.w + x] = rb | g | 0xFF000000u;
        }
    }
    return out;
}

inline uint32_t lerp(uint32_t p, uint32_t q, uint32_t f) { // f in 0..256
    const uint32_t rb = (((p & 0xFF00FF) * (256 - f) + (q & 0xFF00FF) * f) >> 8) & 0xFF00FF;
    const uint32_t g = (((p & 0xFF00) * (256 - f) + (q & 0xFF00) * f) >> 8) & 0xFF00;
    return rb | g | 0xFF000000u;
}

// Part of a level (rx, ry, rw, rh, in its pixels) onto out at x0, y0 (may be off the panel), `step` level pixels
// to a panel pixel; bilinear, or nearest while a finger drags it.
void draw_scaled(Canvas& out, const Level& lv, double rx, double ry, double rw, double rh, double x0, double y0,
                 double step, bool smooth) {
    // In the canvas's stored pixels: a bigger panel shows the page at its own resolution.
    x0 *= out.scale(), y0 *= out.scale(), step /= out.scale();
    const int dw = int(std::ceil(rw / step)), dh = int(std::ceil(rh / step));
    const int xa = std::max(0, int(std::floor(x0))), xb = std::min(out.pixel_w(), int(std::ceil(x0)) + dw);
    const int ya = std::max(0, int(std::floor(y0))), yb = std::min(out.pixel_h(), int(std::ceil(y0)) + dh);
    if (xa >= xb || ya >= yb) return;
    const int lo_x = std::clamp(int(rx), 0, lv.w - 1), hi_x = std::clamp(int(std::ceil(rx + rw)) - 1, lo_x, lv.w - 1);
    const int lo_y = std::clamp(int(ry), 0, lv.h - 1), hi_y = std::clamp(int(std::ceil(ry + rh)) - 1, lo_y, lv.h - 1);
    std::vector<int> col0(size_t(xb - xa)), col1(size_t(xb - xa));
    std::vector<uint32_t> colf(size_t(xb - xa));
    for (int x = xa; x < xb; ++x) {
        const double u = std::clamp(rx + (x - x0 + 0.5) * step - 0.5, double(lo_x), double(hi_x));
        const int i = int(u);
        col0[size_t(x - xa)] = i;
        col1[size_t(x - xa)] = std::min(i + 1, hi_x);
        colf[size_t(x - xa)] = smooth ? uint32_t((u - i) * 256) : 0;
    }
    for (int y = ya; y < yb; ++y) {
        const double v = std::clamp(ry + (y - y0 + 0.5) * step - 0.5, double(lo_y), double(hi_y));
        const int j = int(v);
        const uint32_t fy = smooth ? uint32_t((v - j) * 256) : 0;
        const uint32_t* r0 = lv.px.data() + size_t(j) * lv.w;
        const uint32_t* r1 = lv.px.data() + size_t(std::min(j + 1, hi_y)) * lv.w;
        uint32_t* d = out.data() + size_t(y) * out.pixel_w();
        for (int x = xa; x < xb; ++x) {
            const size_t k = size_t(x - xa);
            if (!smooth) {
                d[x] = r0[col0[k]];
                continue;
            }
            const uint32_t top = lerp(r0[col0[k]], r0[col1[k]], colf[k]);
            const uint32_t bottom = lerp(r1[col0[k]], r1[col1[k]], colf[k]);
            d[x] = lerp(top, bottom, fy);
        }
    }
}

// The whole of `in` fitted into box_w x box_h, its shape kept (a page overview's picture).
Level fitted(const Level& in, int box_w, int box_h) {
    Level from = in;
    while (from.w > box_w * 2 && from.h > box_h * 2) from = halve(from);
    const double scale = std::min(double(box_w) / from.w, double(box_h) / from.h);
    Canvas c(std::max(1, int(from.w * scale)), std::max(1, int(from.h * scale)));
    draw_scaled(c, from, 0, 0, from.w, from.h, 0, 0, 1 / scale, true);
    Level out;
    out.w = c.w();
    out.h = c.h();
    out.px.assign(c.data(), c.data() + size_t(c.w()) * c.h());
    return out;
}

// The page overview's grid, in panel pixels: five columns under the switch's row.
constexpr int grid_cols = 5, grid_side = kit::margin, grid_gap = 12, grid_top = 92, grid_bottom = 72;
constexpr int cell_w = (640 - 2 * grid_side - (grid_cols - 1) * grid_gap) / grid_cols, thumb_h = 124;
constexpr int row_h = thumb_h + 24 + grid_gap;

class ManualView final : public View, public Pinch {
public:
    ManualView(std::string path, std::string place_file) : path_(std::move(path)), place_file_(std::move(place_file)) {
        read_place();
        thread_ = std::thread([this] { work(); });
    }
    ~ManualView() override {
        {
            std::lock_guard<std::mutex> hold(lock_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void update(Core&, uint64_t frame) override {
        frame_ = frame;
        std::lock_guard<std::mutex> hold(lock_);
        if (opened_ && count_ && image_ >= count_) {
            image_ = count_ - 1;
            want(image_);
        }
        resolve_part_locked();
        if (grid_ && std::fabs(glide_) > 0.5 && !pressed_) {
            scroll_ += glide_;
            glide_ *= 0.92;
            if (clamp_scroll()) glide_ = 0;
            visible_thumbs();
        }
    }

    bool shown() const override { return true; }

    uint64_t key(uint64_t frame) const override {
        std::lock_guard<std::mutex> hold(lock_);
        const auto it = cache_.find(image_);
        const uint64_t page = it == cache_.end() ? 0 : uint64_t(size_t(it->second.get()));
        uint64_t k = 1469598103934665603ull;
        for (uint64_t v : {uint64_t(image_), uint64_t(part_ + 8), uint64_t(zoom_), uint64_t(int64_t(pan_x_)),
                           uint64_t(int64_t(pan_y_)), uint64_t(int64_t(swipe_)), uint64_t(dragging_),
                           uint64_t(free_scale_ * 4096), uint64_t(grid_), uint64_t(int64_t(scroll_)), thumbs_serial_,
                           uint64_t(frame < bar_until_), uint64_t(opened_), uint64_t(count_), page})
            k = (k ^ v) * 1099511628211ull;
        return k;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        out.fill(look.background);
        std::shared_ptr<const Decoded> page;
        bool opened, failed;
        size_t count;
        {
            std::lock_guard<std::mutex> hold(lock_);
            opened = opened_;
            failed = !open_error_.empty();
            count = count_;
            const auto it = cache_.find(image_);
            if (it != cache_.end()) page = it->second;
        }
        auto say = [&](const std::string& source) {
            const std::string& text = look.words ? (*look.words)(source) : source;
            const int size = 24;
            const int w = std::min(out.w() - 2 * kit::margin, font.measure(text, size));
            font.draw(out, (out.w() - w) / 2, font.centred(size, out.h() / 2), font.fit(text, size, w), size,
                      look.muted);
        };
        if (opened && failed) return say("This manual could not be opened");
        if (grid_ && opened) return draw_grid(out, look, font, count);
        if (!opened || !page) return say("Loading…");
        if (!page->error.empty() || page->levels.empty()) {
            say(page->error == "not a scan" ? "This page is not a scan" : "This page could not be read");
        } else {
            const Place p = place(*page, out.w(), out.h());
            const Level& top = page->levels.front();
            // The level whose pixels are closest to (and not fewer than) the panel's.
            size_t li = 0;
            while (li + 1 < page->levels.size() && p.scale * top.w / page->levels[li + 1].w <= 1.0) ++li;
            const Level& lv = page->levels[li];
            const double f = double(lv.w) / top.w;
            draw_scaled(out, lv, p.rx * f, p.ry * f, p.rw * f, p.rh * f, p.x, p.y, f / p.scale, !dragging_);
        }
        if (frame < bar_until_ && count) {
            char text[32];
            std::snprintf(text, sizeof text, "%zu / %zu", image_ + 1, count);
            chip(out, look, font, kit::margin, out.h() - kit::margin - 14, text, false);
        }
    }

    bool touch(const Touch& t) override {
        const uint64_t ms = now_ms();
        std::unique_lock<std::mutex> hold(lock_);
        resolve_part_locked();
        const auto it = cache_.find(image_);
        const Decoded* page = it == cache_.end() || it->second->levels.empty() ? nullptr : it->second.get();
        const int W = 640, H = 480;
        if (t.kind == Touch::Down) {
            if (!pinch_live_) pinching_ = false;
            pressed_ = true;
            glide_ = 0;
            move_ms_ = ms;
            down_x_ = last_x_ = t.x;
            down_y_ = last_y_ = t.y;
            dragging_ = false;
            swipe_ = 0;
            return false;
        }
        // A finger that came down elsewhere (on the switch that brought the manual up) is not the reader's.
        if (!pressed_) return false;
        // The first finger of a pinch also arrives here: the pinch moves the page, not it.
        if (pinching_ && t.kind == Touch::Move) return false;
        if (pinching_) {
            pinching_ = pressed_ = dragging_ = false;
            swipe_ = 0;
            last_tap_ms_ = 0;
            return true;
        }
        if (grid_) return grid_touch(t, ms);
        if (t.kind == Touch::Move) {
            if (!dragging_ && std::hypot(t.x - down_x_, t.y - down_y_) < 12) return false;
            dragging_ = true;
            const double dx = t.x - last_x_, dy = t.y - last_y_;
            last_x_ = t.x;
            last_y_ = t.y;
            if (!page) {
                swipe_ = t.x - down_x_;
                return true;
            }
            clamp_pan(*page, W, H); // a pan left "at the top" (turn) becomes a real offset first
            Place p = place(*page, W, H);
            if (p.w > W) pan_x_ += dx;
            else swipe_ = t.x - down_x_;
            if (p.h > H) pan_y_ += dy;
            clamp_pan(*page, W, H);
            return true;
        }
        // Up.
        pressed_ = false;
        const bool was_drag = dragging_;
        dragging_ = false;
        const double swipe = swipe_;
        swipe_ = 0;
        if (was_drag) {
            last_tap_ms_ = 0;
            if (swipe < -80) return turn(+1), true;
            if (swipe > 80) return turn(-1), true;
            return true; // back in place, and drawn smooth again
        }
        if (frame_ < bar_until_ && t.x < 200 && t.y > H - 80) return open_grid(), true; // the "3 / 79" chip
        const bool fits = !page || place(*page, W, H).w <= W;
        const bool second = last_tap_ms_ && ms - last_tap_ms_ < 350 && std::hypot(t.x - tap_x_, t.y - tap_y_) < 48;
        if (fits && !second && (t.x < W / 5 || t.x > W - W / 5)) {
            last_tap_ms_ = 0;
            return turn(t.x < W / 5 ? -1 : +1), true;
        }
        if (page && second) {
            last_tap_ms_ = 0;
            zoom_at(*page, t.x, t.y, W, H);
            return true;
        }
        last_tap_ms_ = ms;
        tap_x_ = t.x;
        tap_y_ = t.y;
        bar_until_ = frame_ + 120;
        return true;
    }

    bool fingers(const std::vector<std::pair<int, int>>& down) override {
        std::lock_guard<std::mutex> hold(lock_);
        resolve_part_locked();
        const auto it = cache_.find(image_);
        const Decoded* page = it == cache_.end() || it->second->levels.empty() ? nullptr : it->second.get();
        const int W = 640, H = 480;
        if (grid_ || (down.size() < 2 && pinch_live_ && pinch_ratio_ < 0.7 && pinch_from_fit_)) {
            // Fingers spread on the overview go back to the page; pinched in from the whole page, the overview.
            if (down.size() >= 2) {
                const double d =
                    std::max(8.0, std::hypot(down[1].first - down[0].first, down[1].second - down[0].second));
                if (!pinch_live_) {
                    pinch_live_ = pinching_ = dragging_ = true;
                    pinch_d0_ = d;
                }
                pinch_ratio_ = d / pinch_d0_;
                return false;
            }
            if (!pinch_live_) return false;
            pinch_live_ = false;
            if (grid_ && pinch_ratio_ > 1.3) grid_ = false;
            else if (!grid_) {
                free_scale_ = 0, zoom_ = 0, pan_x_ = pan_y_ = 0;
                open_grid();
            }
            pinch_ratio_ = 1;
            return true;
        }
        if (down.size() < 2 || !page) {
            if (!pinch_live_) return false;
            pinch_live_ = false;
            pinch_ratio_ = 1;
            // Back to the whole page when let go near it.
            if (page) {
                swipe_ = 0;
                const Place p = place(*page, W, H);
                const double fit = zooms(*page, p.rw, p.rh, W, H).front();
                if (free_scale_ <= fit * 1.04) free_scale_ = 0, zoom_ = 0, pan_x_ = pan_y_ = 0;
                clamp_pan(*page, W, H);
            }
            return true;
        }
        const double ax = down[0].first, ay = down[0].second, bx = down[1].first, by = down[1].second;
        const double d = std::max(8.0, std::hypot(bx - ax, by - ay)), mx = (ax + bx) / 2, my = (ay + by) / 2;
        swipe_ = 0;
        if (!pinch_live_) {
            // The scale and the page point under the fingers' midpoint when the second finger came down.
            clamp_pan(*page, W, H);
            const Place p = place(*page, W, H);
            pinch_live_ = pinching_ = dragging_ = true;
            last_tap_ms_ = 0;
            pinch_d0_ = d;
            pinch_s0_ = p.scale;
            pinch_ratio_ = 1;
            pinch_from_fit_ = p.scale <= zooms(*page, p.rw, p.rh, W, H).front() * 1.04;
            pinch_u_ = (mx - p.x) / p.scale;
            pinch_v_ = (my - p.y) / p.scale;
            return true;
        }
        const Place p = place(*page, W, H);
        const double fit = zooms(*page, p.rw, p.rh, W, H).front();
        pinch_ratio_ = d / pinch_d0_;
        const double scale = std::clamp(pinch_s0_ * d / pinch_d0_, fit, std::max(2.0, fit * 2));
        free_scale_ = scale;
        const double w = p.rw * scale, h = p.rh * scale;
        // The page point the fingers started on stays between them, so two fingers also move the page.
        pan_x_ = mx - pinch_u_ * scale - (W - w) / 2;
        pan_y_ = my - pinch_v_ * scale - (H - h) / 2;
        clamp_pan(*page, W, H);
        return true;
    }

private:
    enum Part { First = -1, Last = -2, Whole = 0, Left = 1, Right = 2 };
    struct Place {
        double rx = 0, ry = 0, rw = 1, rh = 1; // the part shown, in the scan's pixels
        double scale = 1;                      // panel pixels a scan pixel
        double x = 0, y = 0, w = 0, h = 0;     // where it lands
    };

    static bool spread(const Level& l) { return l.w > l.h * 1.6; }

    // ---- The page overview ----
    double content_h() const { return double((count_ + grid_cols - 1) / grid_cols) * row_h; }
    bool clamp_scroll() {
        const double most = std::max(0.0, grid_top + content_h() + grid_bottom - 480);
        const double before = scroll_;
        scroll_ = std::clamp(scroll_, 0.0, most);
        return scroll_ != before;
    }
    // Called with the lock held: the rows on screen (and one either side) get their pictures first.
    void visible_thumbs() {
        const long first_row = std::max(0L, long((scroll_ - grid_top) / row_h) - 1);
        thumb_from_ = size_t(first_row) * grid_cols;
        thumb_to_ = std::min(count_, thumb_from_ + size_t(grid_cols) * (480 / row_h + 3));
        wake_.notify_all();
    }
    void open_grid() {
        grid_ = true;
        glide_ = 0;
        const double row = double(image_ / grid_cols);
        scroll_ = grid_top + row * row_h - (480 - row_h) / 2.0 - grid_top / 2.0;
        clamp_scroll();
        visible_thumbs();
    }
    bool grid_touch(const Touch& t, uint64_t ms) {
        if (t.kind == Touch::Move) {
            if (!dragging_ && std::hypot(t.x - down_x_, t.y - down_y_) < 12) return false;
            dragging_ = true;
            const double dy = t.y - last_y_;
            last_y_ = t.y;
            scroll_ -= dy;
            // Panel pixels a frame, from this move's speed, for the glide after the finger lifts.
            const double dt = std::max<double>(1, double(ms - move_ms_));
            glide_ = std::clamp(-dy / dt * 16.7, -48.0, 48.0);
            move_ms_ = ms;
            clamp_scroll();
            visible_thumbs();
            return true;
        }
        pressed_ = false;
        if (dragging_) {
            dragging_ = false;
            if (ms - move_ms_ > 80) glide_ = 0; // held still before lifting
            return true;
        }
        glide_ = 0;
        // A tap on a page opens it.
        const double y = t.y + scroll_ - grid_top;
        const int col = (t.x - grid_side) / (cell_w + grid_gap);
        if (t.y < grid_top - 8 || y < 0 || col < 0 || col >= grid_cols) return false;
        const size_t index = size_t(y / row_h) * grid_cols + size_t(col);
        if (index >= count_) return false;
        grid_ = false;
        if (index != image_) {
            image_ = index;
            part_ = First;
            want(image_);
            resolve_part_locked();
            save_place();
        }
        zoom_ = 0;
        free_scale_ = 0;
        pan_x_ = 0;
        pan_y_ = 1e9;
        bar_until_ = frame_ + 120;
        return true;
    }
    void draw_grid(Canvas& out, const Look& look, const Font& font, size_t count) const {
        std::map<size_t, std::shared_ptr<const Level>> thumbs;
        double scroll;
        size_t current;
        {
            std::lock_guard<std::mutex> hold(lock_);
            thumbs = thumbs_;
            scroll = scroll_;
            current = image_;
        }
        for (size_t i = 0; i < count; ++i) {
            const int row = int(i / grid_cols), col = int(i % grid_cols);
            const int x = grid_side + col * (cell_w + grid_gap), y = int(grid_top + row * row_h - scroll);
            if (y + row_h < 0) continue;
            if (y > out.h()) break;
            const bool here = i == current;
            if (here) out.rounded_rect(x - 5, y - 5, cell_w + 10, thumb_h + 10, kit::radius, look.accent);
            const auto it = thumbs.find(i);
            if (it != thumbs.end() && it->second->w > 0) {
                const Level& t = *it->second;
                const int tx = x + (cell_w - t.w) / 2, ty = y + (thumb_h - t.h) / 2;
                if (!here) out.rounded_rect(tx - 2, ty - 2, t.w + 4, t.h + 4, 4, look.panel);
                out.blit(t.px.data(), t.w, t.h, tx, ty, 1);
            } else {
                out.rounded_rect(x, y, cell_w, thumb_h, kit::radius / 2, look.panel);
            }
            const std::string number = std::to_string(i + 1);
            const int nw = font.measure(number, 16);
            font.draw(out, x + (cell_w - nw) / 2, y + thumb_h + 20, number, 16, here ? look.accent : look.muted);
        }
        // The switch's row stays clear: the pages slide under it.
        out.fill_rect(0, 0, out.w(), grid_top - 10, look.background);
        char text[32];
        std::snprintf(text, sizeof text, "%zu / %zu", current + 1, count);
        chip(out, look, font, kit::margin, kit::margin + 28, text, true);
    }

    std::vector<double> zooms(const Decoded& d, double rw, double rh, int W, int H) const {
        (void)d;
        const double fit = std::min(W / rw, H / rh), width = W / rw;
        std::vector<double> z{fit};
        if (width > z.back() * 1.15) z.push_back(width);
        if (1.0 > z.back() * 1.15) z.push_back(1.0);
        return z;
    }

    Place place(const Decoded& d, int W, int H) const {
        const Level& l = d.levels.front();
        Place p;
        p.rx = part_ == Right ? l.w / 2 : 0;
        p.rw = part_ == Left ? l.w / 2 : part_ == Right ? l.w - l.w / 2 : l.w;
        p.rh = l.h;
        const auto z = zooms(d, p.rw, p.rh, W, H);
        p.scale = free_scale_ > 0 ? free_scale_ : z[std::min(zoom_, z.size() - 1)];
        p.w = p.rw * p.scale;
        p.h = p.rh * p.scale;
        p.x = p.w <= W ? (W - p.w) / 2 + swipe_ : std::clamp((W - p.w) / 2 + pan_x_, W - p.w, 0.0);
        p.y = p.h <= H ? (H - p.h) / 2 : std::clamp((H - p.h) / 2 + pan_y_, H - p.h, 0.0);
        return p;
    }

    void clamp_pan(const Decoded& d, int W, int H) {
        const double sw = swipe_;
        swipe_ = 0;
        const Place p = place(d, W, H);
        pan_x_ = p.w <= W ? 0 : p.x - (W - p.w) / 2;
        pan_y_ = p.h <= H ? 0 : p.y - (H - p.h) / 2;
        swipe_ = sw;
    }

    void zoom_at(const Decoded& d, int tx, int ty, int W, int H) {
        const Place before = place(d, W, H);
        const double u = (tx - before.x) / before.scale, v = (ty - before.y) / before.scale;
        const size_t levels = zooms(d, before.rw, before.rh, W, H).size();
        // After a pinch, a double tap goes back to the whole page.
        zoom_ = free_scale_ > 0 ? levels - 1 : std::min(zoom_, levels - 1);
        free_scale_ = 0;
        zoom_ = (zoom_ + 1) % levels;
        pan_x_ = pan_y_ = 0;
        Place after = place(d, W, H);
        // The point tapped stays under the finger; fit width starts at the top of the page.
        pan_x_ = tx - u * after.scale - (W - after.w) / 2;
        pan_y_ = zoom_ == 1 && levels == 3 ? 1e9 : ty - v * after.scale - (H - after.h) / 2;
        clamp_pan(d, W, H);
    }

    // Called with the lock held.
    void turn(int dir) {
        bar_until_ = frame_ + 120;
        zoom_ = 0;
        free_scale_ = 0;
        pan_x_ = 0;
        pan_y_ = 1e9; // a tall part starts at its top
        if (dir > 0 && part_ == Left) {
            part_ = Right;
            return;
        }
        if (dir < 0 && part_ == Right) {
            part_ = Left;
            return;
        }
        if (dir > 0 && opened_ && image_ + 1 >= count_) return;
        if (dir < 0 && image_ == 0) return;
        image_ = dir > 0 ? image_ + 1 : image_ - 1;
        part_ = dir > 0 ? First : Last;
        want(image_);
        resolve_part_locked();
        save_place();
    }

    void resolve_part_locked() {
        if (part_ != First && part_ != Last) return;
        const auto it = cache_.find(image_);
        if (it == cache_.end()) return;
        const bool wide = !it->second->levels.empty() && spread(it->second->levels.front());
        part_ = !wide ? Whole : part_ == First ? Left : Right;
    }

    void want(size_t image) {
        want_ = image;
        wake_.notify_all();
    }

    // The worker: opens the file, then keeps the page shown and its neighbours decoded.
    void work() {
        Pages doc = open_pages(path_);
        if (!doc.ok()) std::fprintf(stderr, "play: manual %s: %s\n", path_.c_str(), doc.error.c_str());
        else std::fprintf(stderr, "play: manual %s: %zu pages\n", path_.c_str(), doc.pages.size());
        std::unique_lock<std::mutex> hold(lock_);
        opened_ = true;
        open_error_ = doc.ok() ? "" : doc.error.empty() ? "empty" : doc.error;
        count_ = doc.ok() ? doc.pages.size() : 0;
        if (count_ && image_ >= count_) image_ = count_ - 1;
        while (!stop_) {
            size_t next = SIZE_MAX;
            for (long d : {0L, 1L, -1L, 2L}) {
                const long i = long(want_) + d;
                if (i >= 0 && size_t(i) < count_ && !cache_.count(size_t(i))) {
                    next = size_t(i);
                    break;
                }
            }
            if (next == SIZE_MAX && grid_) {
                size_t thumb = SIZE_MAX;
                for (size_t i = thumb_from_; i < thumb_to_ && i < count_; ++i)
                    if (!thumbs_.count(i)) {
                        thumb = i;
                        break;
                    }
                if (thumb != SIZE_MAX) {
                    hold.unlock();
                    auto made = std::make_shared<Level>();
                    Level page;
                    std::string error;
                    if (read_page(doc, thumb, page.px, page.w, page.h, error, 512))
                        *made = fitted(page, cell_w, thumb_h);
                    hold.lock();
                    thumbs_[thumb] = std::move(made);
                    ++thumbs_serial_;
                    // Kept near what is shown: a 400-page manual stays a few megabytes.
                    for (auto it = thumbs_.begin(); it != thumbs_.end();)
                        it = it->first + 60 < thumb_from_ || it->first > thumb_to_ + 60 ? thumbs_.erase(it)
                                                                                        : std::next(it);
                    continue;
                }
            }
            if (next == SIZE_MAX) {
                wake_.wait(hold);
                continue;
            }
            hold.unlock();
            auto page = std::make_shared<Decoded>();
            Level top;
            if (read_page(doc, next, top.px, top.w, top.h, page->error)) {
                page->levels.push_back(std::move(top));
                while (std::max(page->levels.back().w, page->levels.back().h) > 700)
                    page->levels.push_back(halve(page->levels.back()));
            }
            hold.lock();
            cache_[next] = std::move(page);
            for (auto it = cache_.begin(); it != cache_.end();)
                it = std::labs(long(it->first) - long(want_)) > 2 ? cache_.erase(it) : std::next(it);
            resolve_part_locked();
        }
    }

    void read_place() {
        if (place_file_.empty()) return;
        std::ifstream in(place_file_);
        for (std::string line; std::getline(in, line);) {
            const size_t tab = line.rfind('\t');
            if (tab == std::string::npos) continue;
            if (line.compare(0, tab, path_) == 0) image_ = size_t(std::max(0L, std::atol(line.c_str() + tab + 1)));
        }
        want_ = image_;
    }

    void save_place() const {
        if (place_file_.empty()) return;
        std::vector<std::string> lines;
        {
            std::ifstream in(place_file_);
            for (std::string line; std::getline(in, line);)
                if (line.rfind(path_ + "\t", 0) != 0 && !line.empty()) lines.push_back(line);
        }
        lines.push_back(path_ + "\t" + std::to_string(image_));
        const std::string pending = place_file_ + ".new";
        {
            std::ofstream out(pending, std::ios::trunc);
            for (const auto& l : lines) out << l << '\n';
            if (!out) return;
        }
        std::error_code ec;
        fs::rename(pending, place_file_, ec);
    }

    const std::string path_, place_file_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false, opened_ = false;
    std::string open_error_;
    size_t count_ = 0, want_ = 0;
    std::map<size_t, std::shared_ptr<const Decoded>> cache_;
    // What is shown (under lock_ too: touches and the worker meet here).
    size_t image_ = 0, zoom_ = 0;
    int part_ = First;
    double pan_x_ = 0, pan_y_ = 0, swipe_ = 0;
    bool dragging_ = false, pressed_ = false;
    // A pinch: live while two fingers are down, `pinching_` until the first finger lifts.
    bool pinching_ = false, pinch_live_ = false;
    double free_scale_ = 0; // a pinched scale, or 0 for the steps double tap moves through
    double pinch_d0_ = 1, pinch_s0_ = 1, pinch_u_ = 0, pinch_v_ = 0, pinch_ratio_ = 1;
    bool pinch_from_fit_ = false;
    // The page overview: shown, its scroll and glide (panel pixels a frame), the pictures made so far.
    bool grid_ = false;
    double scroll_ = 0, glide_ = 0;
    uint64_t move_ms_ = 0, thumbs_serial_ = 0;
    size_t thumb_from_ = 0, thumb_to_ = 0;
    std::map<size_t, std::shared_ptr<const Level>> thumbs_;
    int down_x_ = 0, down_y_ = 0, last_x_ = 0, last_y_ = 0, tap_x_ = 0, tap_y_ = 0;
    uint64_t last_tap_ms_ = 0, frame_ = 0, bar_until_ = 120;
};

} // namespace

std::vector<std::string> find_manuals(const std::string& rom, const std::string& art_dir) {
    std::vector<std::string> found;
    std::error_code ec;
    auto add = [&](const fs::path& p) {
        if (!fs::exists(p, ec) || std::find(found.begin(), found.end(), p.string()) != found.end()) return false;
        found.push_back(p.string());
        return true;
    };
    auto beside = [&](const fs::path& dir, const std::string& stem) {
        // One spelling of each kind: a card that ignores case finds "x.pdf" under ".PDF" too.
        for (const auto& kind : {std::pair{".pdf", ".PDF"}, std::pair{".cbz", ".CBZ"}, std::pair{".manual", ".MANUAL"}})
            if (!add(dir / (stem + kind.first))) add(dir / (stem + kind.second));
    };
    if (!rom.empty()) {
        const fs::path p(rom);
        beside(p.parent_path(), p.stem().string());
        // A ScummVM game is its folder: the manual beside the folder, or inside it.
        if (fs::is_directory(p, ec)) beside(p.parent_path(), p.filename().string());
    }
    if (!art_dir.empty())
        for (const char* name : {"manual.pdf", "manual.cbz", "manual"}) add(fs::path(art_dir) / name);
    return found;
}

std::unique_ptr<View> make_manual(const std::string& path, const std::string& place_file) {
    return std::make_unique<ManualView>(path, place_file);
}

} // namespace play
