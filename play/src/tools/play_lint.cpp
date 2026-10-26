// SPDX-License-Identifier: GPL-3.0-or-later
// The companion lint: every shipped second screen, drawn and checked against
// the rules in docs/COMPANION.md, without a game. A fake core answers the
// views' memory reads (all zeros, then all 0x99, the widest BCD numbers),
// with each view's `on` made true, and every screen is drawn in the four
// looks people use (light, dark, high contrast, Pixel classic). Then:
//
//   size      text only at the baked sizes (16, 20, 24, 28, 36): never scaled
//   edge      text inside the screen, 8 px clear of its edges
//   contrast  4.5:1 against what is behind it (3:1 from 24 px; 7 and 4.5 in high contrast)
//   overlap   no text on top of other text
//   corner    the dock's corner (bottom right) free of text
//   target    whatever takes a touch at least 48 x 48
//
//   play-lint --profiles DIR... --fonts DIR [--shots DIR] [--only CODE]
//
// Prints one line per problem and exits 1 when there is any.
#include "app/session.hpp"
#include "companion/companion.hpp"
#include "companion/party.hpp"
#include "companion/profile.hpp"
#include "companion/view.hpp"
#include "ui/kit.hpp"
#include "ui/screens.hpp"
#include "ui/words.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace play;

namespace {

// A game that is only memory: every byte `fill` unless set.
class FakeCore final : public Core {
public:
    explicit FakeCore(std::string system) : system_(std::move(system)), pixels_(160 * 144, 0xFFA8A8A8u) {}
    uint8_t fill = 0;
    std::map<uint64_t, uint8_t> set; // (bank+1) << 32 | address
    const char* system() const override { return system_.c_str(); }
    std::string game_code() const override { return "LINT"; }
    std::string header_title() const override { return "LINT"; }
    double frame_rate() const override { return 60; }
    void run_frame(uint32_t) override {}
    Picture picture() const override { return {pixels_.data(), 160, 144, 160}; }
    bool save_state(std::vector<uint8_t>&) override { return false; }
    bool load_state(const std::vector<uint8_t>&) override { return false; }
    uint32_t peek(uint32_t address, int size) override { return peek_bank(address, -1, size); }
    uint32_t peek_bank(uint32_t address, int bank, int size) override {
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v |= uint32_t(byte(address + uint32_t(i), bank)) << (8 * i);
        return v;
    }
    void poke8(uint32_t address, int bank, uint8_t value) override {
        set[(uint64_t(bank + 1) << 32) | address] = value;
    }
    size_t audio(int16_t*, size_t) override { return 0; }
    unsigned audio_rate() const override { return 32768; }
    std::vector<uint8_t> battery() const override { return {}; }
    uint64_t battery_revision() const override { return 0; }
    void set_layers(unsigned) override {}
    std::unique_ptr<Core> shadow() const override { return nullptr; }

private:
    uint8_t byte(uint32_t address, int bank) const {
        auto it = set.find((uint64_t(bank + 1) << 32) | address);
        if (it == set.end() && bank >= 0) it = set.find(address); // set without a bank: every bank
        return it != set.end() ? it->second : fill;
    }
    std::string system_;
    std::vector<uint32_t> pixels_;
};

// Writes memory so a condition holds (each term's first value, or its range's low end).
void force(FakeCore& core, const Condition& c) {
    for (const auto& t : c.terms) {
        uint32_t v = t.values.empty() ? 0 : t.values[0];
        if (t.op == Condition::Term::Ne) v = v + 1;
        const uint32_t old = peek_address(core, t.ref.address, t.ref.size);
        v = (old & ~t.ref.mask) | (v & t.ref.mask);
        const int bank = t.ref.address & banked_flag ? int((t.ref.address >> 16) & 0xFF) : -1;
        for (int i = 0; i < t.ref.size; ++i)
            core.poke8((t.ref.address & 0xFFFF) + uint32_t(i), bank, uint8_t(v >> (8 * i)));
    }
}

struct LookCase {
    const char* name;
    Look look;
};

std::vector<LookCase> looks() {
    std::vector<LookCase> out;
    out.push_back({"light", make_look(false, -1, false)});
    out.push_back({"dark", make_look(true, 9, false)});
    out.push_back({"contrast", make_look(true, 5, true)});
    Look pixel = make_look(false, 2, false);
    pixel.pixel_text = true;
    pixel.rich_icons = false;
    pixel.grid = false;
    out.push_back({"pixel", pixel});
    return out;
}

struct Problems {
    std::vector<std::string> lines;
    void add(const std::string& where, const std::string& rule, const std::string& what) {
        lines.push_back(where + ": " + rule + ": " + what);
    }
};

bool intersects(const TextMark& a, const TextMark& b) {
    const int x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
    const int y0 = std::max(a.y + 3, b.y + 3), y1 = std::min(a.y + a.h - 3, b.y + b.h - 3);
    return x1 - x0 > 2 && y1 - y0 > 2;
}

void check(const std::string& where, const Look& look, const std::vector<TextMark>& marks,
           const std::vector<Box>& targets, const Box& corner, int W, int H, Problems& p) {
    const double need_small = look.high_contrast ? 7.0 : 4.5, need_large = look.high_contrast ? 4.5 : 3.0;
    for (size_t i = 0; i < marks.size(); ++i) {
        const TextMark& m = marks[i];
        const std::string quoted = "'" + m.text + "'";
        if (m.asked != m.size)
            p.add(where, "size",
                  quoted + " asked for " + std::to_string(m.asked) + " px (drawn at " + std::to_string(m.size) + ")");
        if (m.x < 8 || m.y < 4 || m.x + m.w > W - 8 || m.y + m.h > H - 4)
            p.add(where, "edge",
                  quoted + " at " + std::to_string(m.x) + "," + std::to_string(m.y) + " (" + std::to_string(m.w) +
                      " wide)");
        const double c = contrast(m.ink, m.ground);
        if (c < (m.size >= 24 ? need_large : need_small)) {
            char ratio[16];
            std::snprintf(ratio, sizeof ratio, "%.2f", c);
            p.add(where, "contrast", quoted + " " + ratio + ":1");
        }
        if (!corner.empty() && m.x + m.w > corner.x && m.y + m.h > corner.y)
            p.add(where, "corner", quoted + " in the dock's corner");
        for (size_t j = i + 1; j < marks.size(); ++j)
            if (intersects(m, marks[j])) p.add(where, "overlap", quoted + " and '" + marks[j].text + "'");
    }
    for (const auto& t : targets)
        if (t.w < kit::touch || t.h < kit::touch)
            p.add(where, "target",
                  std::to_string(t.w) + "x" + std::to_string(t.h) + " at " + std::to_string(t.x) + "," +
                      std::to_string(t.y));
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> profile_dirs;
    std::string fonts, shots, only;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--profiles") profile_dirs.push_back(next());
        else if (a == "--fonts") fonts = next();
        else if (a == "--shots") shots = next();
        else if (a == "--only") only = next();
        else {
            std::fprintf(stderr, "usage: play-lint --profiles DIR... --fonts DIR [--shots DIR] [--only CODE]\n");
            return 2;
        }
    }
    Font rounded, dots;
    if (!rounded.load(fonts) || !dots.load(fonts)) {
        std::fprintf(stderr, "play-lint: no fonts in %s\n", fonts.c_str());
        return 2;
    }
    dots.set_pixel(true);
    Words words;
    Problems problems;
    std::set<std::string> seen;
    int screens = 0;
    std::vector<TextMark> marks;
    std::vector<Box> targets;
    set_text_log(&marks);
    set_target_log(&targets);
    Canvas out(640, 480);
    const Dock corner_dock("", "");
    const Box button = corner_dock.box(Dock::Item::Button, 640, 480);
    const Box corner{button.x - kit::gap / 2, button.y - kit::gap / 2, 0, 0};
    auto shoot = [&](const std::string& name) {
        if (!shots.empty()) write_bmp(shots + "/" + name + ".bmp", out);
    };
    auto run = [&](const std::string& where, const LookCase& lc, bool reserve, auto&& draw) {
        Look look = lc.look;
        if (reserve) {
            look.reserve_w = 640 - corner.x;
            look.reserve_h = 480 - corner.y;
        }
        marks.clear();
        targets.clear();
        out.fill({0, 0, 0});
        draw(look, look.pixel_text ? dots : rounded);
        ++screens;
        check(where + " [" + lc.name + "]", look, marks, targets, reserve ? corner : Box{}, out.w(), out.h(), problems);
        std::string file = where;
        for (char& ch : file)
            if (ch == '/' || ch == ' ' || ch == '[' || ch == ']') ch = '_';
        shoot(file + "_" + lc.name);
    };

    // The dock and the title card, once per look.
    for (const auto& lc : looks()) {
        Dock full("Wario Land: Super Mario Land 3", "GAME BOY");
        full.set_full(true);
        run("dock/full", lc, false, [&](const Look& l, const Font& f) { full.draw(out, l, f, words); });
        Dock over("A Game", "GAME BOY");
        over.set_time(10);
        over.touch({Touch::Down, button.x + 10, button.y + 10});
        over.set_time(5000);
        run("dock/open", lc, false, [&](const Look& l, const Font& f) {
            backdrop(out, l);
            over.draw(out, l, f, words);
        });
    }

    for (const auto& dir : profile_dirs) {
        ProfileLibrary lib;
        std::vector<std::string> errors;
        lib.load_directory(dir, errors);
        for (const auto& e : errors) problems.add(e, "profile", "does not parse");
        for (const auto& [key, profile] : lib.all()) {
            if (!only.empty() && profile.codes.front() != only) continue;
            if (!seen.insert(profile.file).second) continue;
            const std::string name = profile.system + "/" + profile.codes.front();
            for (const auto& lc : looks()) {
                // Before there is anything to show: the title card with the profile's note.
                {
                    FakeCore core(profile.system);
                    Companion c(core, nullptr, profile.title, system_display_name(profile.system));
                    run(name + " title", lc, true, [&](const Look& l, const Font& f) { c.render(out, l, f, words); });
                }
                for (const auto& spec : profile.views)
                    for (uint8_t fill : {uint8_t(0), uint8_t(0x99)}) {
                        FakeCore core(profile.system);
                        core.fill = fill;
                        force(core, spec.on);
                        std::string error;
                        auto view = make_view(spec, error);
                        if (!view) {
                            problems.add(name, "view", error);
                            continue;
                        }
                        for (uint64_t f = 0; f <= 60; f += 15) view->update(core, f);
                        if (!view->shown()) continue;
                        const std::string where =
                            name + " view" + (spec.name.empty() ? "" : "." + spec.name) + (fill ? " full" : " empty");
                        run(where, lc, true, [&](const Look& l, const Font& f) { view->draw(out, l, f, 0); });
                        // Each page of a board, through its tabs.
                        for (int page = 1; page < 4; ++page) {
                            if (targets.size() < size_t(page + 1)) break;
                            const Box tab = targets[size_t(page)];
                            if (!view->touch({Touch::Down, tab.x + tab.w / 2, tab.y + tab.h / 2})) break;
                            run(where + " page" + std::to_string(page + 1), lc, true,
                                [&](const Look& l, const Font& f) { view->draw(out, l, f, 0); });
                        }
                    }
                if (profile.has_party)
                    for (uint8_t fill : {uint8_t(0x01), uint8_t(0x99)}) {
                        FakeCore core(profile.system);
                        core.fill = fill;
                        force(core, profile.party.on);
                        Party party(profile.party);
                        party.read(core);
                        if (party.empty()) continue;
                        run(name + " party" + (fill == 1 ? " small" : " full"), lc, true,
                            [&](const Look& l, const Font& f) { party.draw(out, l, f, 0); });
                    }
            }
        }
    }
    set_text_log(nullptr);
    set_target_log(nullptr);
    for (const auto& line : problems.lines) std::printf("%s\n", line.c_str());
    std::printf("play-lint: %d screens, %zu problems\n", screens, problems.lines.size());
    return problems.lines.empty() ? 0 : 1;
}
