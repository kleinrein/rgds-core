// SPDX-License-Identifier: GPL-3.0-or-later
// Party layouts, character sets and icon tables from the pret disassemblies
// (pokered, pokecrystal); addresses checked against Blue (USA, Europe) and
// Crystal (USA, Europe) Rev 1.
#include "companion/party_gb.hpp"

#include "core/core.hpp"

namespace play {

std::string pokemon12_text(const uint8_t* bytes, int max, int generation) {
    std::string out;
    for (int i = 0; i < max; ++i) {
        uint8_t b = bytes[i];
        if (b == 0x50) break;
        if (b >= 0x80 && b <= 0x99) out += char('A' + b - 0x80);
        else if (b >= 0xA0 && b <= 0xB9) out += char('a' + b - 0xA0);
        else if (b >= 0xF6) out += char('0' + b - 0xF6);
        else if (b == 0x7F) out += ' ';
        else if (b == 0xE0) out += '\'';
        else if (b == 0xE3) out += '-';
        else if (b == 0xE6) out += '?';
        else if (b == 0xE7) out += '!';
        else if (b == 0xE8) out += '.';
        else if (b == 0xEF) out += "♂";
        else if (b == 0xF1) out += "×";
        else if (b == 0xF3) out += '/';
        else if (b == 0xF4) out += ',';
        else if (b == 0xF5) out += "♀";
        else if (b == (generation == 1 ? 0xBA : 0xEA)) out += "é";
    }
    return out;
}

namespace {

constexpr int egg_species = 0xFD; // generation 2's party list marks an egg so

uint64_t mix_key(uint64_t key, uint64_t v) { return (key ^ v) * 1099511628211ull; }

// One step of an address (plain or banked) by `n` bytes.
uint32_t offset(uint32_t address, uint32_t n) { return (address & 0xFFFF0000u) | ((address + n) & 0xFFFF); }

uint32_t rgb555(uint32_t v) {
    uint32_t r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
    return (r | r >> 5) | (g | g >> 5) << 8 | (b | b >> 5) << 16 | 0xFF000000u;
}

// An 8x8 2bpp tile (16 bytes) drawn into a 16x16 icon at (x, y), doubled into
// the 32x32 picture, with `colours[1..3]` (0 stays see-through).
void put_tile(std::vector<uint32_t>& px, const uint8_t* tile, int x, int y, bool flip, const uint32_t* colours) {
    for (int row = 0; row < 8; ++row) {
        if (y + row < 0 || y + row >= 16) continue;
        uint8_t lo = tile[row * 2], hi = tile[row * 2 + 1];
        for (int col = 0; col < 8; ++col) {
            int bit = 7 - col;
            int c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!c) continue;
            int tx = x + (flip ? 7 - col : col), ty = y + row;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) px[size_t((ty * 2 + dy) * 32 + tx * 2 + dx)] = colours[c];
        }
    }
}

void read_bytes(Core& core, uint32_t address, uint8_t* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = uint8_t(peek_address(core, offset(address, uint32_t(i)), 1));
}

} // namespace

bool read_pokemon_gb(Core& core, const PokemonGbTables& t, std::vector<PartyMember>& out, uint64_t& key) {
    const bool gen1 = t.generation == 1;
    const int size = gen1 ? 44 : 48;
    std::vector<PartyMember> team;
    int count = int(peek_address(core, t.count, 1));
    if (count > 6) count = 0; // not loaded yet
    uint64_t k = 1469598103934665603ull;
    for (int i = 0; i < count; ++i) {
        uint8_t raw[48], name[11];
        read_bytes(core, offset(t.slots, uint32_t(i * size)), raw, size);
        read_bytes(core, offset(t.names, uint32_t(i * 11)), name, 11);
        const int listed = int(peek_address(core, offset(t.count, uint32_t(1 + i)), 1));
        PartyMember m;
        int status;
        if (gen1) {
            // species, HP (big-endian), box level, status, ... level at 33, max HP at 34
            const int index = raw[0];
            m.species = t.dex_order ? int(peek_address(core, offset(t.dex_order, uint32_t(index - 1)), 1)) : index;
            m.hp = raw[1] << 8 | raw[2];
            status = raw[4];
            m.level = raw[33];
            m.max_hp = raw[34] << 8 | raw[35];
            if (index == 0 || m.species == 0 || m.species > 151) return false; // mid-write
        } else {
            // species, item, moves, ID, exp, stat exp, DVs at 21, ..., level at 31,
            // status at 32, HP at 34, max HP at 36 (big-endian)
            m.species = raw[0];
            m.egg = listed == egg_species;
            m.level = raw[31];
            status = raw[32];
            m.hp = raw[34] << 8 | raw[35];
            m.max_hp = raw[36] << 8 | raw[37];
            // Shiny: defence, speed and special DVs 10, attack 2, 3, 6, 7, 10, 11, 14 or 15.
            const int atk = raw[21] >> 4, def = raw[21] & 15, spd = raw[22] >> 4, spc = raw[22] & 15;
            if (def == 10 && spd == 10 && spc == 10 && (atk & 2)) m.species |= 0x10000;
            if ((m.species & 0xFFFF) == 0 || (m.species & 0xFFFF) > 251) return false;
        }
        m.name = m.egg ? "Egg" : pokemon12_text(name, 11, t.generation);
        m.status = (status & 7)      ? "SLP"
                   : (status & 0x08) ? "PSN"
                   : (status & 0x10) ? "BRN"
                   : (status & 0x20) ? "FRZ"
                   : (status & 0x40) ? "PAR"
                                     : "";
        if (m.level > 100 || m.max_hp > 999 || m.hp > m.max_hp) return false;
        for (uint64_t v : {uint64_t(m.species), uint64_t(m.level), uint64_t(m.hp), uint64_t(m.max_hp), uint64_t(status),
                           uint64_t(m.egg)})
            k = mix_key(k, v);
        for (uint8_t b : name) k = mix_key(k, b);
        team.push_back(std::move(m));
    }
    key = k;
    out = std::move(team);
    return true;
}

bool pokemon_gb_icon(Core& core, const PokemonGbTables& t, int species, std::vector<uint32_t> (&px)[2]) {
    if (!t.icons || !t.icon_kinds) return false;
    px[0].assign(32 * 32, 0);
    px[1].assign(32 * 32, 0);
    if (t.generation == 1) {
        // The shapes share tiles with overworld sprites. The game copies them
        // to sprite VRAM through a table of {source, tiles, bank, VRAM
        // address}: the same copy here gives the shape at tile kind*4 (step 1)
        // and kind*4 + 0x40 (step 2). Red and Blue have ten shapes in 28
        // copies; Yellow adds Pikachu's (kind 10) and a trade bubble, 30.
        const int dex = species;
        if (dex < 1 || dex > 151) return false;
        uint8_t kinds = uint8_t(peek_address(core, offset(t.icon_kinds, uint32_t((dex - 1) / 2)), 1));
        const int kind = (dex & 1) ? kinds >> 4 : kinds & 15;
        if (kind > 10) return false;
        std::vector<uint8_t> vram(128 * 16, 0);
        for (int e = 0; e < 30; ++e) {
            uint8_t entry[6];
            read_bytes(core, offset(t.icons, uint32_t(e * 6)), entry, 6);
            const uint32_t source = entry[0] | entry[1] << 8, dest = entry[4] | entry[5] << 8;
            const int tiles = entry[2], bank = entry[3];
            if (dest < 0x8000 || dest >= 0x8800 || tiles > 8 || source < 0x4000 || source >= 0x8000) {
                if (e < 28) return false;
                break; // past the end of a 28-copy table
            }
            const size_t at = (dest - 0x8000) / 16 * 16;
            for (int b = 0; b < tiles * 16 && at + size_t(b) < vram.size(); ++b)
                vram[at + size_t(b)] = uint8_t(peek_address(core, banked(bank, source + uint32_t(b)), 1));
        }
        // The species' Super Game Boy colours: white, light, dark, black.
        uint32_t colours[4] = {0, 0xFFFFFFFFu, 0xFF909090u, 0xFF202020u};
        if (t.palette_index && t.palettes) {
            const int pal = int(peek_address(core, offset(t.palette_index, uint32_t(dex)), 1));
            if (pal < 0x40)
                for (int c = 1; c < 4; ++c)
                    colours[c] = rgb555(peek_address(core, offset(t.palettes, uint32_t(pal * 8 + c * 2)), 2));
        }
        constexpr int helix = 2, ball = 1;
        for (int step = 0; step < 2; ++step) {
            const int base = (kind << 2) + (step ? 0x40 : 0);
            // The ball and the helix don't change shape: they hop a pixel.
            const int lift = (kind == helix || kind == ball) && step ? 1 : 0;
            for (int row = 0; row < 2; ++row) {
                const uint8_t* left = &vram[size_t(base + row * 2) * 16];
                if (kind == helix) {
                    put_tile(px[step], left, 0, row * 8 + lift, false, colours);
                    put_tile(px[step], &vram[size_t(base + row * 2 + 1) * 16], 8, row * 8 + lift, false, colours);
                } else {
                    // Left half drawn, right half its mirror.
                    put_tile(px[step], left, 0, row * 8 + lift, false, colours);
                    put_tile(px[step], left, 8, row * 8 + lift, true, colours);
                }
            }
        }
        return true;
    }
    // Generation 2: an icon number per species, eight tiles an icon (two
    // 16x16 steps, rows of two tiles), in the bank of the pointer table.
    const int number = species & 0xFFFF;
    if (number < 1 || number > 251) return false;
    const int icon = int(peek_address(core, offset(t.icon_kinds, uint32_t(number - 1)), 1));
    const uint32_t pointer = peek_address(core, offset(t.icons, uint32_t(icon * 2)), 2);
    if (pointer < 0x4000 || pointer >= 0x8000) return false;
    const int bank = int((t.icons >> 16) & 0xFF);
    uint8_t tiles[8 * 16];
    read_bytes(core, banked(bank, pointer), tiles, int(sizeof tiles));
    uint32_t colours[4] = {0, 0xFFF8F8F8u, 0xFF909090u, 0xFF202020u};
    if (t.palettes) {
        // Two colours per species, normal then shiny, lighter first: the
        // icon's light grey takes the first, its dark grey the second. (The
        // game draws every icon in one palette; these follow its pictures.)
        const uint32_t at = uint32_t(number * 8 + ((species & 0x10000) ? 4 : 0));
        colours[1] = rgb555(peek_address(core, offset(t.palettes, at), 2));
        colours[2] = rgb555(peek_address(core, offset(t.palettes, at + 2), 2));
    }
    for (int step = 0; step < 2; ++step)
        for (int i = 0; i < 4; ++i)
            put_tile(px[step], tiles + (step * 4 + i) * 16, (i % 2) * 8, (i / 2) * 8, false, colours);
    return true;
}

bool read_saga1(Core& core, uint32_t slots, std::vector<PartyMember>& out, uint64_t& key) {
    std::vector<PartyMember> team;
    uint64_t k = 1469598103934665603ull;
    for (int i = 0; i < 4; ++i) {
        uint8_t r[16];
        read_bytes(core, offset(slots, uint32_t(i * 32)), r, 16);
        PartyMember m;
        m.hp = r[6] | r[7] << 8;
        m.max_hp = r[8] | r[9] << 8;
        if (m.max_hp == 0 || r[1] == 0x00 || r[1] == 0xFF) continue; // an empty place
        if (m.max_hp > 9999 || m.hp > m.max_hp) return false;
        // Digits from 80, capitals from 8A, small letters from A4; FF ends a short name.
        for (int c = 1; c <= 4 && r[c] != 0xFF; ++c) {
            uint8_t b = r[c];
            if (b >= 0x80 && b <= 0x89) m.name += char('0' + b - 0x80);
            else if (b >= 0x8A && b <= 0xA3) m.name += char('A' + b - 0x8A);
            else if (b >= 0xA4 && b <= 0xBD) m.name += char('a' + b - 0xA4);
            else if (b == 0xBE) m.name += '-';
        }
        m.detail = r[0] < 2 ? "Human" : r[0] < 4 ? "Mutant" : "Monster";
        for (int c = 0; c < 10; ++c) k = mix_key(k, r[c]);
        team.push_back(std::move(m));
    }
    key = k;
    out = std::move(team);
    return true;
}

// Dragon Warrior Monsters: three 0x95-byte monster records from `slots`.
// The monster storage starts at CAC1, and the game puts the party at its
// front (551D0: each member copied to C300, then the three to CAC1), leaving
// a byte per place at `places` (C8C4), FF where the party has nobody. Name at 1 (8 letters, F0 ends it), species
// at 9, level at 4B, HP and max HP at 50 and 52, MP and max MP at 54 and 56
// (little-endian); layout from the dwm1 disassembly's structures/monster.asm.
// `species_names` is the ROM's list of species names, F0 after each.
std::string dwm1_text(const uint8_t* bytes, int max) {
    std::string out;
    for (int i = 0; i < max && bytes[i] != 0xF0; ++i) {
        const uint8_t b = bytes[i];
        if (b <= 0x09) out += char('0' + b);
        else if (b >= 0x24 && b <= 0x3D) out += char('A' + b - 0x24);
        else if (b >= 0x3E && b <= 0x57) out += char('a' + b - 0x3E);
        else if (b == 0x62) out += ' ';
        else if (b == 0x9C) out += '-';
        else if (b == 0x5C) out += '\'';
        else if (b == 0x5F) out += '.';
    }
    return out;
}

bool read_dwm1(Core& core, uint32_t slots, uint32_t places, uint32_t species_names, std::vector<PartyMember>& out,
               uint64_t& key) {
    std::vector<PartyMember> team;
    uint64_t k = 1469598103934665603ull;
    for (int i = 0; i < 3; ++i) {
        if (places && peek_address(core, offset(places, uint32_t(i)), 1) == 0xFF) continue;
        uint8_t r[0x58];
        read_bytes(core, offset(slots, uint32_t(i * 0x95)), r, int(sizeof r));
        PartyMember m;
        m.level = r[0x4B];
        m.hp = r[0x50] | r[0x51] << 8;
        m.max_hp = r[0x52] | r[0x53] << 8;
        m.pp = r[0x54] | r[0x55] << 8;
        m.max_pp = r[0x56] | r[0x57] << 8;
        if (m.level == 0 || m.max_hp == 0 || r[1] == 0xF0) continue; // an empty place
        if (m.level > 99 || m.max_hp > 999 || m.hp > m.max_hp || m.pp > m.max_pp || r[9] > 0xDE) return false;
        m.name = dwm1_text(r + 1, 8);
        if (species_names) {
            // The species' name: the r[9]th of the list.
            uint32_t a = species_names;
            for (int skip = 0; skip < r[9]; ++skip) {
                for (int n = 0; n < 16 && peek_address(core, a, 1) != 0xF0; ++n) a = offset(a, 1);
                a = offset(a, 1);
            }
            uint8_t name[12];
            read_bytes(core, a, name, 12);
            m.detail = dwm1_text(name, 12);
        }
        m.species = r[9];
        for (int c : {int(r[9]), m.level, m.hp, m.max_hp, m.pp, m.max_pp}) k = mix_key(k, uint64_t(c));
        for (char c : m.name) k = mix_key(k, uint8_t(c));
        team.push_back(std::move(m));
    }
    key = k;
    out = std::move(team);
    return true;
}

} // namespace play
