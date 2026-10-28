// SPDX-License-Identifier: GPL-3.0-or-later
// format = grid: an automap for a first-person dungeon kept in memory as a
// grid of squares (Phantasy Star's 16x16 floors). Squares are drawn as the
// player sees them: the corridor ahead up to where something stops the view,
// and the squares beside each step of it, as the game draws them. What has
// been seen is kept per floor beside the save.
//
//   cells = CB00            # the floor: one byte a square, row by row
//   size = 16 16            # squares across and down (decimal; default 16 16)
//   position = C30C         # the player's square: y in the high nibble, x in the low
//   facing = C30B           # 0 up, 1 right, 2 down, 3 left (low two bits)
//   level = mem8[C30D]      # one map per value (expr.hpp)
//   type_mask = 07          # the bits of a square that say what it is
//   open = 0                # types: a floor (hex values)
//   wall = 1 7              #        a wall (a fake wall is drawn as one too: a secret stays one)
//   up = 2                  #        stairs up
//   down = 3                #        stairs down
//   door = 4 5 6            #        a door
//   sight = 4               # squares ahead the player sees (decimal)
//   name = Floor            # the chip in the corner: "Floor 3", or name.N per level
#include "companion/companion.hpp"
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace play {
namespace {

enum Kind : uint8_t { Unknown, Open, Wall, Up, Down, Door };

class Grid final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        for (auto [key, out] :
             {std::pair<const char*, uint32_t*>{"cells", &cells_}, {"position", &position_}, {"facing", &facing_}}) {
            auto found = spec.values.find(key);
            if (found == spec.values.end() || !parse_address(found->second, *out)) {
                error = std::string("[view] grid needs an address for ") + key;
                return false;
            }
        }
        if (!view_expr(spec, "level", level_, false, error)) return false;
        for (const auto& [key, value] : spec.values) {
            std::istringstream in(value);
            bool ok = true;
            if (key == "size") {
                ok = bool(in >> w_ >> h_) && w_ > 0 && h_ > 0 && w_ <= 64 && h_ <= 64;
            } else if (key == "sight") {
                ok = bool(in >> sight_) && sight_ >= 1 && sight_ <= 16;
            } else if (key == "type_mask") {
                ok = parse_address(value, mask_);
            } else if (key == "open" || key == "wall" || key == "up" || key == "down" || key == "door") {
                const Kind kind = key == "open"   ? Open
                                  : key == "wall" ? Wall
                                  : key == "up"   ? Up
                                  : key == "down" ? Down
                                                  : Door;
                for (std::string v; in >> v;) {
                    uint32_t n = 0;
                    ok = ok && parse_address(v, n) && n < 256;
                    if (ok) kinds_[n] = kind;
                }
            } else if (key == "name") {
                name_ = value;
            } else if (key.rfind("name.", 0) == 0) {
                names_[std::strtol(key.c_str() + 5, nullptr, 16)] = value;
            }
            if (!ok) {
                error = "[view] grid: bad " + key + " '" + value + "'";
                return false;
            }
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 4) return;
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        level_now_ = level_.eval(live);
        const uint32_t pos = peek_address(live, position_, 1);
        const int px = int(pos & 0x0F), py = int(pos >> 4);
        const int facing = int(peek_address(live, facing_, 1) & 3);
        if (px != x_ || py != y_ || facing != facing_now_) ++moves_;
        x_ = px, y_ = py, facing_now_ = facing;
        // The floor as the game has it now (doors open, chests taken).
        for (int i = 0; i < w_ * h_; ++i) {
            const uint32_t v = peek_address(live, cells_ + uint32_t(i), 1) & mask_;
            auto k = kinds_.find(v);
            const Kind kind = k == kinds_.end() ? Open : k->second;
            if (floor_[size_t(i)] != kind) floor_[size_t(i)] = kind, ++serial_;
        }
        // What the player sees: ahead until something stops the view, and either side of each step.
        static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        auto& seen = seen_[level_now_];
        auto see = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
            const size_t i = size_t(y * w_ + x);
            if (!seen.test(i)) seen.set(i), ++serial_;
        };
        see(px, py);
        see(px - dy[facing], py + dx[facing]); // left and right of where the player stands
        see(px + dy[facing], py - dx[facing]);
        see(px - dx[facing], py - dy[facing]); // behind
        for (int step = 1, x = px, y = py; step <= sight_; ++step) {
            x += dx[facing];
            y += dy[facing];
            if (x < 0 || y < 0 || x >= w_ || y >= h_) break;
            see(x, y);
            see(x - dy[facing], y + dx[facing]);
            see(x + dy[facing], y - dx[facing]);
            if (floor_[size_t(y * w_ + x)] != Open) break;
        }
    }

    bool shown() const override { return playing_; }

    uint64_t key(uint64_t frame) const override {
        return (uint64_t(serial_) << 20) ^ (uint64_t(moves_) << 44) ^ (uint64_t(level_now_ & 0xFF) << 8) ^
               ((frame / 20) & 1);
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        backdrop(out, look);
        const int top = kit::margin * 2 + 28;
        const int cell =
            std::max(4, std::min((out.w() - 2 * kit::margin - 12) / w_, (out.h() - top - kit::margin - 12) / h_));
        const int gw = cell * w_, gh = cell * h_;
        const int gx = (out.w() - gw) / 2, gy = top + (out.h() - top - kit::margin - gh) / 2;
        card(out, look, {gx - 6, gy - 6, gw + 12, gh + 12}, false, kit::radius);
        const Rgb unseen = mix(look.background, look.panel, 128), floor = mix(look.panel, look.background, 60),
                  rock = mix(look.muted, look.ink, 60), door = look.accent, stairs = Rgb{255, 196, 72};
        auto found = seen_.find(level_now_);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const size_t i = size_t(y * w_ + x);
                const int X = gx + x * cell, Y = gy + y * cell;
                if (found == seen_.end() || !found->second.test(i)) {
                    out.fill_rect(X, Y, cell, cell, unseen);
                    continue;
                }
                const Kind k = floor_[i];
                out.fill_rect(X, Y, cell, cell, k == Wall ? rock : floor);
                if (k == Door)
                    out.rounded_rect(X + cell / 6, Y + cell / 6, cell - cell / 3, cell - cell / 3, cell / 6, door);
                if (k == Up || k == Down) {
                    // Three steps, rising toward the top for stairs up, the bottom for down.
                    for (int s = 0; s < 3; ++s) {
                        const int sw = cell * (s + 1) / 4,
                                  sy = k == Up ? Y + cell / 6 + s * cell / 4 : Y + cell - cell / 6 - (s + 1) * cell / 4;
                        out.fill_rect(X + (cell - sw) / 2, sy, sw, std::max(1, cell / 5), stairs);
                    }
                }
            }
        // The grid's lines, faint, so squares read as squares.
        const Rgb line = mix(unseen, look.ink, 24);
        for (int x = 0; x <= w_; ++x) out.fill_rect(gx + x * cell, gy, 1, gh, line);
        for (int y = 0; y <= h_; ++y) out.fill_rect(gx, gy + y * cell, gw, 1, line);
        // The player: a disc with a nose toward where they face.
        static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        const int cx = gx + x_ * cell + cell / 2, cy = gy + y_ * cell + cell / 2, r = std::max(3, cell / 3);
        const Rgb marker = (frame / 20) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64};
        disc(out, cx, cy, r + 2, Rgb{12, 17, 26});
        disc(out, cx, cy, r, marker);
        disc(out, cx + dx[facing_now_] * r, cy + dy[facing_now_] * r, std::max(2, r / 2), Rgb{12, 17, 26});
        const auto n = names_.find(level_now_);
        const std::string name = n != names_.end() ? n->second
                                 : name_.empty()   ? ""
                                                   : name_ + " " + std::to_string(level_now_);
        if (!name.empty()) chip(out, look, font, kit::margin, kit::margin + 14, name, true);
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGGR" 1 w h, then per level: the level (int32) and a bit per square.
        bytes = "RGGR";
        bytes += char(1);
        bytes += char(w_);
        bytes += char(h_);
        for (const auto& [level, seen] : seen_) {
            const int32_t l = int32_t(level);
            bytes.append(reinterpret_cast<const char*>(&l), 4);
            for (int i = 0; i < 64 * 64; i += 8) {
                uint8_t b = 0;
                for (int j = 0; j < 8; ++j) b |= uint8_t(seen.test(size_t(i + j)) << j);
                bytes += char(b);
            }
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        const size_t entry = 4 + 64 * 64 / 8;
        if (bytes.size() < 7 || bytes.compare(0, 4, "RGGR") != 0 || bytes[4] != 1 || bytes[5] != char(w_) ||
            bytes[6] != char(h_))
            return;
        for (size_t at = 7; at + entry <= bytes.size(); at += entry) {
            int32_t level;
            std::memcpy(&level, bytes.data() + at, 4);
            auto& seen = seen_[level];
            for (int i = 0; i < 64 * 64; ++i)
                if ((uint8_t(bytes[at + 4 + size_t(i / 8)]) >> (i % 8)) & 1) seen.set(size_t(i));
        }
        saved_ = ++serial_;
    }

private:
    Condition on_;
    uint32_t cells_ = 0, position_ = 0, facing_ = 0, mask_ = 0xFF;
    Expr level_;
    int w_ = 16, h_ = 16, sight_ = 4;
    std::map<uint32_t, Kind> kinds_;
    std::string name_;
    std::map<long, std::string> names_;
    std::map<long, std::bitset<64 * 64>> seen_;
    Kind floor_[64 * 64] = {};
    bool playing_ = false;
    long level_now_ = 0;
    int x_ = 0, y_ = 0, facing_now_ = 0;
    uint32_t serial_ = 0, moves_ = 0;
    mutable uint32_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_grid(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Grid>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
