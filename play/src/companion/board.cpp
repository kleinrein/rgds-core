// SPDX-License-Identifier: GPL-3.0-or-later
// format = board: a status board for games whose own screen keeps its numbers
// in a strip or a pause screen. Each tile is a label and a number the game
// keeps in memory, drawn as a launcher card on the second screen, in the order
// the profile numbers them:
//
//   heading = In a course
//   heading_art = lives           # a badge beside the heading (an art or an icon name)
//   art.coin = gb 11:73F0 1x1 clear 1
//   tile1 = Coins | bcd A97A 2 art coin
//   tile2 = Bank | bcd A805 3 wide icon bank
//   tile3 = Hearts | bcd A808 1 of 100 icon heart
//   tile4 = Treasures | bits A80E 2 of 15 icon treasure page Treasures
//   tile5 = Ending | bcd A805 3 wide tiers
//   tiers5 = 300 Birdhouse; 10072 Tree trunk; 40008 Log cabin
//
// Kinds: `bcd ADDRESS BYTES` (two digits a byte, the first byte the highest),
// `num ADDRESS SIZE` (a little-endian number of 1 to 4 bytes) and
// `bits ADDRESS BYTES` (how many bits are set, up to 32 bytes: a Pokédex). Words after it:
//   of N       a meter out of N (for bits up to 16 with an icon: a row of them, lit or not)
//   big        num: the first byte the highest (big-endian)
//   low        bcd: the first byte the lowest (a little-endian word of BCD)
//   mask HEX   num: only these bits (day or night in a flag byte)
//   div N      the number divided by N (health kept in quarters of a heart)
//   wide       the tile takes a whole row
//   icon NAME  a badge with one of the player's icons (ui/icons.hpp: coin, heart, ...)
//   art NAME   a badge with the game's own tiles, from an `art.NAME` key:
//              `gb BANK:ADDRESS COLSxROWS [clear N]` (2bpp tiles in the ROM; colour N see-through)
//   page NAME  the tile is on that page; a board with pages shows them as tabs
//              in its header, chosen by touch (tiles before the first page: the first)
//   tiers      the value names a step from `tiersN` ("AT NAME; AT NAME; ..."):
//              the step reached, and a meter to the next one
//   names      the same list, but the steps are names, not goals (a sword's
//              level, day or night): the name, and a dot for each step
// `covers_hud = yes` puts the board in place of a [hud] strip's band: the
// band still leaves the game picture, but the board already shows its numbers.
#include "companion/companion.hpp"
#include "companion/view.hpp"
#include "core/core.hpp"
#include "ui/kit.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace play {
namespace {

struct Art {
    uint32_t address = 0;
    int cols = 1, rows = 1, clear = -1;
    GameArt art;
    bool read = false;
};

struct Step {
    long at = 0;
    std::string name;
};

struct Tile {
    std::string label, kind;
    uint32_t address = 0;
    int size = 1, of = 0, page = 0, div = 1;
    uint32_t mask = 0xFFFFFFFFu;
    bool wide = false, big = false, low = false, names = false;
    Icon icon = Icon::None;
    std::string art;
    std::vector<Step> tiers;
    long value = 0;
};

std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    return s;
}

class Board final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        auto value = [&](const char* key) {
            auto it = spec.values.find(key);
            return it == spec.values.end() ? std::string() : it->second;
        };
        heading_ = value("heading");
        heading_badge_ = value("heading_art");
        if (heading_badge_.empty()) heading_badge_ = value("heading_icon");
        for (const auto& [key, line] : spec.values) {
            if (key.rfind("art.", 0) != 0) continue;
            Art a;
            std::istringstream words(line);
            std::string kind, address, size, word;
            words >> kind >> address >> size;
            if (kind != "gb" || !parse_address(address, a.address) ||
                std::sscanf(size.c_str(), "%dx%d", &a.cols, &a.rows) != 2 || a.cols < 1 || a.rows < 1 ||
                a.cols * a.rows > 16) {
                error = "[view] board: '" + key + "' needs 'gb BANK:ADDRESS COLSxROWS'";
                return false;
            }
            while (words >> word)
                if (word == "clear" && words >> a.clear) continue;
                else {
                    error = "[view] board: '" + key + "': unknown '" + word + "'";
                    return false;
                }
            arts_[key.substr(4)] = a;
        }
        if (!heading_badge_.empty() && !arts_.count(heading_badge_) && icon_named(heading_badge_) == Icon::None) {
            error = "[view] board: heading_art '" + heading_badge_ + "' is neither an art nor an icon";
            return false;
        }
        // tile1, tile2, ... in number order (the keys sort as text).
        std::vector<std::pair<int, std::string>> lines;
        for (const auto& [key, line] : spec.values) {
            if (key.rfind("tile", 0) != 0) continue;
            const int n = std::atoi(key.c_str() + 4);
            if (n <= 0) {
                error = "[view] board: '" + key + "' needs a number (tile1, tile2, ...)";
                return false;
            }
            lines.emplace_back(n, line);
        }
        std::sort(lines.begin(), lines.end());
        int page = 0;
        for (const auto& [n, line] : lines) {
            Tile t;
            const std::string name = "tile" + std::to_string(n);
            const auto bar = line.find('|');
            if (bar == std::string::npos) {
                error = "[view] board: " + name + " needs 'Label | kind address ...'";
                return false;
            }
            t.label = trim(line.substr(0, bar));
            std::istringstream words(line.substr(bar + 1));
            std::string address;
            words >> t.kind >> address >> t.size;
            if ((t.kind != "bcd" && t.kind != "num" && t.kind != "bits") || !parse_address(address, t.address) ||
                t.size < 1 || t.size > (t.kind == "bits" ? 32 : 4)) {
                error = "[view] board: " + name + " has a bad kind, address or size";
                return false;
            }
            bool tiers = false;
            for (std::string word; words >> word;) {
                std::string arg;
                if (word == "wide") t.wide = true;
                else if (word == "big") t.big = true;
                else if (word == "low") t.low = true;
                else if (word == "mask" && words >> arg && !arg.empty() &&
                         arg.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
                    t.mask = uint32_t(std::strtoul(arg.c_str(), nullptr, 16));
                else if (word == "div" && words >> t.div && t.div > 0) continue;
                else if (word == "tiers") tiers = true;
                else if (word == "names") tiers = t.names = true;
                else if (word == "of" && words >> t.of && t.of > 0) continue;
                else if (word == "icon" && words >> arg && icon_named(arg) != Icon::None) t.icon = icon_named(arg);
                else if (word == "art" && words >> arg && arts_.count(arg)) t.art = arg;
                else if (word == "page" && words >> arg) {
                    auto it = std::find(pages_.begin(), pages_.end(), arg);
                    if (it == pages_.end()) {
                        if (pages_.empty() && !tiles_.empty()) pages_.push_back("Main");
                        pages_.push_back(arg);
                        it = pages_.end() - 1;
                    }
                    page = int(it - pages_.begin());
                } else {
                    error = "[view] board: " + name + ": unknown or incomplete '" + word + "'";
                    return false;
                }
            }
            t.page = page;
            if (tiers) {
                std::istringstream steps(value(("tiers" + std::to_string(n)).c_str()));
                for (std::string item; std::getline(steps, item, ';');) {
                    item = trim(item);
                    Step s;
                    const auto space = item.find(' ');
                    if (space == std::string::npos) break;
                    s.at = std::atol(item.c_str());
                    s.name = trim(item.substr(space));
                    t.tiers.push_back(s);
                }
                if (t.tiers.empty()) {
                    error = "[view] board: " + name + " has no steps in tiers" + std::to_string(n);
                    return false;
                }
            }
            tiles_.push_back(t);
        }
        auto covers = spec.values.find("covers_hud");
        covers_hud_ = covers != spec.values.end() && (covers->second == "yes" || covers->second == "true");
        if (tiles_.empty()) {
            error = "[view] board needs at least one tile";
            return false;
        }
        if (pages_.empty()) pages_.push_back("Main");
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 15) return;
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        for (auto& [name, a] : arts_) {
            if (a.read) continue;
            std::vector<uint8_t> bytes(size_t(a.cols * a.rows * 16));
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = uint8_t(peek_address(live, step(a.address, int(i)), 1));
            a.art = gb_tiles(bytes, a.cols, a.rows, a.clear);
            a.read = true;
        }
        uint64_t key = 1469598103934665603ull;
        for (auto& t : tiles_) {
            long v = 0;
            if (t.kind == "num") {
                uint32_t n = 0;
                for (int i = 0; i < t.size; ++i) {
                    const uint32_t b = peek_address(live, step(t.address, i), 1) & 0xFF;
                    n = t.big ? (n << 8) | b : n | (b << (8 * i));
                }
                v = long(n & t.mask);
            } else
                for (int i = 0; i < t.size; ++i) {
                    const uint32_t b = peek_address(live, step(t.address, t.low ? t.size - 1 - i : i), 1);
                    if (t.kind == "bits") v += __builtin_popcount(b);
                    else v = v * 100 + long((b >> 4) % 10 * 10 + (b & 15) % 10);
                }
            t.value = v / t.div;
            key = (key ^ uint64_t(v)) * 1099511628211ull;
        }
        key_ = key;
        read_ = true;
    }

    bool shown() const override { return playing_ && read_; }
    bool covers_hud() const override { return covers_hud_; }
    uint64_t key(uint64_t) const override { return key_ ^ (uint64_t(page_) << 56); }

    bool touch(const Touch& t) override {
        if (t.kind != Touch::Down) return false;
        for (size_t i = 0; i < tabs_.size(); ++i)
            if (tabs_[i].contains(t.x, t.y) && int(i) != page_) {
                page_ = int(i);
                return true;
            }
        return false;
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t) const override {
        backdrop(out, look);
        const int W = out.w(), H = out.h();
        int top = kit::margin;
        tabs_.clear();
        // A board without a header still keeps out of the switch's corner.
        const bool header = !heading_.empty() || pages_.size() > 1;
        if (!header && look.reserve_top_h) top = look.reserve_top_h;
        if (!heading_.empty() || pages_.size() > 1) {
            // The header: the heading (with its badge) on the left, the pages as tabs on the right.
            const int cy = top + kit::badge / 2;
            int x = kit::margin;
            if (!heading_badge_.empty()) {
                badge(out, look, heading_badge_, x, top);
                x += kit::badge + kit::gap;
            }
            int right = W - kit::margin - (look.reserve_top_w ? look.reserve_top_w - kit::margin + kit::gap : 0);
            if (pages_.size() > 1) right = draw_tabs(out, look, font, right, cy) - kit::gap;
            if (!heading_.empty())
                font.draw(out, x, font.centred(kit::title, cy), font.fit(look.tr(heading_), kit::title, right - x),
                          kit::title, look.ink);
            top += kit::badge + kit::gap + 4;
        }
        // Rows of two tiles; a wide tile takes a row of its own.
        std::vector<std::vector<const Tile*>> rows;
        for (const auto& t : tiles_) {
            if (t.page != page_) continue;
            if (t.wide || rows.empty() || rows.back().size() == 2 || rows.back()[0]->wide) rows.push_back({});
            rows.back().push_back(&t);
        }
        const int n = int(rows.size());
        if (!n) return;
        const int bottom = H - kit::margin + 4;
        const int rh = std::min(104, (bottom - top - kit::gap * (n - 1)) / n);
        for (int r = 0; r < n; ++r) {
            const int y = top + r * (rh + kit::gap);
            const int cols = rows[size_t(r)][0]->wide ? 1 : 2;
            const int cw = (W - kit::margin * 2 - kit::gap * (cols - 1)) / cols;
            for (size_t c = 0; c < rows[size_t(r)].size(); ++c) {
                Box b{kit::margin + int(c) * (cw + kit::gap), y, cw, rh};
                // The dock's corner stays free: the tile there gives up its end.
                if (look.reserve_w && b.y + b.h > H - look.reserve_h && b.x + b.w > W - look.reserve_w)
                    b.w = W - look.reserve_w - kit::gap - b.x;
                draw_tile(out, look, font, *rows[size_t(r)][c], b);
            }
        }
    }

private:
    static uint32_t step(uint32_t address, int n) {
        return (address & 0xFFFF0000u) | ((address + uint32_t(n)) & 0xFFFF);
    }

    void badge(Canvas& out, const Look& look, const std::string& name, int x, int y) const {
        auto it = arts_.find(name);
        if (it != arts_.end()) art_badge(out, look, x, y, it->second.art, look.game_shades);
        else icon_badge(out, look, x, y, icon_named(name));
    }

    // The pages as a segmented control (the launcher's Light | Dark), right-aligned at `right`.
    int draw_tabs(Canvas& out, const Look& look, const Font& font, int right, int cy) const {
        const int h = kit::touch, pad = 20;
        int w = 8;
        for (const auto& p : pages_) w += font.measure(look.tr(p), kit::body) + pad * 2;
        const int x0 = right - w, y = cy - h / 2;
        sunken(out, look, {x0, y, w, h}, h / 2);
        int x = x0 + 4;
        for (size_t i = 0; i < pages_.size(); ++i) {
            const int tw = font.measure(look.tr(pages_[i]), kit::body) + pad * 2;
            const bool on = int(i) == page_;
            if (on) raised(out, look, x, y + 4, tw, h - 8, (h - 8) / 2, look.accent, 0, false);
            font.draw(out, x + pad, font.centred(kit::body, cy), look.tr(pages_[i]), kit::body,
                      on ? look.on_accent : look.muted);
            tabs_.push_back({x, y, tw, h});
            touch_target(tabs_.back());
            x += tw;
        }
        return x0;
    }

    static std::string number(long v) { return std::to_string(v); }

    void draw_tile(Canvas& out, const Look& look, const Font& font, const Tile& t, const Box& b) const {
        card(out, look, b);
        int x = b.x + 16;
        if (t.icon != Icon::None || !t.art.empty()) {
            const int by = b.y + (b.h - kit::badge) / 2;
            if (!t.art.empty()) badge(out, look, t.art, x, by);
            else icon_badge(out, look, x, by, t.icon);
            x += kit::badge + 14;
        }
        const int right = b.x + b.w - 16, roomy = b.h >= 84;
        const int label_base = roomy ? b.y + 30 : font.centred(kit::small, b.y + b.h / 3);
        const int big = roomy ? kit::number : kit::large;
        const int base = roomy ? b.y + b.h - 20 : font.centred(big, b.y + b.h * 2 / 3 + 2);
        if (!t.tiers.empty()) {
            draw_tiers(out, look, font, t, x, right, label_base, base);
            return;
        }
        font.draw(out, x, label_base, font.fit(look.tr(t.label), kit::small, right - x), kit::small, look.muted);
        const std::string value = number(t.value);
        const int vw = font.draw(out, x, base, value, big, look.ink);
        if (t.of <= 0) return;
        const std::string most = "/ " + number(t.of);
        const int mx = x + vw + 8;
        font.draw(out, mx, base, most, kit::small, look.muted);
        const int left = mx + font.measure(most, kit::small) + 18, mid = base - font.ascent(big) / 2;
        if (right - left < 40) return;
        if (t.kind == "bits" && t.of <= 16 && t.icon != Icon::None) {
            // A row of the tile's icon, lit for each one found.
            const int step = std::min(30, (right - left) / t.of), size = std::min(24, step - 4);
            if (step < 18) {
                meter(out, look, {left, mid - 7, right - left, 14}, double(std::min<long>(t.value, t.of)) / t.of,
                      look.accent);
                return;
            }
            for (int i = 0; i < t.of; ++i)
                icon(out, look, t.icon, left + i * step + step / 2, mid, size, i < t.value ? look.accent : look.line,
                     i < t.value ? 255 : 200);
            return;
        }
        meter(out, look, {left, mid - 7, right - left, 14}, double(std::min<long>(t.value, t.of)) / t.of, look.accent);
    }

    // A step reached by the value ("Tree trunk"), and how far it is to the next.
    void draw_tiers(Canvas& out, const Look& look, const Font& font, const Tile& t, int x, int right, int label_base,
                    int base) const {
        int reached = -1;
        for (size_t i = 0; i < t.tiers.size(); ++i)
            if (t.value >= t.tiers[i].at) reached = int(i);
        const bool last = reached + 1 >= int(t.tiers.size());
        if (t.names) {
            // A name, and where it stands among the others.
            font.draw(out, x, label_base, font.fit(look.tr(t.label), kit::small, right - x), kit::small, look.muted);
            const std::string name = look.tr(reached >= 0 ? t.tiers[size_t(reached)].name : "None yet");
            const int nw = font.draw(out, x, base, font.fit(name, kit::large, right - x), kit::large, look.ink);
            const int n = int(t.tiers.size()), d = 14, cy = base - font.ascent(kit::large) / 2;
            if (n > 2 && x + nw + 24 + n * (d + 6) <= right)
                for (int i = 0; i < n; ++i)
                    out.rounded_rect(right - (n - i) * (d + 6), cy - d / 2, d, d, d / 2,
                                     i <= reached ? look.accent : look.line);
            return;
        }
        const std::string now = look.tr(reached >= 0 ? t.tiers[size_t(reached)].name : "Not yet");
        std::string next = last ? look.tr("The best there is")
                                : look.tr("Next:") + " " + look.tr(t.tiers[size_t(reached + 1)].name) + " · " +
                                      number(t.tiers[size_t(reached + 1)].at);
        const int half = x + (right - x) / 2;
        font.draw(out, x, label_base, font.fit(look.tr(t.label), kit::small, half - x - 8), kit::small, look.muted);
        font.draw(out, x, base, font.fit(now, kit::large, half - x - 8), kit::large, look.ink);
        const int nw = font.measure(next, kit::small);
        next = font.fit(next, kit::small, right - half);
        font.draw(out, right - std::min(nw, right - half), label_base, next, kit::small, look.muted);
        const long from = reached >= 0 ? t.tiers[size_t(reached)].at : 0,
                   to = last ? from : t.tiers[size_t(reached + 1)].at;
        const double f = last ? 1.0 : double(t.value - from) / std::max<long>(1, to - from);
        meter(out, look, {half, base - font.ascent(kit::large) / 2 - 7, right - half, 14}, f, look.accent);
    }

    Condition on_;
    std::string heading_, heading_badge_;
    std::map<std::string, Art> arts_;
    std::vector<std::string> pages_;
    std::vector<Tile> tiles_;
    int page_ = 0;
    mutable std::vector<Box> tabs_;
    bool playing_ = false, read_ = false, covers_hud_ = false;
    uint64_t key_ = 0;
};

} // namespace

std::unique_ptr<View> make_board(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Board>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
