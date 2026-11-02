// SPDX-License-Identifier: GPL-3.0-or-later
// A Mode 7 racing game's course seen from above on the second screen, as
// Mario Kart DS has it (Super Mario Kart): the whole course from the game's own
// video memory, sharp at the panel's size, every racer at the place the game
// keeps for it, drawn as the kart its map shows, the items on the map where the
// map has them, and the standings beside it. A tap goes on to the course close
// up, turning with the player so ahead is up, then to the game's own lower
// picture (its map or rear view, the [hud] band), then back.
//
//   [view]
//   format = course
//   on = mem8[7E10C1] in 7F..84
//   sprites = 7E0420                 the map's sprite table in RAM (OAM's layout)
//   places = 7E1017 7E101B 100 8     X, Y (16.8, the fraction byte first), step, how many
//   tiles = 180 184 ...              the first tile of each racer's kart, in the places' order
//   ranks = 7E10E6 2                 each racer's place from 0, times 2 (same step)
//   lap = mem8[7E10C1] - 7F          the player's lap (0 before the start line), and laps = 5
//
// The first place is the player's. The course is read again when its tile map
// changes (a new course), and drawn at the panel's size once.
#include "companion/expr.hpp"
#include "companion/mode7.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace play {
namespace {

using namespace mode7;

class Course final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "lap", lap_, false, error)) return false;
        auto bad = [&](const std::string& key, const std::string& value) {
            error = "[view] course: bad " + key + " '" + value + "'";
            return false;
        };
        for (const auto& [key, value] : spec.values) {
            std::istringstream words(value);
            if (key == "sprites") {
                if (!parse_address(value, sprites_)) return bad(key, value);
            } else if (key == "places") {
                std::string x, y, step;
                words >> x >> y >> step >> count_;
                if (!parse_address(x, x_) || !parse_address(y, y_) || !parse_address(step, step_) || count_ < 1 ||
                    count_ > 16)
                    return bad(key, value);
            } else if (key == "tiles") {
                for (std::string word; words >> word;) {
                    uint32_t tile = 0;
                    if (!parse_address(word, tile) || tile >= 0x200) return bad(key, value);
                    tiles_.push_back(int(tile));
                }
            } else if (key == "ranks") {
                std::string at;
                words >> at >> rank_div_;
                if (!parse_address(at, ranks_) || rank_div_ < 1) return bad(key, value);
            } else if (key == "laps") {
                laps_ = std::atoi(value.c_str());
            }
        }
        if (!count_) {
            error = "[view] course: needs places = X Y STEP COUNT";
            return false;
        }
        racers_.resize(size_t(count_));
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        frame_ = frame;
        shown_ = on_.empty() || on_.match(live);
        Core::Mode7 m;
        if (!shown_ || !live.mode7(m)) {
            shown_ = false;
            return;
        }
        // The course: read again when its tile map changes (checked every half second).
        if (world_.empty() || frame % 30 == 0) {
            uint64_t hash = 1469598103934665603ull;
            for (int i = 0; i < 0x8000; i += 2) hash = (hash ^ m.vram[i]) * 1099511628211ull;
            if (hash != course_hash_ || world_.empty()) {
                course_hash_ = hash;
                world_.resize(1024 * 1024);
                for (int y = 0; y < 1024; ++y)
                    for (int x = 0; x < 1024; ++x) world_[size_t(y) * 1024 + x] = course_index(m.vram, x, y);
                overview_.clear();
            }
        }
        // The course's own colours (the sprites' half flashes every frame) drawn
        // again when they change, at most once a second (a course that cycles them).
        uint64_t shades = 1469598103934665603ull;
        for (int i = 0; i < 256; ++i) {
            colours_[i] = colour(m.palette[i]);
            if (i < 128) shades = (shades ^ colours_[i]) * 1099511628211ull;
        }
        if (shades != shades_ && (overview_.empty() || frame - shaded_at_ >= 60))
            shades_ = shades, shaded_at_ = frame, overview_.clear();
        // Ahead: from where the race's last line looks to where its first does.
        int s = 0, e = 0;
        const bool race = race_lines(m, s, e);
        if (race) {
            double nx, ny, fx, fy;
            course_at(m, e, 128, nx, ny);
            course_at(m, s + 2, 128, fx, fy);
            if (const double d = std::hypot(fx - nx, fy - ny); d > 1)
                ahead_x_ = (fx - nx) / d, ahead_y_ = (fy - ny) / d;
        }
        for (int i = 0; i < count_; ++i) {
            Racer& r = racers_[size_t(i)];
            r.x = peek_address(live, x_ + step_ * uint32_t(i), 3) / 256.0;
            r.y = peek_address(live, y_ + step_ * uint32_t(i), 3) / 256.0;
            r.place = ranks_ ? int(peek_address(live, ranks_ + step_ * uint32_t(i), 1)) / rank_div_ + 1 : 0;
        }
        lap_now_ = lap_.empty() ? 0 : lap_.eval(live);
        // The karts and items as the game's map draws them, from their own tiles, every other frame.
        int ms = 0, me = 0;
        if (!sprites_ || !m.objsel || !race || frame % 2) return;
        items_.clear();
        if (!map_lines(m, e, ms, me)) return;
        uint8_t oam[544];
        for (uint32_t i = 0; i < sizeof oam; ++i) oam[i] = uint8_t(peek_address(live, sprites_ + i, 1));
        for (const Sprite& sp : sprites(oam, m.obj_sizes)) {
            Art art;
            art.w = sp.w, art.h = sp.h;
            art.px.resize(size_t(sp.w) * sp.h);
            for (int y = 0; y < sp.h; ++y)
                for (int x = 0; x < sp.w; ++x)
                    if (const int c = pixel(m, sp, x, y)) art.px[size_t(y) * sp.w + x] = colours_[c];
            bool racer = false;
            for (size_t q = 0; q < racers_.size() && q < tiles_.size(); ++q)
                if (sp.drawn_from(tiles_[q])) racers_[q].art = art, racer = true;
            const int line = sp.y + sp.h / 2 + m.first_line;
            if (racer || line < ms || line > me) continue;
            Item item;
            course_at(m, line, sp.x + sp.w / 2.0, item.x, item.y);
            item.art = std::move(art);
            items_.push_back(std::move(item));
        }
    }

    bool shown() const override { return shown_ && !world_.empty(); }
    uint64_t key(uint64_t frame) const override {
        // Racers move every frame: drawn at half the rate, and at once after a tap.
        return (frame / 2) * 1099511628211ull ^ (uint64_t(mode_) << 56) ^ serial_ * 0x9E3779B97F4A7C15ull;
    }
    bool covers_hud() const override { return mode_ != Game; }
    bool touch(const Touch& t) override {
        if (t.kind != Touch::Down) return false;
        mode_ = Mode((int(mode_) + 1) % 3);
        changed_at_ = frame_;
        ++serial_;
        return true;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        const int column = count_ > 1 && ranks_ ? 132 : 0;
        const int size = std::min(out.h() - 2 * kit::gap, out.w() - column - 3 * kit::gap);
        const Box map{kit::gap, (out.h() - size) / 2, size, size};
        out.fill(look.background);
        backdrop(out, look);
        out.rounded_rect(map.x - 4, map.y - 4, map.w + 8, map.h + 8, kit::radius, look.panel);
        if (mode_ == Whole) draw_whole(out, look, map);
        else draw_close(out, look, map);
        // What a tap brought up, for a moment.
        if (frame - changed_at_ < 90 && changed_at_)
            chip(out, look, font, map.x + 10, map.y + 24, look.tr(mode_ == Whole ? "Whole course" : "Close up"), true);
        if (column) draw_standings(out, look, font, {map.x + map.w + kit::gap + 4, map.y, column - 4, map.h});
    }

private:
    enum Mode { Whole, Close, Game };
    struct Art {
        int w = 0, h = 0;
        std::vector<uint32_t> px; // 0 where clear
    };
    struct Racer {
        double x = 0, y = 0;
        int place = 0;
        Art art;
    };
    struct Item {
        double x = 0, y = 0;
        Art art;
    };

    // A sprite as the game draws it, centred on (cx, cy), inside the box.
    static void stamp(Canvas& out, const Art& art, int cx, int cy, const Box& clip, int rows = 0) {
        const int h = rows ? std::min(rows, art.h) : art.h;
        const int left = cx - art.w / 2, top = cy - h / 2;
        for (int y = 0; y < h; ++y) {
            const int oy = top + y;
            if (oy < clip.y || oy >= clip.y + clip.h) continue;
            for (int x = 0; x < art.w; ++x) {
                const int ox = left + x;
                const uint32_t c = art.px[size_t(y) * art.w + x];
                if (c && ox >= clip.x && ox < clip.x + clip.w) out.data()[size_t(oy) * out.w() + ox] = c;
            }
        }
    }
    // The player's spot: a ring in the accent under its kart.
    static void marker(Canvas& out, const Look& look, int cx, int cy) {
        disc(out, cx, cy + 6, 21, look.shadow(), 120);
        ring(out, cx, cy + 6, 19, 4, 1.0, look.accent);
    }

    void draw_whole(Canvas& out, const Look& look, const Box& map) const {
        // The course at the box's size, once: each pixel the mean of the course under it.
        if (overview_size_ != map.w || overview_.empty()) {
            overview_size_ = map.w;
            overview_.assign(size_t(map.w) * map.w, 0);
            for (int y = 0; y < map.w; ++y) {
                const int y0 = y * 1024 / map.w, y1 = std::max(y0 + 1, (y + 1) * 1024 / map.w);
                for (int x = 0; x < map.w; ++x) {
                    const int x0 = x * 1024 / map.w, x1 = std::max(x0 + 1, (x + 1) * 1024 / map.w);
                    uint32_t r = 0, g = 0, b = 0, n = 0;
                    for (int v = y0; v < y1; ++v)
                        for (int u = x0; u < x1; ++u, ++n) {
                            const uint32_t c = colours_[world_[size_t(v) * 1024 + u]];
                            r += c & 0xFF, g += (c >> 8) & 0xFF, b += (c >> 16) & 0xFF;
                        }
                    overview_[size_t(y) * map.w + x] = 0xFF000000u | (r / n) | ((g / n) << 8) | ((b / n) << 16);
                }
            }
        }
        out.blit(overview_.data(), map.w, map.w, map.x, map.y, 1);
        auto at = [&](double wx, double wy, int& sx, int& sy) {
            sx = map.x + int(wx * map.w / 1024), sy = map.y + int(wy * map.w / 1024);
        };
        int sx, sy;
        for (const Item& item : items_) {
            at(item.x, item.y, sx, sy);
            stamp(out, item.art, sx, sy, map);
        }
        // Farther up the course first, the player last and marked.
        std::vector<const Racer*> order;
        for (size_t i = 1; i < racers_.size(); ++i) order.push_back(&racers_[i]);
        std::sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) { return a->y < b->y; });
        order.push_back(&racers_[0]);
        for (const Racer* r : order) {
            at(r->x, r->y, sx, sy);
            if (r == &racers_[0]) marker(out, look, sx, sy);
            if (!r->art.px.empty()) stamp(out, r->art, sx, sy, map);
            else disc(out, sx, sy, 6, r == &racers_[0] ? look.accent : look.ink);
        }
    }

    void draw_close(Canvas& out, const Look& look, const Box& map) const {
        // About 180 course units across (a kart is about 8, so two side by side
        // both show), ahead up, the player low in the box; the course sampled
        // every other pixel (the game's own pixels are this coarse up close).
        const double zoom = map.w / 180.0;
        const double fx = ahead_x_, fy = ahead_y_, rx = -fy, ry = fx;
        const double px = racers_[0].x, py = racers_[0].y;
        const int cx = map.x + map.w / 2, cy = map.y + map.h * 62 / 100;
        uint32_t* dst = out.data();
        for (int y = 0; y < map.h; y += 2)
            for (int x = 0; x < map.w; x += 2) {
                const double dx = (map.x + x + 1 - cx) / zoom, dy = (cy - map.y - y - 1) / zoom;
                const int wx = int(std::floor(px + rx * dx + fx * dy)), wy = int(std::floor(py + ry * dx + fy * dy));
                const uint32_t c = colours_[world_[size_t(wy & 1023) * 1024 + (wx & 1023)]];
                const size_t i = size_t(map.y + y) * out.w() + map.x + x;
                dst[i] = c;
                if (x + 1 < map.w) dst[i + 1] = c;
                if (y + 1 < map.h) {
                    dst[i + out.w()] = c;
                    if (x + 1 < map.w) dst[i + out.w() + 1] = c;
                }
            }
        auto at = [&](double wx, double wy, int& sx, int& sy) {
            const double dx = wx - px, dy = wy - py;
            sx = cx + int(std::lround((dx * rx + dy * ry) * zoom)),
            sy = cy - int(std::lround((dx * fx + dy * fy) * zoom));
        };
        int sx, sy;
        for (const Item& item : items_) {
            at(item.x, item.y, sx, sy);
            stamp(out, item.art, sx, sy, map);
        }
        // Farther ahead first, the player last.
        std::vector<std::pair<double, const Racer*>> order;
        for (size_t i = 1; i < racers_.size(); ++i)
            order.push_back({(racers_[i].x - px) * fx + (racers_[i].y - py) * fy, &racers_[i]});
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
        order.push_back({0, &racers_[0]});
        for (const auto& [ahead, r] : order) {
            at(r->x, r->y, sx, sy);
            if (r == &racers_[0]) marker(out, look, sx, sy);
            if (!r->art.px.empty()) stamp(out, r->art, sx, sy, map);
            else disc(out, sx, sy, 6, r == &racers_[0] ? look.accent : look.ink);
        }
    }

    void draw_standings(Canvas& out, const Look& look, const Font& font, const Box& b) const {
        int y = b.y;
        if (!lap_.empty() && lap_now_ > 0) {
            std::string lap = look.tr("Lap") + " " + std::to_string(std::min<long>(lap_now_, laps_ ? laps_ : lap_now_));
            if (laps_) lap += "/" + std::to_string(laps_);
            card(out, look, {b.x, y, b.w, 40});
            font.draw(out, b.x + 14, font.centred(kit::body, y + 20), lap, kit::body, look.ink);
            y += 40 + kit::gap / 2;
        }
        // Clear of the dock's corner at the bottom right.
        const int room = out.h() - look.reserve_h - kit::gap - y;
        const int row = std::min(44, room / count_);
        std::vector<const Racer*> order;
        for (const Racer& r : racers_) order.push_back(&r);
        std::stable_sort(order.begin(), order.end(),
                         [](const Racer* a, const Racer* c) { return a->place < c->place; });
        for (const Racer* r : order) {
            const bool you = r == &racers_[0];
            const Box line{b.x, y, b.w, row - 4};
            if (you) out.rounded_rect(line.x, line.y, line.w, line.h, kit::radius, look.accent);
            else card(out, look, line);
            const Rgb ink = you ? look.on_accent : look.ink;
            font.draw(out, line.x + 12, font.centred(kit::body, line.y + line.h / 2), std::to_string(r->place),
                      kit::body, ink);
            // The racer's kart, its top part: the driver.
            if (!r->art.px.empty()) stamp(out, r->art, line.x + line.w - 30, line.y + line.h / 2 + 4, line, line.h + 4);
            y += row;
        }
    }

    Condition on_;
    Expr lap_;
    uint32_t sprites_ = 0, x_ = 0, y_ = 0, step_ = 0, ranks_ = 0;
    int count_ = 0, rank_div_ = 1, laps_ = 0;
    std::vector<int> tiles_;

    bool shown_ = false;
    Mode mode_ = Whole;
    uint64_t frame_ = 0, changed_at_ = 0, serial_ = 0;
    std::vector<uint8_t> world_; // the course's colour indices, 1024 x 1024
    uint32_t colours_[256] = {};
    uint64_t course_hash_ = 0, shades_ = 0, shaded_at_ = 0;
    mutable std::vector<uint32_t> overview_; // the whole course at the box's size
    mutable int overview_size_ = 0;
    double ahead_x_ = 0, ahead_y_ = -1;
    std::vector<Racer> racers_;
    std::vector<Item> items_;
    long lap_now_ = 0;
};

} // namespace

std::unique_ptr<View> make_course(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Course>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
