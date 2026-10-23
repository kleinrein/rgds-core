// SPDX-License-Identifier: GPL-3.0-or-later
// Headless runner: plays a script against a game and writes both screens.
//
//   play-shot --rom GAME --data DIR --profiles DIR --fonts DIR --script FILE --out DIR
//             [--panel 640x480] [--no-companion] [--hashes FILE] [--async]
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
//   and the script commands without a frame: shot, hudshot, status, ...
// Captures normally run to completion before the next frame, so a run is the
// same every time; --async lets them race the game as on the handheld.
#include "app/session.hpp"
#include "companion/profile.hpp"
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
    std::string script, out_dir = ".", hashes, state_hashes;
    int pw = 640, ph = 480;
    bool async = false, bench = false, drive = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rom") o.rom = next();
        else if (a == "--data") o.data_root = next();
        else if (a == "--profiles") o.profile_dirs.push_back(next());
        else if (a == "--fonts") o.font_dir = next();
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
    Look look;
    uint32_t buttons = 0;
    std::vector<double> emulate_ms, draw_ms;
    auto execute = [&](const Command& c, long frame) {
        if (c.what == "capture" && s.companion()) {
            bool ok = s.companion()->capture_now(s.core());
            std::printf("frame %ld capture %s\n", frame, ok ? "ok" : s.companion()->status().last_failure.c_str());
        } else if (c.what == "shot") {
            draw_game(top, s.game(), Fit::Integer);
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
                    if (s.companion())
                        while (s.companion()->busy()) std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                std::printf("ok %ld\n", frame);
            } else if (what == "read") {
                std::string address;
                int size = 4;
                in >> address >> size;
                std::printf("%x\n", s.core().peek(uint32_t(std::stoul(address, nullptr, 16)), size));
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
            } else if (what == "layers") {
                // Which layers the live picture draws (bits 0-3 BG0-3, bit 4 sprites): to see what is where.
                std::string mask;
                in >> mask;
                s.core().set_layers(unsigned(std::stoul(mask, nullptr, 16)));
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
            draw_game(top, s.game(), Fit::Integer);
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
