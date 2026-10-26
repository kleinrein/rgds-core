// SPDX-License-Identifier: GPL-3.0-or-later
// The team in the Game Boy Pokémon games, read live from work RAM, and their
// party-menu icons drawn from the cartridge's own tables (companion/party.hpp
// draws the cards).
//
//   pokemon1  Red, Blue (and Green): 44-byte party structs, internal species
//             numbers turned into Pokédex numbers through the ROM's dex order,
//             the ten shared icon shapes in the colours of the Super Game
//             Boy's palette for each species.
//   pokemon2  Gold, Silver and Crystal: 48-byte party structs, one icon per
//             species in its own normal (or shiny) colours. On a Game Boy
//             Color the party sits in work RAM bank 1, so profiles give
//             banked addresses ("1:DCD7").
#pragma once
#include "companion/party.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace play {

class Core;

// Generation 1 and 2 text (both share the letters, digits and marks; é differs).
std::string pokemon12_text(const uint8_t* bytes, int max, int generation);

// Where the team and its icon tables are, from the profile's [party].
struct PokemonGbTables {
    int generation = 1;
    uint32_t count = 0, slots = 0, names = 0;
    uint32_t dex_order = 0;     // gen 1: internal number - 1 -> Pokédex number
    uint32_t icon_kinds = 0;    // gen 1: a nybble per Pokédex number; gen 2: a byte per species
    uint32_t icons = 0;         // gen 1: the icon tile loads; gen 2: one pointer per icon
    uint32_t palette_index = 0; // gen 1: a palette per Pokédex number
    uint32_t palettes = 0;      // gen 1: the SGB palettes; gen 2: each species' colours
};

// Reads the team. False while it is being written (or not loaded yet). `key`
// changes whenever the drawing would.
bool read_pokemon_gb(Core& core, const PokemonGbTables& t, std::vector<PartyMember>& out, uint64_t& key);

// The member's icon as two 32x32 steps (the 16x16 icon doubled), 0 = see-through.
// `species` is PartyMember::species as read_pokemon_gb sets it: the Pokédex
// number, with 0x10000 added for a shiny one in generation 2. False when the tables don't hold up (another
// release of the game): the card shows without an icon.
bool pokemon_gb_icon(Core& core, const PokemonGbTables& t, int species, std::vector<uint32_t> (&px)[2]);

// The Final Fantasy Legend (SaGa): four 32-byte member records from `slots`:
// race, a 4-letter name, HP and max HP (little-endian), strength, defence,
// agility, mana, then eight abilities with their uses. An empty record is all
// zero. False while one is being written.
bool read_saga1(Core& core, uint32_t slots, std::vector<PartyMember>& out, uint64_t& key);

// Dragon Warrior Monsters: the three monsters of the party from `slots` (0x95
// bytes each; `places`: a byte each, FF for an empty place), with their
// species' names from the ROM list `species_names`.
bool read_dwm1(Core& core, uint32_t slots, uint32_t places, uint32_t species_names, std::vector<PartyMember>& out,
               uint64_t& key);

} // namespace play
