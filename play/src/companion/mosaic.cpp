// SPDX-License-Identifier: GPL-3.0-or-later
// format = mosaic: a map of everywhere the player has been, made from the
// game's own screen. Each few frames the playfield is pasted, small, at the
// place in the world the game says it shows, so the map is the game's own
// picture of the places seen, one canvas per area (the overworld, each
// dungeon). For games with no map at all (Metroid) or one kept in a menu
// (The Legend of Zelda's dungeons, Super Metroid).
//
//   area = mem8[10]                       # one map per value (default: one map)
//   x = (mem8[EB] & F) * 100              # where the frame's top left is in the world,
//   y = (mem8[EB] >> 4) * B0 - 40         # in the game's pixels (expr.hpp)
//   native = 256 240                      # the frame the numbers are for (a core that
//                                         # crops overscan shows less: centred)
//   crop = 0 64 256 176                   # the playfield in that frame (no HUD)
//   when = mem8[12] == 05                 # pasted only while this holds (not mid-scroll)
//   player_x = mem8[70] + 8               # optional: a marker, in world pixels
//   player_y = mem8[84] + 8
//   scale = 8                             # world pixels per map pixel (decimal)
//   every = 6                             # frames between pastes (decimal)
//   name.0 = Hyrule                       # what the chip in the corner says per area
//   name = Level                          # ... for areas without a name.N ("Level 3")
#include "companion/companion.hpp"
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

namespace play {
namespace {

constexpr int tile = 32; // map pixels a side per stored tile

int floor_div(long a, int b) { return int(a >= 0 ? a / b : -((-a + b - 1) / b)); }

struct Tile {
    uint32_t px[tile * tile] = {}; // 0: not seen
};

struct Area {
    std::map<std::pair<int, int>, Tile> tiles;
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN; // seen, in map pixels
    void grow(int x, int y) {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x + 1);
        max_y = std::max(max_y, y + 1);
    }
    bool empty() const { return max_x <= min_x; }
    uint32_t at(int x, int y) const {
        auto t = tiles.find({floor_div(x, tile), floor_div(y, tile)});
        if (t == tiles.end()) return 0;
        return t->second.px[size_t((y - floor_div(y, tile) * tile) * tile + (x - floor_div(x, tile) * tile))];
    }
    uint32_t& set(int x, int y) {
        Tile& t = tiles[{floor_div(x, tile), floor_div(y, tile)}];
        return t.px[size_t((y - floor_div(y, tile) * tile) * tile + (x - floor_div(x, tile) * tile))];
    }
};

bool parse_ints(const std::string& text, std::vector<int>& out, size_t count) {
    std::istringstream in(text);
    out.clear();
    for (int v; in >> v;) out.push_back(v);
    return out.size() == count;
}

class Mosaic final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "x", x_, true, error) || !view_expr(spec, "y", y_, true, error) ||
            !view_expr(spec, "area", area_, false, error) || !view_expr(spec, "player_x", player_x_, false, error) ||
            !view_expr(spec, "player_y", player_y_, false, error))
            return false;
        std::vector<int> v;
        for (const auto& [key, value] : spec.values) {
            bool ok = true;
            if (key == "native") {
                ok = parse_ints(value, v, 2) && v[0] > 0 && v[1] > 0;
                if (ok) native_w_ = v[0], native_h_ = v[1];
            } else if (key == "crop") {
                ok = parse_ints(value, v, 4) && v[2] > 0 && v[3] > 0;
                if (ok) crop_ = {v[0], v[1], v[2], v[3]};
            } else if (key == "scale") {
                ok = parse_ints(value, v, 1) && v[0] >= 1 && v[0] <= 32;
                if (ok) scale_ = v[0];
            } else if (key == "every") {
                ok = parse_ints(value, v, 1) && v[0] >= 1;
                if (ok) every_ = v[0];
            } else if (key == "when") {
                std::string why;
                ok = parse_condition(value, when_, why);
            } else if (key == "name") {
                name_ = value;
            } else if (key.rfind("name.", 0) == 0) {
                names_[std::strtol(key.c_str() + 5, nullptr, 16)] = value;
            }
            if (!ok) {
                error = "[view] mosaic: bad " + key + " '" + value + "'";
                return false;
            }
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        const long area = area_.eval(live);
        if (area != area_now_) {
            area_now_ = area;
            ++serial_;
        }
        if (!player_x_.empty()) {
            const int px = floor_div(player_x_.eval(live), scale_), py = floor_div(player_y_.eval(live), scale_);
            player_x_now_ = px;
            player_y_now_ = py;
        }
        if (frame % uint64_t(every_) || (!when_.empty() && !when_.match(live))) return;
        paste(live);
    }

    bool shown() const override { return playing_; }

    uint64_t key(uint64_t frame) const override {
        return (uint64_t(serial_) << 24) ^ (uint64_t(area_now_ & 0xFF) << 56) ^ (uint64_t(player_x_now_ & 0xFFF) << 12) ^
               uint64_t(player_y_now_ & 0xFFF) ^ (has_player() && (frame / 20) & 1 ? 0x800000 : 0);
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        backdrop(out, look);
        auto found = areas_.find(area_now_);
        const int top = kit::margin * 2 + 28; // under the chips
        if (found != areas_.end() && !found->second.empty()) draw_area(out, look, found->second, top, frame);
        const std::string name = area_name(area_now_);
        if (!name.empty()) chip(out, look, font, kit::margin, kit::margin + 14, name, true);
        if (found == areas_.end() || found->second.empty())
            chip(out, look, font, kit::margin, kit::margin + 14 + (name.empty() ? 0 : 44),
                 look.tr("The map draws itself as you explore"), false);
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGMS" 1 scale, then per tile: area, x, y (int32 each) and its pixels.
        bytes = "RGMS";
        bytes += char(1);
        bytes += char(scale_);
        for (const auto& [area, a] : areas_)
            for (const auto& [at, t] : a.tiles) {
                const int32_t head[3] = {int32_t(area), at.first, at.second};
                bytes.append(reinterpret_cast<const char*>(head), sizeof head);
                bytes.append(reinterpret_cast<const char*>(t.px), sizeof t.px);
            }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        const size_t entry = 12 + sizeof(Tile::px);
        // A map made at another scale is of no use at this one.
        if (bytes.size() < 6 || bytes.compare(0, 4, "RGMS") != 0 || bytes[4] != 1 || bytes[5] != char(scale_))
            return;
        for (size_t at = 6; at + entry <= bytes.size(); at += entry) {
            int32_t head[3];
            std::memcpy(head, bytes.data() + at, sizeof head);
            Area& a = areas_[head[0]];
            Tile& t = a.tiles[{head[1], head[2]}];
            std::memcpy(t.px, bytes.data() + at + 12, sizeof t.px);
            for (int y = 0; y < tile; ++y)
                for (int x = 0; x < tile; ++x)
                    if (t.px[y * tile + x]) a.grow(head[1] * tile + x, head[2] * tile + y);
        }
        saved_ = ++serial_;
    }

private:
    bool has_player() const { return !player_x_.empty(); }

    std::string area_name(long area) const {
        auto n = names_.find(area);
        if (n != names_.end()) return n->second;
        return name_.empty() ? std::string() : name_ + " " + std::to_string(area);
    }

    void paste(Core& live) {
        const Picture p = live.picture();
        if (!p.px || p.w <= 0 || p.h <= 0) return;
        const int nw = native_w_ ? native_w_ : p.w, nh = native_h_ ? native_h_ : p.h;
        // A core that crops the overscan shows the middle of the native frame.
        const int ox = (nw - p.w) / 2, oy = (nh - p.h) / 2;
        Rect c = crop_.empty() ? Rect{0, 0, nw, nh} : crop_;
        // The playfield in the picture's own pixels.
        const int x0 = std::max(c.x - ox, 0), y0 = std::max(c.y - oy, 0);
        const int x1 = std::min(c.x + c.w - ox, p.w), y1 = std::min(c.y + c.h - oy, p.h);
        if (x1 - x0 < scale_ || y1 - y0 < scale_) return;
        const long wx = x_.eval(live) + ox, wy = y_.eval(live) + oy; // the world under picture pixel 0, 0
        Area& area = areas_[area_now_];
        bool changed = false;
        // Every map pixel whose block of world pixels lies wholly in the playfield.
        const int mx0 = floor_div(wx + x0 + scale_ - 1, scale_), mx1 = floor_div(wx + x1, scale_);
        const int my0 = floor_div(wy + y0 + scale_ - 1, scale_), my1 = floor_div(wy + y1, scale_);
        const int n = scale_ * scale_;
        for (int my = my0; my < my1; ++my)
            for (int mx = mx0; mx < mx1; ++mx) {
                const int sx = int(long(mx) * scale_ - wx), sy = int(long(my) * scale_ - wy);
                unsigned r = 0, g = 0, b = 0;
                for (int yy = 0; yy < scale_; ++yy) {
                    const uint32_t* row = p.px + size_t(sy + yy) * p.stride + sx;
                    for (int xx = 0; xx < scale_; ++xx) {
                        r += row[xx] & 0xFF;
                        g += (row[xx] >> 8) & 0xFF;
                        b += (row[xx] >> 16) & 0xFF;
                    }
                }
                const uint32_t v = (r / n) | ((g / n) << 8) | ((b / n) << 16) | 0xFF000000u;
                uint32_t& dst = area.set(mx, my);
                if (dst != v) {
                    if (!dst) area.grow(mx, my);
                    dst = v;
                    changed = true;
                }
            }
        view_x_ = mx0;
        view_y_ = my0;
        view_w_ = mx1 - mx0;
        view_h_ = my1 - my0;
        if (changed) ++serial_;
    }

    void draw_area(Canvas& out, const Look& look, const Area& a, int top, uint64_t frame) const {
        const int W = out.w(), H = out.h() - top - kit::margin, left = kit::margin, room_w = W - 2 * kit::margin;
        const int bw = a.max_x - a.min_x, bh = a.max_y - a.min_y;
        // The whole area at the largest whole scale that fits; an area too big
        // for the panel at 1:1 follows the player (or the screen) instead.
        int zoom = std::max(1, std::min(room_w / std::max(1, bw), H / std::max(1, bh)));
        zoom = std::min(zoom, 6);
        const int vw = room_w / zoom, vh = H / zoom; // map pixels shown
        const int fx = has_player() ? player_x_now_ : view_x_ + view_w_ / 2;
        const int fy = has_player() ? player_y_now_ : view_y_ + view_h_ / 2;
        const int ox = bw <= vw ? a.min_x : std::clamp(fx - vw / 2, a.min_x, a.max_x - vw);
        const int oy = bh <= vh ? a.min_y : std::clamp(fy - vh / 2, a.min_y, a.max_y - vh);
        const uint32_t unseen = mix(look.background, look.panel, 128).px();
        uint32_t* px = out.data();
        // A card the size of what is shown, so the map has an edge on the theme's ground.
        const int cw = std::min(bw, vw) * zoom, ch = std::min(bh, vh) * zoom;
        const int cx = left + (room_w - cw) / 2, cy = top + (H - ch) / 2;
        card(out, look, {cx - 6, cy - 6, cw + 12, ch + 12}, false, kit::radius);
        for (int y = 0; y < ch; ++y) {
            uint32_t* row = px + size_t(cy + y) * W + cx;
            for (int x = 0; x < cw; ++x) {
                const uint32_t v = a.at(ox + x / zoom, oy + y / zoom);
                row[x] = v ? v : unseen;
            }
        }
        // The screen in view, and the player blinking on it.
        auto to_x = [&](int mx) { return cx + (mx - ox) * zoom; };
        auto to_y = [&](int my) { return cy + (my - oy) * zoom; };
        auto inside = [&](int x, int y) { return x >= cx && y >= cy && x < cx + cw && y < cy + ch; };
        const Rgb edge = look.accent;
        const int t = std::max(1, zoom / 2 + 1);
        const int rx = to_x(view_x_), ry = to_y(view_y_), rw = view_w_ * zoom, rh = view_h_ * zoom;
        auto hline = [&](int x, int y, int w) {
            for (int yy = y; yy < y + t; ++yy)
                for (int xx = x; xx < x + w; ++xx)
                    if (inside(xx, yy)) px[size_t(yy) * W + xx] = edge.px();
        };
        auto vline = [&](int x, int y, int h) {
            for (int yy = y; yy < y + h; ++yy)
                for (int xx = x; xx < x + t; ++xx)
                    if (inside(xx, yy)) px[size_t(yy) * W + xx] = edge.px();
        };
        if (view_w_ > 0) {
            hline(rx, ry, rw);
            hline(rx, ry + rh - t, rw);
            vline(rx, ry, rh);
            vline(rx + rw - t, ry, rh);
        }
        if (has_player()) {
            const int r = std::max(4, zoom * 2);
            const int mx = to_x(player_x_now_), my = to_y(player_y_now_);
            const Rgb marker = (frame / 20) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64};
            if (inside(mx, my)) {
                disc(out, mx, my, r + 2, Rgb{12, 17, 26});
                disc(out, mx, my, r, marker);
            }
        }
    }

    Condition on_, when_;
    Expr x_, y_, area_, player_x_, player_y_;
    Rect crop_;
    int native_w_ = 0, native_h_ = 0, scale_ = 4, every_ = 6;
    std::string name_;
    std::map<long, std::string> names_;
    std::map<long, Area> areas_;
    bool playing_ = false;
    long area_now_ = 0;
    int player_x_now_ = 0, player_y_now_ = 0, view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0;
    uint32_t serial_ = 0;
    mutable uint32_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_mosaic(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Mosaic>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
