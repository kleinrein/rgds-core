// SPDX-License-Identifier: GPL-3.0-or-later
// The player's team, read live from the game's memory and drawn on the second
// screen: one card per member with its icon, name, level, HP and status, the
// way the DS games keep the team in view on the touch screen.
//
// Formats:
//   pokemon3   Pokémon Ruby, Sapphire, Emerald, FireRed and LeafGreen: the
//              100-byte party structs (species in the encrypted block), icons
//              from the ROM's own icon table.
//   pokemon1   Pokémon Red and Blue, and
//   pokemon2   Gold, Silver and Crystal: see companion/party_gb.hpp.
//   saga1      The Final Fantasy Legend: see companion/party_gb.hpp.
//   goldensun  Golden Sun: a byte of party flags (bit n: character n) and the
//              characters' 0x14C-byte records (name, level, HP and PP).
#pragma once
#include "companion/profile.hpp"
#include "ui/canvas.hpp"

#include <map>
#include <string>
#include <vector>

namespace play {

class Core;
struct Look;
struct PokemonGbTables;

struct PartyMember {
    std::string name;
    int species = 0, level = 0, hp = 0, max_hp = 0;
    int pp = 0, max_pp = 0; // a second bar (Golden Sun's PP); max_pp 0: none
    std::string detail;     // shown where the level is for games without levels (Final Fantasy Legend's race)
    bool egg = false;
    std::string status; // "PSN", "BRN", ... or "" (fainted: hp 0)
};

class Party {
public:
    explicit Party(const PartySpec& spec) : spec_(spec) {}
    // Reads the team; true when it changed since the last read.
    bool read(Core& core);
    bool empty() const { return members_.empty(); }
    // Changes when the drawing would (the team, or the icons' step).
    uint64_t key(uint64_t frame) const { return key_ ^ ((frame / 16) & 1 ? 0x5bd1e995ull : 0); }
    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const;

private:
    struct Icon {
        std::vector<uint32_t> px[2]; // 32x32, two steps; 0 = see-through
    };
    const Icon* icon(int species) const;
    bool read_pokemon3(Core& core, std::vector<PartyMember>& team, uint64_t& key);
    bool read_goldensun(Core& core, std::vector<PartyMember>& team, uint64_t& key);
    PokemonGbTables gb_tables() const;

    const PartySpec& spec_;
    std::vector<PartyMember> members_;
    uint64_t key_ = 0;
    Core* core_ = nullptr;
    mutable std::map<int, Icon> icons_;
};

// Pokémon generation 3 text (the international character set) as UTF-8.
std::string pokemon3_text(const uint8_t* bytes, int max);

} // namespace play
