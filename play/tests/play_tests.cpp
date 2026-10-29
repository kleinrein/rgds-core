// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for RG DS Play: profiles, conditions, the capture engine against a
// scripted fake game, and the save rules. Run: build/play/host/play-tests
#include "app/cheats.hpp"
#include "app/storage.hpp"
#include "companion/companion.hpp"
#include "companion/map_capture.hpp"
#include "companion/party_gb.hpp"
#include "companion/profile.hpp"
#include "companion/expr.hpp"
#include "companion/view.hpp"
#include "companion/dock.hpp"
#include "ui/kit.hpp"
#include "ui/canvas.hpp"
#include "ui/colour.hpp"
#include "ui/gb_palettes.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace play;

namespace {

int failures = 0, checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

// A game with one room and a map screen: SELECT opens the map after `delay`
// frames; byte 0x02000000 reads 3 while it is up. The marker blinks every
// 8 frames. Its whole state is a few bytes, so save/load are exact.
class FakeGame final : public Core {
public:
    struct State {
        uint8_t scene = 1; // 1 play, 2 opening, 3 map
        uint8_t timer = 0;
        uint16_t prev = 0;
        uint32_t frame = 0;
        uint16_t room = 0;
    } s;
    int delay = 6;
    bool blank_map = false, never_opens = false;
    int shadows_made = 0;
    mutable int peeks = 0;
    std::vector<uint32_t> fb = std::vector<uint32_t>(240 * 160);

    const char* system() const override { return "gba"; }
    std::string game_code() const override { return "FAKE"; }
    std::string header_title() const override { return "FAKE"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t buttons) override {
        uint16_t pressed = buttons & ~s.prev;
        s.prev = static_cast<uint16_t>(buttons);
        ++s.frame;
        if (s.scene == 1 && (pressed & Select) && !never_opens) {
            s.scene = 2;
            s.timer = static_cast<uint8_t>(delay);
        } else if (s.scene == 2 && --s.timer == 0) s.scene = 3;
        else if (s.scene == 3 && (pressed & B)) s.scene = 1;
        draw();
    }
    void draw() {
        uint32_t bg = s.scene == 3 ? rgb_to_px(0x29007B) : s.scene == 2 ? 0xFF000000u : rgb_to_px(0x206020 + s.room);
        std::fill(fb.begin(), fb.end(), bg);
        if (s.scene == 3 && !blank_map) {
            for (int y = 40; y < 120; ++y)
                for (int x = 60; x < 180; ++x) fb[y * 240 + x] = rgb_to_px(0x60B0FF);
            if ((s.frame / 8) & 1)
                for (int y = 70; y < 80; ++y)
                    for (int x = 100; x < 110; ++x) fb[y * 240 + x] = rgb_to_px(0xFFFFFF);
        }
    }
    Picture picture() const override { return {fb.data(), 240, 160, 240}; }
    bool save_state(std::vector<uint8_t>& out) override {
        out.assign(reinterpret_cast<uint8_t*>(&s), reinterpret_cast<uint8_t*>(&s) + sizeof(s));
        return true;
    }
    bool load_state(const std::vector<uint8_t>& in) override {
        if (in.size() != sizeof(s)) return false;
        std::copy(in.begin(), in.end(), reinterpret_cast<uint8_t*>(&s));
        draw();
        return true;
    }
    uint32_t peek(uint32_t address, int) override {
        ++peeks;
        if (address == 0x02000000) return s.scene;
        if (address == 0x02000002) return s.room;
        return 0;
    }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32768; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override {
        auto copy = std::make_unique<FakeGame>();
        copy->delay = delay;
        copy->blank_map = blank_map;
        copy->never_opens = never_opens;
        const_cast<FakeGame*>(this)->shadows_made++;
        return copy;
    }
};

const char* fake_profile = R"(# a comment
[game]
title = Fake Quest
codes = FAKE fak2

[companion]
live = mem8[02000000] == 1
room = mem16[02000002]
interval = 30

[map]
open = SELECT:2   # press it
timeout = 40
stable = 3
max_diff = 120
ready = mem8[02000000] == 3
pixels = 1,1=#29007B 238,158=#29007B
background = #29007B
min_foreground = 100
anim_frames = 6
anim_step = 4
)";

Profile parse_ok(const std::string& text) {
    Profile p;
    std::string error;
    bool ok = parse_profile(text, "test.ini", p, error);
    if (!ok) std::fprintf(stderr, "parse: %s\n", error.c_str());
    CHECK(ok);
    return p;
}

void test_parsing() {
    Condition c;
    std::string e;
    CHECK(parse_condition("mem8[03000BDE] == 03", c, e) && c.terms.size() == 1 && c.terms[0].ref.address == 0x03000BDE);
    CHECK(parse_condition("mem16[0x02000000] & FF in 2..5 and mem32[02000004] != 0|1", c, e) && c.terms.size() == 2);
    CHECK(c.terms[0].ref.mask == 0xFF && c.terms[0].op == Condition::Term::In && c.terms[1].values.size() == 2);
    CHECK(!parse_condition("mem8[zz] == 1", c, e));
    CHECK(!parse_condition("mem8[02000000] = 1", c, e));
    CHECK(!parse_condition("mem8[02000000] == 1 and", c, e));
    CHECK(!parse_condition("ram[02000000] == 1", c, e));
    uint32_t b;
    CHECK(parse_buttons("B+UP", b) && b == (B | Up));
    CHECK(parse_buttons("-", b) && b == 0);
    CHECK(!parse_buttons("JUMP", b));

    Profile p = parse_ok(fake_profile);
    CHECK(p.title == "Fake Quest" && p.codes.size() == 2 && p.codes[1] == "FAK2");
    CHECK(p.has_map && p.map.open.size() == 1 && p.map.open[0].buttons == Select && p.map.open[0].frames == 2);
    CHECK(p.map.pixels.size() == 2 && p.map.pixels[0].rgb == 0x29007B && p.map.has_background);
    CHECK(p.interval == 30 && p.room.size() == 1 && p.room[0].size == 2);

    Profile bad;
    CHECK(!parse_profile("[game]\ntitle = x\n", "x.ini", bad, e) && e.find("codes") != std::string::npos);
    CHECK(!parse_profile("[game]\ntitle = x\ncodes = AB\n[map]\nopen = A\n", "x.ini", bad, e));
    CHECK(!parse_profile("[game]\ntitle = x\ncodes = AB\nspeed = 3\n", "x.ini", bad, e) && e.find("x.ini:4") == 0);
}

void test_shipped_profiles() {
    ProfileLibrary lib;
    std::vector<std::string> errors;
    lib.load_directory(PLAY_PROFILES "/gba", errors);
    for (const auto& e : errors) std::fprintf(stderr, "profile error: %s\n", e.c_str());
    CHECK(errors.empty());
    CHECK(lib.find("gba", "AMTE") && lib.find("gba", "A2CE") && lib.find("gba", "ZRDT"));
    CHECK(lib.find("gba", "amte") != nullptr);
    CHECK(lib.find("gb", "AMTE") == nullptr);
}

void test_capture() {
    Profile p = parse_ok(fake_profile);
    FakeGame live;
    live.s.frame = 3;
    std::vector<uint8_t> state;
    live.save_state(state);
    auto before = state;

    auto shadow = live.shadow();
    MapImage m = capture_map(*shadow, p.map, state);
    CHECK(m.ok);
    CHECK(m.w == 240 && m.h == 160);
    CHECK(m.frames.size() > 1); // the marker blinks
    CHECK(m.background == rgb_to_px(0x29007B));
    CHECK(m.frames_to_open < 2 + 2 + 6 + 10 + 6 * 4);

    // The live game is untouched by the capture.
    std::vector<uint8_t> after;
    live.save_state(after);
    CHECK(after == before);

    p.map.crop = {60, 40, 120, 80};
    m = capture_map(*shadow, p.map, state);
    CHECK(m.ok && m.w == 120 && m.h == 80 && m.frames[0][0] == rgb_to_px(0x60B0FF));

    FakeGame never;
    never.never_opens = true;
    m = capture_map(*never.shadow(), p.map, state);
    CHECK(!m.ok && m.why == "the map did not appear");

    FakeGame blank;
    blank.blank_map = true;
    p.map.crop = {};
    m = capture_map(*blank.shadow(), p.map, state);
    CHECK(!m.ok && m.why == "the map was blank");

    // A button held in the snapshot: the release frames make SELECT a fresh press.
    FakeGame held;
    held.run_frame(Select);
    held.s.scene = 1;
    held.save_state(state);
    m = capture_map(*held.shadow(), parse_ok(fake_profile).map, state);
    CHECK(m.ok);
    p.map.release = 0;
    m = capture_map(*held.shadow(), p.map, state);
    CHECK(!m.ok);
}

void test_companion_scheduling() {
    Profile p = parse_ok(fake_profile);
    FakeGame live;
    Companion c(live, &p, "Fake Quest", "GAME BOY ADVANCE");
    auto wait = [&] {
        while (c.busy()) usleep(100);
    };
    for (int i = 0; i < 200; ++i) {
        live.run_frame(0);
        c.after_frame(live);
        wait();
    }
    auto st = c.status();
    CHECK(st.showing_map && st.failures == 0);
    CHECK(st.requests >= 5 && st.requests <= 8); // every 30 frames, after the first

    // Entering a room triggers a capture well before the timer.
    int before = st.requests;
    live.s.room = 1;
    for (int i = 0; i < 16; ++i) {
        live.run_frame(0);
        c.after_frame(live);
        wait();
    }
    CHECK(c.status().requests == before + 1);

    // While the map is open in the live game (not gameplay), nothing is requested.
    live.run_frame(Select);
    before = c.status().requests;
    for (int i = 0; i < 120; ++i) {
        live.run_frame(0);
        c.after_frame(live);
        wait();
    }
    CHECK(c.status().requests == before);

    Canvas out(640, 480);
    Font font;
    c.render(out, Look{}, font, Words{});
    CHECK(out.at(2, 2) == rgb_to_px(0x29007B)); // edge to edge on the map's colour
}

std::string temp_dir() {
    char tmpl[] = "/tmp/play-tests-XXXXXX";
    return mkdtemp(tmpl);
}

void write(const std::string& path, const std::string& text) {
    std::string e;
    CHECK(write_file_atomic(path, std::vector<uint8_t>(text.begin(), text.end()), e));
}
std::string read(const std::string& path) {
    std::vector<uint8_t> b;
    if (!read_file(path, b)) return "<missing>";
    return std::string(b.begin(), b.end());
}

void test_cheats() {
    auto codes = parse_cht("cheats = 3\n\ncheat0_desc = \"Infinite lives\"\ncheat0_code = \"82003DE4+0063\"\n"
                           "cheat0_enable = false\ncheat1_desc = \"No code\"\ncheat2_code = \"01FF12D3\"\n"
                           "cheat2_enable = true\n");
    CHECK(codes.size() == 2); // a cheat with no code is dropped
    CHECK(codes[0].name == "Infinite lives" && codes[0].code == "82003DE4+0063" && !codes[0].on);
    CHECK(codes[1].name == "Code 3" && codes[1].on); // unnamed: numbered, the file's enable flag is the start

    std::string dir = temp_dir(), roms = dir + "/Roms/GBA", root = dir + "/play";
    make_directories(roms);
    write(roms + "/Hero (USA).gba", "rom");
    CheatBook none;
    CHECK(!none.load(root, "gba", roms + "/Hero (USA).gba"));
    write(roms + "/Hero (USA).cht", "cheats = 2\ncheat0_desc = \"A\"\ncheat0_code = \"11111111 2222\"\n"
                                    "cheat1_desc = \"B\"\ncheat1_code = \"33333333 4444\"\ncheat1_enable = true\n");
    CheatBook book;
    CHECK(book.load(root, "gba", roms + "/Hero (USA).gba"));
    CHECK(book.codes().size() == 2 && !book.codes()[0].on && book.codes()[1].on);
    CHECK(book.set(0, true) && book.set(1, false) && !book.set(2, true));
    CheatBook again; // the choice is remembered by name, over the file's own flags
    CHECK(again.load(root, "gba", roms + "/Hero (USA).gba"));
    CHECK(again.codes()[0].on && !again.codes()[1].on);
}

void test_saves() {
    std::string dir = temp_dir();
    std::string roms = dir + "/Roms/GBA", root = dir + "/play";
    make_directories(roms);
    write(roms + "/Hero (USA).gba", "rom");
    write(roms + "/Hero (USA).srm", "old-save");

    SaveSlot slot;
    std::string e;
    CHECK(prepare_save(root, "gba", roms + "/Hero (USA).gba", {}, slot, e));
    CHECK(slot.imported_from == roms + "/Hero (USA).srm");
    CHECK(std::string(slot.bytes.begin(), slot.bytes.end()) == "old-save");
    CHECK(read(roms + "/Hero (USA).srm") == "old-save"); // the original stays
    CHECK(read(root + "/saves/gba/Hero (USA).sav") == "old-save");

    // Next start: our save wins over the outside one, and the previous save is backed up.
    write(roms + "/Hero (USA).srm", "outside-changed");
    write(root + "/saves/gba/Hero (USA).sav", "v2");
    CHECK(prepare_save(root, "gba", roms + "/Hero (USA).gba", {}, slot, e));
    CHECK(slot.imported_from.empty() && std::string(slot.bytes.begin(), slot.bytes.end()) == "v2");
    CHECK(read(root + "/saves/gba/backups/Hero (USA).sav.1") == "v2");
    write(root + "/saves/gba/Hero (USA).sav", "v3");
    CHECK(prepare_save(root, "gba", roms + "/Hero (USA).gba", {}, slot, e));
    CHECK(prepare_save(root, "gba", roms + "/Hero (USA).gba", {}, slot, e)); // same bytes: no extra rotation
    CHECK(read(root + "/saves/gba/backups/Hero (USA).sav.1") == "v3");
    CHECK(read(root + "/saves/gba/backups/Hero (USA).sav.2") == "v2");
    CHECK(read(root + "/saves/gba/backups/Hero (USA).sav.3") == "<missing>");

    // The writer replaces the file whole; no temporary file is left behind.
    {
        SaveWriter w(root + "/saves/gba/Hero (USA).sav");
        w.submit({'v', '4'});
        w.submit({'v', '5'});
        w.finish();
        CHECK(w.written() >= 1 && w.last_error().empty());
    }
    CHECK(read(root + "/saves/gba/Hero (USA).sav") == "v5");
    CHECK(read(root + "/saves/gba/Hero (USA).sav.writing") == "<missing>");

    // An unreadable save stops the start instead of being replaced.
    std::string locked = root + "/saves/gba/Locked.sav";
    write(locked, "keep");
    chmod(locked.c_str(), 0);
    if (geteuid() != 0) {
        CHECK(!prepare_save(root, "gba", roms + "/Locked.gba", {}, slot, e));
        CHECK(e.find("could not be read") != std::string::npos);
    }
    chmod(locked.c_str(), 0644);

    // Extra sources (stock RetroArch's folder) are searched after the ROM's own folder.
    make_directories(dir + "/retro");
    write(dir + "/retro/Other.srm", "stock");
    CHECK(prepare_save(root, "gba", roms + "/Other.gba", {dir + "/retro"}, slot, e));
    CHECK(slot.imported_from == dir + "/retro/Other.srm" && read(dir + "/retro/Other.srm") == "stock");
}

void test_canvas() {
    Canvas c(8, 8);
    c.fill({0, 0, 0});
    uint32_t src[4] = {rgb_to_px(0xFF0000), rgb_to_px(0x00FF00), rgb_to_px(0x0000FF), rgb_to_px(0xFFFFFF)};
    c.blit(src, 2, 2, 2, 2, 2);
    CHECK(c.at(2, 2) == rgb_to_px(0xFF0000) && c.at(3, 3) == rgb_to_px(0xFF0000) && c.at(4, 2) == rgb_to_px(0x00FF00));
    CHECK(c.at(5, 5) == rgb_to_px(0xFFFFFF) && c.at(0, 0) == rgb_to_px(0));
    Font f;
    if (f.load(PLAY_FONTS)) {
        CHECK(f.measure("Metroid", 28) > 60 && f.measure("Metroid", 28) < 200);
        std::string fitted = f.fit("A very long title that will not fit at all", 28, 200);
        CHECK(f.measure(fitted, 28) <= 200 && fitted.size() > 3);
    }
}

} // namespace

// Game Boy memory as a map of bytes: plain addresses, and banked ones (ROM
// banks at 4000-7FFF, work RAM banks at D000-DFFF) as core.hpp's banked().
class GbMemory final : public Core {
public:
    std::map<uint32_t, uint8_t> bytes;
    int mapped = 6; // the work RAM bank the "game" has switched in
    void put(uint32_t address, std::initializer_list<uint8_t> values) {
        for (uint8_t v : values) bytes[address++] = v;
    }
    const char* system() const override { return "gbc"; }
    std::string game_code() const override { return "TEST"; }
    std::string header_title() const override { return "TEST"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {}
    Picture picture() const override { return {}; }
    bool save_state(std::vector<uint8_t>&) override { return false; }
    bool load_state(const std::vector<uint8_t>&) override { return false; }
    uint32_t byte(uint32_t key) const {
        auto f = bytes.find(key);
        return f == bytes.end() ? 0 : f->second;
    }
    uint32_t peek(uint32_t address, int size) override {
        // What the bus shows: the mapped bank at D000-DFFF.
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) {
            uint32_t a = address + uint32_t(i);
            v |= (a >= 0xD000 && a < 0xE000 ? byte(banked(mapped, a)) : byte(a)) << (8 * i);
        }
        return v;
    }
    uint32_t peek_bank(uint32_t address, int bank, int size) override {
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v |= byte(banked(bank, address + uint32_t(i))) << (8 * i);
        return v;
    }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32768; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }
};

void test_game_boy() {
    // Banked addresses in conditions and keys.
    Condition c;
    std::string e;
    CHECK(parse_condition("mem8[1:DCD7] == 2", c, e) && c.terms[0].ref.address == banked(1, 0xDCD7));
    CHECK(!parse_condition("mem8[100:DCD7] == 2", c, e));
    uint32_t a;
    CHECK(parse_address("1C:590D", a) && a == banked(0x1C, 0x590D) && parse_address("D163", a) && a == 0xD163);
    GbMemory mem;
    mem.put(banked(1, 0xDCD7), {2});
    CHECK(c.match(mem)); // read from bank 1 though bank 6 is mapped

    Profile p = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[hud]\non = mem8[FF4A] == 80\n"
                         "strip = 0 128 160 16\n[party]\nformat = pokemon2\ncount = 1:DCD7\nslots = 1:DCDF\n"
                         "names = 1:DE41\nicons = 23:6BBF\n[view]\nformat = metroid2\non = mem8[FF9B] == 04\n"
                         "level_bank = D058\n");
    CHECK(p.has_hud && p.hud.strip.y == 128 && p.hud.strip.h == 16 && p.party.names == banked(1, 0xDE41));
    CHECK(p.has_view && p.view.format == "metroid2" && p.view.values.at("level_bank") == "D058");
    // A view missing its addresses is not made, with the reason.
    CHECK(make_view(p.view, e) == nullptr && e.find("camera") != std::string::npos);
    ViewSpec unknown;
    unknown.format = "zork";
    CHECK(make_view(unknown, e) == nullptr && e.find("zork") != std::string::npos);
    Profile bad;
    CHECK(!parse_profile("[game]\ntitle = x\ncodes = AB\n[hud]\non = mem8[FF4A] == 80\nstrip = 4 0 160 16\n", "x.ini",
                         bad, e));

    // Generation 2: two members in bank 1 (the second an egg), big-endian HP.
    mem.put(banked(1, 0xDCD7), {2, 0x9E, 0xFD, 0xFF});
    std::vector<uint8_t> totodile(48, 0), egg(48, 0);
    totodile[0] = 0x9E, totodile[31] = 5, totodile[32] = 0x08, totodile[34] = 0, totodile[35] = 19, totodile[37] = 21;
    totodile[21] = 0xAA, totodile[22] = 0xAA; // shiny DVs
    egg[0] = 0xAF, egg[31] = 5, egg[35] = 0, egg[37] = 10;
    for (int i = 0; i < 48; ++i) {
        mem.bytes[banked(1, 0xDCDF + uint32_t(i))] = totodile[size_t(i)];
        mem.bytes[banked(1, 0xDCDF + 48 + uint32_t(i))] = egg[size_t(i)];
    }
    mem.put(banked(1, 0xDE41), {0x93, 0x8E, 0x93, 0x8E, 0x83, 0x88, 0x8B, 0x84, 0x50});
    PokemonGbTables t;
    t.generation = 2;
    t.count = banked(1, 0xDCD7);
    t.slots = banked(1, 0xDCDF);
    t.names = banked(1, 0xDE41);
    std::vector<PartyMember> team;
    uint64_t key = 0, before;
    CHECK(read_pokemon_gb(mem, t, team, key) && team.size() == 2);
    CHECK(team[0].name == "TOTODILE" && team[0].level == 5 && team[0].hp == 19 && team[0].max_hp == 21);
    CHECK(team[0].status == "PSN" && team[0].species == (158 | 0x10000) && team[1].egg && team[1].name == "Egg");
    before = key;
    mem.bytes[banked(1, 0xDCDF + 35)] = 18;
    CHECK(read_pokemon_gb(mem, t, team, key) && key != before && team[0].hp == 18);
    mem.bytes[banked(1, 0xDCDF + 35)] = 99; // more than its maximum: mid-write
    CHECK(!read_pokemon_gb(mem, t, team, key));
    CHECK(pokemon12_text(std::vector<uint8_t>{0x80, 0xA1, 0xF7, 0xE7, 0x50, 0x80}.data(), 6, 1) == "Ab1!");

    // Final Fantasy Legend: the first and third places filled.
    GbMemory ffl;
    ffl.put(0xCC00, {0x00, 0x8B, 0xA4, 0x89, 0x80, 0xB1, 60, 0, 60, 0});
    ffl.put(0xCC40, {0x05, 0x8A, 0xFF, 0xFF, 0xFF, 0x00, 0x2C, 0x01, 0x2C, 0x01});
    CHECK(read_saga1(ffl, 0xCC00, team, key) && team.size() == 2);
    CHECK(team[0].name == "Ba90" && team[0].detail == "Human" && team[0].hp == 60 && team[0].level == 0);
    CHECK(team[1].name == "A" && team[1].detail == "Monster" && team[1].max_hp == 300);

    // Dragon Warrior Monsters: the first and second places filled, the third
    // empty (FF), species names from a list that ends each with F0.
    GbMemory dwm;
    std::vector<uint8_t> slib(0x95, 0);
    slib[1] = 0x36, slib[2] = 0x49, slib[3] = 0x46, slib[4] = 0x3F, slib[5] = 0xF0; // "Slib"
    slib[9] = 1, slib[0x4B] = 7, slib[0x50] = 22, slib[0x52] = 30, slib[0x54] = 4, slib[0x56] = 12;
    for (uint32_t i = 0; i < 0x95; ++i) {
        dwm.bytes[0xCAC1 + i] = slib[i];
        dwm.bytes[0xCAC1 + 0x95 + i] = slib[i];
    }
    dwm.put(0xC8C4, {0, 1, 0xFF});
    // "Ab" then "Cd": species 1 is the second name
    dwm.put(banked(0x41, 0x5B1F), {0x24, 0x3F, 0xF0, 0x26, 0x41, 0xF0});
    CHECK(read_dwm1(dwm, 0xCAC1, 0xC8C4, banked(0x41, 0x5B1F), team, key) && team.size() == 2);
    CHECK(team[0].name == "Slib" && team[0].detail == "Cd" && team[0].level == 7 && team[0].hp == 22 &&
          team[0].max_pp == 12);
    dwm.put(0xC8C5, {0xFF}); // the second place emptied
    CHECK(read_dwm1(dwm, 0xCAC1, 0xC8C4, 0, team, key) && team.size() == 1 && team[0].detail.empty());
    dwm.put(0xCAC1 + 0x50, {31}); // more HP than its most: being written
    CHECK(!read_dwm1(dwm, 0xCAC1, 0xC8C4, 0, team, key));
}

// A Super Nintendo picture with Mode 7 (Super Mario Kart): the race view
// as a perspective on lines 24-106, the map's own perspective from 115, and
// above the race (the sky, not Mode 7 now) a flat block an earlier screen left
// in the core's record, as after a restart.
class FakeMode7 final : public Core {
public:
    FakeMode7() {
        fb.assign(256 * 224, rgb_to_px(0x3060A0));
        vram.assign(0x10000, 0);
        for (int i = 0; i < 0x4000; ++i) vram[size_t(i) * 2] = uint8_t(i & 1); // tiles 0 and 1 in turn
        for (int i = 0; i < 64; ++i) vram[1 + (1 << 7) + size_t(i) * 2] = 5;    // tile 1 is colour 5
        palette.assign(256, 0);
        palette[5] = 0x7C1F;
        for (int l = 8; l < 24; ++l) set(l, 1078, 0, 0, 2099, 512, 984, 384, 770); // stale and flat
        for (int l = 24; l <= 106; ++l) {
            const int scale = 5000 / (l - 20);
            set(l, scale, 0, 0, scale * 3, 1005, 162, 878, 61);
        }
        for (int l = 115; l < 239; ++l) set(l, 2000 - l * 4, 0, 0, 4000 - l * 8, 512, 984, 384, 770);
        for (uint32_t i = 0; i < 128; ++i) ram[0x7E0420 + 4 * i + 1] = 0xF0; // off the screen
    }
    const char* system() const override { return "snes"; }
    std::string game_code() const override { return "SUPER_MARIOKART"; }
    std::string header_title() const override { return "SUPER_MARIOKART"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {}
    Picture picture() const override { return {fb.data(), 256, 224, 256}; }
    bool save_state(std::vector<uint8_t>&) override { return false; }
    bool load_state(const std::vector<uint8_t>&) override { return false; }
    uint32_t peek(uint32_t address, int size) override {
        if (address == 0x7E10C1) return 0x80;
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) {
            auto found = ram.find(address + uint32_t(i));
            v |= uint32_t(found == ram.end() ? 0 : found->second) << (8 * i);
        }
        return v;
    }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32040; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }
    bool mode7(Mode7& out) const override {
        out = {lines.data(), palette.data(), vram.data(), 8};
        out.objsel = true, out.obj_base = 0x8000, out.obj_second = 0xA000, out.obj_sizes = 0; // 8x8 and 16x16
        return true;
    }
    // A racer's place (16.8, at 7E1017 and 7E101B, 100 bytes apart) and its kart
    // on the map: one 16x16 sprite of its own tile, in a palette of its own colour.
    void racer(int n, double x, double y, int tile, int sprite, int map_x, int map_y, uint16_t colour) {
        auto put = [&](uint32_t at, uint32_t v) {
            for (int i = 0; i < 3; ++i) ram[at + uint32_t(i)] = uint8_t(v >> (8 * i));
        };
        put(0x7E1017 + 0x100 * uint32_t(n), uint32_t(x * 256));
        put(0x7E101B + 0x100 * uint32_t(n), uint32_t(y * 256));
        const uint32_t o = 0x7E0420 + 4 * uint32_t(sprite);
        ram[o] = uint8_t(map_x), ram[o + 1] = uint8_t(map_y), ram[o + 2] = uint8_t(tile), ram[o + 3] = uint8_t((tile >> 8) | (n << 1));
        ram[0x7E0420 + 512 + uint32_t(sprite) / 4] |= uint8_t(2 << (2 * (sprite & 3))); // large
        for (int t : {0, 1, 16, 17})
            for (int r = 0; r < 8; ++r) vram[size_t(0xA000 + ((tile + t) & 0xFF) * 32 + r * 2)] = 0xFF; // colour 1
        palette[size_t(128 + n * 16 + 1)] = colour;
    }
    std::map<uint32_t, uint8_t> ram;

private:
    void set(int l, int a, int b, int c, int d, int x0, int y0, int h, int v) {
        const int16_t row[8] = {int16_t(a), int16_t(b), int16_t(c), int16_t(d),
                                int16_t(x0), int16_t(y0), int16_t(h), int16_t(v)};
        std::copy(row, row + 8, lines.begin() + size_t(l) * 8);
    }
    std::vector<uint32_t> fb;
    std::vector<int16_t> lines = std::vector<int16_t>(240 * 8, 0);
    std::vector<uint16_t> palette;
    std::vector<uint8_t> vram;
};

void test_mode7_extend() {
    // The race goes on under the game's own rows: the stale block above it and
    // the map below it are not taken for the race.
    Profile p = parse_ok("[game]\ntitle = T\ncodes = SUPER_MARIOKART\nsystem = snes\n[hud]\n"
                         "on = mem8[7E10C1] in 7F..84\nstrip = 0 108 256 116\nstrip_fill = yes\n"
                         "game_rows = 100\nextend = 92\n");
    FakeMode7 snes;
    Companion companion(snes, &p, "T", "SUPER NINTENDO");
    companion.after_frame(snes);
    const Picture shown = companion.compose_game(snes.picture());
    CHECK(shown.w == 256 && shown.h == 99 + 92); // lines 8-106 kept, 92 drawn
    CHECK(shown.px && shown.px[0] == snes.picture().px[0]);
    bool drawn = false; // the course's colour 5 somewhere in the new rows
    for (int y = 99; y < shown.h && shown.px; ++y)
        for (int x = 0; x < 256; ++x) drawn = drawn || shown.px[size_t(y) * 256 + x] != snes.picture().px[0];
    CHECK(drawn);
    CHECK(companion.compose_game(snes.picture(), true).h == 224); // a screen filter: the game as it is

    // What stands behind the player: the map's sprites at the racers' places, in
    // the rows that look there (the course here is about X 1005, Y 166-194 down
    // the new rows), the player's own (the first place) not drawn again.
    Profile q = parse_ok("[game]\ntitle = T\ncodes = SUPER_MARIOKART\nsystem = snes\n[hud]\n"
                         "on = mem8[7E10C1] in 7F..84\nstrip = 0 108 256 116\nstrip_fill = yes\n"
                         "game_rows = 100\nextend = 92\nextend_sprites = 7E0420\n"
                         "extend_places = 7E1017 7E101B 100 2\nextend_tiles = 180 184\n");
    FakeMode7 race;
    race.racer(0, 1000, 190, 0x180, 0, 40, 150, 0x001F); // the player, red
    race.racer(1, 1005, 185, 0x184, 1, 90, 150, 0x03E0); // behind them, green
    Companion behind(race, &q, "T", "SUPER NINTENDO");
    behind.after_frame(race);
    const Picture seen = behind.compose_game(race.picture());
    std::map<uint32_t, int> colours;
    int lowest = -1, left = 256, right = -1;
    for (int y = 99; y < seen.h && seen.px; ++y)
        for (int x = 0; x < 256; ++x) {
            const uint32_t c = seen.px[size_t(y) * 256 + x];
            ++colours[c];
            if (c != seen.px[size_t(seen.h - 1) * 256] && colours[c] < 2000) {
                lowest = std::max(lowest, y);
                left = std::min(left, x), right = std::max(right, x);
            }
        }
    CHECK(colours.size() == 3); // the road's two colours and one kart's
    CHECK(lowest > 120 && lowest < 180 && left < 128 && right > 128); // standing in the middle, larger than 16
    CHECK(right - left + 1 > 16);
}

void test_hud_strip() {
    // The band leaves the picture while `on` holds, and only then.
    Profile p = parse_ok("[game]\ntitle = T\ncodes = AB\n[hud]\non = mem8[02000000] == 1\nstrip = 0 144 240 16\n");
    FakeGame game;
    Companion companion(game, &p, "T", "GAME BOY");
    game.run_frame(0);
    companion.after_frame(game);
    Picture shown = companion.compose_game(game.picture());
    CHECK(shown.h == 144 && shown.px == game.picture().px && companion.status().showing_hud);
    CHECK(companion.compose_game(game.picture(), true).h == 160); // a screen filter: whole
    Canvas out(640, 480);
    Look look;
    Font font;
    companion.render(out, look, font, Words{});
    // The band's own colour in its card at the bottom of the second screen.
    CHECK(out.at(320, 480 - 24 - 4) == (game.picture().at(0, 159) | 0xFF000000u));
    uint64_t key = companion.visual_key();
    game.run_frame(0);
    companion.after_frame(game);
    CHECK(companion.visual_key() == key); // nothing changed: not drawn again
    game.s.scene = 3;                     // the map screen: no band
    game.run_frame(0);
    companion.after_frame(game);
    CHECK(companion.compose_game(game.picture()).h == 160 && !companion.status().showing_hud);

    // A second picture under the first (Super Mario Kart): the game keeps its
    // own rows without the gap between them, and the band fills the panel's
    // width at a scale that isn't whole (640 / 240).
    Profile two = parse_ok("[game]\ntitle = T\ncodes = AB\n[hud]\non = mem8[02000000] == 1\nstrip = 0 100 240 60\n"
                           "strip_fill = yes\ngame_rows = 90\n");
    game.s.scene = 1;
    game.run_frame(0);
    Companion lower(game, &two, "T", "GAME BOY");
    lower.after_frame(game);
    CHECK(lower.compose_game(game.picture()).h == 90 && lower.compose_game(game.picture(), true).h == 160);
    Canvas filled(640, 480);
    lower.render(filled, look, font, Words{});
    CHECK(filled.at(0, 240) == (game.picture().at(0, 130) | 0xFF000000u) &&
          filled.at(639, 240) == (game.picture().at(239, 130) | 0xFF000000u));
}

void test_gb_views() {
    std::string e;
    // A status board: BCD numbers and a count of set bits, read live.
    Profile p = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[view]\nformat = board\n"
                         "on = mem8[A8C3] == 03\ncovers_hud = yes\ntile1 = Coins | bcd A97A 2\n"
                         "tile2 = Treasures | bits A80E 2 wide of 15\n");
    auto board = make_view(p.view, e);
    CHECK(board != nullptr && board->covers_hud());
    GbMemory mem;
    mem.put(0xA8C3, {3});
    mem.put(0xA97A, {0x01, 0x23});
    mem.put(0xA80E, {0x06, 0x01});
    board->update(mem, 0);
    CHECK(board->shown());
    uint64_t before = board->key(0);
    mem.put(0xA97B, {0x24});
    board->update(mem, 15);
    CHECK(board->key(15) != before);
    Canvas out(640, 480);
    Look look;
    Font font;
    board->draw(out, look, font, 0);
    mem.put(0xA8C3, {1}); // the map: not shown
    board->update(mem, 30);
    CHECK(!board->shown());
    ViewSpec bad = p.view;
    bad.values["tile3"] = "Lives | hex A809 1";
    CHECK(make_view(bad, e) == nullptr && e.find("tile3") != std::string::npos);

    // A generation 1 battle: Pikachu against a Pidgey, while D057 says so.
    Profile b = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[view]\nformat = pokemon_battle\n"
                         "on = mem8[D057] == 01 | 02\ngeneration = 1\nplayer = D014\nplayer_name = D009\n"
                         "enemy = CFE5\nenemy_name = CFDA\nmove_names = 2C:4000\nmoves = 0E:4000\n"
                         "type_names = 09:7DAE\ndex_order = 10:5024\n");
    auto battle = make_view(b.view, e);
    CHECK(battle != nullptr);
    if (!battle) return;
    GbMemory gb;
    std::vector<uint8_t> you(29, 0), them(29, 0);
    you[0] = 0x54, you[2] = 21, you[5] = you[6] = 0x17, you[8] = 0x54, you[14] = 12, you[16] = 35, you[25] = 0x40 | 28;
    them[0] = 0x24, them[2] = 6, them[14] = 5, them[16] = 20;
    gb.bytes[banked(0x10, 0x5024 + 0x53)] = 25; // internal numbers to the Pokédex's
    gb.bytes[banked(0x10, 0x5024 + 0x23)] = 16;
    for (uint32_t i = 0; i < 29; ++i) {
        gb.bytes[banked(6, 0xD014 + i)] = you[i]; // D000-DFFF: the mapped work RAM bank
        gb.bytes[0xCFE5 + i] = them[i];
    }
    gb.put(banked(6, 0xD009), {0x8F, 0x88, 0x8A, 0x80, 0x82, 0x87, 0x94, 0x50});
    gb.put(0xCFDA, {0x8F, 0x88, 0x83, 0x86, 0x84, 0x98, 0x50});
    battle->update(gb, 0);
    CHECK(!battle->shown()); // no battle on
    gb.put(banked(6, 0xD057), {1});
    battle->update(gb, 15);
    CHECK(battle->shown());
    battle->draw(out, look, font, 0);
    gb.put(0xCFE5 + 1, {0x00, 0x30}); // more HP than its most: being written
    battle->update(gb, 30);
    CHECK(!battle->shown());

    // [map.<name>]: another capture while its `when` holds, else [map].
    Profile m = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[map]\nopen = SELECT:4\n"
                         "ready = mem8[DB95] == 07\n[map.dungeon]\nwhen = mem8[DBA5] == 01\nopen = START:4\n"
                         "ready = mem8[DB95] == 0C\ncrop = 72 36 88 108\n");
    CHECK(m.alt_maps.size() == 1 && m.alt_maps[0].crop.w == 88);
    GbMemory world;
    CHECK(&m.map_for(world) == &m.map);
    world.put(banked(6, 0xDBA5), {1});
    CHECK(&m.map_for(world) == &m.alt_maps[0]);
    Profile bad_map;
    CHECK(!parse_profile("[game]\ntitle = x\ncodes = AB\n[map]\nopen = A:4\nready = mem8[C000] == 1\n"
                         "[map.x]\nopen = B:4\nready = mem8[C000] == 2\n",
                         "x.ini", bad_map, e) &&
          e.find("when") != std::string::npos);
}

void test_shipped_gb_profiles() {
    ProfileLibrary lib;
    std::vector<std::string> errors;
    lib.load_directory(PLAY_PROFILES "/gb", errors);
    for (const auto& e : errors) std::fprintf(stderr, "profile error: %s\n", e.c_str());
    CHECK(errors.empty());
    // A colour game finds its profile under gb; old carts by their header title.
    CHECK(lib.find("gbc", "BYTE") && lib.find("gb", "AZLE") && lib.find("gb", "POKEMON_BLUE"));
    CHECK(lib.find("gb", "METROID2") && lib.find("gb", "METROID2")->has_view);
    std::string e;
    CHECK(make_view(lib.find("gb", "METROID2")->view, e) != nullptr);
    // Every shipped view builds (the boards and the battles too).
    for (const char* code :
         {"POKEMON_BLUE", "POKEMON_YELLOW", "BYTE", "AAUE", "AAXE", "SUPERMARIOLAND3", "WARIOLAND2"}) {
        const Profile* p = lib.find("gb", code);
        CHECK(p && p->has_view && make_view(p->view, e) != nullptr);
        if (!p || !make_view(p->view, e)) std::fprintf(stderr, "%s: %s\n", code, e.c_str());
    }
    CHECK(lib.find("gb", "AZLE")->alt_maps.size() == 2 && lib.find("gb", "GOLF")->has_map);
    CHECK(lib.find("gbc", "AWQE") && lib.find("gbc", "AWQE")->party.format == "dwm1");
    CHECK(lib.find("gbc", "AZ8P")->has_map && lib.find("gb", "AW8A")->room.size() == 4);
    CHECK(lib.find("gba", "BYTE") == nullptr);
}

void test_colour() {
    // Four greys the way the emulator draws a Game Boy game, one colourful pixel (not an original Game Boy picture).
    const uint32_t greys[4] = {rgb_to_px(0xF8F8F8), rgb_to_px(0xA8A8A8), rgb_to_px(0x505050), rgb_to_px(0x000000)};
    std::vector<uint32_t> pixels = {greys[0], greys[1], greys[2], greys[3], rgb_to_px(0xC03020)};
    Picture in{pixels.data(), 5, 1, 5};
    Colour colour;
    colour.set("gb", false, parse_palette("greys"), 0); // black and white: the same picture comes back
    CHECK(!colour.active() && colour.apply(in).px == in.px);
    colour.set("gb", false, 0, 0); // the default is the first Game Boy's screen
    CHECK(std::string(palette_slug(0)) == "dmg");
    Picture out = colour.apply(in);
    CHECK(colour.active() && out.px != in.px && out.w == 5);
    for (int i = 0; i < 4; ++i) CHECK(px_to_rgb(out.at(i, 0)) == gb_palettes[0].shade[i]);
    CHECK(out.at(4, 0) == in.at(4, 0)); // not grey: kept
    CHECK(pixels[0] == greys[0]);       // the source is never written
    colour.set("gb", false, parse_palette("pocket"), 0);
    CHECK(px_to_rgb(colour.apply(in).at(1, 0)) == gb_palettes[1].shade[1]);
    // Old slugs (and unknown ones) are the default screen.
    CHECK(parse_palette("bright") == 0 && parse_palette("default") == 0 && parse_palette("nonsense") == 0);
    // The Game Boy Color's colours for an original game: the core draws them, then the GBC screen corrects.
    colour.set("gb", false, parse_palette("color"), parse_correction("vivid"));
    CHECK(colour.active() && px_to_rgb(colour.apply(in).at(3, 0)) == 0x000000);
    // A game that runs on the Game Boy Color takes the correction whatever its file says; the GBA nothing.
    colour.set("gb", true, 0, parse_correction("vivid"));
    CHECK(!colour.active());
    colour.set("gbc", false, 0, parse_correction("vivid"));
    CHECK(!colour.active());
    colour.set("gba", false, 1, 0);
    CHECK(!colour.active());
    colour.set("gbc", false, 0, 0); // the default is the Game Boy Color's own screen
    out = colour.apply(in);
    CHECK(colour.active());
    CHECK(px_to_rgb(out.at(3, 0)) == 0x000000); // black stays black
    CHECK(px_to_rgb(out.at(0, 0)) >= 0xF0F0F0); // near white stays near white
    const Rgb red = Rgb::from_px(out.at(4, 0)); // 0xC03020 is a saturated red: blue gains, red loses
    CHECK(red.r < 0xC0 && red.b > 0x20);
    colour.set("gb", true, 0, 0); // a .gb made for both handhelds, on the colour one
    CHECK(colour.active() && Rgb::from_px(colour.apply(in).at(4, 0)).r == red.r);
    // The picture is the colour's own buffer: read it before applying again.
    const int plain = Rgb::from_px(out.at(1, 0)).r;
    colour.set("gbc", false, 0, parse_correction("gbc-dim"));
    CHECK(Rgb::from_px(colour.apply(in).at(1, 0)).r < plain); // dimmer than plain
    CHECK(parse_correction("off") == 0 && parse_correction("nonsense") == 0);
    CHECK(std::string(palette_slug(99)) == palette_slug(palette_count() - 1));
}

void test_second_screen() {
    // Off (or the info screen), the game keeps its whole picture: no band is cut away.
    Profile p = parse_ok("[game]\ntitle = T\ncodes = AB\n[hud]\non = mem8[02000000] == 1\nstrip = 0 144 240 16\n");
    FakeGame game;
    Companion companion(game, &p, "T", "GAME BOY");
    game.run_frame(0);
    companion.after_frame(game);
    CHECK(companion.compose_game(game.picture()).h == 144);
    companion.set_shown(false);
    game.run_frame(0);
    companion.after_frame(game);
    CHECK(companion.compose_game(game.picture()).h == 160 && !companion.status().showing_hud);
    companion.set_shown(true);
    game.run_frame(0);
    companion.after_frame(game);
    CHECK(companion.compose_game(game.picture()).h == 144);

    // Text is drawn at the baked sizes only.
    CHECK(Font::snap(16) == 16 && Font::snap(17) == 16 && Font::snap(26) == 24 && Font::snap(48) == 36);
    // The launcher's own theme, accent and high contrast.
    const Look light = make_look(false, -1, false), dark = make_look(true, -1, false);
    CHECK(light.background.r == 242 && light.background.g == 245 && dark.background.r == 12);
    CHECK(make_look(false, 9, false).accent.g > make_look(false, 9, false).accent.r); // sea green
    CHECK(contrast(make_look(true, 5, true).ink, make_look(true, 5, true).background) >= 7);

    // A board's pages are tabs, chosen by touch.
    std::string e;
    Profile b =
        parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[view]\nformat = board\nheading = H\n"
                 "tile1 = Coins | bcd A97A 2 icon coin page One\ntile2 = Lives | num A809 1 icon life page Two\n"
                 "tile3 = Ending | bcd A805 3 wide tiers icon trophy\ntiers3 = 300 Birdhouse; 10072 Tree trunk\n");
    auto board = make_view(b.view, e);
    CHECK(board != nullptr);
    if (!board) return;
    GbMemory mem;
    mem.put(0xA805, {0x00, 0x30, 0x00});
    board->update(mem, 0);
    Canvas out(640, 480);
    Font font;
    CHECK(font.load(PLAY_FONTS));
    std::vector<Box> targets;
    set_target_log(&targets);
    board->draw(out, light, font, 0);
    set_target_log(nullptr);
    CHECK(targets.size() == 2 && targets[1].w >= kit::touch && targets[1].h >= kit::touch);
    const uint64_t first = board->key(0);
    CHECK(!targets.empty() && board->touch({Touch::Down, targets.back().x + 4, targets.back().y + 4}));
    CHECK(board->key(0) != first);
    CHECK(!board->touch({Touch::Down, 5, 470})); // not on a tab
    // Wrong words are refused with the tile's name.
    ViewSpec bad = b.view;
    bad.values["tile4"] = "Keys | bits A80B 1 icon nonesuch";
    CHECK(make_view(bad, e) == nullptr && e.find("tile4") != std::string::npos);

    // The dock: the switcher on a tap, Return home only when held, screen off and back on a touch.
    Dock dock("T", "GAME BOY");
    dock.set_full(true);
    const Box switcher = dock.box(Dock::Item::Switcher, 640, 480), home = dock.box(Dock::Item::Home, 640, 480),
              off = dock.box(Dock::Item::Off, 640, 480);
    dock.set_time(100);
    CHECK(dock.touch({Touch::Down, home.x + 10, home.y + 10}));
    dock.set_time(600);
    CHECK(dock.take_action() == Dock::Action::None); // not yet
    dock.set_time(1150);
    CHECK(dock.take_action() == Dock::Action::Home);
    dock.touch({Touch::Up, home.x + 10, home.y + 10});
    dock.set_time(2000);
    dock.touch({Touch::Down, home.x + 10, home.y + 10});
    dock.set_time(2100);
    dock.touch({Touch::Up, home.x + 10, home.y + 10}); // a tap: not home
    dock.set_time(4000);
    CHECK(dock.take_action() == Dock::Action::None);
    dock.touch({Touch::Down, switcher.x + 10, switcher.y + 10});
    dock.touch({Touch::Up, switcher.x + 10, switcher.y + 10});
    CHECK(dock.take_action() == Dock::Action::Switcher);
    dock.touch({Touch::Down, switcher.x + 10, switcher.y + 10});
    dock.touch({Touch::Move, off.x + 10, off.y + 10});
    dock.touch({Touch::Up, off.x + 10, off.y + 10}); // slid off: nothing, and not the screen off either
    CHECK(dock.take_action() == Dock::Action::None);
    dock.touch({Touch::Down, off.x + 10, off.y + 10});
    dock.touch({Touch::Up, off.x + 10, off.y + 10});
    CHECK(dock.screen_off());
    dock.touch({Touch::Down, home.x + 10, home.y + 10}); // wakes, and does nothing else
    dock.set_time(6000);
    CHECK(!dock.screen_off() && dock.take_action() == Dock::Action::None);
    // Over the companion only the corner button takes a touch while closed.
    Dock corner("T", "GAME BOY");
    corner.set_time(10);
    CHECK(!corner.touch({Touch::Down, 100, 100}) && !corner.open());
    const Box button = corner.box(Dock::Item::Button, 640, 480);
    CHECK(corner.touch({Touch::Down, button.x + 5, button.y + 5}) && corner.open());

    // An alternative board waits behind the switch while another view shows.
    Profile alt =
        parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = gb\n[view.main]\nformat = board\n"
                 "on = mem8[A8C3] == 03\ntile1 = Coins | bcd A97A 2 icon coin\n[view.status]\nformat = board\n"
                 "alternative = yes\non = mem8[A8C3] == 03\ntile1 = Lives | num A809 1 icon life\n");
    CHECK(alt.views.size() == 2 && alt.views[1].name == "status");
    GbMemory live;
    live.put(0xA8C3, {3});
    Companion c(live, &alt, "T", "GAME BOY");
    for (int i = 0; i < 15; ++i) c.after_frame(live); // boards read every 15 frames
    CHECK(c.has_switch() && !c.showing_alternative());
    CHECK(c.touch({Touch::Down, 640 - kit::margin - 20, kit::margin + 20}) && c.showing_alternative());
}

// A scrolling world 4 screens wide: the picture is the world under a camera at
// mem16[0010], each world column a colour of its own, so a map can be checked.
class Scroller final : public Core {
public:
    std::map<uint32_t, uint8_t> bytes;
    std::vector<uint32_t> fb = std::vector<uint32_t>(64 * 48);
    void set_camera(int x) {
        bytes[0x10] = uint8_t(x), bytes[0x11] = uint8_t(x >> 8);
        for (int y = 0; y < 48; ++y)
            for (int c = 0; c < 64; ++c) fb[size_t(y * 64 + c)] = rgb_to_px(uint32_t(((x + c) & 0xFF) << 16 | y));
    }
    const char* system() const override { return "nes"; }
    std::string game_code() const override { return "CRC_00000000"; }
    std::string header_title() const override { return "SCROLLER"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {}
    Picture picture() const override { return {fb.data(), 64, 48, 64}; }
    bool save_state(std::vector<uint8_t>&) override { return false; }
    bool load_state(const std::vector<uint8_t>&) override { return false; }
    uint32_t peek(uint32_t address, int size) override {
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) {
            auto f = bytes.find(address + uint32_t(i));
            v |= uint32_t(f == bytes.end() ? 0 : f->second) << (8 * i);
        }
        return v;
    }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 48000; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }
};

void test_libretro_views() {
    std::string e;
    // Expressions: C's precedence, hex numbers, memory reads.
    Scroller game;
    game.set_camera(0x123);
    Expr x;
    CHECK(x.parse("(mem16[10] >> 4) + 2 * 3 - 1", e) && x.eval(game) == 0x12 + 5);
    CHECK(x.parse("mem8[11] << 8 | mem8[10] & F0", e) && x.eval(game) == 0x120);
    CHECK(!x.parse("mem8[10] +", e) && !e.empty());
    CHECK(!x.parse("mem9[10]", e));
    // A float's whole part (an N64 place), in the core's own order.
    const float real = -1234.75f;
    uint32_t bits;
    std::memcpy(&bits, &real, 4);
    for (int i = 0; i < 4; ++i) game.bytes[0x60 + uint32_t(i)] = uint8_t(bits >> (8 * i));
    e.clear();
    CHECK(x.parse("f32[60] + 4", e) && x.eval(game) == -1230);

    // mosaic: the world pasted where the camera says, two screens side by side.
    Profile m = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = nes\n[view]\nformat = mosaic\n"
                         "x = mem16[10]\ny = 0\ncrop = 0 8 64 40\nscale = 2\nevery = 1\nplayer_x = mem16[10] + 20\n"
                         "player_y = 20\nname.0 = Overworld\n");
    auto mosaic = make_view(m.view, e);
    CHECK(mosaic != nullptr);
    if (!mosaic) return;
    game.set_camera(0);
    mosaic->update(game, 0);
    CHECK(mosaic->shown() && mosaic->dirty());
    const uint64_t first = mosaic->key(0);
    game.set_camera(64);
    mosaic->update(game, 1);
    CHECK(mosaic->key(1) != first);
    Canvas out(640, 480);
    Look look;
    Font font;
    mosaic->draw(out, look, font, 0);
    std::string saved;
    CHECK(mosaic->save(saved) && !mosaic->dirty());
    // 128 world columns at scale 2: 64 map columns, two 32-pixel tiles across, one down.
    CHECK(saved.size() == 6 + 2 * (12 + 32 * 32 * 4));
    auto again = make_view(m.view, e);
    again->load(saved);
    std::string resaved;
    CHECK(again->save(resaved) && resaved == saved);
    ViewSpec bad = m.view;
    bad.values["x"] = "mem16[10] +";
    CHECK(make_view(bad, e) == nullptr && e.find(" x") != std::string::npos);

    // grid: a floor in memory, seen from where the player stands and faces.
    Profile g = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = sms\n[view]\nformat = grid\n"
                         "cells = 100\nposition = 20\nfacing = 21\nlevel = mem8[22]\ntype_mask = 07\nwall = 1\n"
                         "up = 2\ndoor = 4\nsight = 3\n");
    auto grid = make_view(g.view, e);
    CHECK(grid != nullptr);
    if (!grid) return;
    for (int i = 0; i < 256; ++i) game.bytes[0x100 + uint32_t(i)] = (i % 16 == 0 || i / 16 == 0) ? 1 : 0;
    game.bytes[0x20] = 0x55; // x 5, y 5
    game.bytes[0x21] = 0;    // facing up
    grid->update(game, 0);
    CHECK(grid->shown() && grid->dirty());
    std::string floor1;
    CHECK(grid->save(floor1));
    game.bytes[0x21] = 1; // turned right: more of the floor seen
    grid->update(game, 4);
    CHECK(grid->dirty());
    grid->draw(out, look, font, 0);

    // trace: a course drawn from where the machine has been.
    Profile t = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = snes\n[view]\nformat = trace\n"
                         "x = mem16[10]\ny = mem8[30]\ncourse = mem8[31]\nname = Course\n");
    auto trace = make_view(t.view, e);
    CHECK(trace != nullptr);
    if (!trace) return;
    for (int step = 0; step < 40; ++step) {
        game.bytes[0x10] = uint8_t(step * 10), game.bytes[0x11] = 0, game.bytes[0x30] = uint8_t(step * 3);
        trace->update(game, uint64_t(step) * 2);
    }
    CHECK(trace->shown() && trace->dirty());
    std::string course;
    CHECK(trace->save(course) && course.size() > 5 + 8);
    trace->draw(out, look, font, 0);

    // F-Zero's places count round (x at 0x1000, y at 0x2000) and a course may
    // cross where they wrap: a loop through both wraps is traced in one piece,
    // the junk place before the race starts is dropped, and a restart lands
    // back on the drawn loop instead of a second copy one wrap away.
    Profile w = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = snes\n[view]\nformat = trace\n"
                         "x = mem16[40]\ny = 0 - mem16[42]\ncourse = mem8[44]\nwrap_x = 1000\nwrap_y = 2000\n");
    auto wrapped = make_view(w.view, e);
    CHECK(wrapped != nullptr);
    if (!wrapped) return;
    auto place = [&](double a, uint64_t frame) {
        const int x = (int(4060 + 300 * std::cos(a)) + 4096) % 4096, y = (int(8150 + 300 * std::sin(a)) + 8192) % 8192;
        game.bytes[0x40] = uint8_t(x), game.bytes[0x41] = uint8_t(x >> 8);
        game.bytes[0x42] = uint8_t(y), game.bytes[0x43] = uint8_t(y >> 8);
        wrapped->update(game, frame);
    };
    game.bytes[0x40] = game.bytes[0x41] = game.bytes[0x42] = game.bytes[0x43] = game.bytes[0x44] = 0;
    wrapped->update(game, 0); // the place before the race: (0, 0)
    uint64_t f = 2;
    for (int lap = 0; lap < 2; ++lap)
        for (int i = 0; i < 400; ++i, f += 2) place(i * 2 * M_PI / 400, f);
    place(M_PI, f += 2); // restarted, on the far side of the loop
    for (int i = 200; i < 260; ++i, f += 2) place(i * 2 * M_PI / 400, f);
    std::string kept;
    CHECK(wrapped->save(kept) && kept.size() > 17 && kept[4] == 3);
    int32_t cell = 1, head[2] = {0, 0};
    if (kept.size() > 17) std::memcpy(&cell, kept.data() + 5, 4), std::memcpy(head, kept.data() + 9, sizeof head);
    int lo_x = INT_MAX, hi_x = INT_MIN, lo_y = INT_MAX, hi_y = INT_MIN;
    for (int i = 0; i < head[1] && 17 + size_t(i) * 4 + 4 <= kept.size(); ++i) {
        int16_t sq[2];
        std::memcpy(sq, kept.data() + 17 + size_t(i) * 4, sizeof sq);
        lo_x = std::min<int>(lo_x, sq[0] * cell), hi_x = std::max<int>(hi_x, sq[0] * cell);
        lo_y = std::min<int>(lo_y, sq[1] * cell), hi_y = std::max<int>(hi_y, sq[1] * cell);
    }
    // One loop, 600 across and the road's width (40) wide; the place before the race is gone.
    CHECK(head[0] == 0 && head[1] > 20 && hi_x - lo_x < 700 && hi_y - lo_y < 700 && hi_x - lo_x > 600);
    wrapped->draw(out, look, font, 0);
    // A second lap on another line, inside the road's width, widens the road a little, no second loop.
    const int32_t one_lap = head[1];
    for (int i = 0; i < 400; ++i, f += 2) {
        const double a = i * 2 * M_PI / 400;
        const int x = (int(4060 + 316 * std::cos(a)) + 4096) % 4096, y = (int(8150 + 316 * std::sin(a)) + 8192) % 8192;
        game.bytes[0x40] = uint8_t(x), game.bytes[0x41] = uint8_t(x >> 8);
        game.bytes[0x42] = uint8_t(y), game.bytes[0x43] = uint8_t(y >> 8);
        wrapped->update(game, f);
    }
    CHECK(wrapped->save(kept) && kept.size() > 17);
    if (kept.size() > 17) std::memcpy(head, kept.data() + 9, sizeof head);
    CHECK(head[1] > one_lap && head[1] < one_lap * 3 / 2);
    // Kept and read back: the same squares; points kept by version 2 are painted as strokes.
    auto reread = make_view(w.view, e);
    reread->load(kept);
    std::string back;
    CHECK(reread->save(back) && back.size() == kept.size());
    std::string v2 = std::string("RGTR\x02", 5);
    const int32_t v2_head[2] = {0, 3};
    const int16_t v2_points[6] = {100, 100, 130, 100, 160, 110};
    v2.append(reinterpret_cast<const char*>(v2_head), 8).append(reinterpret_cast<const char*>(v2_points), 12);
    auto old = make_view(w.view, e);
    old->load(v2);
    CHECK(old->save(back) && back.size() > 17 + 4 * 20);

    // A course from the game's own path (Mario Kart 64's waypoints): drawn whole at once, edges giving the
    // road's width; driving then paints nothing, so a shortcut over the grass stays off the map.
    Profile pa = parse_ok("[game]\ntitle = T\ncodes = AB\nsystem = n64\n[view]\nformat = trace\n"
                          "x = mem16[50]\ny = mem16[52]\ncourse = 1\npath = mem16[54]\npath_count = mem16[56]\n"
                          "path_left = 2000\npath_right = 3000\nwidth = 20\n");
    auto pathed = make_view(pa.view, e);
    CHECK(pathed != nullptr);
    if (!pathed) return;
    auto put16 = [&](uint32_t a, int v) { game.bytes[a] = uint8_t(v), game.bytes[a + 1] = uint8_t(v >> 8); };
    const int points = 80;
    for (int i = 0; i < points; ++i) {
        const double a = i * 2 * M_PI / points;
        const int x = int(1000 + 400 * std::cos(a)), y = int(1000 + 400 * std::sin(a));
        put16(0x1000 + uint32_t(i) * 8, x), put16(0x1004 + uint32_t(i) * 8, y);
        put16(0x2000 + uint32_t(i) * 8, int(1000 + 360 * std::cos(a))), put16(0x2004 + uint32_t(i) * 8, int(1000 + 360 * std::sin(a)));
        put16(0x3000 + uint32_t(i) * 8, int(1000 + 440 * std::cos(a))), put16(0x3004 + uint32_t(i) * 8, int(1000 + 440 * std::sin(a)));
    }
    put16(0x54, 0x1000), put16(0x56, points), put16(0x50, 3000), put16(0x52, 3000);
    pathed->update(game, 0); // the player nowhere near it: the last course's path, still in memory while this one loads
    std::string none;
    CHECK(!pathed->save(none) || none.size() <= 9);
    put16(0x50, 1400), put16(0x52, 1000);
    pathed->update(game, 30);
    std::string road;
    CHECK(pathed->shown() && pathed->save(road) && road.size() > 17);
    int32_t pathed_head[2] = {0, 0}, pathed_cell = 1;
    if (road.size() > 17) std::memcpy(&pathed_cell, road.data() + 5, 4), std::memcpy(pathed_head, road.data() + 9, 8);
    // A ring 800 across and 80 wide (the edges), not the 20 of `width`: about pi * 800 * 80 units.
    const double area = double(pathed_head[1]) * pathed_cell * pathed_cell;
    CHECK(area > 150000 && area < 260000);
    put16(0x50, 1000), put16(0x52, 1000); // across the grass in the middle
    for (uint64_t frame = 32; frame < 80; frame += 2) pathed->update(game, frame);
    std::string after;
    CHECK(pathed->save(after) && after.size() == road.size());
    pathed->draw(out, look, font, 0);

    // The shipped profiles of the libretro systems: every view builds, Duck Hunt is a light-gun game.
    ProfileLibrary lib;
    std::vector<std::string> errors;
    for (const char* system : {"/nes", "/snes", "/sms", "/n64"})
        lib.load_directory(std::string(PLAY_PROFILES) + system, errors);
    for (const auto& error : errors) std::fprintf(stderr, "profile error: %s\n", error.c_str());
    CHECK(errors.empty());
    for (const auto& [key, p] : lib.all())
        for (const auto& v : p.views) {
            const bool ok = make_view(v, e) != nullptr;
            CHECK(ok);
            if (!ok) std::fprintf(stderr, "%s: %s\n", key.c_str(), e.c_str());
            // The N64 keeps its numbers big-endian: a board's num tile there says so (Mario Kart's lap read 257).
            for (const auto& [name, value] : v.values) {
                if (p.system != "n64" || name.rfind("tile", 0) != 0 || value.find("| num ") == std::string::npos) continue;
                const bool big = value.find(" big") != std::string::npos;
                CHECK(big);
                if (!big) std::fprintf(stderr, "%s: %s is not big-endian\n", key.c_str(), name.c_str());
            }
        }
    CHECK(lib.find("nes", "DUCK_HUNT") && lib.find("nes", "DUCK_HUNT")->lightgun);
    CHECK(lib.find("nes", "LEGEND_OF_ZELDA") && lib.find("snes", "SUPER_MARIOKART")->hud.strip_fill);
    CHECK(lib.find("nes", "HOGANS_ALLEY") && lib.find("nes", "HOGANS_ALLEY")->lightgun);
    CHECK(lib.find("snes", "LEGEND_OF_ZELDA") && lib.find("n64", "MARIO_KART_64"));
    // Mario Paint: the SNES Mouse, the cursor read so the mouse takes it to the finger.
    const Profile* paint = lib.find("snes", "MARIOPAINT");
    CHECK(paint && paint->mouse && !paint->lightgun && !paint->cursor_x.empty());
    Expr cursor;
    e.clear();
    CHECK(paint && cursor.parse(paint->cursor_y, e));
    CHECK(lib.find("sms", "PHANTASY_STAR") && lib.find("snes", "SUPER_METROID") && lib.find("snes", "F_ZERO"));
}

int main() {
    test_second_screen();
    test_colour();
    test_parsing();
    test_shipped_profiles();
    test_game_boy();
    test_hud_strip();
    test_mode7_extend();
    test_gb_views();
    test_shipped_gb_profiles();
    test_libretro_views();
    test_capture();
    test_companion_scheduling();
    test_cheats();
    test_saves();
    test_canvas();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
