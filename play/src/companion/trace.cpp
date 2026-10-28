// SPDX-License-Identifier: GPL-3.0-or-later
// format = trace: a course map drawn from where the player has driven. The
// game keeps no map of its course in memory, only the machine's place on it,
// so the first lap draws the course and every lap after runs on it, the
// player a blinking dot. One map per course, kept beside the save.
//
//   x = mem16[7E0B70]        # the player's place on the course (expr.hpp)
//   y = mem16[7E0B90]
//   course = mem8[7E00F2] + mem8[7E0053]   # one map per value
//   when = mem8[7E005B] == 00              # traced only while this holds (racing, not a replay)
//   name = Course                          # the chip: "Course 3", or name.N per course
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
#include <vector>

namespace play {
namespace {

struct Point {
    int16_t x, y;
};

class Trace final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "x", x_, true, error) || !view_expr(spec, "y", y_, true, error) ||
            !view_expr(spec, "course", course_, false, error))
            return false;
        for (const auto& [key, value] : spec.values) {
            if (key == "when") {
                std::string why;
                if (!parse_condition(value, when_, why)) {
                    error = "[view] trace: bad when '" + value + "': " + why;
                    return false;
                }
            } else if (key == "name") {
                name_ = value;
            } else if (key.rfind("name.", 0) == 0) {
                names_[std::strtol(key.c_str() + 5, nullptr, 16)] = value;
            }
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 2) return;
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        const long course = course_.eval(live);
        const int x = int(x_.eval(live)), y = int(y_.eval(live));
        if (course != course_now_ || ((x >> 2) != (px_ >> 2) || (y >> 2) != (py_ >> 2))) ++moves_;
        course_now_ = course;
        px_ = x, py_ = y;
        if (!when_.empty() && !when_.match(live)) return;
        auto& points = courses_[course];
        // A point each time the machine has gone a few lengths from the last one.
        if (!points.empty()) {
            const Point& last = points.back();
            if (std::abs(last.x - x) + std::abs(last.y - y) < 24) return;
        }
        // A course is drawn once: a point next to one already there adds nothing.
        for (const Point& p : points)
            if (std::abs(p.x - x) + std::abs(p.y - y) < 16) return;
        if (points.size() < 8000) points.push_back({int16_t(x), int16_t(y)}), ++serial_;
    }

    bool shown() const override { return playing_; }

    uint64_t key(uint64_t frame) const override {
        return (uint64_t(serial_) << 28) ^ (uint64_t(moves_) << 2) ^ (uint64_t(course_now_ & 0xFF) << 56) ^
               ((frame / 16) & 1);
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        backdrop(out, look);
        const auto n = names_.find(course_now_);
        const std::string name = n != names_.end() ? n->second
                                 : name_.empty()   ? ""
                                                   : name_ + " " + std::to_string(course_now_ + 1);
        if (!name.empty()) chip(out, look, font, kit::margin, kit::margin + 14, name, true);
        auto found = courses_.find(course_now_);
        if (found == courses_.end() || found->second.size() < 2) {
            // Under the name, so a long translation still fits the row.
            chip(out, look, font, kit::margin, kit::margin + 14 + (name.empty() ? 0 : 44),
                 look.tr("The course draws itself on the first lap"), false);
            return;
        }
        const auto& points = found->second;
        int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
        for (const Point& p : points) {
            min_x = std::min<int>(min_x, p.x), max_x = std::max<int>(max_x, p.x);
            min_y = std::min<int>(min_y, p.y), max_y = std::max<int>(max_y, p.y);
        }
        min_x = std::min(min_x, px_), max_x = std::max(max_x, px_);
        min_y = std::min(min_y, py_), max_y = std::max(max_y, py_);
        const int top = kit::margin * 2 + 28, room_w = out.w() - 2 * kit::margin - 40,
                  room_h = out.h() - top - kit::margin - 40;
        const double s =
            std::min(room_w / double(std::max(1, max_x - min_x)), room_h / double(std::max(1, max_y - min_y)));
        const int cx = kit::margin + 20 + (room_w - int((max_x - min_x) * s)) / 2;
        const int cy = top + 20 + (room_h - int((max_y - min_y) * s)) / 2;
        auto at = [&](int x, int y) {
            return std::pair<int, int>{cx + int((x - min_x) * s), cy + int((y - min_y) * s)};
        };
        // The road: a wide light band, then a thin line down its middle.
        const Rgb road = mix(look.muted, look.ink, 50), centre = mix(look.background, look.panel, 120);
        const int r = std::max(5, out.w() / 70);
        for (const Point& p : points) {
            auto [x, y] = at(p.x, p.y);
            disc(out, x, y, r, road);
        }
        for (const Point& p : points) {
            auto [x, y] = at(p.x, p.y);
            disc(out, x, y, std::max(1, r / 4), centre);
        }
        auto [mx, my] = at(px_, py_);
        const Rgb marker = (frame / 16) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64};
        disc(out, mx, my, r + 4, Rgb{12, 17, 26});
        disc(out, mx, my, r + 2, marker);
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGTR" 1, then per course: the course, the point count (int32 each) and the points.
        bytes = "RGTR";
        bytes += char(1);
        for (const auto& [course, points] : courses_) {
            const int32_t head[2] = {int32_t(course), int32_t(points.size())};
            bytes.append(reinterpret_cast<const char*>(head), sizeof head);
            bytes.append(reinterpret_cast<const char*>(points.data()), points.size() * sizeof(Point));
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        if (bytes.size() < 5 || bytes.compare(0, 4, "RGTR") != 0 || bytes[4] != 1) return;
        for (size_t at = 5; at + 8 <= bytes.size();) {
            int32_t head[2];
            std::memcpy(head, bytes.data() + at, sizeof head);
            at += 8;
            const size_t size = size_t(std::max(0, head[1])) * sizeof(Point);
            if (at + size > bytes.size()) break;
            auto& points = courses_[head[0]];
            points.resize(size_t(head[1]));
            std::memcpy(points.data(), bytes.data() + at, size);
            at += size;
        }
        saved_ = ++serial_;
    }

private:
    Condition on_, when_;
    Expr x_, y_, course_;
    std::string name_;
    std::map<long, std::string> names_;
    std::map<long, std::vector<Point>> courses_;
    bool playing_ = false;
    long course_now_ = 0;
    int px_ = 0, py_ = 0;
    uint32_t serial_ = 0, moves_ = 0;
    mutable uint32_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_trace(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Trace>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
