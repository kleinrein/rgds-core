// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for RG DS Play: profiles, conditions, the capture engine against a
// scripted fake game, and the save rules. Run: build/play/host/play-tests
#include "app/storage.hpp"
#include "companion/companion.hpp"
#include "companion/map_capture.hpp"
#include "companion/profile.hpp"
#include "ui/canvas.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
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
    c.render(out, Look{}, font);
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

int main() {
    test_parsing();
    test_shipped_profiles();
    test_capture();
    test_companion_scheduling();
    test_saves();
    test_canvas();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
