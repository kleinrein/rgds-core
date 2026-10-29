// SPDX-License-Identifier: GPL-3.0-or-later
// Headless runner: plays a script against a game and writes both screens.
//
//   play-shot --rom GAME --data DIR --profiles DIR --fonts DIR --script FILE --out DIR
//             [--panel 640x480] [--no-companion] [--hashes FILE] [--async]
//             [--preferences userdata/preferences.ini] [--palette dmg|pocket|...]
//             [--core CORE.so --system snes [--system-dir BIOS]]
//
// Script lines (frame numbers ascend):
//   120 START        hold START from frame 120 (A, B+UP, - for nothing)
//   300 shot name    write OUT/name.bmp (game on top, companion below)
//   300 capture      capture the map now, on this thread
//   600 quit
// --drive reads commands from stdin instead, one at a time, for a script that
// plays the game (tools/play_drive.py steers a kart round whole races):
//   run 30 A+LEFT    run 30 frames holding those buttons, answers "ok FRAME"
//   read 03001234 2  answers the value in hex
//   dump FILE        writes EWRAM (256 KiB) then IWRAM (32 KiB)
//   poke A B...      writes bytes from address A (Game Boy, "1:D206" a bank)
//   bytes A N        N bytes from address A as one hex string
//   state save FILE  the whole machine to a file ("state load FILE" to come back)
//   second off       the second screen turned off (on again with "second on", "second info": the info screen)
//   tap X Y          a finger down and up on the second screen ("touch down|move|up X Y" for each)
//   and the script commands without a frame: shot, hudshot, status, ...
// Captures normally run to completion before the next frame, so a run is the
// same every time; --async lets them race the game as on the handheld.
#include "app/session.hpp"
#include "companion/profile.hpp"
#include "ui/colour.hpp"
#include "ui/screens.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using namespace play;

namespace {
struct Command {
    std::string what, arg;
};
} // namespace

int main(int argc, char** argv) {
    SessionOptions o;
    std::string script, out_dir = ".", hashes, state_hashes, palette;
    int pw = 640, ph = 480;
    bool async = false, bench = false, drive = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rom") o.rom = next();
        else if (a == "--data") o.data_root = next();
        else if (a == "--profiles") o.profile_dirs.push_back(next());
        else if (a == "--fonts") o.font_dir = next();
        else if (a == "--art") o.art_dir = next();
        else if (a == "--core") o.core_library = next(); // a libretro core (with --system), as rgds-play takes it
        else if (a == "--system") o.system = next();
        else if (a == "--system-dir") o.system_dir = next();
        else if (a == "--preferences") o.preferences = next();
        else if (a == "--palette") palette = next();
        else if (a == "--save-source") o.save_sources.push_back(next());
        else if (a == "--script") script = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--hashes") hashes = next();
        else if (a == "--state-hashes") state_hashes = next();
        else if (a == "--no-companion") o.companion = false;
        else if (a == "--async") async = true;
        else if (a == "--drive") drive = true;
        else if (a == "--bench") bench = true; // time each frame, drawing the companion every frame like the player
        else if (a == "--panel") {
            if (std::sscanf(next().c_str(), "%dx%d", &pw, &ph) != 2) return 2;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (o.rom.empty() || o.data_root.empty()) {
        std::fprintf(stderr, "usage: play-shot --rom GAME --data DIR [--profiles DIR] [--script FILE] [--out DIR]\n");
        return 2;
    }
    std::multimap<long, Command> commands;
    std::map<long, uint32_t> holds;
    long last = 600;
    if (!script.empty()) {
        std::ifstream f(script);
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", script.c_str());
            return 2;
        }
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream in(line);
            long frame;
            std::string what, arg;
            if (!(in >> frame >> what) || what[0] == '#') continue;
            std::getline(in, arg); // the rest of the line: paths may hold spaces
            arg.erase(0, arg.find_first_not_of(" \t"));
            uint32_t buttons;
            if (what == "shot" || what == "capture" || what == "quit" || what == "status" || what == "hudshot" ||
                what == "state-save" || what == "state-load")
                commands.insert({frame, {what, arg}});
            else if (parse_buttons(what, buttons)) holds[frame] = buttons;
            else {
                std::fprintf(stderr, "bad script line: %s\n", line.c_str());
                return 2;
            }
            if (what == "quit") last = frame;
        }
    }
    Session s;
    std::string error;
    if (!s.open(o, error)) {
        std::fprintf(stderr, "play-shot: %s\n", error.c_str());
        return 1;
    }
    std::printf("game %s %s \"%s\" profile %s\n", s.core().system(), s.core().game_code().c_str(), s.title().c_str(),
                s.profile() ? s.profile()->file.c_str() : "none");
    std::FILE* hash_out = hashes.empty() ? nullptr : std::fopen(hashes.c_str(), "w");
    // The whole emulator state (memory, registers, timing), not the picture:
    // what the HUD split must never change.
    std::FILE* state_out = state_hashes.empty() ? nullptr : std::fopen(state_hashes.c_str(), "w");
    std::vector<uint8_t> state;
    Canvas top(pw, ph), bottom(pw, ph);
    const Look& look = s.look();
    // --palette: the top screen in a Game Boy palette, and the companion's game pictures with it.
    Colour colour;
    if (!palette.empty()) {
        colour.set(s.core(), parse_palette(palette), 0);
        if (s.companion()) s.companion()->set_colour(colour);
    }
    uint32_t buttons = 0;
    std::vector<double> emulate_ms, draw_ms;
    auto execute = [&](const Command& c, long frame) {
        if (c.what == "capture" && s.companion()) {
            bool ok = s.companion()->capture_now(s.core());
            std::printf("frame %ld capture %s\n", frame, ok ? "ok" : s.companion()->status().last_failure.c_str());
        } else if (c.what == "shot") {
            draw_game(top, colour.apply(s.game()), Fit::Integer);
            s.render_companion(bottom, look);
            std::string path = out_dir + "/" + c.arg + ".bmp";
            if (!write_bmp(path, top, &bottom)) std::fprintf(stderr, "cannot write %s\n", path.c_str());
        } else if (c.what == "state-save" || c.what == "state-load") {
            // What the in-game menu's checkpoints do (F5 / F7 under RG DS Core).
            std::string why;
            bool ok = c.what == "state-save" ? s.save_state_file(c.arg, why) : s.load_state_file(c.arg, why);
            std::printf("frame %ld %s %s%s\n", frame, c.what.c_str(), ok ? "ok" : "failed: ", why.c_str());
        } else if (c.what == "hudshot") {
            // The raw HUD layer at 1x (the key colour where there is none).
            Picture hud = s.core().hud();
            if (hud.px) {
                Canvas raw(hud.w, hud.h);
                for (int y = 0; y < hud.h; ++y)
                    for (int x = 0; x < hud.w; ++x) raw.data()[y * hud.w + x] = hud.at(x, y) | 0xFF000000u;
                write_bmp(out_dir + "/" + c.arg + ".bmp", raw);
            }
        } else if (c.what == "status" && s.companion()) {
            auto st = s.companion()->status();
            std::printf("frame %ld status map=%d serial=%u requests=%d failures=%d hud=%d split=%d %s\n", frame,
                        st.showing_map, st.serial, st.requests, st.failures, st.showing_hud, st.hud_split,
                        st.last_failure.c_str());
        }
    };
    if (drive) {
        std::string line;
        long frame = 0;
        while (std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string what, arg;
            in >> what;
            if (what == "run") {
                long n = 0;
                std::string keys;
                in >> n >> keys;
                if (!parse_buttons(keys, buttons)) buttons = 0;
                for (long i = 0; i < n; ++i, ++frame) {
                    s.frame(buttons);
                    if (s.companion()) {
                        // Frame time stands in for the wall clock, so a run is the same every time.
                        s.companion()->set_time(uint64_t(frame + 1) * 1000 / 60);
                        // What the frontend would hand the runtime from the dock (here: say so).
                        const Dock::Action act = s.companion()->dock().take_action();
                        if (act != Dock::Action::None)
                            std::printf("action %s\n", act == Dock::Action::Switcher ? "switcher" : "home");
                    }
                    if (s.companion())
                        while (s.companion()->busy()) std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                std::printf("ok %ld\n", frame);
            } else if (what == "read") {
                std::string address;
                int size = 4;
                in >> address >> size;
                std::printf("%x\n", s.core().peek(uint32_t(std::stoul(address, nullptr, 16)), size));
            } else if (what == "bytes") {
                // bytes ADDRESS COUNT : that many bytes as one hex string, then "ok" ("3:5A20" a bank)
                std::string address;
                long count = 0;
                in >> address >> count;
                uint32_t a = 0;
                std::string hex;
                if (parse_address(address, a))
                    for (long i = 0; i < count; ++i) {
                        const uint32_t at = (a & 0xFFFF0000u) | ((a + uint32_t(i)) & 0xFFFF);
                        char b[3];
                        std::snprintf(b, sizeof b, "%02x", peek_address(s.core(), at, 1) & 0xFF);
                        hex += b;
                    }
                std::printf("%s\nok\n", hex.c_str());
            } else if (what == "dump") {
                std::getline(in, arg);
                arg.erase(0, arg.find_first_not_of(" \t"));
                std::ofstream out(arg, std::ios::binary);
                for (auto [base, size] : {std::pair<uint32_t, uint32_t>{0x02000000, 0x40000}, {0x03000000, 0x8000}})
                    for (uint32_t a = 0; a < size; a += 4) {
                        uint32_t v = s.core().peek(base + a, 4);
                        out.write(reinterpret_cast<const char*>(&v), 4);
                    }
                std::printf("ok\n");
            } else if (what == "state") {
                // state save|load FILE : the whole machine to or from a file (to come back to a scene)
                std::string verb, error;
                in >> verb;
                std::getline(in, arg);
                arg.erase(0, arg.find_first_not_of(" \t"));
                const bool ok = verb == "save"   ? s.save_state_file(arg, error)
                                : verb == "load" ? s.load_state_file(arg, error)
                                                 : (error = "state save|load FILE", false);
                std::printf("%s%s\n", ok ? "ok" : "failed: ", error.c_str());
            } else if (what == "poke") {
                // poke ADDRESS BYTE... : bytes into memory from ADDRESS on ("1:D206" names a bank)
                std::string address, byte;
                in >> address;
                uint32_t a = 0;
                if (!parse_address(address, a)) {
                    std::printf("bad address\n");
                    std::fflush(stdout);
                    continue;
                }
                const int bank = (a & banked_flag) ? int((a >> 16) & 0xFF) : -1;
                const uint32_t base = bank >= 0 ? a & 0xFFFF : a;
                for (uint32_t i = 0; in >> byte; ++i)
                    s.core().poke8(base + i, bank, uint8_t(std::stoul(byte, nullptr, 16)));
                std::printf("ok\n");
            } else if (what == "layers") {
                // Which layers the live picture draws (bits 0-3 BG0-3, bit 4 sprites): to see what is where.
                std::string mask;
                in >> mask;
                s.core().set_layers(unsigned(std::stoul(mask, nullptr, 16)));
                std::printf("ok\n");
            } else if (what == "second") {
                // second on|off|info : the second screen shown, turned off (the game keeps its HUD)
                // or the info screen (the title card and the dock's button)
                in >> arg;
                if (s.companion()) {
                    s.companion()->set_shown(arg == "on");
                    s.companion()->set_info(arg == "info");
                }
                std::printf("ok\n");
            } else if (what == "touch" || what == "tap") {
                // touch down|move|up X Y, or tap X Y (down then up): a finger on the second screen
                std::string kind = what == "tap" ? "down" : "";
                if (what == "touch") in >> kind;
                Touch t;
                in >> t.x >> t.y;
                t.kind = kind == "up" ? Touch::Up : kind == "move" ? Touch::Move : Touch::Down;
                bool changed = s.companion() && s.companion()->touch(t);
                if (what == "tap" && s.companion()) {
                    t.kind = Touch::Up;
                    changed = s.companion()->touch(t) || changed;
                }
                std::printf("%s\nok\n", changed ? "changed" : "same");
            } else if (what == "point") {
                // point X Y down|up : a finger on the game picture's pixel X, Y (a light gun or a mouse game)
                int x = -1, y = -1;
                in >> x >> y >> arg;
                s.core().set_pointer(x, y, arg != "up");
                std::printf("ok\n");
            } else if (what == "quit") break;
            else {
                std::getline(in, arg);
                arg.erase(0, arg.find_first_not_of(" \t"));
                execute({what, arg}, frame);
                std::printf("ok\n");
            }
            std::fflush(stdout);
        }
        s.flush();
        return 0;
    }
    for (long frame = 0; frame <= last; ++frame) {
        auto hold = holds.find(frame);
        if (hold != holds.end()) buttons = hold->second;
        auto range = commands.equal_range(frame);
        bool quit = false;
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.what == "quit") quit = true;
            else execute(it->second, frame);
        }
        if (quit) break;
        auto t0 = std::chrono::steady_clock::now();
        s.frame(buttons);
        if (bench) {
            auto t1 = std::chrono::steady_clock::now();
            draw_game(top, colour.apply(s.game()), Fit::Integer);
            s.render_companion(bottom, look);
            auto t2 = std::chrono::steady_clock::now();
            emulate_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            draw_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        if (hash_out) std::fprintf(hash_out, "%ld %016llx\n", frame, (unsigned long long)picture_hash(s.game()));
        if (state_out && s.core().save_state(state)) {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : state) h = (h ^ b) * 1099511628211ull;
            std::fprintf(state_out, "%ld %016llx\n", frame, (unsigned long long)h);
        }
        if (!async && s.companion())
            while (s.companion()->busy()) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    s.flush();
    auto report = [](const char* what, std::vector<double> v, size_t from) {
        if (v.size() <= from) return;
        v.erase(v.begin(), v.begin() + static_cast<long>(from));
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        std::printf("bench %s: mean %.2f ms, p95 %.2f ms, max %.2f ms over %zu frames\n", what, sum / v.size(),
                    v[v.size() * 95 / 100], v.back(), v.size());
    };
    if (bench) {
        // the last 600 frames: the race, not the boot
        size_t from = emulate_ms.size() > 600 ? emulate_ms.size() - 600 : 0;
        report("emulate", emulate_ms, from);
        report("draw both panels", draw_ms, from);
    }
    if (hash_out) std::fclose(hash_out);
    if (state_out) std::fclose(state_out);
    if (s.companion()) {
        auto st = s.companion()->status();
        std::printf("end frames=%llu map=%d serial=%u requests=%d failures=%d %s\n", (unsigned long long)s.frames(),
                    st.showing_map, st.serial, st.requests, st.failures, st.last_failure.c_str());
    }
    std::printf("save %s%s\n", s.save().path.c_str(), s.save().imported_from.empty() ? "" : " (imported)");
    return 0;
}
