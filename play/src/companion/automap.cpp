// SPDX-License-Identifier: GPL-3.0-or-later
// format = metroid2: Metroid II: Return of Samus has no map. This one draws
// each screen of SR388 the first time it comes into view, from the
// cartridge's own level data: every 8x8 tile solid or open, the room edges
// the camera stops at, and Samus on it. Layout facts from the M2RoS
// disassembly (metroidret/M2RoS).
//
// The world is seven level banks of 16x16 screens of 256x256 pixels. A
// screen is 16x16 metatiles (an index each, from a pointer per screen at the
// start of its bank); a metatile is four tiles, as the room's tileset (copied
// to work RAM) defines them; a tile is solid below the room's solidity index.
// A byte per screen says which of its edges stop the camera: the room's walls.
#include "companion/companion.hpp"
#include "companion/view.hpp"
#include "ui/kit.hpp"
#include "core/core.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace play {
namespace {

constexpr int tiles = 32; // a screen is 32x32 tiles

struct Screen {
    uint8_t edges = 0; // bit 0 right, 1 left, 2 up, 3 down: the camera stops there
    uint8_t solid[tiles * tiles / 8] = {};
    bool is_solid(int x, int y) const { return (solid[(y * tiles + x) >> 3] >> (x & 7)) & 1; }
};

class Automap final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        for (auto [key, out] : {std::pair<const char*, uint32_t*>{"level_bank", &level_bank_},
                                {"camera", &camera_},
                                {"samus", &samus_},
                                {"metatiles", &metatiles_},
                                {"solid_below", &solid_below_},
                                {"screens", &screens_},
                                {"edges", &edges_}}) {
            auto found = spec.values.find(key);
            if (found == spec.values.end() || !parse_address(found->second, *out)) {
                error = std::string("[view] metroid2 needs an address for ") + key;
                return false;
            }
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 4) return;
        playing_ = on_.empty() || on_.match(live);
        if (!playing_) return;
        const int bank = int(peek_address(live, level_bank_, 1));
        if (bank < 1 || bank > 0x7F) return;
        // The camera is the middle of the view: 160 wide, 136 tall above the status bar.
        const int cam_y = int(peek_address(live, camera_, 1)) + 256 * int(peek_address(live, camera_ + 1, 1) & 15);
        const int cam_x = int(peek_address(live, camera_ + 2, 1)) + 256 * int(peek_address(live, camera_ + 3, 1) & 15);
        const int left = cam_x - 80, top = cam_y - 72;
        bank_ = bank;
        // Samus where she is drawn (sprite coordinates are 8 right, 16 down).
        samus_x_ = left + int(peek_address(live, samus_ + 1, 1)) - 8;
        samus_y_ = top + int(peek_address(live, samus_, 1)) - 16;
        auto& area = areas_[bank];
        for (int y = top >> 8; y <= (top + 135) >> 8; ++y)
            for (int x = left >> 8; x <= (left + 159) >> 8; ++x) {
                if (x < 0 || y < 0 || x > 15 || y > 15) continue;
                const int index = y * 16 + x;
                if (area.count(index)) continue;
                area[index] = read_screen(live, bank, index);
                ++serial_;
            }
    }

    bool shown() const override { return playing_ && bank_ != 0; }

    uint64_t key(uint64_t frame) const override {
        // Redrawn as screens are found, as Samus moves a tile, and as her marker blinks.
        return (uint64_t(serial_) << 32) ^ (uint64_t(bank_) << 56) ^ (uint64_t(samus_x_ >> 3) << 16) ^
               uint64_t(samus_y_ >> 3) ^ ((frame / 20) & 1 ? 0x8000 : 0);
    }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        backdrop(out, look);
        auto found = areas_.find(bank_);
        if (found == areas_.end()) return;
        const auto& area = found->second;
        // A tile is a few panel pixels: five screens across a 640-wide panel.
        const int cell = std::max(2, out.w() / 160);
        // Samus in the middle, the area's edges kept on screen once there is enough of it.
        const int view_w = out.w() / cell, view_h = out.h() / cell; // in tiles
        int ox = samus_x_ / 8 - view_w / 2, oy = samus_y_ / 8 - view_h / 2;
        int min_x = 512, min_y = 512, max_x = 0, max_y = 0;
        for (const auto& [index, s] : area) {
            min_x = std::min(min_x, (index % 16) * tiles);
            max_x = std::max(max_x, (index % 16 + 1) * tiles);
            min_y = std::min(min_y, (index / 16) * tiles);
            max_y = std::max(max_y, (index / 16 + 1) * tiles);
        }
        if (max_x - min_x <= view_w) ox = (min_x + max_x - view_w) / 2;
        else ox = std::clamp(ox, min_x, max_x - view_w);
        if (max_y - min_y <= view_h) oy = (min_y + max_y - view_h) / 2;
        else oy = std::clamp(oy, min_y, max_y - view_h);

        // Rock light, caverns dark, the rooms' walls in the accent colour.
        const Rgb rock = mix(look.muted, look.ink, 40), open = mix(look.background, look.panel, 160),
                  wall = look.accent;
        uint32_t* px = out.data();
        const int W = out.w(), H = out.h();
        auto fill = [&](int x, int y, int w, int h, uint32_t c) {
            for (int yy = std::max(0, y); yy < std::min(H, y + h); ++yy)
                std::fill(px + size_t(yy) * W + std::max(0, x), px + size_t(yy) * W + std::min(W, x + w), c);
        };
        for (const auto& [index, s] : area) {
            const int sx = (index % 16) * tiles - ox, sy = (index / 16) * tiles - oy;
            if (sx + tiles < 0 || sy + tiles < 0 || sx > view_w || sy > view_h) continue;
            fill(sx * cell, sy * cell, tiles * cell, tiles * cell, open.px());
            const uint32_t r = rock.px();
            for (int y = 0; y < tiles; ++y)
                for (int x = 0; x < tiles; ++x)
                    if (s.is_solid(x, y)) fill((sx + x) * cell, (sy + y) * cell, cell, cell, r);
            // The room's walls: the edges the camera stops at.
            const int t = std::max(1, cell / 2), size = tiles * cell, X = sx * cell, Y = sy * cell;
            const uint32_t w = wall.px();
            if (s.edges & 1) fill(X + size - t, Y, t, size, w);
            if (s.edges & 2) fill(X, Y, t, size, w);
            if (s.edges & 4) fill(X, Y, size, t, w);
            if (s.edges & 8) fill(X, Y + size - t, size, t, w);
        }
        // Samus: a blinking marker the size of her suit (16x32 pixels).
        const int mx = (samus_x_ / 8 - ox) * cell, my = (samus_y_ / 8 - oy) * cell;
        const Rgb marker = (frame / 20) & 1 ? Rgb{255, 214, 92} : Rgb{255, 140, 64};
        out.rounded_rect(mx - cell, my - cell, cell * 4, cell * 6, cell, Rgb{12, 17, 26});
        out.rounded_rect(mx, my, cell * 2, cell * 4, cell / 2 + 1, marker);
        // Where this is, as a chip in the corner, and how much of it has been seen.
        const int w = chip(out, look, font, kit::margin, kit::margin + 14, "SR388", true);
        chip(out, look, font, kit::margin + w + 8, kit::margin + 14,
             std::to_string(area.size()) + " " + look.tr("screens"), false);
    }

    bool dirty() const override { return serial_ != saved_; }

    bool save(std::string& bytes) const override {
        // "RGAM" 1, then per screen: bank, index, edges, 128 bytes of tiles.
        bytes = "RGAM";
        bytes += char(1);
        for (const auto& [bank, area] : areas_)
            for (const auto& [index, s] : area) {
                bytes += char(bank);
                bytes += char(index);
                bytes += char(s.edges);
                bytes.append(reinterpret_cast<const char*>(s.solid), sizeof s.solid);
            }
        saved_ = serial_;
        return true;
    }

    void load(const std::string& bytes) override {
        const size_t entry = 3 + sizeof(Screen::solid);
        if (bytes.size() < 5 || bytes.compare(0, 4, "RGAM") != 0 || bytes[4] != 1) return;
        for (size_t at = 5; at + entry <= bytes.size(); at += entry) {
            Screen s;
            s.edges = uint8_t(bytes[at + 2]);
            std::memcpy(s.solid, bytes.data() + at + 3, sizeof s.solid);
            areas_[uint8_t(bytes[at])][uint8_t(bytes[at + 1])] = s;
        }
        saved_ = ++serial_;
    }

private:
    Screen read_screen(Core& live, int bank, int index) const {
        Screen s;
        s.edges = uint8_t(live.peek_bank((edges_ & 0xFFFF) + uint32_t(index), bank, 1));
        const uint32_t data = live.peek_bank((screens_ & 0xFFFF) + uint32_t(index) * 2, bank, 2);
        if (data < 0x4000 || data > 0x7F00) return s;
        const int solid_below = int(peek_address(live, solid_below_, 1));
        for (int my = 0; my < 16; ++my)
            for (int mx = 0; mx < 16; ++mx) {
                const uint32_t metatile = live.peek_bank(data + uint32_t(my * 16 + mx), bank, 1) & 0x7F;
                for (int q = 0; q < 4; ++q) {
                    const int tile = int(peek_address(live, metatiles_ + metatile * 4 + uint32_t(q), 1));
                    if (tile >= solid_below) continue;
                    const int x = mx * 2 + (q & 1), y = my * 2 + (q >> 1);
                    s.solid[(y * tiles + x) >> 3] |= uint8_t(1 << (x & 7));
                }
            }
        return s;
    }

    Condition on_;
    uint32_t level_bank_ = 0, camera_ = 0, samus_ = 0, metatiles_ = 0, solid_below_ = 0, screens_ = 0, edges_ = 0;
    std::map<int, std::map<int, Screen>> areas_; // bank -> screen index -> screen
    bool playing_ = false;
    int bank_ = 0, samus_x_ = 0, samus_y_ = 0;
    uint32_t serial_ = 0;
    mutable uint32_t saved_ = 0;
};

} // namespace

std::unique_ptr<View> make_automap(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Automap>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
