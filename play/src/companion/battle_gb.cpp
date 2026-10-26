// SPDX-License-Identifier: GPL-3.0-or-later
// format = pokemon_battle: the Game Boy Pokémon games' battle, read live from
// work RAM, while one is on (the team panel comes back after it). The
// opponent on top: its party icon, name, level, types and HP. Yours under
// it, with its four moves: each one's type, power and the PP it has left of
// its most. Names, moves and types come from the cartridge's own tables.
//
// The battle structs, from the pret disassemblies (macros/ram.asm):
//   generation 1  species, HP (big-endian), box level, status, type 1, type 2,
//                 catch rate, 4 moves, DVs, level at 14, max HP at 15, the
//                 four stats, 4 PP at 25
//   generation 2  species, item, 4 moves, DVs at 6, 4 PP at 8, happiness,
//                 level at 13, status, HP at 16, max HP at 18, five stats,
//                 type 1 and 2 at 30
// A move's PP byte keeps its PP Ups in the top two bits. The move table has a
// record per move: animation, effect, power, type, accuracy, PP (and, in
// generation 2, the effect's chance).
#include "companion/companion.hpp"
#include "companion/party_gb.hpp"
#include "companion/view.hpp"
#include "ui/kit.hpp"
#include "core/core.hpp"

#include <algorithm>
#include <map>

namespace play {
namespace {

uint32_t at(uint32_t address, uint32_t n) { return (address & 0xFFFF0000u) | ((address + n) & 0xFFFF); }

struct Move {
    int id = 0, pp = 0, max_pp = 0, power = 0, type = 0;
};

struct Fighter {
    int species = 0; // the Pokédex number (+0x10000 when shiny), as the team panel keeps it
    int level = 0, hp = 0, max_hp = 0, type1 = 0, type2 = 0;
    std::string name;
    Move moves[4];
};

// Each type's colour, by the name the cartridge gives it.
Rgb type_colour(const std::string& t) {
    static const std::map<std::string, Rgb> colours = {
        {"NORMAL", {168, 168, 120}},  {"FIGHTING", {192, 48, 40}}, {"FLYING", {168, 144, 240}},
        {"POISON", {160, 64, 160}},   {"GROUND", {224, 192, 104}}, {"ROCK", {184, 160, 56}},
        {"BUG", {168, 184, 32}},      {"GHOST", {112, 88, 152}},   {"STEEL", {184, 184, 208}},
        {"FIRE", {240, 128, 48}},     {"WATER", {104, 144, 240}},  {"GRASS", {120, 200, 80}},
        {"ELECTRIC", {248, 208, 48}}, {"PSYCHIC", {248, 88, 136}}, {"ICE", {152, 216, 216}},
        {"DRAGON", {112, 56, 248}},   {"DARK", {112, 88, 72}}};
    auto found = colours.find(t);
    return found == colours.end() ? Rgb{120, 120, 140} : found->second;
}

class Battle final : public View {
public:
    bool parse(const ViewSpec& spec, std::string& error) {
        on_ = spec.on;
        auto get = [&](const char* key, uint32_t& out, bool needed) {
            auto found = spec.values.find(key);
            if (found == spec.values.end()) return !needed;
            return parse_address(found->second, out);
        };
        auto gen = spec.values.find("generation");
        tables_.generation = gen != spec.values.end() && gen->second == "2" ? 2 : 1;
        for (auto [key, out] : {std::pair<const char*, uint32_t*>{"player", &player_},
                                {"player_name", &player_name_},
                                {"enemy", &enemy_},
                                {"enemy_name", &enemy_name_},
                                {"move_names", &move_names_},
                                {"moves", &moves_},
                                {"type_names", &type_names_}})
            if (!get(key, *out, true)) {
                error = std::string("[view] pokemon_battle needs an address for ") + key;
                return false;
            }
        // The team panel's icon tables, the same keys as [party].
        if (!get("dex_order", tables_.dex_order, tables_.generation == 1) ||
            !get("icon_kinds", tables_.icon_kinds, false) || !get("icons", tables_.icons, false) ||
            !get("icon_palette_index", tables_.palette_index, false) ||
            !get("icon_palettes", tables_.palettes, false)) {
            error = "[view] pokemon_battle: a bad icon table address";
            return false;
        }
        return true;
    }

    void update(Core& live, uint64_t frame) override {
        if (frame % 15) return;
        core_ = &live;
        playing_ = on_.match(live);
        if (!playing_) return;
        Fighter you, them;
        valid_ = read(live, player_, player_name_, you) && read(live, enemy_, enemy_name_, them);
        if (!valid_) return;
        uint64_t k = 1469598103934665603ull;
        for (const Fighter* f : {&you, &them}) {
            for (int v : {f->species, f->level, f->hp, f->max_hp, f->type1, f->type2})
                k = (k ^ uint64_t(v)) * 1099511628211ull;
            for (const Move& m : f->moves) k = (k ^ uint64_t(m.id << 8 | m.pp)) * 1099511628211ull;
            for (char c : f->name) k = (k ^ uint8_t(c)) * 1099511628211ull;
        }
        you_ = you;
        them_ = them;
        key_ = k;
    }

    bool shown() const override { return playing_ && valid_; }
    uint64_t key(uint64_t frame) const override { return key_ ^ ((frame / 16) & 1); }

    void draw(Canvas& out, const Look& look, const Font& font, uint64_t frame) const override {
        backdrop(out, look);
        const int W = out.w(), H = out.h(), m = kit::margin, gap = kit::gap;
        const int them_h = 132;
        fighter(out, look, font, them_, {m, m, W - m * 2, them_h}, frame, false);
        fighter(out, look, font, you_, {m, m + them_h + gap, W - m * 2, H - m * 2 - them_h - gap}, frame, true);
    }

private:
    struct BattleIcon {
        std::vector<uint32_t> px[2];
    };

    bool read(Core& core, uint32_t base, uint32_t name_at, Fighter& f) const {
        uint8_t r[32];
        for (uint32_t i = 0; i < 32; ++i) r[i] = uint8_t(peek_address(core, at(base, i), 1));
        const bool gen1 = tables_.generation == 1;
        int index;
        uint8_t pp[4], moves[4];
        if (gen1) {
            index = r[0];
            f.species =
                tables_.dex_order ? int(peek_address(core, at(tables_.dex_order, uint32_t(index - 1)), 1)) : index;
            f.hp = r[1] << 8 | r[2];
            f.type1 = r[5];
            f.type2 = r[6];
            f.level = r[14];
            f.max_hp = r[15] << 8 | r[16];
            std::copy(r + 8, r + 12, moves);
            std::copy(r + 25, r + 29, pp);
        } else {
            index = f.species = r[0];
            f.level = r[13];
            f.hp = r[16] << 8 | r[17];
            f.max_hp = r[18] << 8 | r[19];
            f.type1 = r[30];
            f.type2 = r[31];
            std::copy(r + 2, r + 6, moves);
            std::copy(r + 8, r + 12, pp);
            const int atk = r[6] >> 4, def = r[6] & 15, spd = r[7] >> 4, spc = r[7] & 15;
            if (def == 10 && spd == 10 && spc == 10 && (atk & 2)) f.species |= 0x10000;
        }
        const int number = f.species & 0xFFFF;
        if (index == 0 || number == 0 || number > (gen1 ? 151 : 251) || f.max_hp == 0 || f.hp > f.max_hp ||
            f.level == 0 || f.level > 100)
            return false; // not there yet, or being written
        uint8_t name[11];
        for (uint32_t i = 0; i < 11; ++i) name[i] = uint8_t(peek_address(core, at(name_at, i), 1));
        f.name = pokemon12_text(name, 11, tables_.generation);
        const uint32_t size = gen1 ? 6 : 7;
        for (int i = 0; i < 4; ++i) {
            Move& m = f.moves[i];
            m.id = moves[i];
            if (!m.id || m.id > 251) {
                m.id = 0;
                continue;
            }
            const uint32_t record = at(moves_, uint32_t(m.id - 1) * size);
            m.power = int(peek_address(core, at(record, 2), 1));
            m.type = int(peek_address(core, at(record, 3), 1));
            const int base = int(peek_address(core, at(record, 5), 1)), ups = pp[i] >> 6;
            m.pp = pp[i] & 63;
            m.max_pp = std::min(base + base / 5 * ups, 61);
        }
        return true;
    }

    // A move's name: the id'th of a list of names that each end with 50, cached.
    const std::string& move_name(int id) const {
        auto found = move_name_cache_.find(id);
        if (found != move_name_cache_.end()) return found->second;
        uint32_t a = move_names_;
        for (int skip = 1; skip < id; ++skip) {
            for (int n = 0; n < 32 && peek_address(*core_, a, 1) != 0x50; ++n) a = at(a, 1);
            a = at(a, 1);
        }
        uint8_t bytes[16];
        for (uint32_t i = 0; i < 16; ++i) bytes[i] = uint8_t(peek_address(*core_, at(a, i), 1));
        return move_name_cache_[id] = pokemon12_text(bytes, 16, tables_.generation);
    }

    // A type's name, through the table of pointers to them (in the table's bank).
    const std::string& type_name(int type) const {
        auto found = type_cache_.find(type);
        if (found != type_cache_.end()) return found->second;
        const uint32_t pointer = peek_address(*core_, at(type_names_, uint32_t(type) * 2), 2);
        std::string name;
        if (pointer >= 0x4000 && pointer < 0x8000) {
            uint8_t bytes[12];
            for (uint32_t i = 0; i < 12; ++i)
                bytes[i] = uint8_t(peek_address(*core_, (type_names_ & 0xFFFF0000u) | (pointer + i), 1));
            name = pokemon12_text(bytes, 12, tables_.generation);
        }
        return type_cache_[type] = name;
    }

    const BattleIcon* icon(int species) const {
        auto found = icons_.find(species);
        if (found != icons_.end()) return found->second.px[0].empty() ? nullptr : &found->second;
        BattleIcon& ic = icons_[species];
        if (!pokemon_gb_icon(*core_, tables_, species, ic.px)) ic.px[0].clear();
        return ic.px[0].empty() ? nullptr : &ic;
    }

    // A type as a chip in the games' own colour.
    static int type_chip(Canvas& out, const Font& font, int x, int cy, const std::string& text) {
        const int w = font.measure(text, kit::small) + 20, h = 26;
        const Rgb c = type_colour(text);
        out.rounded_rect(x, cy - h / 2, w, h, h / 2, c);
        font.draw(out, x + 10, font.centred(kit::small, cy), text, kit::small, Look::ink_on(c));
        return w;
    }

    static Rgb hp_colour(const Fighter& f) {
        return f.hp * 2 > f.max_hp ? Rgb{56, 176, 96} : f.hp * 5 > f.max_hp ? Rgb{222, 170, 30} : Rgb{214, 64, 56};
    }

    void fighter(Canvas& out, const Look& look, const Font& font, const Fighter& f, const Box& b, uint64_t frame,
                 bool moves) const {
        card(out, look, b);
        const int pad = 16, head = moves ? 124 : b.h;
        const int scale = std::max(1, std::min(3, (head - pad * 2) / 32));
        int tx = b.x + pad;
        if (const BattleIcon* ic = icon(f.species)) {
            const int box = 32 * scale + 8, by = b.y + (head - box) / 2;
            out.rounded_rect(tx, by, box, box, 12, look.slot);
            const auto& px = ic->px[f.hp == 0 ? 0 : (frame / 16) & 1];
            out.blit_keyed(px.data(), 32, 32, tx + 4, by + 4, scale, 0);
            tx += box + pad;
        }
        if (!font.loaded()) return;
        const int big = kit::title, small = kit::small, right = b.x + b.w - pad;
        int line = b.y + pad + 24;
        const std::string level = look.tr("Lv") + " " + std::to_string(f.level);
        font.draw(out, right - font.measure(level, small), line, level, small, look.muted);
        font.draw(out, tx, line, font.fit(f.name, big, right - tx - font.measure(level, small) - pad), big,
                  f.hp ? look.ink : look.muted);
        line += 30;
        int cx = tx;
        for (int i = 0; i < (f.type2 == f.type1 ? 1 : 2); ++i) {
            const std::string& name = type_name(i ? f.type2 : f.type1);
            if (name.empty()) continue;
            cx += type_chip(out, font, cx, line, name) + 8;
        }
        // HP: a meter in the games' colours, the numbers beside it.
        line += 30;
        const std::string numbers = std::to_string(f.hp) + " / " + std::to_string(f.max_hp);
        const int bw = right - tx - font.measure(numbers, small) - pad;
        meter(out, look, {tx, line - 6, bw, 12}, double(f.hp) / std::max(1, f.max_hp), hp_colour(f));
        font.draw(out, right - font.measure(numbers, small), font.centred(small, line), numbers, small, look.ink);
        if (!moves) return;
        // The four moves: two by two, each its name, type, power and PP, raised like the menu's buttons.
        const int top = b.y + head, mgap = 10;
        int mw = (b.w - pad * 2 - mgap) / 2;
        const int mh = (b.y + b.h - pad - top - mgap) / 2;
        for (int i = 0; i < 4; ++i) {
            const Move& mv = f.moves[i];
            const int mx = b.x + pad + (i % 2) * (mw + mgap), my = top + (i / 2) * (mh + mgap);
            int w = mw;
            if (look.reserve_w && my + mh > out.h() - look.reserve_h && mx + w > out.w() - look.reserve_w)
                w = out.w() - look.reserve_w - kit::gap - mx;
            if (!mv.id) {
                sunken(out, look, {mx, my, w, mh}, 12);
                continue;
            }
            raised(out, look, mx, my, w, mh, 12, look.panel, 0, false);
            const std::string& type = type_name(mv.type);
            out.rounded_rect(mx + 6, my + 8, 6, mh - 16, 3, type_colour(type));
            const int ix = mx + 22;
            font.draw(out, ix, my + 8 + 20, font.fit(move_name(mv.id), small, w - 34), small,
                      mv.pp ? look.ink : look.muted);
            const int line2 = my + mh - 12;
            const std::string detail = type + (mv.power > 1 ? "  " + std::to_string(mv.power) : "");
            const std::string pp = std::to_string(mv.pp) + "/" + std::to_string(mv.max_pp);
            const int ppw = font.measure(pp, small);
            font.draw(out, ix, line2, font.fit(detail, small, w - 34 - ppw - 8), small, look.muted);
            const Rgb low = look.dark ? Rgb{240, 200, 64} : Rgb{150, 100, 0},
                      none = look.dark ? Rgb{240, 110, 100} : Rgb{180, 40, 32};
            const Rgb ppc = mv.pp == 0 ? none : mv.pp * 4 <= mv.max_pp ? low : look.ink;
            font.draw(out, mx + w - 12 - ppw, line2, pp, small, ppc);
        }
    }

    Condition on_;
    PokemonGbTables tables_;
    uint32_t player_ = 0, player_name_ = 0, enemy_ = 0, enemy_name_ = 0, move_names_ = 0, moves_ = 0, type_names_ = 0;
    Core* core_ = nullptr;
    bool playing_ = false, valid_ = false;
    Fighter you_, them_;
    uint64_t key_ = 0;
    mutable std::map<int, std::string> move_name_cache_, type_cache_;
    mutable std::map<int, BattleIcon> icons_;
};

} // namespace

std::unique_ptr<View> make_pokemon_battle(const ViewSpec& spec, std::string& error) {
    auto view = std::make_unique<Battle>();
    if (!view->parse(spec, error)) return nullptr;
    return view;
}

} // namespace play
