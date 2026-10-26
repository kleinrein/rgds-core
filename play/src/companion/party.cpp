// SPDX-License-Identifier: GPL-3.0-or-later
#include "companion/party.hpp"
#include "ui/kit.hpp"

#include "companion/companion.hpp"
#include "companion/party_gb.hpp"
#include "core/core.hpp"

#include <algorithm>

namespace play {

std::string pokemon3_text(const uint8_t* bytes, int max) {
    std::string out;
    for (int i = 0; i < max; ++i) {
        uint8_t b = bytes[i];
        if (b == 0xFF) break;
        if (b >= 0xBB && b <= 0xD4) out += char('A' + b - 0xBB);
        else if (b >= 0xD5 && b <= 0xEE) out += char('a' + b - 0xD5);
        else if (b >= 0xA1 && b <= 0xAA) out += char('0' + b - 0xA1);
        else if (b == 0x00) out += ' ';
        else if (b == 0xAB) out += '!';
        else if (b == 0xAC) out += '?';
        else if (b == 0xAD) out += '.';
        else if (b == 0xAE) out += '-';
        else if (b == 0xB0) out += "…";
        else if (b == 0xB3 || b == 0xB4) out += '\'';
        else if (b == 0xB5) out += "♂";
        else if (b == 0xB6) out += "♀";
        else if (b == 0xB8) out += ',';
        else if (b == 0xBA) out += '/';
        else if (b == 0x1B) out += "é";
    }
    return out;
}

namespace {

// Where the growth block (species first) sits among the four, by personality % 24.
constexpr uint8_t growth_slot[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 2, 3, 1, 1, 2, 3, 2, 3, 1, 1, 2, 3, 2, 3};

uint64_t mix_key(uint64_t key, uint64_t v) { return (key ^ v) * 1099511628211ull; }

} // namespace

bool Party::read(Core& core) {
    core_ = &core;
    std::vector<PartyMember> team;
    uint64_t key = 1469598103934665603ull;
    bool ok = spec_.format == "goldensun" ? read_goldensun(core, team, key)
              : spec_.format == "saga1"   ? read_saga1(core, spec_.slots, team, key)
              : spec_.format == "dwm1"    ? read_dwm1(core, spec_.slots, spec_.count, spec_.names, team, key)
              : spec_.format == "pokemon1" || spec_.format == "pokemon2" ? read_pokemon_gb(core, gb_tables(), team, key)
                                                                         : read_pokemon3(core, team, key);
    if (!ok) return false; // mid-write: keep the last good team
    bool changed = key != key_;
    key_ = key;
    members_ = std::move(team);
    return changed;
}

PokemonGbTables Party::gb_tables() const {
    PokemonGbTables t;
    t.generation = spec_.format == "pokemon1" ? 1 : 2;
    t.count = spec_.count;
    t.slots = spec_.slots;
    t.names = spec_.names;
    t.dex_order = spec_.dex_order;
    t.icon_kinds = spec_.icon_kinds;
    t.icons = spec_.icons;
    t.palette_index = spec_.icon_palette_index;
    t.palettes = spec_.icon_palettes;
    return t;
}

bool Party::read_goldensun(Core& core, std::vector<PartyMember>& team, uint64_t& key) {
    const uint32_t flags = core.peek(spec_.count, 1);
    for (int i = 0; i < 8; ++i) {
        if (!((flags >> i) & 1)) continue;
        const uint32_t base = spec_.slots + uint32_t(i) * 0x14C;
        PartyMember m;
        for (int c = 0; c < 15; ++c) {
            char ch = char(core.peek(base + uint32_t(c), 1));
            if (!ch) break;
            m.name += ch >= 32 && ch < 127 ? ch : '?';
        }
        m.level = int(core.peek(base + 0x0F, 1));
        m.max_hp = int(core.peek(base + 0x34, 2));
        m.max_pp = int(core.peek(base + 0x36, 2));
        m.hp = int(core.peek(base + 0x38, 2));
        m.pp = int(core.peek(base + 0x3A, 2));
        if (m.max_hp > 9999 || m.hp > m.max_hp || m.pp > m.max_pp) return false;
        for (uint64_t v :
             {uint64_t(i), uint64_t(m.level), uint64_t(m.hp), uint64_t(m.max_hp), uint64_t(m.pp), uint64_t(m.max_pp)})
            key = mix_key(key, v);
        team.push_back(std::move(m));
    }
    return true;
}

bool Party::read_pokemon3(Core& core, std::vector<PartyMember>& team, uint64_t& key) {
    int count = int(core.peek(spec_.count, 1));
    if (count > 6) count = 0; // not loaded yet
    for (int i = 0; i < count; ++i) {
        const uint32_t base = spec_.slots + uint32_t(i) * 100;
        uint8_t raw[100];
        for (int b = 0; b < 100; b += 4) {
            uint32_t v = core.peek(base + uint32_t(b), 4);
            for (int k = 0; k < 4; ++k) raw[b + k] = uint8_t(v >> (8 * k));
        }
        auto u32 = [&](int o) {
            return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 |
                   uint32_t(raw[o + 3]) << 24;
        };
        PartyMember m;
        const uint32_t personality = u32(0), crypt = personality ^ u32(4);
        const int growth = growth_slot[personality % 24];
        m.species = int((u32(0x20 + growth * 12) ^ crypt) & 0xFFFF);
        m.egg = (raw[0x13] & 0x04) != 0;
        m.name = m.egg ? "Egg" : pokemon3_text(raw + 8, 10);
        m.level = raw[0x54];
        m.hp = raw[0x56] | raw[0x57] << 8;
        m.max_hp = raw[0x58] | raw[0x59] << 8;
        const uint32_t s = u32(0x50);
        m.status = (s & 7)      ? "SLP"
                   : (s & 0x88) ? "PSN"
                   : (s & 0x10) ? "BRN"
                   : (s & 0x20) ? "FRZ"
                   : (s & 0x40) ? "PAR"
                                : "";
        if (m.species <= 0 || m.species > 1000 || m.max_hp > 999 || m.hp > m.max_hp) return false; // mid-write
        for (uint64_t v : {uint64_t(m.species), uint64_t(m.level), uint64_t(m.hp), uint64_t(m.max_hp), uint64_t(s),
                           uint64_t(m.egg), uint64_t(personality)})
            key = mix_key(key, v);
        team.push_back(std::move(m));
    }
    return true;
}

const Party::Icon* Party::icon(int species) const {
    auto found = icons_.find(species);
    if (found != icons_.end()) return found->second.px[0].empty() ? nullptr : &found->second;
    Icon& ic = icons_[species];
    if (spec_.format == "pokemon1" || spec_.format == "pokemon2") {
        if (!core_ || !pokemon_gb_icon(*core_, gb_tables(), species, ic.px)) ic = Icon{};
        return ic.px[0].empty() ? nullptr : &ic;
    }
    if (!core_ || !spec_.icons || !spec_.icon_palette_index || !spec_.icon_palettes) return nullptr;
    const uint32_t gfx = core_->peek(spec_.icons + uint32_t(species) * 4, 4);
    const uint32_t which = core_->peek(spec_.icon_palette_index + uint32_t(species), 1);
    if (which > 5 || gfx < 0x08000000 || gfx >= 0x0A000000) return nullptr;
    // The palette table's own tags (0xDAC0 and up): wrong addresses (another
    // release or revision of the game) show no icons rather than noise.
    if (core_->peek(spec_.icon_palettes + 4, 2) != 0xDAC0 || core_->peek(spec_.icon_palettes + 12, 2) != 0xDAC1)
        return nullptr;
    const uint32_t pal = core_->peek(spec_.icon_palettes + which * 8, 4);
    uint32_t colours[16];
    for (int c = 0; c < 16; ++c) {
        uint32_t v = core_->peek(pal + uint32_t(c) * 2, 2);
        uint32_t r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
        colours[c] = (r | r >> 5) | (g | g >> 5) << 8 | (b | b >> 5) << 16 | 0xFF000000u;
    }
    // Two 32x32 steps of 4x4 tiles, 4 bits a pixel.
    for (int step = 0; step < 2; ++step) {
        auto& px = ic.px[step];
        px.assign(32 * 32, 0);
        for (int tile = 0; tile < 16; ++tile)
            for (int y = 0; y < 8; ++y) {
                uint32_t row = core_->peek(gfx + uint32_t(step * 512 + tile * 32 + y * 4), 4);
                for (int x = 0; x < 8; ++x) {
                    int c = (row >> (4 * x)) & 15;
                    if (c) px[size_t(((tile / 4) * 8 + y) * 32 + (tile % 4) * 8 + x)] = colours[c];
                }
            }
    }
    return &ic;
}

void Party::draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const {
    backdrop(out, look);
    const int W = out.w(), H = out.h(), m = kit::margin, gap = kit::gap;
    // The header: a badge, "Team" and how many are in it.
    icon_badge(out, look, m, m, play::Icon::Life);
    const int cy = m + kit::badge / 2;
    const int tw0 =
        font.draw(out, m + kit::badge + gap, font.centred(kit::title, cy), look.tr("Team"), kit::title, look.ink);
    // Pokémon carry six; Golden Sun's and Final Fantasy Legend's parties are four, Dragon Warrior Monsters' three.
    const int cols = 2,
              rows = spec_.format == "pokemon1" || spec_.format == "pokemon2" || spec_.format == "pokemon3" ? 3 : 2,
              slots = spec_.format == "dwm1" ? 3 : cols * rows;
    chip(out, look, font, m + kit::badge + gap + tw0 + gap, cy,
         std::to_string(members_.size()) + " / " + std::to_string(slots), false);
    const int top = m + kit::badge + gap + 4;
    const int cw = (W - m * 2 - gap) / cols, ch = (H - top - m + 4 - gap * (rows - 1)) / rows;
    for (int i = 0; i < slots; ++i) {
        Box b{m + (i % cols) * (cw + gap), top + (i / cols) * (ch + gap), cw, ch};
        // The dock's corner stays free.
        if (look.reserve_w && b.y + b.h > H - look.reserve_h && b.x + b.w > W - look.reserve_w)
            b.w = W - look.reserve_w - gap - b.x;
        const bool there = i < int(members_.size());
        if (!there) {
            // An empty place: recessed, like an empty slot in the launcher.
            sunken(out, look, b, kit::radius);
            continue;
        }
        card(out, look, b);
        const PartyMember& p = members_[size_t(i)];
        const bool fainted = !p.egg && p.hp == 0;
        const int x = b.x, y = b.y;
        // The icon at a whole scale on a slot, stepping like the game's own party screen.
        const int scale = std::max(1, std::min(2, (ch - 20) / 32)), box = 32 * scale + 8;
        int tx = x + 16;
        if (spec_.icons) {
            const int bx = x + 12, by = y + (ch - box) / 2;
            out.rounded_rect(bx, by, box, box, 12, look.slot);
            if (const Icon* ic = icon(p.species)) {
                const auto& px = ic->px[fainted ? 0 : (frame / 16) & 1];
                out.blit_keyed(px.data(), 32, 32, bx + 4, by + 4, scale, 0);
                if (fainted) out.fill_rect(bx + 4, by + 4, 32 * scale, 32 * scale, look.slot, 150);
            }
            tx = bx + box + 12;
        }
        const int right = x + b.w - 14, tw = right - tx;
        const int big = kit::body, small = kit::small;
        const Rgb hp_colour = p.hp * 2 > p.max_hp   ? Rgb{56, 176, 96}
                              : p.hp * 5 > p.max_hp ? Rgb{222, 170, 30}
                                                    : Rgb{214, 64, 56};
        if (p.max_pp > 0) {
            // Name and level, then a row each for HP and PP: label, bar, numbers.
            int line = y + 30;
            // The species beside the level where the name is the player's own (Dragon Warrior Monsters).
            std::string level =
                (p.detail.empty() ? "" : p.detail + "  ") + look.tr("Lv") + " " + std::to_string(p.level);
            font.draw(out, right - font.measure(level, small), line, level, small, look.muted);
            font.draw(out, tx, line, font.fit(p.name, big, tw - font.measure(level, small) - 8), big,
                      fainted ? look.muted : look.ink);
            const int label = font.measure("HP", small) + 8, numbers = font.measure("9999/9999", small) + 8;
            const struct {
                const char* name;
                int now, most;
                Rgb colour;
            } bars[2] = {{"HP", p.hp, p.max_hp, hp_colour},
                         {spec_.format == "dwm1" ? "MP" : "PP", p.pp, p.max_pp, Rgb{64, 128, 232}}};
            for (int r = 0; r < 2; ++r) {
                line += std::max(24, (ch - 40) / 2 - 2);
                font.draw(out, tx, line, bars[r].name, small, look.muted);
                meter(out, look, {tx + label, line - font.ascent(small) / 2 - 6, tw - label - numbers, 12},
                      bars[r].most ? double(bars[r].now) / bars[r].most : 0, bars[r].colour);
                std::string n = std::to_string(bars[r].now) + "/" + std::to_string(bars[r].most);
                font.draw(out, right - font.measure(n, small), line, n, small, look.ink);
            }
            continue;
        }
        int line = y + 32;
        font.draw(out, tx, line, font.fit(p.name, big, tw), big, fainted ? look.muted : look.ink);
        if (p.egg) continue;
        std::string level = p.level > 0 ? look.tr("Lv") + " " + std::to_string(p.level) : p.detail;
        line += 26;
        const int lw = font.draw(out, tx, line, level, small, look.muted);
        if (!p.status.empty() || fainted) {
            const std::string tag = fainted ? "FNT" : p.status;
            const Rgb c = fainted        ? Rgb{150, 150, 160}
                          : tag == "PSN" ? Rgb{176, 92, 220}
                          : tag == "BRN" ? Rgb{240, 120, 60}
                          : tag == "PAR" ? Rgb{230, 200, 60}
                          : tag == "FRZ" ? Rgb{120, 200, 240}
                                         : Rgb{184, 184, 204};
            const int sx = tx + lw + 10, sw = font.measure(tag, small) + 16;
            out.rounded_rect(sx, line - font.ascent(small) - 5, sw, font.ascent(small) + 10, 8, c);
            font.draw(out, sx + 8, line, tag, small, Look::ink_on(c));
        }
        // HP: a meter in the game's colours, the numbers under it.
        const int by = line + 12;
        meter(out, look, {tx, by, tw, 12}, p.max_hp ? double(p.hp) / p.max_hp : 0, hp_colour);
        const std::string hp = std::to_string(p.hp) + " / " + std::to_string(p.max_hp);
        if (by + 12 + 22 <= y + ch) font.draw(out, right - font.measure(hp, small), by + 12 + 20, hp, small, look.ink);
    }
}

} // namespace play
