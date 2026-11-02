// SPDX-License-Identifier: GPL-3.0-or-later
// A competition of events (California Games): each event with the score that
// won it and the record to beat, the one being played standing out, and the
// players ranked by their medals. The game prints each event's scores once and
// forgets them when the next begins, so what won each is kept beside the save.
//
//   [view]
//   format = events
//   on = mem8[081D] in 1..8 and mem8[081E] in 1..7
//   players = mem8[081D]
//   event = mem8[081E]            1 the first; past the last, the final standings
//   names = Half Pipe; Foot Bag   the events in order, as the game names them
//   records = 0800 20             the players' table: where, and each player's size (hex)
//   record_name = 0 F             offset and length (hex) of the name
//   record_score = 10 6           of the last event's score, as the game prints it
//   record_medals = 19            of three counts: gold, silver, bronze
//   best = 0900 15                the record table, an entry for each event
//   best_name = 0 C
//   best_score = C 6
//
// Medals count 5, 3 and 1 points, as the game's own standings do.
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace play {
namespace {

struct Field {
    uint32_t offset = 0;
    int size = 0;
};

class Events final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        if (!view_expr(spec, "players", players_, true, error) || !view_expr(spec, "event", event_, true, error))
            return false;
        for (const auto& [key, value] : spec.values) {
            std::istringstream words(value);
            bool ok = true;
            std::string a, b;
            if (key == "names") {
                std::istringstream list(value);
                for (std::string name; std::getline(list, name, ';');) {
                    name.erase(0, name.find_first_not_of(' '));
                    name.erase(name.find_last_not_of(' ') + 1);
                    if (!name.empty()) names_.push_back(name);
                }
                ok = !names_.empty() && names_.size() <= 8;
            } else if (key == "records" || key == "best") {
                uint32_t& at = key == "records" ? records_ : best_;
                uint32_t& step = key == "records" ? record_step_ : best_step_;
                ok = bool(words >> a >> b) && parse_address(a, at) && parse_address(b, step) && step > 0;
            } else if (key == "record_name" || key == "record_score" || key == "best_name" || key == "best_score") {
                Field& f = key == "record_name"    ? record_name_
                           : key == "record_score" ? record_score_
                           : key == "best_name"    ? best_name_
                                                   : best_score_;
                uint32_t size = 0;
                ok = bool(words >> a >> b) && parse_address(a, f.offset) && parse_address(b, size) && size >= 1 &&
                     size <= 16;
                f.size = int(size);
            } else if (key == "record_medals") {
                ok = bool(words >> a) && parse_address(a, medals_at_);
            }
            if (!ok) {
                error = "[view] events: bad " + key + " '" + value + "'";
                return false;
            }
        }
        if (names_.empty() || !record_name_.size || !record_score_.size || !best_name_.size || !best_score_.size) {
            error = "[view] events: needs names, records, best and their fields";
            return false;
        }
        won_.resize(names_.size());
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 6) return;
        const int players = int(players_.eval(live));
        const int event = int(event_.eval(live));
        shown_ = (on_.empty() || on_.match(live)) && players >= 1 && players <= 8;
        if (!shown_) return;

        std::vector<Player> now;
        for (int i = 0; i < players; ++i) {
            const uint32_t at = records_ + uint32_t(i) * record_step_;
            Player p;
            p.name = read(live, at, record_name_);
            for (int m = 0; m < 3; ++m) p.medals[m] = int(peek_address(live, at + medals_at_ + uint32_t(m), 1));
            now.push_back(p);
        }
        // The game sorts its table between events: the same players in any order are the same competition.
        std::vector<std::string> sorted;
        for (const auto& p : now) sorted.push_back(p.name);
        std::sort(sorted.begin(), sorted.end());
        std::string who;
        for (const auto& name : sorted) who += name + '\n';
        int medals = 0;
        for (const auto& p : now) medals += p.medals[0] + p.medals[1] + p.medals[2];
        // Other players, fewer medals or an earlier event: a new competition.
        if (who != who_ || medals < medal_sum_ || event < event_now_) {
            for (auto& w : won_) w = {};
            ++serial_;
        }
        who_ = who, medal_sum_ = medals;

        // The event's scores appear as it ends, and are blank again when the next begins.
        if (event >= 1 && size_t(event) <= won_.size())
            for (int i = 0; i < players; ++i) {
                const std::string text = read(live, records_ + uint32_t(i) * record_step_, record_score_);
                if (text.empty()) continue;
                const double value = std::strtod(text.c_str(), nullptr);
                Won& w = won_[size_t(event - 1)];
                if (w.score.empty() || value > std::strtod(w.score.c_str(), nullptr)) {
                    w = {now[size_t(i)].name, plain_number(text)};
                    ++serial_;
                }
            }

        std::stable_sort(now.begin(), now.end(), [](const Player& a, const Player& b) { return a.points() > b.points(); });
        if (!(now == players_now_) || event != event_now_) ++drawn_;
        players_now_ = std::move(now);
        event_now_ = event;
        records_now_.clear();
        for (size_t i = 0; i < names_.size(); ++i) {
            const uint32_t at = best_ + uint32_t(i) * best_step_;
            Won r{read(live, at, best_name_), plain_number(read(live, at, best_score_))};
            if (i >= records_seen_.size() || records_seen_[i].name != r.name || records_seen_[i].score != r.score) ++drawn_;
            records_now_.push_back(r);
        }
        records_seen_ = records_now_;
    }

    bool shown() const override { return shown_; }
    uint64_t key(uint64_t) const override { return (uint64_t(serial_) << 24) ^ drawn_; }
    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGEV" 1, then for each event the name and score that won it, each with its length first.
        bytes = "RGEV";
        bytes += char(1);
        for (const auto& w : won_) {
            bytes += char(w.name.size());
            bytes += w.name;
            bytes += char(w.score.size());
            bytes += w.score;
        }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        if (bytes.size() < 5 || bytes.compare(0, 4, "RGEV") != 0 || bytes[4] != 1) return;
        size_t at = 5;
        auto text = [&](std::string& out) {
            if (at >= bytes.size()) return false;
            const size_t n = uint8_t(bytes[at++]);
            if (at + n > bytes.size()) return false;
            out = bytes.substr(at, n);
            at += n;
            return true;
        };
        for (auto& w : won_)
            if (!text(w.name) || !text(w.score)) break;
        saved_ = ++serial_;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const int W = out.w();
        const int head_cy = kit::margin + kit::badge / 2;
        const std::string heading = look.tr("Events");
        const int hx =
            kit::margin + font.draw(out, kit::margin, font.centred(kit::title, head_cy), heading, kit::title, look.ink);
        const bool final_now = size_t(event_now_) > names_.size();
        chip(out, look, font, hx + kit::gap, head_cy,
             final_now ? look.tr("Final")
                       : look.tr("Event") + " " + std::to_string(event_now_) + " / " + std::to_string(names_.size()),
             true);

        // The events down the left.
        const int top = kit::margin + kit::badge + kit::gap, left_w = 366;
        const int bottom = out.h() - kit::margin, gap = 6, count = int(names_.size());
        const int row_h = (bottom - top - gap * (count - 1)) / count;
        for (int i = 0; i < count; ++i) {
            const Box b{kit::margin, top + i * (row_h + gap), left_w, row_h};
            card(out, look, b, i + 1 == event_now_);
            const int pad = 14, right = b.x + b.w - pad, base = b.y + b.h / 2 - 4;
            const Won& w = won_[size_t(i)];
            const int score_w = w.score.empty() ? 0 : font.measure(w.score, kit::body);
            font.draw(out, b.x + pad, base, font.fit(names_[size_t(i)], kit::body, b.w - 2 * pad - score_w - kit::gap),
                      kit::body, look.ink);
            if (score_w) font.draw(out, right - score_w, base, w.score, kit::body, look.ink);
            // Under it: the record to beat on the left, who won here on the right.
            const int line = b.y + b.h - 8;
            const int who_w = w.name.empty() ? 0 : std::min(font.measure(w.name, kit::small), 110);
            if (who_w) font.draw(out, right - who_w, line, font.fit(w.name, kit::small, 110), kit::small, look.muted);
            if (size_t(i) < records_now_.size() && !records_now_[size_t(i)].score.empty()) {
                const Won& r = records_now_[size_t(i)];
                font.draw(out, b.x + pad, line,
                          font.fit(r.name + " " + r.score, kit::small, b.w - 2 * pad - who_w - kit::gap), kit::small,
                          look.muted);
            }
        }

        // The players on the right, the dock's corner left free.
        const int x = kit::margin + left_w + kit::gap, w = W - kit::margin - x;
        const int room = (look.reserve_h ? out.h() - look.reserve_h - kit::gap : bottom) - top;
        font.draw(out, x + 4, font.centred(kit::small, top + 12), font.fit(look.tr("Standings"), kit::small, w - 4),
                  kit::small, look.muted);
        const int list_y = top + 28, count_p = int(players_now_.size());
        const int step = count_p ? std::min(44, (room - 28) / count_p) : 44;
        static const Rgb metal[3] = {{226, 178, 44}, {176, 182, 190}, {190, 122, 66}};
        for (int i = 0; i < count_p; ++i) {
            const Player& p = players_now_[size_t(i)];
            const Box b{x, list_y + i * step, w, step - 4};
            card(out, look, b, false, 10);
            const int cy = b.y + b.h / 2;
            const std::string points = std::to_string(p.points());
            const int points_w = font.measure(points, kit::body);
            font.draw(out, b.x + b.w - 10 - points_w, font.centred(kit::body, cy), points, kit::body, look.ink);
            // A medal's disc and count for each won.
            int mx = b.x + b.w - 10 - points_w - 8;
            for (int m = 2; m >= 0; --m) {
                if (!p.medals[m]) continue;
                const std::string n = std::to_string(p.medals[m]);
                mx -= font.measure(n, kit::small);
                font.draw(out, mx, font.centred(kit::small, cy), n, kit::small, look.ink);
                mx -= 10;
                disc(out, mx, cy, 6, metal[m]);
                mx -= 12;
            }
            font.draw(out, b.x + 10, font.centred(kit::small, cy), font.fit(p.name, kit::small, mx - b.x - 12), kit::small,
                      look.ink);
        }
    }

private:
    struct Won {
        std::string name, score;
    };
    struct Player {
        std::string name;
        int medals[3] = {0, 0, 0};
        int points() const { return medals[0] * 5 + medals[1] * 3 + medals[2]; }
        bool operator==(const Player& o) const { return name == o.name && std::equal(medals, medals + 3, o.medals); }
    };

    static std::string read(Core& live, uint32_t at, const Field& f) {
        std::string s;
        for (int i = 0; i < f.size; ++i) {
            const char c = char(peek_address(live, at + f.offset + uint32_t(i), 1));
            if (c >= ' ' && c < 0x7F) s += c;
        }
        s.erase(0, s.find_first_not_of(' '));
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    }
    // The game pads some events' scores with zeros: 000030 is 30, 0.0 stays.
    static std::string plain_number(std::string s) {
        while (s.size() > 1 && s[0] == '0' && s[1] >= '0' && s[1] <= '9') s.erase(0, 1);
        return s;
    }

    Condition on_;
    Expr players_, event_;
    std::vector<std::string> names_;
    uint32_t records_ = 0, record_step_ = 1, best_ = 0, best_step_ = 1, medals_at_ = 0;
    Field record_name_, record_score_, best_name_, best_score_;
    std::vector<Won> won_, records_now_, records_seen_;
    std::vector<Player> players_now_;
    std::string who_;
    int event_now_ = 0, medal_sum_ = 0;
    bool shown_ = false;
    uint64_t serial_ = 1, drawn_ = 0;
    mutable uint64_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_events(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Events>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
