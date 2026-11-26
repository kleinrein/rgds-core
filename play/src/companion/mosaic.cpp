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
//   zoom = 2                              # panel pixels per map pixel at least (decimal): a
//                                         # long level is shown around the player, larger
//   every = 6                             # frames between pastes (decimal)
//   lag = 1                               # the picture shows x and y of this many frames
//                                         # ago (games that scroll the screen in the next
//                                         # vertical blank: Sonic), up to 3
//   source = plane a                      # Mega Drive: the map is drawn from the foreground tile plane
//                                         # (a or b) in video memory and the sprites on it, not from
//                                         # the picture, so a background that scrolls at its own
//                                         # speed (Sonic's hills and clouds) is not smeared into it;
//                                         # where the plane is empty the map is open air, in the
//                                         # background's mean colour. With a core that does not
//                                         # export its video memory the picture is pasted as before.
//   plane_crop = 0 0 320 192              # the part of the screen taken from the plane (default: all)
//   plane_offset = 0 0                    # the plane's pixel under the world's 0, 0
//   skip = 0 0 136 64                     # with a plane: parts of the screen never pasted (the
//   skip2 = 0 192 96 32                   # game's score, drawn as sprites; skip2 ... skip4 for more)
//   player_box = -24 -32 48 64            # ... and this box around player_x, player_y, so the hero
//                                         # is not left standing all over the map
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
#include <iterator>
#include <map>
#include <sstream>
#include <vector>

namespace play {
namespace {

constexpr int tile = 32;      // map pixels a side per stored tile
constexpr int move_every = 3; // frames between moves of the player's spot
// A stored pixel: 0 not seen, `air` seen with nothing there in the foreground plane, else bit 15 and 5 bits each
// of red, green and blue. Two bytes a pixel: at half size one act of Sonic is a few megabytes.
constexpr uint16_t air = 1;
uint16_t pack(unsigned r, unsigned g, unsigned b) {
    return uint16_t(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}
uint32_t unpack(uint16_t v) {
    const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}
uint16_t pack_px(uint32_t px) { return pack(px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF); }

int floor_div(long a, int b) { return int(a >= 0 ? a / b : -((-a + b - 1) / b)); }

struct Tile {
    uint16_t px[tile * tile] = {};
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
    uint32_t sky = 0; // what open air is drawn as: the mean colour behind the plane (0: not yet known)
    bool empty() const { return max_x <= min_x; }
    uint16_t& set(int x, int y) {
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
            } else if (key == "plane_crop") {
                ok = parse_ints(value, v, 4) && v[2] > 0 && v[3] > 0;
                if (ok) plane_crop_ = {v[0], v[1], v[2], v[3]};
            } else if (key == "plane_offset") {
                ok = parse_ints(value, v, 2);
                if (ok) plane_dx_ = v[0], plane_dy_ = v[1];
            } else if (key == "skip" ||
                       (key.size() == 5 && key.rfind("skip", 0) == 0 && key[4] >= '2' && key[4] <= '4')) {
                ok = parse_ints(value, v, 4) && v[2] > 0 && v[3] > 0;
                if (ok) skip_.push_back({v[0], v[1], v[2], v[3]});
            } else if (key == "player_box") {
                ok = parse_ints(value, v, 4) && v[2] > 0 && v[3] > 0;
                if (ok) player_box_ = {v[0], v[1], v[2], v[3]};
            } else if (key == "source") {
                ok = value == "picture" || value == "plane a" || value == "plane b";
                plane_ = value == "plane a" ? 1 : value == "plane b" ? 2 : 0;
            } else if (key == "scale") {
                ok = parse_ints(value, v, 1) && v[0] >= 1 && v[0] <= 32;
                if (ok) scale_ = v[0];
            } else if (key == "zoom") {
                ok = parse_ints(value, v, 1) && v[0] >= 1 && v[0] <= 6;
                if (ok) zoom_ = v[0];
            } else if (key == "every") {
                ok = parse_ints(value, v, 1) && v[0] >= 1;
                if (ok) every_ = v[0];
            } else if (key == "lag") {
                ok = parse_ints(value, v, 1) && v[0] >= 0 && v[0] < int(std::size(seen_));
                if (ok) lag_ = v[0];
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
        // The player's spot moves the map at most 20 times a second: each move is a whole
        // panel to draw and upload on the thread that also runs the game.
        if (!player_x_.empty() && frame % uint64_t(move_every) == 0) {
            const int px = floor_div(player_x_.eval(live), scale_), py = floor_div(player_y_.eval(live), scale_);
            player_x_now_ = px;
            player_y_now_ = py;
        }
        // Where the frame's top left is, this frame and the last few: the picture may show an older one.
        std::move_backward(std::begin(seen_), std::end(seen_) - 1, std::end(seen_));
        seen_[0] = {x_.eval(live), y_.eval(live), frame};
        if (frame % uint64_t(every_) || (!when_.empty() && !when_.match(live))) return;
        // The planes in video memory are the coming frame's, as the camera in memory is.
        if (plane_ && paste_plane(live, seen_[0].x, seen_[0].y)) return;
        if (seen_[lag_].frame + uint64_t(lag_) != frame) return; // not yet seen for that many frames in a row
        paste(live, seen_[lag_].x, seen_[lag_].y);
    }

    bool shown() const override { return playing_; }

    uint64_t key(uint64_t frame) const override {
        return (uint64_t(serial_) << 24) ^ (uint64_t(area_now_ & 0xFF) << 56) ^
               (uint64_t(player_x_now_ & 0xFFF) << 12) ^ uint64_t(player_y_now_ & 0xFFF) ^
               (has_player() && (frame / 20) & 1 ? 0x800000 : 0);
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
        // "RGMS" 3 scale, then per tile: area, x, y (int32 each) and its pixels (16 bits each); an area's sky is
        // an entry of its own at a place no tile is. (1: the pixels were 32 bits each.)
        bytes = "RGMS";
        bytes += char(3);
        bytes += char(scale_);
        for (const auto& [area, a] : areas_) {
            if (a.sky) {
                const int32_t head[3] = {int32_t(area), INT32_MIN, 0};
                Tile t;
                t.px[0] = pack_px(a.sky);
                bytes.append(reinterpret_cast<const char*>(head), sizeof head);
                bytes.append(reinterpret_cast<const char*>(t.px), sizeof t.px);
            }
            for (const auto& [at, t] : a.tiles) {
                const int32_t head[3] = {int32_t(area), at.first, at.second};
                bytes.append(reinterpret_cast<const char*>(head), sizeof head);
                bytes.append(reinterpret_cast<const char*>(t.px), sizeof t.px);
            }
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        // A map made at another scale is of no use at this one.
        if (bytes.size() < 6 || bytes.compare(0, 4, "RGMS") != 0 || (bytes[4] != 1 && bytes[4] != 3) ||
            bytes[5] != char(scale_))
            return;
        const bool wide = bytes[4] == 1;
        const size_t entry = 12 + sizeof(Tile::px) * (wide ? 2 : 1);
        for (size_t at = 6; at + entry <= bytes.size(); at += entry) {
            int32_t head[3];
            std::memcpy(head, bytes.data() + at, sizeof head);
            Area& a = areas_[head[0]];
            Tile fresh;
            if (wide)
                for (int i = 0; i < tile * tile; ++i) {
                    uint32_t px;
                    std::memcpy(&px, bytes.data() + at + 12 + size_t(i) * 4, 4);
                    fresh.px[i] = px ? pack_px(px) : 0;
                }
            else std::memcpy(fresh.px, bytes.data() + at + 12, sizeof fresh.px);
            if (head[1] == INT32_MIN) {
                a.sky = unpack(fresh.px[0]);
                continue;
            }
            a.tiles[{head[1], head[2]}] = fresh;
            for (int y = 0; y < tile; ++y)
                for (int x = 0; x < tile; ++x)
                    if (fresh.px[y * tile + x]) a.grow(head[1] * tile + x, head[2] * tile + y);
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

    void paste(Core& live, long x, long y) {
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
        // A playfield of one colour is a fade between places (QuackShot's), not a place.
        bool uniform = true;
        const uint32_t first = p.px[size_t(y0) * p.stride + x0] & 0xFFFFFF;
        for (int y = y0; y < y1 && uniform; y += 8)
            for (int x = x0; x < x1; x += 8)
                if ((p.px[size_t(y) * p.stride + x] & 0xFFFFFF) != first) {
                    uniform = false;
                    break;
                }
        if (uniform) return;
        const long wx = x + ox, wy = y + oy; // the world under picture pixel 0, 0
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
                const uint16_t v = pack(r / n, g / n, b / n);
                uint16_t& dst = area.set(mx, my);
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

    static uint32_t md_colour(uint16_t c) {
        const uint32_t r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
        return (r * 255 / 7) | ((g * 255 / 7) << 8) | ((b * 255 / 7) << 16) | 0xFF000000u;
    }

    // The Mega Drive's foreground as its video memory holds it: the plane's tiles where the screen is, then the
    // sprites, each pixel at its place in the world. false: this core does not show its video memory.
    bool paste_plane(Core& live, long cam_x, long cam_y) {
        Core::MdVideo v;
        if (!live.md_video(v)) return false;
        const int sw = (v.reg[12] & 1) ? 320 : 256, sh = (v.reg[1] & 8) ? 240 : 224;
        Rect c = plane_crop_.empty() ? Rect{0, 0, sw, sh} : plane_crop_;
        c.w = std::min(c.w, sw - c.x), c.h = std::min(c.h, sh - c.y);
        if (c.x < 0 || c.y < 0 || c.w < scale_ || c.h < scale_) return true;
        static const int cells[4] = {32, 64, 32, 128};
        const int pw = cells[v.reg[16] & 3] * 8, ph = cells[(v.reg[16] >> 4) & 3] * 8;
        const bool b = plane_ == 2;
        const unsigned names = unsigned(b ? (v.reg[4] & 7) << 12 : (v.reg[2] & 0x38) << 9); // in words
        // The scroll of the first line taken (one for the screen, per 8 lines or per line).
        const int mode = v.reg[11] & 3, line = mode == 3 ? c.y : mode == 2 ? (c.y & ~7) : 0;
        const int hs = v.vram[(((v.reg[13] & 0x3F) << 9) + line * 2 + (b ? 1 : 0)) & 0x7FFF] & 0x3FF;
        const int vs = v.vsram[b ? 1 : 0] & 0x3FF;
        // The plane's pixel under the screen's 0, 0, and the world's: the camera, set right by the scroll (the
        // camera in memory may be a frame ahead of it or behind).
        const int px0 = (pw - hs % pw) % pw, py0 = vs % ph;
        auto settle = [](long cam, int plane, int size) {
            int d = int(((plane - cam) % size + size) % size);
            if (d > size / 2) d -= size;
            return d;
        };
        const int dx = settle(cam_x, px0 + plane_dx_, pw), dy = settle(cam_y, py0 + plane_dy_, ph);
        if (std::abs(dx) > 48 || std::abs(dy) > 48) return true; // not this world's scroll: a cutscene, a jump cut
        const long wx = cam_x + dx, wy = cam_y + dy;             // the world under screen pixel 0, 0
        uint32_t colours[64];
        bool lit = false;
        for (int i = 0; i < 64; ++i) {
            colours[i] = md_colour(v.cram[i]);
            lit = lit || (colours[i] & 0xFFFFFF);
        }
        if (!lit) return true; // faded to black
        // The screen's part of the plane, 0 where it is empty.
        shot_.assign(size_t(sw) * sh, 0);
        for (int y = c.y; y < c.y + c.h; ++y) {
            const int py = (py0 + y) & (ph - 1);
            uint32_t* to = shot_.data() + size_t(y) * sw;
            for (int x = c.x; x < c.x + c.w; ++x) {
                const int px = (px0 + x) & (pw - 1);
                const unsigned cell =
                    v.vram[(names + unsigned(py >> 3) * unsigned(pw >> 3) + unsigned(px >> 3)) & 0x7FFF];
                const int fx = (cell & 0x800) ? 7 - (px & 7) : px & 7, fy = (cell & 0x1000) ? 7 - (py & 7) : py & 7;
                const unsigned word = v.vram[((cell & 0x7FF) * 16 + unsigned(fy) * 2 + unsigned(fx >> 2)) & 0x7FFF];
                const unsigned ink = (word >> (12 - 4 * (fx & 3))) & 15;
                if (ink) to[x] = colours[((cell >> 13) & 3) * 16 + ink];
            }
        }
        // The sprites, the first of the list on top.
        struct Sprite {
            int x, y, w, h;
            unsigned cell;
        } sprites[80];
        int count = 0;
        const unsigned table = unsigned(v.reg[5] & (sw == 320 ? 0x7E : 0x7F)) << 8; // in words
        for (unsigned at = 0, n = 0; n < 80; ++n) {
            const uint16_t* e = v.vram + ((table + at * 4) & 0x7FFF);
            sprites[count++] = {(e[3] & 0x1FF) - 128, (e[0] & 0x3FF) - 128, (((e[1] >> 10) & 3) + 1) * 8,
                                (((e[1] >> 8) & 3) + 1) * 8, e[2]};
            at = e[1] & 0x7F;
            if (!at || at >= 80) break;
        }
        for (int i = count - 1; i >= 0; --i) {
            const Sprite& sp = sprites[i];
            const unsigned first = sp.cell & 0x7FF;
            const uint32_t* pal = colours + ((sp.cell >> 13) & 3) * 16;
            for (int yy = std::max(sp.y, c.y); yy < std::min(sp.y + sp.h, c.y + c.h); ++yy)
                for (int xx = std::max(sp.x, c.x); xx < std::min(sp.x + sp.w, c.x + c.w); ++xx) {
                    const int fx = (sp.cell & 0x800) ? sp.w - 1 - (xx - sp.x) : xx - sp.x;
                    const int fy = (sp.cell & 0x1000) ? sp.h - 1 - (yy - sp.y) : yy - sp.y;
                    // Tiles run down each column of the sprite, then to the next column.
                    const unsigned t = first + unsigned(fx >> 3) * unsigned(sp.h >> 3) + unsigned(fy >> 3);
                    const unsigned word =
                        v.vram[((t & 0x7FF) * 16 + unsigned(fy & 7) * 2 + unsigned((fx & 7) >> 2)) & 0x7FFF];
                    const unsigned ink = (word >> (12 - 4 * (fx & 3))) & 15;
                    if (ink) shot_[size_t(yy) * sw + xx] = pal[ink];
                }
        }
        // Parts of the screen that are not the world: the score and the hero.
        std::vector<Rect> holes = skip_;
        if (has_player())
            holes.push_back({int(player_x_.eval(live) - wx) + player_box_.x,
                             int(player_y_.eval(live) - wy) + player_box_.y, player_box_.w, player_box_.h});
        Area& area = areas_[area_now_];
        bool changed = false;
        // Open air is drawn in the colour the game shows most behind the plane (Green Hill's blue), counted in
        // 4096 shades since the area was entered.
        const Picture p = live.picture();
        if (p.px && p.w == sw && p.h == sh) {
            if (sky_area_ != area_now_ || sky_count_.empty()) sky_count_.assign(4096, 0), sky_area_ = area_now_;
            for (int y = c.y; y < c.y + c.h; y += 4)
                for (int x = c.x; x < c.x + c.w; x += 4)
                    if (!shot_[size_t(y) * sw + x]) {
                        const uint32_t q = p.px[size_t(y) * p.stride + x];
                        ++sky_count_[((q >> 4) & 0xF) | ((q >> 8) & 0xF0) | ((q >> 12) & 0xF00)];
                    }
            const size_t most = size_t(std::max_element(sky_count_.begin(), sky_count_.end()) - sky_count_.begin());
            if (sky_count_[most] >= 64) {
                const uint32_t now = uint32_t((most & 0xF) * 17) | (uint32_t(((most >> 4) & 0xF) * 17) << 8) |
                                     (uint32_t((most >> 8) * 17) << 16) | 0xFF000000u;
                if (now != area.sky) area.sky = now, changed = true;
            }
        }
        const int mx0 = floor_div(wx + c.x + scale_ - 1, scale_), mx1 = floor_div(wx + c.x + c.w, scale_);
        const int my0 = floor_div(wy + c.y + scale_ - 1, scale_), my1 = floor_div(wy + c.y + c.h, scale_);
        const int n = scale_ * scale_;
        for (int my = my0; my < my1; ++my)
            for (int mx = mx0; mx < mx1; ++mx) {
                const int sx = int(long(mx) * scale_ - wx), sy = int(long(my) * scale_ - wy);
                bool hidden = false;
                for (const Rect& h : holes)
                    hidden = hidden || (sx + scale_ > h.x && sx < h.x + h.w && sy + scale_ > h.y && sy < h.y + h.h);
                if (hidden) continue;
                unsigned r = 0, g = 0, bl = 0, solid = 0;
                for (int yy = 0; yy < scale_; ++yy) {
                    const uint32_t* row = shot_.data() + size_t(sy + yy) * sw + sx;
                    for (int xx = 0; xx < scale_; ++xx)
                        if (row[xx])
                            r += row[xx] & 0xFF, g += (row[xx] >> 8) & 0xFF, bl += (row[xx] >> 16) & 0xFF, ++solid;
                }
                const uint16_t value = int(solid) * 2 >= n ? pack(r / solid, g / solid, bl / solid) : air;
                uint16_t& dst = area.set(mx, my);
                if (dst != value) {
                    if (!dst) area.grow(mx, my);
                    dst = value;
                    changed = true;
                }
            }
        view_x_ = mx0;
        view_y_ = my0;
        view_w_ = mx1 - mx0;
        view_h_ = my1 - my0;
        if (changed) ++serial_;
        return true;
    }

    void draw_area(Canvas& out, const Look& look, const Area& a, int top, uint64_t frame) const {
        const int W = out.w(), H = out.h() - top - kit::margin, left = kit::margin, room_w = W - 2 * kit::margin;
        const int bw = a.max_x - a.min_x, bh = a.max_y - a.min_y;
        // The whole area at the largest whole scale that fits; an area too big
        // for the panel at 1:1 follows the player (or the screen) instead.
        int zoom = std::max(1, std::min(room_w / std::max(1, bw), H / std::max(1, bh)));
        zoom = std::clamp(zoom, zoom_, 6);
        const int vw = room_w / zoom, vh = H / zoom; // map pixels shown
        const int fx = has_player() ? player_x_now_ : view_x_ + view_w_ / 2;
        const int fy = has_player() ? player_y_now_ : view_y_ + view_h_ / 2;
        const int ox = bw <= vw ? a.min_x : std::clamp(fx - vw / 2, a.min_x, a.max_x - vw);
        const int oy = bh <= vh ? a.min_y : std::clamp(fy - vh / 2, a.min_y, a.max_y - vh);
        const uint32_t unseen = mix(look.background, look.panel, 128).px();
        const uint32_t sky = a.sky ? a.sky : unseen;
        // A card the size of what is shown, so the map has an edge on the theme's ground.
        const int cw = std::min(bw, vw) * zoom, ch = std::min(bh, vh) * zoom;
        const int cx = left + (room_w - cw) / 2, cy = top + (H - ch) / 2;
        card(out, look, {cx - 6, cy - 6, cw + 12, ch + 12}, false, kit::radius);
        // The map's pixels in the layout's units, then onto the canvas at its scale.
        std::vector<uint32_t> map(size_t(std::max(0, cw)) * size_t(std::max(0, ch)));
        uint32_t* px = map.data();
        // Each stored tile is looked up once per row of tiles: once per panel pixel, a
        // big map cost the handheld more than a whole frame's time.
        const int cols = cw / zoom, rows = ch / zoom;
        const int first = floor_div(ox, tile), last = floor_div(ox + cols - 1, tile);
        std::vector<const Tile*> stored(size_t(std::max(0, last - first + 1)));
        int stored_row = INT_MIN;
        for (int my = 0; my < rows; ++my) {
            const int ty = floor_div(oy + my, tile), ly = oy + my - ty * tile;
            if (ty != stored_row) {
                stored_row = ty;
                for (size_t i = 0; i < stored.size(); ++i) {
                    auto t = a.tiles.find({first + int(i), ty});
                    stored[i] = t == a.tiles.end() ? nullptr : &t->second;
                }
            }
            uint32_t* row = px + size_t(my * zoom) * cw;
            uint32_t* to = row;
            for (int mx = ox; mx < ox + cols;) {
                const int tx = floor_div(mx, tile), lx = mx - tx * tile, n = std::min(tile - lx, ox + cols - mx);
                const Tile* t = stored[size_t(tx - first)];
                for (int i = 0; i < n; ++i) {
                    const uint16_t v = t ? t->px[ly * tile + lx + i] : 0;
                    const uint32_t shown = v == air ? sky : v ? unpack(v) : unseen;
                    for (int z = 0; z < zoom; ++z) *to++ = shown;
                }
                mx += n;
            }
            for (int z = 1; z < zoom; ++z) std::copy(row, row + cw, row + size_t(z) * cw);
        }
        if (cw > 0 && ch > 0) out.blit(px, cw, ch, cx, cy, 1);
        // The screen in view, and the player blinking on it.
        auto to_x = [&](int mx) { return cx + (mx - ox) * zoom; };
        auto to_y = [&](int my) { return cy + (my - oy) * zoom; };
        auto inside = [&](int x, int y) { return x >= cx && y >= cy && x < cx + cw && y < cy + ch; };
        const Rgb edge = look.accent;
        const int t = std::max(1, zoom / 2 + 1);
        const int rx = to_x(view_x_), ry = to_y(view_y_), rw = view_w_ * zoom, rh = view_h_ * zoom;
        // A box's edge kept inside the map's card.
        auto clipped = [&](int x, int y, int w, int h) {
            const int x0 = std::max(x, cx), y0 = std::max(y, cy), x1 = std::min(x + w, cx + cw),
                      y1 = std::min(y + h, cy + ch);
            if (x1 > x0 && y1 > y0) out.fill_rect(x0, y0, x1 - x0, y1 - y0, edge);
        };
        auto hline = [&](int x, int y, int w) { clipped(x, y, w, t); };
        auto vline = [&](int x, int y, int h) { clipped(x, y, t, h); };
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
    Rect crop_, plane_crop_, player_box_{-24, -32, 48, 64};
    std::vector<Rect> skip_;
    int plane_ = 0, plane_dx_ = 0, plane_dy_ = 0; // 1: plane A, 2: plane B
    std::vector<uint32_t> shot_;                  // the screen's part of the plane and the sprites on it
    std::vector<uint32_t> sky_count_;             // how often each shade was behind the plane, in this area
    long sky_area_ = 0;
    int native_w_ = 0, native_h_ = 0, scale_ = 4, every_ = 6, lag_ = 0, zoom_ = 1;
    struct Seen {
        long x = 0, y = 0;
        uint64_t frame = ~uint64_t(0);
    } seen_[4];
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
