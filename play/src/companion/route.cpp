// SPDX-License-Identifier: GPL-3.0-or-later
// A race through stages that fork (Out Run): the stages in rows, one more
// each level, every stage leading on to the two below it. The way driven is
// drawn in the accent with each stage's time, the stage now with its running
// clock, and the two roads ahead stand out. The game keeps only the stage
// it is on, so the way there and the times are kept beside the save.
//
//   [view]
//   format = route
//   on = mem8[8356] in 1..5
//   stage = mem8[8355]       the stage now: 1 at the top, then left to right a level at a time
//   levels = 5               rows (decimal)
//   clock = 8238 6           the stage's clock as the game writes it (text: ASCII, top bit dropped)
//   last = 8232 6            the last stage's time, written when a stage is done
//   total = 823E 6           the time of the stages done
//
// Times are the game's own text (2'18"3: minutes, seconds, tenths); the total
// adds the stage now to the stages done.
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace play {
namespace {

struct Text {
    uint32_t address = 0;
    int size = 0;
};

class Route final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "stage", stage_, true, error)) return false;
        for (const auto& [key, value] : spec.values) {
            std::istringstream words(value);
            bool ok = true;
            if (key == "levels") {
                ok = bool(words >> levels_) && levels_ >= 2 && levels_ <= 6;
            } else if (key == "clock" || key == "last" || key == "total") {
                Text& t = key == "clock" ? clock_ : key == "last" ? last_ : total_;
                std::string at;
                ok = bool(words >> at >> t.size) && parse_address(at, t.address) && t.size >= 1 && t.size <= 12;
            }
            if (!ok) {
                error = "[view] route: bad " + key + " '" + value + "'";
                return false;
            }
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 6) return;
        const int stage = int(stage_.eval(live));
        shown_ = (on_.empty() || on_.match(live)) && stage >= 1 && stage <= stages();
        if (!shown_) return;
        if (path_.empty() || stage != path_.back()) {
            if (stage == 1 || path_.empty() || !leads_to(path_.back(), stage)) path_.clear(), times_.clear();
            path_.push_back(stage);
            ++serial_;
        }
        // The stage just done: its time, written by the game as the next one starts.
        if (path_.size() >= 2) {
            const std::string t = read(live, last_);
            std::string& kept = times_[path_[path_.size() - 2]];
            if (!t.empty() && t != kept) kept = t, ++serial_;
        }
        clock_now_ = read(live, clock_);
        total_now_ = read(live, total_);
    }

    bool shown() const override { return shown_; }

    uint64_t key(uint64_t) const override {
        uint64_t h = 1469598103934665603ull ^ (uint64_t(serial_) << 32);
        for (char c : clock_now_ + total_now_) h = (h ^ uint8_t(c)) * 1099511628211ull;
        return h;
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGRT" 1, then each stage of the way: its number and its time's length and text.
        bytes = "RGRT";
        bytes += char(1);
        for (int s : path_) {
            auto t = times_.find(s);
            const std::string time = t == times_.end() ? std::string() : t->second;
            bytes += char(s);
            bytes += char(time.size());
            bytes += time;
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        if (bytes.size() < 5 || bytes.compare(0, 4, "RGRT") != 0 || bytes[4] != 1) return;
        path_.clear(), times_.clear();
        for (size_t at = 5; at + 2 <= bytes.size();) {
            const int s = uint8_t(bytes[at]);
            const size_t n = uint8_t(bytes[at + 1]);
            if (at + 2 + n > bytes.size()) break;
            path_.push_back(s);
            if (n) times_[s] = bytes.substr(at + 2, n);
            at += 2 + n;
        }
        saved_ = ++serial_;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const int W = out.w(), H = out.h();
        // The heading and the level, clear of the switch's corner.
        const int head_cy = kit::margin + kit::badge / 2;
        const std::string heading = look.tr("Route");
        int hx =
            kit::margin + font.draw(out, kit::margin, font.centred(kit::title, head_cy), heading, kit::title, look.ink);
        if (!path_.empty())
            chip(out, look, font, hx + kit::gap, head_cy,
                 look.tr("Stage") + " " + std::to_string(level_of(path_.back()) + 1) + " / " + std::to_string(levels_),
                 true);

        // The times: this stage and all of them, in a card along the bottom (the dock's corner left free).
        const int card_h = 76, card_y = H - kit::margin - card_h;
        Box times{kit::margin, card_y, W - 2 * kit::margin, card_h};
        if (look.reserve_w) times.w = W - look.reserve_w - kit::gap - times.x;
        card(out, look, times);
        const int col = times.w / 2;
        const std::string total = format(tenths(total_now_) + tenths(clock_now_));
        const std::pair<const char*, std::string> cols[2] = {{"This stage", clock_now_}, {"Total", total}};
        for (int i = 0; i < 2; ++i) {
            const int x = times.x + 20 + i * col, right = times.x + (i + 1) * col - 12;
            font.draw(out, x, times.y + 28, font.fit(look.tr(cols[i].first), kit::small, right - x), kit::small,
                      look.muted);
            font.draw(out, x, times.y + card_h - 14, font.fit(cols[i].second, kit::large, right - x), kit::large,
                      look.ink);
        }

        // The stages: a row for each level, every stage joined to the two it leads to.
        const int node_w = 96, node_h = 44;
        const int top = kit::margin + kit::badge + kit::gap + node_h / 2, bottom = card_y - kit::gap - node_h / 2 - 4;
        const int step_y = (bottom - top) / (levels_ - 1);
        const int step_x = std::min(118, (W - 2 * kit::margin - node_w) / (levels_ - 1));
        auto centre = [&](int s, int& cx, int& cy) {
            const int level = level_of(s), k = s - first_of(level);
            cx = W / 2 + int(std::lround((k - level / 2.0) * step_x));
            cy = top + level * step_y;
        };
        // Every way starts at the top, even one whose start this run didn't see (a state loaded mid-way).
        auto on_path = [&](int s) { return s == 1 || std::find(path_.begin(), path_.end(), s) != path_.end(); };
        const int now = path_.empty() ? 0 : path_.back();
        for (int s = 1; s <= stages(); ++s) {
            if (level_of(s) + 1 >= levels_) continue;
            int x0, y0;
            centre(s, x0, y0);
            for (int child : {s + level_of(s) + 1, s + level_of(s) + 2}) {
                int x1, y1;
                centre(child, x1, y1);
                const bool driven = on_path(s) && on_path(child);
                const bool ahead = s == now;
                road(out, x0, y0 + node_h / 2, x1, y1 - node_h / 2,
                     driven  ? 4
                     : ahead ? 3
                             : 2,
                     driven || ahead ? look.accent : look.line);
            }
        }
        for (int s = 1; s <= stages(); ++s) {
            int cx, cy;
            centre(s, cx, cy);
            const int x = cx - node_w / 2, y = cy - node_h / 2;
            const bool ahead = now && level_of(s) == level_of(now) + 1 && leads_to(now, s);
            // Stages done a shade off the accent (the accent itself with High contrast, for its 7:1); the
            // stage now in the accent without the highlight line, ringed in ink.
            const Rgb done = look.high_contrast ? look.accent : mix(look.accent, look.panel, 88);
            if (s == now) {
                out.rounded_rect(x - 4, y - 4, node_w + 8, node_h + 8, node_h / 2 + 4, look.ink);
                raised(out, look, x, y, node_w, node_h, node_h / 2, look.accent, 0, true, false);
                text_in(out, font, clock_now_, cx, cy, Look::ink_on(look.accent));
            } else if (on_path(s)) {
                raised(out, look, x, y, node_w, node_h, node_h / 2, done);
                auto t = times_.find(s);
                text_in(out, font, t == times_.end() ? std::string() : t->second, cx, cy, Look::ink_on(done));
            } else if (ahead) {
                card(out, look, {x, y, node_w, node_h}, true, node_h / 2);
            } else {
                sunken(out, look, {x, y, node_w, node_h}, node_h / 2);
            }
        }
    }

private:
    int stages() const { return levels_ * (levels_ + 1) / 2; }
    static int level_of(int s) {
        int level = 0;
        while (s > first_of(level + 1) - 1) ++level;
        return level;
    }
    static int first_of(int level) { return level * (level + 1) / 2 + 1; }
    // Stage a's road forks to the stage under it and the one to that one's right.
    static bool leads_to(int a, int b) {
        const int down = a + level_of(a) + 1;
        return b == down || b == down + 1;
    }

    static std::string read(Core& live, const Text& t) {
        std::string s;
        for (int i = 0; i < t.size; ++i) {
            const char c = char(peek_address(live, t.address + uint32_t(i), 1) & 0x7F);
            if (c >= ' ' && c < 0x7F) s += c;
        }
        s.erase(0, s.find_first_not_of(' '));
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    }

    // 2'18"3 as tenths of a second, and back.
    static long tenths(const std::string& t) {
        long parts[3] = {0, 0, 0};
        int n = 0;
        for (char c : t) {
            if (c >= '0' && c <= '9') parts[n] = parts[n] * 10 + (c - '0');
            else if (n < 2) ++n;
        }
        return n == 2 ? (parts[0] * 60 + parts[1]) * 10 + parts[2] : 0;
    }
    static std::string format(long t) {
        const long minutes = t / 600, seconds = t / 10 % 60;
        return std::to_string(minutes) + "'" + (seconds < 10 ? "0" : "") + std::to_string(seconds) + "\"" +
               std::to_string(t % 10);
    }

    static void text_in(Canvas& out, const Font& font, const std::string& text, int cx, int cy, Rgb ink) {
        if (text.empty()) return;
        font.draw(out, cx - font.measure(text, kit::small) / 2, font.centred(kit::small, cy), text, kit::small, ink);
    }

    // A road between two stages: a line of discs, `r` across.
    static void road(Canvas& out, int x0, int y0, int x1, int y1, int r, Rgb c) {
        const int n = std::max(1, int(std::hypot(x1 - x0, y1 - y0)));
        for (int i = 0; i <= n; ++i) disc(out, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r, c);
    }

    Condition on_;
    Expr stage_;
    int levels_ = 5;
    Text clock_, last_, total_;
    std::vector<int> path_;
    std::map<int, std::string> times_;
    std::string clock_now_, total_now_;
    bool shown_ = false;
    uint64_t serial_ = 1;
    mutable uint64_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_route(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Route>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
