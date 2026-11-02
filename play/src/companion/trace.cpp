// SPDX-License-Identifier: GPL-3.0-or-later
// format = trace: a course map drawn from where the player has driven. The
// game keeps no map of its course in memory, only the machine's place on it,
// so the first lap draws the course and every lap after runs on it, the
// player a blinking arrow. One map per course, kept beside the save.
//
//   x = mem16[7E0B70]        # the player's place on the course (expr.hpp)
//   y = mem16[7E0B90]
//   course = mem8[7E00F2] + mem8[7E0053]   # one map per value
//   when = mem8[7E005B] == 00              # traced only while this holds (racing, not a replay)
//   name = Course                          # the chip: "Course 3", or name.N per course
//   wrap_x = 1000                          # x and y count round (hex periods): the course may cross
//   wrap_y = 2000                          # where they wrap, so the trace unwraps them as it goes
//   width = 60                             # the road's width in the game's units (hex)
// A jump (a race starting or restarting, a replay) puts the machine back on
// the course already drawn, at whichever wrap of it is nearest.
//
// A game that keeps its course's path for the computer's drivers (Mario Kart
// 64) gives the whole road at once, the start line where the path begins:
//   path = mem32[164550] & 7FFFFF          # where its first point is (expr.hpp)
//   path_count = mem16[1645C8]             # how many points
//   path_left = mem32[164560] & 7FFFFF     # optional: the road's edges, point for point,
//   path_right = mem32[164570] & 7FFFFF    # for its width (else `width`)
//   path_stride = 8                        # bytes a point (decimal); signed 16-bit x at
//   path_x = 0                             # path_x and y at path_y (hex offsets), the
//   path_y = 4                             # same units as x and y
// Driving then paints nothing: a shortcut over the grass is not the road.
//
// The other racers, if the game keeps them in a table, go on the map with the
// standings beside it (on a course with a path: an arena has no places):
//   racers = 0F6990 DD8 8                  # the table, the bytes a racer, how many; the first is the player
//   racer_exists = 0 8000                  # offset and mask of what says the slot is in use (16-bit)
//   racer_x = 14                           # float x and y, the same units as x and y
//   racer_y = 1C
//   racer_rank = 4                         # 16-bit: the place from 0
//   racer_lap = 8                          # 16-bit: the laps done, and how many a race has
//   laps = 3
//   racer_kind = 254                       # 16-bit: who drives, for the face
// Their faces, as the game's own standings draw them, come from the ROM: a
// MIO0 block of 8-bit textures with 16-bit palettes (RGBA 5551), offsets into
// the block once unpacked, per kind. Without them a racer is a dot.
//   faces = 132B50 32 32                   # the block in the ROM, each face's width and height
//   face.0 = 1A4D8 1B6D8                   # kind 0's palette and texture
//
// The road is painted, not dotted: each stretch driven is a stroke of the
// road's width into a grid of small squares, so a second lap on a different
// line widens the road a little instead of drawing a second squiggle. The
// squares are drawn softened, the edges smooth whatever the scale.
#include "companion/companion.hpp"
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace play {
namespace {

struct Point {
    int16_t x, y;
};

// The painted squares of one course, in squares of `cell` units.
struct Road {
    std::unordered_set<uint32_t> cells;
    int lo_x = INT_MAX, hi_x = INT_MIN, lo_y = INT_MAX, hi_y = INT_MIN;
    int strokes = 0; // stretches painted since the course was started here (a loaded one counts as many)
    long path_count = 0; // the path's points the road was painted from (0: painted by driving)
    bool has_start = false;
    int start_x = 0, start_y = 0, start_dx = 0, start_dy = 0, start_w = 0; // the start line: where, which way, how wide
    static uint32_t pack(int x, int y) { return (uint32_t(uint16_t(x)) << 16) | uint16_t(y); }
    static int unpack_x(uint32_t c) { return int16_t(c >> 16); }
    static int unpack_y(uint32_t c) { return int16_t(c & 0xFFFF); }
    bool add(int x, int y) {
        if (!cells.insert(pack(x, y)).second) return false;
        lo_x = std::min(lo_x, x), hi_x = std::max(hi_x, x), lo_y = std::min(lo_y, y), hi_y = std::max(hi_y, y);
        return true;
    }
};

int floor_div(long v, int d) { return int(v >= 0 ? v / d : -((-v + d - 1) / d)); }

uint32_t big32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A MIO0 block: a bit per step (1: a byte as it is, 0: a copy from what is already out), the copies' and the bytes'
// tables at the offsets its header gives. Empty when the block is not one or runs off its end.
std::vector<uint8_t> unpack_mio0(const std::vector<uint8_t>& in) {
    if (in.size() < 16 || std::memcmp(in.data(), "MIO0", 4) != 0) return {};
    const size_t size = big32(&in[4]);
    size_t copies = big32(&in[8]), bytes = big32(&in[12]), step = 0;
    if (size > 0x800000) return {};
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size) {
        const size_t flag = 16 + step / 8;
        if (flag >= in.size()) return {};
        if ((in[flag] >> (7 - step % 8)) & 1) {
            if (bytes >= in.size()) return {};
            out.push_back(in[bytes++]);
        } else {
            if (copies + 1 >= in.size()) return {};
            const int word = in[copies] << 8 | in[copies + 1];
            copies += 2;
            const size_t back = size_t(word & 0xFFF) + 1;
            if (back > out.size()) return {};
            for (int n = (word >> 12) + 3; n > 0; --n) out.push_back(out[out.size() - back]);
        }
        ++step;
    }
    out.resize(size);
    return out;
}

// A face, in the canvas's pixels (alpha 0 where clear).
struct Face {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool empty() const { return px.empty(); }
};

// Drawn with its clear pixels left out, whatever is under them.
void stamp(Canvas& out, const Face& face, int left, int top) {
    for (int y = 0; y < face.h; ++y) {
        const int oy = top + y;
        if (oy < 0 || oy >= out.h()) continue;
        for (int x = 0; x < face.w; ++x) {
            const int ox = left + x;
            const uint32_t c = face.px[size_t(y) * face.w + x];
            if (c >> 24 && ox >= 0 && ox < out.w()) out.data()[size_t(oy) * out.w() + ox] = c;
        }
    }
}

// Smaller by whole-area means, each output pixel what lies under it: the rival's face on a crowded map.
Face shrunk(const Face& big, int w, int h) {
    Face small;
    small.w = w, small.h = h;
    small.px.assign(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t r = 0, g = 0, b = 0, solid = 0, all = 0;
            for (int v = y * big.h / h; v < std::max(y * big.h / h + 1, (y + 1) * big.h / h); ++v)
                for (int u = x * big.w / w; u < std::max(x * big.w / w + 1, (x + 1) * big.w / w); ++u, ++all) {
                    const uint32_t c = big.px[size_t(v) * big.w + u];
                    if (c >> 24) r += c & 0xFF, g += (c >> 8) & 0xFF, b += (c >> 16) & 0xFF, ++solid;
                }
            if (solid * 2 >= all && solid) small.px[size_t(y) * w + x] = 0xFF000000u | (r / solid) | ((g / solid) << 8) | ((b / solid) << 16);
        }
    return small;
}

class Trace final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "x", x_, true, error) || !view_expr(spec, "y", y_, true, error) ||
            !view_expr(spec, "course", course_, false, error) || !view_expr(spec, "path", path_, false, error) ||
            !view_expr(spec, "path_count", path_count_, false, error) ||
            !view_expr(spec, "path_left", path_left_, false, error) ||
            !view_expr(spec, "path_right", path_right_, false, error))
            return false;
        for (const auto& [key, value] : spec.values) {
            if (key == "when") {
                std::string why;
                if (!parse_condition(value, when_, why)) {
                    error = "[view] trace: bad when '" + value + "': " + why;
                    return false;
                }
            } else if (key == "wrap_x" || key == "wrap_y") {
                (key == "wrap_x" ? wrap_x_ : wrap_y_) = std::strtol(value.c_str(), nullptr, 16);
            } else if (key == "width") {
                width_ = int(std::strtol(value.c_str(), nullptr, 16));
                if (width_ < 4 || width_ > 0x1000) {
                    error = "[view] trace: bad width '" + value + "'";
                    return false;
                }
            } else if (key == "path_stride" || key == "path_x" || key == "path_y") {
                const long v = std::strtol(value.c_str(), nullptr, key == "path_stride" ? 10 : 16);
                if (v < 0 || v > 256) {
                    error = "[view] trace: bad " + key + " '" + value + "'";
                    return false;
                }
                (key == "path_stride" ? path_stride_ : key == "path_x" ? path_dx_ : path_dy_) = int(v);
            } else if (key == "racers") {
                std::istringstream words(value);
                std::string base, step;
                words >> base >> step >> racer_count_;
                if (!parse_address(base, racer_base_) || !parse_address(step, racer_step_) || racer_count_ < 1 ||
                    racer_count_ > 16) {
                    error = "[view] trace: bad racers '" + value + "'";
                    return false;
                }
            } else if (key == "racer_exists") {
                std::istringstream words(value);
                std::string at, mask;
                words >> at >> mask;
                if (!parse_address(at, racer_exists_at_) || !parse_address(mask, racer_exists_mask_)) {
                    error = "[view] trace: bad racer_exists '" + value + "'";
                    return false;
                }
            } else if (key == "racer_x" || key == "racer_y" || key == "racer_rank" || key == "racer_lap" ||
                       key == "racer_kind") {
                uint32_t& at = key == "racer_x"    ? racer_x_
                               : key == "racer_y"  ? racer_y_
                               : key == "racer_rank" ? racer_rank_
                               : key == "racer_lap"  ? racer_lap_
                                                     : racer_kind_;
                if (!parse_address(value, at)) {
                    error = "[view] trace: bad " + key + " '" + value + "'";
                    return false;
                }
            } else if (key == "laps") {
                laps_ = std::atoi(value.c_str());
            } else if (key == "faces") {
                std::istringstream words(value);
                std::string at;
                words >> at >> face_w_ >> face_h_;
                if (!parse_address(at, faces_at_) || face_w_ < 8 || face_w_ > 64 || face_h_ < 8 || face_h_ > 64) {
                    error = "[view] trace: bad faces '" + value + "'";
                    return false;
                }
            } else if (key.rfind("face.", 0) == 0) {
                std::istringstream words(value);
                std::string palette, texture;
                words >> palette >> texture;
                uint32_t p = 0, t = 0;
                if (!parse_address(palette, p) || !parse_address(texture, t)) {
                    error = "[view] trace: bad " + key + " '" + value + "'";
                    return false;
                }
                face_places_[int(std::strtol(key.c_str() + 5, nullptr, 16))] = {p, t};
            } else if (key == "name") {
                name_ = value;
            } else if (key.rfind("name.", 0) == 0) {
                names_[std::strtol(key.c_str() + 5, nullptr, 16)] = value;
            }
        }
        if (!path_.empty() && path_count_.empty()) {
            error = "[view] trace: path needs path_count";
            return false;
        }
        // Squares an eighth of the road across: round enough a stroke, few enough to paint.
        cell_ = std::max(1, width_ / 8);
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 2) return;
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        const long course = course_.eval(live);
        Road& road = courses_[course];
        if (!path_.empty() && frame % 30 == 0) read_path(live, road); // twice a second: a new course's comes soon
        if (racer_count_ && frame % 4 == 0) read_racers(live);
        long x = x_.eval(live) + off_x_, y = y_.eval(live) + off_y_;
        // Unwrapped: the step from the last sample is the short way round.
        auto near = [](long& v, long& off, long last, long period) {
            if (period <= 0) return;
            for (; v - last > period / 2; v -= period) off -= period;
            for (; v - last < -period / 2; v += period) off += period;
        };
        if (sampled_ && course == course_now_) near(x, off_x_, px_, wrap_x_), near(y, off_y_, py_, wrap_y_);
        if (!sampled_ || course != course_now_ || std::abs(x - px_) + std::abs(y - py_) > 256) {
            // A jump: the wrap of the course already drawn that is nearest.
            x -= off_x_, y -= off_y_;
            long best = LONG_MAX, bx = 0, by = 0;
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j) {
                    const long ox = wrap_x_ * i, oy = wrap_y_ * j;
                    long d = road.cells.empty() ? std::abs(ox) + std::abs(oy) : LONG_MAX; // no course yet: as it comes
                    for (uint32_t c : road.cells)
                        d = std::min(d, std::abs(Road::unpack_x(c) * long(cell_) + cell_ / 2 - (x + ox)) +
                                            std::abs(Road::unpack_y(c) * long(cell_) + cell_ / 2 - (y + oy)));
                    if (d < best) best = d, bx = ox, by = oy;
                }
            off_x_ = bx, off_y_ = by;
            x += bx, y += by;
            // A few strokes nowhere near the rest: where the race was before it started.
            if (best > 256 && road.strokes < 8 && !road.cells.empty()) road = Road{}, ++serial_;
            pen_down_ = false;
        }
        if ((x >> 2) != (px_ >> 2) || (y >> 2) != (py_ >> 2) || course != course_now_) ++moves_;
        if (course != course_now_) heading_x_ = heading_y_ = 0, hx_ = x, hy_ = y;
        // Which way the machine points: where it is going, over the last stretch.
        if (std::abs(x - hx_) + std::abs(y - hy_) > 40) heading_x_ = int(x - hx_), heading_y_ = int(y - hy_), hx_ = x, hy_ = y;
        course_now_ = course;
        px_ = x, py_ = y;
        sampled_ = true;
        const bool tracing = (when_.empty() || when_.match(live)) && std::abs(x) < 0x40000 && std::abs(y) < 0x40000 &&
                             !road.path_count;
        if (!tracing) {
            pen_down_ = false;
        } else if (!pen_down_) {
            pen_down_ = true, sx_ = x, sy_ = y; // painted once it moves: a lone blob where a race restarted is none
        } else if (std::abs(x - sx_) + std::abs(y - sy_) >= cell_) {
            if (stroke(road, sx_, sy_, x, y)) ++serial_;
            ++road.strokes;
            sx_ = x, sy_ = y;
        }
        // The picture of the road is redrawn twice a second while it grows, not with every square.
        if (serial_ != layer_serial_ && (frame >= layer_frame_ + 30 || frame < layer_frame_ || road.strokes < 2))
            layer_serial_ = serial_, layer_frame_ = frame;
    }

    bool shown() const override { return playing_; }

    uint64_t key(uint64_t frame) const override {
        // With rivals on it the map moves all the time: drawn as they are read, 15 times a second.
        return (uint64_t(layer_serial_) << 28) ^ (uint64_t(racer_count_ ? frame / 4 : moves_) << 2) ^
               (uint64_t(course_now_ & 0xFF) << 56) ^ ((frame / 16) & 1);
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        const auto found = courses_.find(course_now_);
        const bool drawn = found != courses_.end() && found->second.cells.size() > 1;
        // Everything but the player is the same until the road grows: kept, and copied.
        const uint64_t want = (uint64_t(layer_serial_) << 32) ^ (uint64_t(course_now_ & 0xFFFF) << 16) ^
                              (uint64_t(out.w()) << 8) ^ uint64_t(out.h()) ^ colour_key(look) ^ (drawn ? 1ull << 63 : 0);
        if (want != layer_key_ || layer_.w() != out.w() || layer_.h() != out.h()) {
            layer_.resize(out.w(), out.h());
            paint_layer(layer_, look, font, drawn ? &found->second : nullptr);
            layer_key_ = want;
        }
        std::memcpy(out.data(), layer_.data(), size_t(out.w()) * size_t(out.h()) * sizeof(uint32_t));
        if (!drawn) return;
        const bool places = standings(&found->second);
        const int map_right = places ? column_x(out) - kit::gap : out.w();
        // The rivals, last place first so the leaders stay on top; the player over them all, below.
        std::vector<const Racer*> order;
        for (size_t i = 0; i < racers_.size(); ++i)
            if (racers_[i].there) order.push_back(&racers_[i]);
        std::stable_sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) { return a->place > b->place; });
        for (const Racer* racer : order) {
            if (racer == &racers_[0]) continue;
            const Face* face = face_of(*racer, true);
            const int half = face ? face->w / 2 + 1 : 8;
            const int mx = std::clamp(int(ox_ + (racer->x - wx0_) * s_), half + 2, map_right - half - 2);
            const int my = std::clamp(int(oy_ + (racer->y - wy0_) * s_), half + 2, out.h() - half - 2);
            if (face) {
                out.rounded_rect(mx - half, my - half, half * 2, half * 2, 4, Rgb{12, 17, 26});
                stamp(out, *face, mx - face->w / 2, my - face->h / 2);
            } else {
                disc(out, mx, my, 8, Rgb{12, 17, 26});
                disc(out, mx, my, 6, look.ink);
            }
        }
        if (places) draw_standings(out, look, font, order);
        const Face* you = racers_.empty() || !racers_[0].there ? nullptr : face_of(racers_[0], false);
        if (you) {
            // The player's own face, larger, on a ring in the accent with the nose toward where it is going.
            const int half = you->w / 2 + 3;
            const int mx = std::clamp(int(ox_ + (px_ - wx0_) * s_), half + 2, map_right - half - 2);
            const int my = std::clamp(int(oy_ + (py_ - wy0_) * s_), half + 2, out.h() - half - 2);
            const double len = std::hypot(double(heading_x_), double(heading_y_));
            const Rgb lit = (frame / 16) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64}, dark{12, 17, 26};
            if (len > 0) {
                const double ux = heading_x_ / len, uy = heading_y_ / len;
                disc(out, mx + int(ux * (half + 6)), my + int(uy * (half + 6)), 8, dark);
                disc(out, mx + int(ux * (half + 6)), my + int(uy * (half + 6)), 6, lit);
            }
            out.rounded_rect(mx - half - 1, my - half - 1, half * 2 + 2, half * 2 + 2, 7, dark);
            out.rounded_rect(mx - half, my - half, half * 2, half * 2, 6, lit);
            stamp(out, *you, mx - you->w / 2, my - you->h / 2);
            return;
        }
        // The player: a disc with a nose toward where it is going, kept inside the map.
        const int r = std::max(4, out.w() / 80);
        const int mx = std::clamp(int(ox_ + (px_ - wx0_) * s_), r + 4, out.w() - r - 4);
        const int my = std::clamp(int(oy_ + (py_ - wy0_) * s_), r + 4, out.h() - r - 4);
        const Rgb marker = (frame / 16) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64}, dark{12, 17, 26};
        disc(out, mx, my, r + 3, dark);
        disc(out, mx, my, r + 1, marker);
        const double len = std::hypot(double(heading_x_), double(heading_y_));
        if (len > 0) {
            const double ux = heading_x_ / len, uy = heading_y_ / len;
            disc(out, mx + int(ux * (r + 5)), my + int(uy * (r + 5)), std::max(2, r / 2) + 1, dark);
            disc(out, mx + int(ux * (r + 5)), my + int(uy * (r + 5)), std::max(2, r / 2), marker);
        }
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGTR" 3, the square's size, then per course: the course, the square count
        // (int32 each) and the squares (int16 x, y). 2 kept points, 1 wrapped points.
        bytes = "RGTR";
        bytes += char(3);
        const int32_t size = cell_;
        bytes.append(reinterpret_cast<const char*>(&size), 4);
        for (const auto& [course, road] : courses_) {
            if (road.cells.empty()) continue;
            const int32_t head[2] = {int32_t(course), int32_t(road.cells.size())};
            bytes.append(reinterpret_cast<const char*>(head), sizeof head);
            for (uint32_t c : road.cells) {
                const int16_t p[2] = {int16_t(Road::unpack_x(c)), int16_t(Road::unpack_y(c))};
                bytes.append(reinterpret_cast<const char*>(p), sizeof p);
            }
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        if (bytes.size() < 5 || bytes.compare(0, 4, "RGTR") != 0) return;
        const int version = bytes[4];
        if (version != 2 && version != 3) return;
        size_t at = 5;
        int32_t size = cell_;
        if (version == 3) {
            if (bytes.size() < 9) return;
            std::memcpy(&size, bytes.data() + 5, 4);
            at = 9;
            if (size < 1) return;
        }
        while (at + 8 <= bytes.size()) {
            int32_t head[2];
            std::memcpy(head, bytes.data() + at, sizeof head);
            at += 8;
            const size_t n = size_t(std::max(0, head[1]));
            if (at + n * sizeof(Point) > bytes.size()) break;
            std::vector<Point> points(n);
            std::memcpy(points.data(), bytes.data() + at, n * sizeof(Point));
            at += n * sizeof(Point);
            Road& road = courses_[head[0]];
            if (version == 3) {
                // Squares of this size: as they are; of another (a profile's width changed): their middles repainted.
                for (const Point& p : points) {
                    if (size == cell_) road.add(p.x, p.y);
                    else road.add(floor_div(p.x * long(size) + size / 2, cell_), floor_div(p.y * long(size) + size / 2, cell_));
                }
            } else {
                // Points a few lengths apart, one lap after another: painted as strokes where they follow on.
                for (size_t i = 0; i < n; ++i) {
                    const Point& a = i ? points[i - 1] : points[i];
                    const Point& b = points[i];
                    const bool follows = std::abs(a.x - b.x) + std::abs(a.y - b.y) < 96;
                    stroke(road, follows ? a.x : b.x, follows ? a.y : b.y, b.x, b.y);
                }
            }
            road.strokes = INT_MAX / 2;
        }
        saved_ = ++serial_;
        layer_serial_ = serial_;
    }

private:
    struct Racer {
        bool there = false;
        double x = 0, y = 0;
        int place = 0, lap = 0, kind = -1;
    };

    // Where the table says everyone is. A float that is no place (the table not filled in yet) hides that racer.
    void read_racers(Core& live) {
        racers_.resize(size_t(racer_count_));
        if (!faces_read_) read_faces(live);
        for (int i = 0; i < racer_count_; ++i) {
            const uint32_t at = racer_base_ + racer_step_ * uint32_t(i);
            Racer& r = racers_[size_t(i)];
            r.there = !racer_exists_mask_ || (peek_address(live, at + racer_exists_at_, 2) & racer_exists_mask_);
            const uint32_t fx = peek_address(live, at + racer_x_, 4), fy = peek_address(live, at + racer_y_, 4);
            float x, y;
            std::memcpy(&x, &fx, 4), std::memcpy(&y, &fy, 4);
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 0x40000 || std::abs(y) > 0x40000) r.there = false;
            r.x = x, r.y = y;
            r.place = std::clamp(int(int16_t(peek_address(live, at + racer_rank_, 2))) + 1, 1, racer_count_);
            r.lap = std::clamp(int(int16_t(peek_address(live, at + racer_lap_, 2))) + 1, 1, laps_ ? laps_ : 99);
            r.kind = int(peek_address(live, at + racer_kind_, 2));
        }
    }

    // The faces, once: unpacked from the ROM and turned into pixels, the block then let go.
    void read_faces(Core& live) {
        faces_read_ = true;
        if (face_places_.empty()) return;
        std::vector<uint8_t> packed;
        if (!live.read_rom(faces_at_, 0x80000, packed)) return;
        const std::vector<uint8_t> block = unpack_mio0(packed);
        for (const auto& [kind, where] : face_places_) {
            const size_t palette = where.first, texture = where.second, texels = size_t(face_w_) * face_h_;
            if (palette + 512 > block.size() || texture + texels > block.size()) continue;
            Face face;
            face.w = face_w_, face.h = face_h_;
            face.px.resize(texels);
            for (size_t i = 0; i < texels; ++i) {
                const uint8_t* entry = &block[palette + size_t(block[texture + i]) * 2];
                const int c = entry[0] << 8 | entry[1];
                const uint32_t r = uint32_t((c >> 11) & 31) * 255 / 31, g = uint32_t((c >> 6) & 31) * 255 / 31,
                               b = uint32_t((c >> 1) & 31) * 255 / 31;
                face.px[i] = c & 1 ? 0xFF000000u | r | (g << 8) | (b << 16) : 0;
            }
            small_faces_[kind] = shrunk(face, face_w_ * 3 / 4, face_h_ * 3 / 4);
            faces_[kind] = std::move(face);
        }
    }

    const Face* face_of(const Racer& racer, bool small) const {
        const auto& from = small ? small_faces_ : faces_;
        const auto found = from.find(racer.kind);
        return found == from.end() ? nullptr : &found->second;
    }

    // A race's course (it has a path) and a table of racers: places to show. An arena has neither laps nor places.
    bool standings(const Road* road) const { return road && road->path_count && racer_count_; }
    static int column_x(const Canvas& out) { return out.w() - kit::gap - column_w; }

    // The player's lap, then everyone by place, clear of the dock's corner.
    void draw_standings(Canvas& out, const Look& look, const Font& font, const std::vector<const Racer*>& worst_first) const {
        const int x = column_x(out);
        int y = kit::gap;
        if (racers_[0].there) {
            std::string lap = look.tr("Lap") + " " + std::to_string(racers_[0].lap);
            if (laps_) lap += "/" + std::to_string(laps_);
            card(out, look, {x, y, column_w, 40});
            font.draw(out, x + 14, font.centred(kit::body, y + 20), lap, kit::body, look.ink);
            y += 40 + kit::gap / 2;
        }
        if (worst_first.empty()) return;
        const int room = out.h() - look.reserve_h - kit::gap - y;
        const int row = std::min(44, room / int(worst_first.size()));
        for (auto it = worst_first.rbegin(); it != worst_first.rend(); ++it) {
            const Racer& racer = **it;
            const bool you = &racer == &racers_[0];
            const Box line{x, y, column_w, row - 4};
            if (you) out.rounded_rect(line.x, line.y, line.w, line.h, kit::radius, look.accent);
            else card(out, look, line);
            font.draw(out, line.x + 14, font.centred(kit::body, line.y + line.h / 2), std::to_string(racer.place),
                      kit::body, you ? look.on_accent : look.ink);
            const Face* face = face_of(racer, line.h < face_h_ + 2);
            if (face) stamp(out, *face, line.x + line.w - face->w - 8, line.y + (line.h - face->h) / 2);
            y += row;
        }
    }

    // The course from the game's own path, once a course (again if its length changes): each stretch between two
    // points painted as wide as the edges are apart there, or `width`.
    // While a course loads, memory still holds the last one's path: taken only once the player is on it.
    void read_path(Core& live, Road& road) {
        const long count = path_count_.eval(live);
        if (count < 8 || count > 8192 || count == road.path_count) return;
        const long at = path_.eval(live), left = path_left_.empty() ? -1 : path_left_.eval(live),
                   right = path_right_.empty() ? -1 : path_right_.eval(live);
        if (at <= 0) return;
        auto point = [&](long base, long i, long& x, long& y) {
            const uint32_t a = uint32_t(base + i * path_stride_);
            x = int16_t(peek_address(live, a + uint32_t(path_dx_), 2));
            y = int16_t(peek_address(live, a + uint32_t(path_dy_), 2));
        };
        Road fresh;
        long px = 0, py = 0, nearest = LONG_MAX;
        const long player_x = x_.eval(live), player_y = y_.eval(live);
        point(at, count - 1, px, py);
        for (long i = 0; i < count; ++i) {
            long x, y;
            point(at, i, x, y);
            nearest = std::min(nearest, std::abs(x - player_x) + std::abs(y - player_y));
            double width = 0;
            if (left > 0 && right > 0) {
                long lx, ly, rx, ry;
                point(left, i, lx, ly);
                point(right, i, rx, ry);
                width = std::hypot(double(lx - rx), double(ly - ry));
                if (width > width_ * 8) width = 0; // an edge that isn't one
            }
            // A step much longer than its neighbours is a jump in the path (a shortcut's other end): not painted.
            if (std::abs(x - px) + std::abs(y - py) < 512) stroke(fresh, px, py, x, y, std::max<double>(width, cell_ * 2));
            if (i == 0) fresh.has_start = true, fresh.start_x = int(x), fresh.start_y = int(y), fresh.start_w = int(width);
            if (i == 1) fresh.start_dx = int(x - fresh.start_x), fresh.start_dy = int(y - fresh.start_y);
            px = x, py = y;
        }
        if (fresh.cells.empty() || nearest > std::max<long>(width_ * 6, 256)) return;
        fresh.path_count = count;
        fresh.strokes = INT_MAX / 2;
        road = std::move(fresh);
        ++serial_;
        layer_serial_ = serial_;
    }

    // Paints the road's width along a stretch; true when a square was new.
    bool stroke(Road& road, long x0, long y0, long x1, long y1, double width = 0) const {
        const double r = (width > 0 ? width : width_) / 2.0 / cell_; // the brush's radius in squares
        const int reach = int(std::ceil(r));
        const long length = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        const int steps = std::max(1, int(length / std::max(1, cell_)));
        bool grew = false;
        for (int i = 0; i <= steps; ++i) {
            const double fx = (x0 + (x1 - x0) * double(i) / steps) / cell_, fy = (y0 + (y1 - y0) * double(i) / steps) / cell_;
            const int cx = int(std::floor(fx)), cy = int(std::floor(fy));
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx) {
                    const double ddx = cx + dx + .5 - fx, ddy = cy + dy + .5 - fy;
                    if (ddx * ddx + ddy * ddy <= r * r) grew |= road.add(cx + dx, cy + dy);
                }
        }
        return grew;
    }

    static uint64_t colour_key(const Look& look) {
        auto rgb = [](Rgb c) { return uint64_t(c.r) << 16 | uint64_t(c.g) << 8 | c.b; };
        return (rgb(look.background) * 31 + rgb(look.panel)) * 31 + rgb(look.ink) * 7 + rgb(look.muted) +
               (look.grid ? 0x9E3779B9ull : 0);
    }

    void paint_layer(Canvas& out, const Look& look, const Font& font, const Road* road) const {
        backdrop(out, look);
        const auto n = names_.find(course_now_);
        const std::string name = n != names_.end() ? n->second
                                 : name_.empty()   ? ""
                                                   : name_ + " " + std::to_string(course_now_ + 1);
        if (!name.empty()) chip(out, look, font, kit::margin, kit::margin + 14, name, true);
        if (!road) {
            // Under the name, so a long translation still fits the row.
            chip(out, look, font, kit::margin, kit::margin + 14 + (name.empty() ? 0 : 44),
                 look.tr("The course draws itself on the first lap"), false);
            return;
        }
        // The course fitted to the room under the chips, its shape kept.
        wx0_ = double(road->lo_x) * cell_, wy0_ = double(road->lo_y) * cell_;
        const double ww = double(road->hi_x - road->lo_x + 1) * cell_, wh = double(road->hi_y - road->lo_y + 1) * cell_;
        const int top = kit::margin * 2 + 28, room_w = (standings(road) ? column_x(out) : out.w()) - 2 * kit::margin - 24,
                  room_h = out.h() - top - kit::margin - 24;
        s_ = std::min(room_w / ww, room_h / wh);
        ox_ = kit::margin + 12 + (room_w - ww * s_) / 2;
        oy_ = top + 12 + (room_h - wh * s_) / 2;
        // A long course shrinks its road to a thread: painted wider, never under a finger's width.
        const double cell_px = cell_ * s_, min_px = std::max(12, out.w() / 45);
        const int grow = std::clamp(int(std::ceil((min_px - width_ * s_) / 2 / cell_px)), 0, 12);
        // The squares, softened: a tent blur two squares wide each way, read
        // back between squares, so the road's edge is where it is half painted.
        const int pad = 3 + grow, gw = road->hi_x - road->lo_x + 1 + 2 * pad, gh = road->hi_y - road->lo_y + 1 + 2 * pad;
        std::vector<float> field(size_t(gw) * gh, 0.f), tmp(field.size(), 0.f);
        for (uint32_t c : road->cells) {
            const int cx = Road::unpack_x(c) - road->lo_x + pad, cy = Road::unpack_y(c) - road->lo_y + pad;
            for (int dy = -grow; dy <= grow; ++dy)
                for (int dx = -grow; dx <= grow; ++dx)
                    if (dx * dx + dy * dy <= grow * grow + grow) field[size_t(cy + dy) * gw + size_t(cx + dx)] = 1.f;
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    float sum = 0;
                    for (int d = -1; d <= 1; ++d) sum += field[size_t(y) * gw + size_t(std::clamp(x + d, 0, gw - 1))];
                    tmp[size_t(y) * gw + x] = sum / 3;
                }
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    float sum = 0;
                    for (int d = -1; d <= 1; ++d) sum += tmp[size_t(std::clamp(y + d, 0, gh - 1)) * gw + x];
                    field[size_t(y) * gw + x] = sum / 3;
                }
        }
        // A kerb in the accent, the road a grey between panel and ink, its middle a shade lighter.
        const Rgb edge = mix(look.accent, look.ink, look.dark ? 40 : 70),
                  tarmac = mix(look.panel, look.ink, look.dark ? 70 : 110), lane = mix(tarmac, look.panel, 90);
        auto sample = [&](double u, double v) {
            u = std::clamp(u, 0.0, gw - 1.001), v = std::clamp(v, 0.0, gh - 1.001);
            const int i = int(u), j = int(v);
            const double fu = u - i, fv = v - j;
            const float* row = &field[size_t(j) * gw];
            const double a = row[i] + (row[i + 1] - row[i]) * fu, b = row[gw + i] + (row[gw + i + 1] - row[gw + i]) * fu;
            return a + (b - a) * fv;
        };
        auto smooth = [](double lo, double hi, double v) {
            const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
            return t * t * (3 - 2 * t);
        };
        // Wide enough on screen that the edge reads as a kerb.
        const double feather = std::clamp(0.6 / cell_px, 0.04, 0.12); // never down to empty ground: no grey box
        const int x0 = std::max(0, int(ox_ - pad * cell_ * s_)), x1 = std::min(out.w(), int(ox_ + (ww + pad * cell_) * s_) + 1);
        const int y0 = std::max(0, int(oy_ - pad * cell_ * s_)), y1 = std::min(out.h(), int(oy_ + (wh + pad * cell_) * s_) + 4);
        // A soft shadow a few pixels under the road, as the launcher lifts its cards.
        const Rgb shade = look.shadow();
        const double drop = 3 / cell_px;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const double u = (x + .5 - ox_) / cell_px + pad - .5, v = (y + .5 - oy_) / cell_px + pad - .5;
                const double below = sample(u, v - drop);
                if (below > 0.1) out.fill_rect(x, y, 1, 1, shade, int((look.dark ? 90 : 46) * smooth(0.1, 0.4, below)));
            }
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const double u = (x + .5 - ox_) / cell_px + pad - .5, v = (y + .5 - oy_) / cell_px + pad - .5;
                const double f = sample(u, v);
                if (f < 0.17 - feather) continue;
                const int a_edge = int(255 * smooth(0.17 - feather, 0.17 + feather, f));
                const int a_road = int(255 * smooth(0.40 - feather, 0.40 + feather, f));
                const int a_lane = int(255 * smooth(0.93 - feather / 2, 0.93 + feather / 2, f));
                out.fill_rect(x, y, 1, 1, edge, a_edge);
                if (a_road) out.fill_rect(x, y, 1, 1, tarmac, a_road);
                if (a_lane) out.fill_rect(x, y, 1, 1, lane, a_lane / 2);
            }
        // The start line: a chequered bar across the road where the path begins.
        const double len = std::hypot(double(road->start_dx), double(road->start_dy));
        if (road->has_start && len > 0) {
            const double ux = road->start_dx / len, uy = road->start_dy / len; // the way the course runs
            const double cx = ox_ + (road->start_x - wx0_) * s_, cy = oy_ + (road->start_y - wy0_) * s_;
            // As wide as the painted road: the brush, and what a thin road is grown by on screen.
            const double road_px = std::max((road->start_w > 0 ? road->start_w : width_) * s_, min_px);
            const double half = road_px / 2 + 3, sq = std::max(3.0, std::round(road_px / 5));
            const Rgb light{246, 248, 250}, dark{24, 28, 36};
            for (int y = int(cy - half - 4); y <= int(cy + half + 4); ++y)
                for (int x = int(cx - half - 4); x <= int(cx + half + 4); ++x) {
                    if (x < 0 || y < 0 || x >= out.w() || y >= out.h()) continue;
                    const double dx = x + .5 - cx, dy = y + .5 - cy;
                    const double along = dx * ux + dy * uy, across = -dx * uy + dy * ux;
                    if (std::abs(across) > half || along < -sq || along >= sq) continue;
                    const bool odd = (int(std::floor(across / sq)) + (along >= 0)) & 1;
                    out.fill_rect(x, y, 1, 1, odd ? dark : light);
                }
        }
    }

    Condition on_, when_;
    Expr x_, y_, course_;
    std::string name_;
    std::map<long, std::string> names_;
    std::map<long, Road> courses_;
    int width_ = 0x40, cell_ = 8;
    Expr path_, path_count_, path_left_, path_right_;
    static constexpr int column_w = 116;
    uint32_t racer_base_ = 0, racer_step_ = 0, racer_exists_at_ = 0, racer_exists_mask_ = 0, racer_x_ = 0, racer_y_ = 0,
             racer_rank_ = 0, racer_lap_ = 0, racer_kind_ = 0, faces_at_ = 0;
    int racer_count_ = 0, laps_ = 0, face_w_ = 32, face_h_ = 32;
    std::map<int, std::pair<uint32_t, uint32_t>> face_places_;
    std::vector<Racer> racers_;
    std::map<int, Face> faces_, small_faces_;
    bool faces_read_ = false;
    int path_stride_ = 8, path_dx_ = 0, path_dy_ = 4;
    bool playing_ = false, sampled_ = false, pen_down_ = false;
    long course_now_ = 0;
    long px_ = 0, py_ = 0, sx_ = 0, sy_ = 0, hx_ = 0, hy_ = 0;
    int heading_x_ = 0, heading_y_ = 0;
    long wrap_x_ = 0, wrap_y_ = 0, off_x_ = 0, off_y_ = 0;
    uint32_t serial_ = 0, moves_ = 0, layer_serial_ = 0;
    uint64_t layer_frame_ = 0;
    mutable uint32_t saved_ = 0;
    // The last picture of everything but the player, and where it put the course.
    mutable Canvas layer_;
    mutable uint64_t layer_key_ = ~0ull;
    mutable double s_ = 1, ox_ = 0, oy_ = 0, wx0_ = 0, wy0_ = 0;
};

} // namespace

std::unique_ptr<View> make_trace(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Trace>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
